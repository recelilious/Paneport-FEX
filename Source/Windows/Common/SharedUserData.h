// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 recelilious and the Paneport contributors
#pragma once

#include <cstdint>

namespace FEX::Windows::SharedUserData {
// What the handler made of an access violation in generated code.
enum class AccessKind {
  // Not an access to the shared user data at its fixed address, or an instruction that the handler does not know.
  Unknown,
  // A load of the page, emulated from where the page really is: the destination registers, any writeback of the base
  // register and the program counter are updated, and execution can continue.
  Emulated,
  // An atomic or exclusive access to the page, which writes too. The host reports its fault as a read, because the read
  // that it starts with faults, but Windows reports a write to the page, which is read-only for user mode.
  Write,
};

// Handles a fault of generated code at FaultAddress, when the host has no page at the shared user data's fixed address
// and the page is at Redirected instead. A load from the page at its fixed address, in a form that FEX's JIT emits, is
// emulated when the whole access lies in the page. PC points to the faulting instruction, GPRs holds x0 to x30 and then
// sp, and FPRs holds v0 to v31.
AccessKind HandleAccessViolation(uint64_t Redirected, uint64_t FaultAddress, uint64_t* GPRs, __uint128_t* FPRs, uint64_t& PC);
} // namespace FEX::Windows::SharedUserData
