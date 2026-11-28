// SPDX-License-Identifier: MIT
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/TypeDefines.h>
#include "Windows/Common/FEXUnixLib.h"

#include <array>
#include <chrono>
#include <libloaderapi.h>
#include <sysinfoapi.h>
#include <synchapi.h>
#include <windef.h>
#include <winternl.h>
#include <winnt.h>
#include <wine/debug.h>

namespace FEX::Windows::Allocator {
// Wine for macOS on Apple silicon keeps generated code either writable or executable for each thread.
static NTSTATUS (*WineJITWriteProtect)(BOOL Enable) {};

static void JITWriteProtect(bool Protect) {
  WineJITWriteProtect(Protect);
}

// Paneport's Wine on iOS holds generated code in a pool that the host maps twice, executable at the address
// that VirtualAlloc2 returns and writable at another, and tells how much code the pool holds at once.
static NTSTATUS (*WineJITGetWriteAddress)(const void* Address, SIZE_T Size, void** Write) {};

static void* JITGetWriteAddress(const void* Exec, size_t Size) {
  void* Write {};
  if (WineJITGetWriteAddress(Exec, Size, &Write) != 0) {
    return nullptr;
  }
  return Write;
}

void SetupHooks(HMODULE ntdll) {
  FEXCore::Allocator::HookPtrs Ptrs {};

  Ptrs = {
    .VirtualName = UnixLib::VirtualName,
    .VirtualTHPControl = UnixLib::VirtualTHPControl,
  };

  SYSTEM_INFO system_info {};
  GetSystemInfo(&system_info);
  FEXCore::Allocator::SetupHooks(system_info.dwPageSize, Ptrs);

  // Other Wine builds export it as well, and answer that they don't need it.
  WineJITWriteProtect = reinterpret_cast<decltype(WineJITWriteProtect)>(GetProcAddress(ntdll, "__wine_jit_write_protect"));
  if (WineJITWriteProtect && WineJITWriteProtect(TRUE) == 0) {
    FEXCore::Allocator::JITWriteProtect = JITWriteProtect;
  }

  // A Wine without a pool answers that it has no limit to tell, and nothing changes.
  WineJITGetWriteAddress = reinterpret_cast<decltype(WineJITGetWriteAddress)>(GetProcAddress(ntdll, "__wine_jit_get_write_address"));
  const auto WineJITGetCodeLimit = reinterpret_cast<NTSTATUS (*)(SIZE_T*)>(GetProcAddress(ntdll, "__wine_jit_get_code_limit"));
  SIZE_T CodeLimit {};
  if (WineJITGetWriteAddress && WineJITGetCodeLimit && WineJITGetCodeLimit(&CodeLimit) == 0) {
    FEXCore::Allocator::JITGetWriteAddress = JITGetWriteAddress;
    FEXCore::Allocator::JITCodeLimit = CodeLimit;
    FEXCore::Allocator::JITDualMapped = true;
  }
}
} // namespace FEX::Windows::Allocator
