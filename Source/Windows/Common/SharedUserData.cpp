// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 recelilious and the Paneport contributors

#include "Windows/Common/SharedUserData.h"

#include <FEXCore/Core/Context.h>

#include <cstring>
#include <optional>

// Wine on Apple silicon cannot map the shared user data at its fixed address, because the low 4 GB of every process are
// the executable's page zero there, so it maps the page where macOS chooses. The JIT redirects loads from a constant
// address in the page when it translates them (OpDispatchBuilder::RedirectSharedUserDataLoad). Any other guest load of
// the page, such as one through a register that holds the page's address, faults, and this handler emulates the host
// load that faulted, from where the page really is. The encodings are those that FEX's code emitter writes for the IR
// operations that read guest memory: LoadMem, LoadMemTSO, LoadMemPair, VLoadVectorElement, VBroadcastFromMem, Pop and
// MemCpy.
namespace FEX::Windows::SharedUserData {
namespace {
  constexpr uint64_t PageBegin = FEXCore::Context::SharedUserDataFixedAddress;
  constexpr uint64_t PageEnd = PageBegin + FEXCore::Context::SharedUserDataSize;

  // As a base register, 31 is sp, which GPRs holds after x30; as a destination or index register, it is the zero register.
  constexpr uint32_t ZeroRegister = 31;

  // One load that the handler emulates.
  struct Load {
    // The address that the load reads, and the bytes that it reads into each register.
    uint64_t Address {};
    uint32_t Size {};
    uint32_t Rt {};
    // The second register of a load pair.
    bool Pair {};
    uint32_t Rt2 {};
    // The destination is a SIMD&FP register.
    bool Vector {};
    // A general-purpose load that sign-extends its value, to 64 bits or to 32 bits.
    bool SignExtend {};
    bool SignExtendTo64 {};
    // An ASIMD load of one lane, or of one element into every lane. Q selects all 128 bits; without it, the upper 64 bits
    // of the register become zero.
    bool Lane {};
    uint32_t LaneIndex {};
    bool Replicate {};
    bool Q {};
    // A pre- or post-indexed load writes its base register.
    bool Writeback {};
    uint32_t Rn {};
    uint64_t NewBase {};
  };

  int64_t SignExtendBits(uint64_t Value, uint32_t Bits) {
    const uint64_t Sign = 1ULL << (Bits - 1);
    return static_cast<int64_t>((Value ^ Sign) - Sign);
  }

  // The size, sign extension and destination of a load from the opc and size fields that the register loads share.
  bool DecodeRegisterLoadKind(uint32_t SizeField, bool Vector, uint32_t Opc, Load& L, uint32_t& Scale) {
    Scale = SizeField;
    if (Vector) {
      if (Opc == 0b01) {
        // LDR (SIMD&FP) of a B, H, S or D register.
        L.Size = 1U << SizeField;
        return true;
      }
      if (Opc == 0b11 && SizeField == 0) {
        // LDR (SIMD&FP) of a Q register.
        L.Size = 16;
        Scale = 4;
        return true;
      }
      // A store, or unallocated.
      return false;
    }

    if (Opc == 0b01) {
      // LDRB, LDRH, LDR of a W or an X register.
      L.Size = 1U << SizeField;
      return true;
    }
    if (Opc == 0b10 && SizeField != 0b11) {
      // LDRSB, LDRSH and LDRSW to an X register.
      L.Size = 1U << SizeField;
      L.SignExtend = true;
      L.SignExtendTo64 = true;
      return true;
    }
    if (Opc == 0b11 && SizeField < 0b10) {
      // LDRSB and LDRSH to a W register.
      L.Size = 1U << SizeField;
      L.SignExtend = true;
      return true;
    }
    // A store, PRFM, or unallocated.
    return false;
  }

