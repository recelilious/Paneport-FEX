// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 recelilious and the Paneport contributors

// A test of the decoder of Source/Windows/Common/SharedUserData.cpp, which emulates a guest load of the shared user data when
// the load faults at the page's fixed address. The test runs the decoder on the host, with an array in place of the page and
// register files of its own, so it needs neither Windows nor an Arm host, and FEX's build does not compile it. run.sh, next to
// it, builds it with AddressSanitizer and UndefinedBehaviorSanitizer and runs it, on 64-bit Linux or macOS.
//
// It prints every check that fails and how many passed, and exits with 0 only when all of them pass. Each instruction word is
// LLVM's encoding of the instruction that its check names, as clang --target=aarch64-linux-gnu -march=armv8.4-a+rcpc+lse
// assembles it.

#include "Windows/Common/SharedUserData.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

using FEX::Windows::SharedUserData::AccessKind;
using FEX::Windows::SharedUserData::HandleAccessViolation;

namespace {
// The page's fixed address, and the array that stands in for the page where it really is.
constexpr uint64_t Fixed = 0x7ffe0000;
alignas(4096) uint8_t Page[4096];

int Checks = 0;
int Failures = 0;

void Check(bool Passed, const char* Instruction, const char* What) {
  Checks++;
  if (!Passed) {
    Failures++;
    printf("FAIL: %s: %s\n", Instruction, What);
  }
}

// What the handler gets: x0 to x30 and then sp, v0 to v31, and the program counter, which points to the instruction.
struct State {
  uint64_t GPRs[32] {};
  alignas(16) __uint128_t FPRs[32] {};
  uint32_t Instruction {};
  uint64_t PC {};

  AccessKind Run(uint32_t Word, uint64_t FaultOffset, uint64_t Redirected = reinterpret_cast<uint64_t>(Page)) {
    Instruction = Word;
    PC = reinterpret_cast<uint64_t>(&Instruction);
    return HandleAccessViolation(Redirected, Fixed + FaultOffset, GPRs, FPRs, PC);
  }

  bool Advanced() const {
    return PC == reinterpret_cast<uint64_t>(&Instruction) + 4;
  }

