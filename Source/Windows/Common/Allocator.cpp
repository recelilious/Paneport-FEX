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
}
} // namespace FEX::Windows::Allocator