  // Load/store register: unsigned immediate, unscaled immediate, pre- and post-indexed, unprivileged and register offset.
  std::optional<Load> DecodeLoadRegister(uint32_t Instr, const uint64_t* GPRs) {
    const bool UnsignedImmediate = (Instr & 0x3B00'0000) == 0x3900'0000;
    const bool Immediate9 = (Instr & 0x3B20'0000) == 0x3800'0000;
    const bool RegisterOffset = (Instr & 0x3B20'0C00) == 0x3820'0800;
    if (!UnsignedImmediate && !Immediate9 && !RegisterOffset) {
      return std::nullopt;
    }

    Load L {};
    uint32_t Scale {};
    if (!DecodeRegisterLoadKind(Instr >> 30, (Instr >> 26) & 1, (Instr >> 22) & 0b11, L, Scale)) {
      return std::nullopt;
    }
    L.Vector = (Instr >> 26) & 1;
    L.Rt = Instr & 0x1F;
    L.Rn = (Instr >> 5) & 0x1F;
    const uint64_t Base = GPRs[L.Rn];

    if (UnsignedImmediate) {
      L.Address = Base + (static_cast<uint64_t>((Instr >> 10) & 0xFFF) << Scale);
    } else if (Immediate9) {
      const int64_t Offset = SignExtendBits((Instr >> 12) & 0x1FF, 9);
      switch ((Instr >> 10) & 0b11) {
      case 0b00: // Unscaled, LDUR.
      case 0b10: // Unprivileged, which reads as LDUR does in user mode.
        L.Address = Base + Offset;
        break;
      case 0b01: // Post-indexed.
        L.Address = Base;
        L.Writeback = true;
        L.NewBase = Base + Offset;
        break;
      case 0b11: // Pre-indexed.
        L.Address = Base + Offset;
        L.Writeback = true;
        L.NewBase = L.Address;
        break;
      }
    } else {
      const uint32_t Rm = (Instr >> 16) & 0x1F;
      uint64_t Offset = Rm == ZeroRegister ? 0 : GPRs[Rm];
      switch ((Instr >> 13) & 0b111) {
      case 0b010: Offset = static_cast<uint32_t>(Offset); break;                                         // UXTW
      case 0b011: break;                                                                                 // LSL
      case 0b110: Offset = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(Offset))); break; // SXTW
      case 0b111: break;                                                                                 // SXTX
      default: return std::nullopt;
      }
      if ((Instr >> 12) & 1) {
        Offset <<= Scale;
      }
      L.Address = Base + Offset;
    }
    return L;
  }

  // LDAPUR and LDAPURS*, the loads with an unscaled offset that LoadMemTSO emits on hosts with RCpc 2.
  std::optional<Load> DecodeLoadAcquireUnscaled(uint32_t Instr, const uint64_t* GPRs) {
    if ((Instr & 0x3F20'0C00) != 0x1900'0000) {
      return std::nullopt;
    }
    Load L {};
    uint32_t Scale {};
    // These share the opc values of the register loads, without SIMD&FP forms; opc 0b00 is STLUR.
    if (!DecodeRegisterLoadKind(Instr >> 30, false, (Instr >> 22) & 0b11, L, Scale)) {
      return std::nullopt;
    }
    L.Rt = Instr & 0x1F;
    L.Rn = (Instr >> 5) & 0x1F;
    L.Address = GPRs[L.Rn] + SignExtendBits((Instr >> 12) & 0x1FF, 9);
    return L;
  }

  // LDAR, LDLAR and LDAPR, which LoadMemTSO and MemCpy emit, read through a base register alone.
  std::optional<Load> DecodeLoadAcquire(uint32_t Instr, const uint64_t* GPRs) {
    const uint32_t Masked = Instr & 0x3FFF'FC00;
    if (Masked != 0x08DF'FC00 && Masked != 0x08DF'7C00 && Masked != 0x38BF'C000) {
      return std::nullopt;
    }
    Load L {};
    L.Size = 1U << (Instr >> 30);
    L.Rt = Instr & 0x1F;
    L.Rn = (Instr >> 5) & 0x1F;
    L.Address = GPRs[L.Rn];
    return L;
  }

  // LDP, LDPSW and LDNP of general-purpose and SIMD&FP registers, with an offset, pre- or post-indexed.
  std::optional<Load> DecodeLoadPair(uint32_t Instr, const uint64_t* GPRs) {
    if ((Instr & 0x3A00'0000) != 0x2800'0000 || ((Instr >> 22) & 1) == 0) {
      return std::nullopt;
    }
    const uint32_t Opc = Instr >> 30;
    const bool Vector = (Instr >> 26) & 1;
    const uint32_t Indexing = (Instr >> 23) & 0b11;

    Load L {};
    uint32_t Scale {};
    if (Vector) {
      if (Opc == 0b11) {
        return std::nullopt;
      }
      Scale = 2 + Opc;
    } else if (Opc == 0b00) {
      Scale = 2;
    } else if (Opc == 0b10) {
      Scale = 3;
    } else if (Opc == 0b01 && Indexing != 0b00) {
      // LDPSW.
      Scale = 2;
      L.SignExtend = true;
      L.SignExtendTo64 = true;
    } else {
      return std::nullopt;
    }

    L.Size = 1U << Scale;
    L.Pair = true;
    L.Vector = Vector;
    L.Rt = Instr & 0x1F;
    L.Rt2 = (Instr >> 10) & 0x1F;
    L.Rn = (Instr >> 5) & 0x1F;
    const uint64_t Base = GPRs[L.Rn];
    const int64_t Offset = SignExtendBits((Instr >> 15) & 0x7F, 7) * static_cast<int64_t>(L.Size);
    switch (Indexing) {
    case 0b00: // LDNP.
    case 0b10: // Signed offset.
      L.Address = Base + Offset;
      break;
    case 0b01: // Post-indexed.
      L.Address = Base;
      L.Writeback = true;
      L.NewBase = Base + Offset;
      break;
    case 0b11: // Pre-indexed.
      L.Address = Base + Offset;
      L.Writeback = true;
      L.NewBase = L.Address;
      break;
    }
    return L;
  }

  // LD1 of one lane and LD1R, without an offset or post-indexed, which VLoadVectorElement and VBroadcastFromMem emit.
  std::optional<Load> DecodeLoadSingleStructure(uint32_t Instr, const uint64_t* GPRs) {
    const bool NoOffset = (Instr & 0xBFFF'0000) == 0x0D40'0000;
    const bool PostIndexed = (Instr & 0xBFE0'0000) == 0x0DC0'0000;
    if (!NoOffset && !PostIndexed) {
      return std::nullopt;
    }
    const uint32_t Q = (Instr >> 30) & 1;
    const uint32_t S = (Instr >> 12) & 1;
    const uint32_t SizeField = (Instr >> 10) & 0b11;

    Load L {};
    switch ((Instr >> 13) & 0b111) {
    case 0b000:
      L.Lane = true;
      L.Size = 1;
      L.LaneIndex = (Q << 3) | (S << 2) | SizeField;
      break;
    case 0b010:
      if (SizeField & 1) {
        return std::nullopt;
      }
      L.Lane = true;
      L.Size = 2;
      L.LaneIndex = (Q << 2) | (S << 1) | (SizeField >> 1);
      break;
    case 0b100:
      if (SizeField == 0b00) {
        L.Lane = true;
        L.Size = 4;
        L.LaneIndex = (Q << 1) | S;
      } else if (SizeField == 0b01 && S == 0) {
        L.Lane = true;
        L.Size = 8;
        L.LaneIndex = Q;
      } else {
        return std::nullopt;
      }
      break;
    case 0b110:
      if (S != 0) {
        return std::nullopt;
      }
      L.Replicate = true;
      L.Size = 1U << SizeField;
      break;
    default: return std::nullopt;
    }

    L.Vector = true;
    L.Q = Q != 0;
    L.Rt = Instr & 0x1F;
    L.Rn = (Instr >> 5) & 0x1F;
    L.Address = GPRs[L.Rn];
    if (PostIndexed) {
      const uint32_t Rm = (Instr >> 16) & 0x1F;
      L.Writeback = true;
      L.NewBase = L.Address + (Rm == ZeroRegister ? L.Size : GPRs[Rm]);
    }
    return L;
  }

  std::optional<Load> DecodeLoad(uint32_t Instr, const uint64_t* GPRs) {
    if (auto L = DecodeLoadRegister(Instr, GPRs)) {
      return L;
    }
    if (auto L = DecodeLoadAcquireUnscaled(Instr, GPRs)) {
      return L;
    }
    if (auto L = DecodeLoadAcquire(Instr, GPRs)) {
      return L;
    }
    if (auto L = DecodeLoadPair(Instr, GPRs)) {
      return L;
    }
    return DecodeLoadSingleStructure(Instr, GPRs);
  }

  // The exclusive, ordered and compare-and-swap instructions, other than the ordered loads, and the atomic memory
  // operations. FEX emits them for locked x86 instructions, XCHG and CMPXCHG, which write memory.
  bool IsAtomicWrite(uint32_t Instr) {
    if ((Instr & 0x3F00'0000) == 0x0800'0000) {
      return true;
    }
    return (Instr & 0x3F20'0C00) == 0x3820'0000;
  }

  // Reads Size bytes from the page where it really is, with one access when they are aligned, as x86 reads them.
  void ReadPage(uint64_t Redirected, uint64_t Address, uint32_t Size, uint8_t* Out) {
    const uint64_t Source = Redirected + (Address - PageBegin);
    const bool Aligned = (Source & (Size - 1)) == 0;
    if (Aligned && Size == 1) {
      const uint8_t Value = __atomic_load_n(reinterpret_cast<const uint8_t*>(Source), __ATOMIC_ACQUIRE);
      memcpy(Out, &Value, sizeof(Value));
    } else if (Aligned && Size == 2) {
      const uint16_t Value = __atomic_load_n(reinterpret_cast<const uint16_t*>(Source), __ATOMIC_ACQUIRE);
      memcpy(Out, &Value, sizeof(Value));
    } else if (Aligned && Size == 4) {
      const uint32_t Value = __atomic_load_n(reinterpret_cast<const uint32_t*>(Source), __ATOMIC_ACQUIRE);
      memcpy(Out, &Value, sizeof(Value));
    } else if (Aligned && Size == 8) {
      const uint64_t Value = __atomic_load_n(reinterpret_cast<const uint64_t*>(Source), __ATOMIC_ACQUIRE);
      memcpy(Out, &Value, sizeof(Value));
    } else {
      memcpy(Out, reinterpret_cast<const void*>(Source), Size);
    }
  }

  void WriteVector(__uint128_t* FPRs, uint32_t Register, const uint8_t* Bytes) {
    memcpy(&FPRs[Register], Bytes, 16);
  }

  void Apply(const Load& L, uint64_t Redirected, uint64_t* GPRs, __uint128_t* FPRs) {
    const uint32_t Count = L.Pair ? 2 : 1;
    for (uint32_t Index = 0; Index < Count; Index++) {
      const uint32_t Register = Index == 0 ? L.Rt : L.Rt2;
      uint8_t Value[16] {};
      ReadPage(Redirected, L.Address + (Index * L.Size), L.Size, Value);

      if (L.Vector) {
        uint8_t Vector[16] {};
        if (L.Lane) {
          memcpy(Vector, &FPRs[Register], sizeof(Vector));
          memcpy(Vector + (L.LaneIndex * L.Size), Value, L.Size);
          if (!L.Q) {
            memset(Vector + 8, 0, 8);
          }
        } else if (L.Replicate) {
          const uint32_t Bytes = L.Q ? 16 : 8;
          for (uint32_t Offset = 0; Offset < Bytes; Offset += L.Size) {
            memcpy(Vector + Offset, Value, L.Size);
          }
        } else {
          memcpy(Vector, Value, L.Size);
        }
        WriteVector(FPRs, Register, Vector);
        continue;
      }

      if (Register == ZeroRegister) {
        continue;
      }
      uint64_t Result {};
      memcpy(&Result, Value, L.Size);
      if (L.SignExtend) {
        const int64_t Extended = SignExtendBits(Result, L.Size * 8);
        Result = L.SignExtendTo64 ? static_cast<uint64_t>(Extended) : static_cast<uint32_t>(Extended);
      }
      GPRs[Register] = Result;
    }

    if (L.Writeback) {
      GPRs[L.Rn] = L.NewBase;
    }
  }
} // namespace

AccessKind HandleAccessViolation(uint64_t Redirected, uint64_t FaultAddress, uint64_t* GPRs, __uint128_t* FPRs, uint64_t& PC) {
  if (Redirected == 0 || FaultAddress < PageBegin || FaultAddress >= PageEnd) {
    return AccessKind::Unknown;
  }

  const uint32_t Instr = *reinterpret_cast<const uint32_t*>(PC);
  const auto L = DecodeLoad(Instr, GPRs);
  if (!L) {
    return IsAtomicWrite(Instr) ? AccessKind::Write : AccessKind::Unknown;
  }

  // The access must lie in the page, and must be the one that faulted.
  const uint64_t Total = static_cast<uint64_t>(L->Size) * (L->Pair ? 2 : 1);
  if (L->Address < PageBegin || L->Address > PageEnd - Total || FaultAddress < L->Address || FaultAddress - L->Address >= Total) {
    return AccessKind::Unknown;
  }

  Apply(*L, Redirected, GPRs, FPRs);
  PC += 4;
  return AccessKind::Emulated;
}
} // namespace FEX::Windows::SharedUserData