  bool Stayed() const {
    return PC == reinterpret_cast<uint64_t>(&Instruction);
  }
};

uint64_t PageValue(uint64_t Offset, uint32_t Size) {
  uint64_t Value = 0;
  memcpy(&Value, Page + Offset, Size);
  return Value;
}

__uint128_t PageVector(uint64_t Offset, uint32_t Size) {
  __uint128_t Value = 0;
  memcpy(&Value, Page + Offset, Size);
  return Value;
}

// A register value whose bytes all differ, so that a lane written at the wrong place, or a byte that changes, shows.
__uint128_t Pattern() {
  uint8_t Bytes[16];
  for (uint32_t Index = 0; Index < sizeof(Bytes); Index++) {
    Bytes[Index] = static_cast<uint8_t>(0xa0 + Index);
  }
  __uint128_t Value;
  memcpy(&Value, Bytes, sizeof(Value));
  return Value;
}

constexpr __uint128_t AllOnes = ~static_cast<__uint128_t>(0);

// A load into x1 of Size bytes at Offset in the page, zero-extended, from x2 and maybe x3, which must not change.
void LoadGPR(const char* Instruction, uint32_t Word, uint64_t Base, uint64_t Index, uint64_t Offset, uint32_t Size) {
  State S;
  S.GPRs[1] = 0xdeadbeefdeadbeefULL;
  S.GPRs[2] = Fixed + Base;
  S.GPRs[3] = Index;
  Check(S.Run(Word, Offset) == AccessKind::Emulated && S.Advanced(), Instruction, "emulated, and the PC moves on");
  Check(S.GPRs[1] == PageValue(Offset, Size), Instruction, "the value");
  Check(S.GPRs[2] == Fixed + Base && S.GPRs[3] == Index, Instruction, "the base and index registers keep their values");
}

// A sign-extending load into x1 or w1 from the bytes at 0x280, whose top bits are set.
void LoadSigned(const char* Instruction, uint32_t Word, uint64_t Expected) {
  State S;
  S.GPRs[1] = 0xdeadbeefdeadbeefULL;
  S.GPRs[2] = Fixed + 0x280;
  Check(S.Run(Word, 0x280) == AccessKind::Emulated && S.Advanced(), Instruction, "emulated, and the PC moves on");
  Check(S.GPRs[1] == Expected, Instruction, "the sign-extended value");
}

// A load of a whole B, H, S, D or Q register from x2 and maybe x3, which sets the rest of the register to zero.
void LoadVector(const char* Instruction, uint32_t Word, uint64_t Base, uint64_t Offset, uint32_t Size, uint64_t Index = 0) {
  State S;
  S.FPRs[0] = AllOnes;
  S.GPRs[2] = Fixed + Base;
  S.GPRs[3] = Index;
  Check(S.Run(Word, Offset) == AccessKind::Emulated && S.Advanced(), Instruction, "emulated, and the PC moves on");
  Check(S.FPRs[0] == PageVector(Offset, Size), Instruction, "the value, with the rest of the register zero");
}

// LD1 of one lane of v0 from 0x268. It writes only that lane and keeps the rest of the register, whatever Q is, because Q is
// only the top bit of the lane index. A post-indexed load adds Increment to the base register x2; x3 holds 0x10.
void LoadLane(const char* Instruction, uint32_t Word, uint32_t Lane, uint32_t Size, uint64_t Increment) {
  State S;
  S.FPRs[0] = Pattern();
  S.GPRs[2] = Fixed + 0x268;
  S.GPRs[3] = 0x10;
  Check(S.Run(Word, 0x268) == AccessKind::Emulated && S.Advanced(), Instruction, "emulated, and the PC moves on");
  uint8_t Expected[16];
  const __uint128_t Initial = Pattern();
  memcpy(Expected, &Initial, sizeof(Expected));
  memcpy(Expected + (Lane * Size), Page + 0x268, Size);
  Check(memcmp(&S.FPRs[0], Expected, sizeof(Expected)) == 0, Instruction, "the lane, with the rest of the register kept");
  Check(S.GPRs[2] == Fixed + 0x268 + Increment, Instruction, "the base register");
}

// LD1R from 0x268 into every lane of Bytes bytes of v0, all 128 or the lower 64, with the rest of the register zero.
void LoadReplicate(const char* Instruction, uint32_t Word, uint32_t Size, uint32_t Bytes) {
  State S;
  S.FPRs[0] = Pattern();
  S.GPRs[2] = Fixed + 0x268;
  Check(S.Run(Word, 0x268) == AccessKind::Emulated && S.Advanced(), Instruction, "emulated, and the PC moves on");
  uint8_t Expected[16] {};
  for (uint32_t Offset = 0; Offset < Bytes; Offset += Size) {
    memcpy(Expected + Offset, Page + 0x268, Size);
  }
  Check(memcmp(&S.FPRs[0], Expected, sizeof(Expected)) == 0, Instruction, "every lane, with the rest of the register zero");
}

// An access to 0x268 that the handler does not emulate: it reports it as a write, or leaves it to the guest as Unknown, and
// changes no register.
void NotEmulated(const char* Instruction, uint32_t Word, AccessKind Expected) {
  State S;
  S.GPRs[1] = 1;
  S.GPRs[2] = Fixed + 0x268;
  S.GPRs[3] = 3;
  S.FPRs[0] = Pattern();
  Check(S.Run(Word, 0x268) == Expected, Instruction, Expected == AccessKind::Write ? "reported as a write" : "left to the guest");
  Check(S.Stayed() && S.GPRs[1] == 1 && S.GPRs[2] == Fixed + 0x268 && S.GPRs[3] == 3 && S.FPRs[0] == Pattern(), Instruction, "no register changes");
}

// A load from x2 at Base, faulting at Fault, that the handler must refuse, because the access leaves the page or the fault is
// not the access's.
void Refused(const char* Instruction, const char* Why, uint32_t Word, uint64_t Base, uint64_t Fault) {
  State S;
  S.GPRs[1] = 1;
  S.GPRs[2] = Fixed + Base;
  Check(S.Run(Word, Fault) == AccessKind::Unknown && S.Stayed() && S.GPRs[1] == 1, Instruction, Why);
}
} // namespace

int main() {
  for (uint32_t Index = 0; Index < sizeof(Page); Index++) {
    Page[Index] = static_cast<uint8_t>((Index * 13) + 7);
  }
  const uint8_t Negative[8] = {0x9a, 0xbc, 0xde, 0xf0, 0x87, 0x65, 0x43, 0xa1};
  memcpy(Page + 0x280, Negative, sizeof(Negative));

  // General-purpose loads: immediate and register offsets, each extension of the index, and the acquire loads of LoadMemTSO.
  LoadGPR("ldrb w1, [x2]", 0x39400041, 0x268, 0, 0x268, 1);
  LoadGPR("ldrh w1, [x2, #4]", 0x79400841, 0x268, 0, 0x26c, 2);
  LoadGPR("ldr w1, [x2, #0x6c]", 0xb9406c41, 0x200, 0, 0x26c, 4);
  LoadGPR("ldr x1, [x2, #8]", 0xf9400441, 0x268, 0, 0x270, 8);
  LoadGPR("ldur w1, [x2, #-4]", 0xb85fc041, 0x270, 0, 0x26c, 4);
  LoadGPR("ldr w1, [x2, x3]", 0xb8636841, 0x200, 0x6c, 0x26c, 4);
  LoadGPR("ldr w1, [x2, x3, lsl #2]", 0xb8637841, 0x200, 0x1b, 0x26c, 4);
  LoadGPR("ldr w1, [x2, w3, uxtw]", 0xb8634841, 0x200, 0xffffffff0000006cULL, 0x26c, 4);
  LoadGPR("ldr w1, [x2, w3, sxtw #2]", 0xb863d841, 0x300, static_cast<uint32_t>(-0x25), 0x26c, 4);
  // SXTX is the extension that FEX writes for the index of a guest load. An index of 2^32 is not the extension of its low
  // 32 bits, so SXTX, SXTW and UXTW give different addresses.
  LoadGPR("ldr w1, [x2, x3, sxtx]", 0xb863e841, 0x26c - 0x100000000ULL, 0x100000000ULL, 0x26c, 4);
  LoadGPR("ldr w1, [x2, x3, sxtx #2]", 0xb863f841, 0x300, static_cast<uint64_t>(-0x25LL), 0x26c, 4);
  LoadGPR("ldapr w1, [x2]", 0xb8bfc041, 0x26c, 0, 0x26c, 4);
  LoadGPR("ldar x1, [x2]", 0xc8dffc41, 0x268, 0, 0x268, 8);
  LoadGPR("ldarb w1, [x2]", 0x08dffc41, 0x268, 0, 0x268, 1);
  LoadGPR("ldaprh w1, [x2]", 0x78bfc041, 0x26c, 0, 0x26c, 2);
  LoadGPR("ldlar w1, [x2]", 0x88df7c41, 0x26c, 0, 0x26c, 4);
  LoadGPR("ldapur w1, [x2, #-4]", 0x995fc041, 0x270, 0, 0x26c, 4);
  // The last bytes of the page.
  LoadGPR("ldr w1, [x2, #4]", 0xb9400441, 0xff8, 0, 0xffc, 4);

  LoadSigned("ldrsb x1, [x2]", 0x39800041, 0xffffffffffffff9aULL);
  LoadSigned("ldrsh w1, [x2]", 0x79c00041, 0x00000000ffffbc9aULL);
  LoadSigned("ldrsw x1, [x2]", 0xb9800041, 0xfffffffff0debc9aULL);
  LoadSigned("ldapursb x1, [x2]", 0x19800041, 0xffffffffffffff9aULL);
  LoadSigned("ldapursb w1, [x2]", 0x19c00041, 0x00000000ffffff9aULL);

  LoadVector("ldr b0, [x2]", 0x3d400040, 0x268, 0x268, 1);
  LoadVector("ldr h0, [x2]", 0x7d400040, 0x268, 0x268, 2);
  LoadVector("ldr s0, [x2]", 0xbd400040, 0x268, 0x268, 4);
  LoadVector("ldr d0, [x2]", 0xfd400040, 0x268, 0x268, 8);
  LoadVector("ldr q0, [x2]", 0x3dc00040, 0x260, 0x260, 16);
  // The offset of a Q register load is scaled by 16, not by its size field.
  LoadVector("ldr q0, [x2, #16]", 0x3dc00440, 0x250, 0x260, 16);
  LoadVector("ldur q0, [x2, #-16]", 0x3cdf0040, 0x270, 0x260, 16);
  // A register offset with SXTX, scaled or not, as FEX writes it for a vector load through a base and an index.
  LoadVector("ldr q0, [x2, x3, sxtx]", 0x3ce3e840, 0x260 - 0x100000000ULL, 0x260, 16, 0x100000000ULL);
  LoadVector("ldr d0, [x2, x3, sxtx #3]", 0xfc63f840, 0x300, 0x268, 8, static_cast<uint64_t>(-0x13LL));

  // Writeback of the base register.
  {
    const char* Instruction = "ldrb w1, [x2], #1";
    State S;
    S.GPRs[2] = Fixed + 0x268;
    Check(S.Run(0x38401441, 0x268) == AccessKind::Emulated && S.Advanced(), Instruction, "emulated, and the PC moves on");
    Check(S.GPRs[1] == PageValue(0x268, 1) && S.GPRs[2] == Fixed + 0x269, Instruction, "the value, and the base after the load");
  }
  {
    const char* Instruction = "ldr x1, [x2, #8]!";
    State S;
    S.GPRs[2] = Fixed + 0x260;
    Check(S.Run(0xf8408c41, 0x268) == AccessKind::Emulated && S.Advanced(), Instruction, "emulated, and the PC moves on");
    Check(S.GPRs[1] == PageValue(0x268, 8) && S.GPRs[2] == Fixed + 0x268, Instruction, "the value, and the base before the load");
  }

  // Pairs, which the handler loads whole whichever register's access faulted.
  {
    const char* Instruction = "ldp w1, w3, [x2]";
    State S;
    S.GPRs[2] = Fixed + 0x268;
    Check(S.Run(0x29400c41, 0x26c) == AccessKind::Emulated && S.Advanced(), Instruction, "emulated when the second access faults");
    Check(S.GPRs[1] == PageValue(0x268, 4) && S.GPRs[3] == PageValue(0x26c, 4), Instruction, "both values");
  }
  {
    const char* Instruction = "ldp x1, x3, [x2, #16]";
    State S;
    S.GPRs[2] = Fixed + 0x250;
    Check(S.Run(0xa9410c41, 0x260) == AccessKind::Emulated && S.Advanced(), Instruction, "emulated, and the PC moves on");
    Check(S.GPRs[1] == PageValue(0x260, 8) && S.GPRs[3] == PageValue(0x268, 8) && S.GPRs[2] == Fixed + 0x250, Instruction,
          "both values, and the base keeps its value");
  }
  {
    const char* Instruction = "ldp q0, q1, [x2], #32";
    State S;
    S.GPRs[2] = Fixed + 0x260;
    Check(S.Run(0xacc10440, 0x260) == AccessKind::Emulated && S.Advanced(), Instruction, "emulated, and the PC moves on");
    Check(S.FPRs[0] == PageVector(0x260, 16) && S.FPRs[1] == PageVector(0x270, 16) && S.GPRs[2] == Fixed + 0x280, Instruction,
          "both values, and the base after the load");
  }
  {
    const char* Instruction = "ldp s0, s1, [x2]";
    State S;
    S.FPRs[0] = AllOnes;
    S.FPRs[1] = AllOnes;
    S.GPRs[2] = Fixed + 0x268;
    Check(S.Run(0x2d400440, 0x268) == AccessKind::Emulated && S.Advanced(), Instruction, "emulated, and the PC moves on");
    Check(S.FPRs[0] == PageVector(0x268, 4) && S.FPRs[1] == PageVector(0x26c, 4), Instruction, "both values, with the rest of each register zero");
  }
  {
    const char* Instruction = "ldpsw x1, x3, [x2]";
    State S;
    S.GPRs[2] = Fixed + 0x280;
    Check(S.Run(0x69400c41, 0x280) == AccessKind::Emulated && S.Advanced(), Instruction, "emulated, and the PC moves on");
    Check(S.GPRs[1] == 0xfffffffff0debc9aULL && S.GPRs[3] == 0xffffffffa1436587ULL, Instruction, "both sign-extended values");
  }
  {
    const char* Instruction = "ldnp d0, d1, [x2]";
    State S;
    S.GPRs[2] = Fixed + 0x268;
    Check(S.Run(0x6c400440, 0x268) == AccessKind::Emulated && S.Advanced(), Instruction, "emulated, and the PC moves on");
    Check(S.FPRs[0] == PageVector(0x268, 8) && S.FPRs[1] == PageVector(0x270, 8), Instruction, "both values");
  }

  // Loads of one lane, with Q clear (a lane in the lower 64 bits) and set, and post-indexed by the size or by a register.
  // movlps, movlpd, pinsr* and their VEX forms read memory this way.
  LoadLane("ld1 {v0.b}[5], [x2]", 0x0d401440, 5, 1, 0);
  LoadLane("ld1 {v0.b}[13], [x2]", 0x4d401440, 13, 1, 0);
  LoadLane("ld1 {v0.h}[3], [x2]", 0x0d405840, 3, 2, 0);
  LoadLane("ld1 {v0.h}[6], [x2]", 0x4d405040, 6, 2, 0);
  LoadLane("ld1 {v0.s}[0], [x2]", 0x0d408040, 0, 4, 0);
  LoadLane("ld1 {v0.s}[2], [x2]", 0x4d408040, 2, 4, 0);
  LoadLane("ld1 {v0.d}[0], [x2]", 0x0d408440, 0, 8, 0);
  LoadLane("ld1 {v0.d}[1], [x2]", 0x4d408440, 1, 8, 0);
  LoadLane("ld1 {v0.s}[1], [x2], #4", 0x0ddf9040, 1, 4, 4);
  LoadLane("ld1 {v0.h}[1], [x2], x3", 0x0dc34840, 1, 2, 0x10);
  LoadReplicate("ld1r {v0.4s}, [x2]", 0x4d40c840, 4, 16);
  LoadReplicate("ld1r {v0.8b}, [x2]", 0x0d40c040, 1, 8);

  // sp as a base register, and the zero register as a destination, which must leave sp alone.
  {
    const char* Instruction = "ldr x1, [sp, #8]";
    State S;
    S.GPRs[31] = Fixed + 0x260;
    Check(S.Run(0xf94007e1, 0x268) == AccessKind::Emulated && S.Advanced(), Instruction, "emulated, and the PC moves on");
    Check(S.GPRs[1] == PageValue(0x268, 8) && S.GPRs[31] == Fixed + 0x260, Instruction, "the value, and sp keeps its value");
  }
  {
    const char* Instruction = "ldr wzr, [x2]";
    State S;
    S.GPRs[2] = Fixed + 0x268;
    S.GPRs[31] = 0x1234;
    Check(S.Run(0xb940005f, 0x268) == AccessKind::Emulated && S.Advanced(), Instruction, "emulated, and the PC moves on");
    Check(S.GPRs[31] == 0x1234, Instruction, "sp keeps its value");
  }

  // Atomic and exclusive accesses write, so they are reported as writes; stores are left to the guest.
  NotEmulated("ldaxr w1, [x2]", 0x885ffc41, AccessKind::Write);
  NotEmulated("stlxr w4, w1, [x2]", 0x8804fc41, AccessKind::Write);
  NotEmulated("casal w1, w3, [x2]", 0x88e1fc43, AccessKind::Write);
  NotEmulated("ldaddal w1, w3, [x2]", 0xb8e10043, AccessKind::Write);
  NotEmulated("swpal w1, w3, [x2]", 0xb8e18043, AccessKind::Write);
  NotEmulated("ldxp x1, x3, [x2]", 0xc87f0c41, AccessKind::Write);
  NotEmulated("str w1, [x2]", 0xb9000041, AccessKind::Unknown);
  NotEmulated("stp x1, x3, [x2]", 0xa9000c41, AccessKind::Unknown);
  NotEmulated("st1 {v0.s}[1], [x2]", 0x0d009040, AccessKind::Unknown);

  // Accesses that leave the page, and faults that are not the access's.
  Refused("ldr w1, [x2, #4]", "a load past the end of the page", 0xb9400441, 0xffc, 0x1000);
  Refused("ldr x1, [x2, #8]", "a load across the end of the page", 0xf9400441, 0xff4, 0xffc);
  Refused("ldur x1, [x2, #-8]", "a load across the start of the page", 0xf85f8041, 0x4, 0x0);
  Refused("ldrb w1, [x2]", "a fault outside the access", 0x39400041, 0x268, 0x300);
  {
    State S;
    S.GPRs[2] = 0x10000;
    S.GPRs[1] = 1;
    Check(S.Run(0x39400041, 0x10000 - Fixed) == AccessKind::Unknown && S.Stayed() && S.GPRs[1] == 1, "ldrb w1, [x2]", "a fault outside the page");
  }
  {
    State S;
    S.GPRs[2] = Fixed + 0x268;
    S.GPRs[1] = 1;
    Check(S.Run(0x39400041, 0x268, 0) == AccessKind::Unknown && S.Stayed() && S.GPRs[1] == 1, "ldrb w1, [x2]",
          "a fault while the page has no other address");
  }

  printf("%d of %d checks passed\n", Checks - Failures, Checks);
  return Failures == 0 ? 0 : 1;
}
