// SPDX-License-Identifier: MIT
#include "Interface/Core/LookupCache.h"
#include "Interface/Core/SharedCodeBufferManager.h"

#include <FEXCore/fextl/memory.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/MathUtils.h>

#ifndef _WIN32
#include <FEXCore/Utils/PrctlUtils.h>
#else
#include <synchapi.h>
#endif

#include <algorithm>
#include <atomic>

namespace FEXCore::CPU {
static constexpr size_t INITIAL_CODE_SIZE = 1024 * 1024 * 16;
// We don't want to move above 128MB atm because that means we will have to encode longer jumps
static constexpr size_t MAX_CODE_SIZE = 1024 * 1024 * 128;
// The smallest cap: a buffer must hold the largest block and its guard region.
static constexpr size_t MIN_CODE_SIZE = 1024 * 1024;

// How many code buffers are alive, and their bytes, for the report of a failed allocation.
static std::atomic<size_t> LiveCodeBuffers {};
static std::atomic<size_t> LiveCodeBufferBytes {};

// The largest code buffer. A host that holds generated code in a pool of a fixed size, see
// FEXCore::Allocator::JITCodeLimit, caps it at the largest power of two that is at most half the
// limit, because a full buffer and its successor are alive together until every thread has let
// go of the old one. The MaxCodeBufferSize option caps it further.
static size_t MaxCodeSize() {
  size_t Max = MAX_CODE_SIZE;
  if (const size_t Limit = FEXCore::Allocator::JITCodeLimit) {
    while (Max > Limit / 2 && Max > MIN_CODE_SIZE) {
      Max /= 2;
    }
  }
  if (const size_t Configured = FEXCore::Config::Get_MAXCODEBUFFERSIZE()) {
    Max = std::min<size_t>(Max, FEXCore::AlignUp(Configured * 1024, 64 * 1024));
  }
  return std::max<size_t>(Max, MIN_CODE_SIZE);
}

CodeBuffer::CodeBuffer(size_t Size, bool ShouldBeNamed)
  : AllocatedSize(Size) {
  Ptr = static_cast<uint8_t*>(FEXCore::Allocator::VirtualAlloc(Size, true));
#ifdef _WIN32
  // A host with a pool of a fixed size has no room while other threads still hold older buffers, which they
  // let go when they next compile a block. Wait for that for two seconds, then give up with the numbers.
  for (int Attempt = 0; !Ptr && FEXCore::Allocator::JITCodeLimit && Attempt < 200; ++Attempt) {
    ::Sleep(10);
    Ptr = static_cast<uint8_t*>(FEXCore::Allocator::VirtualAlloc(Size, true));
  }
  if (!Ptr) {
    ERROR_AND_DIE_FMT("Couldn't allocate a code buffer of {} bytes: {} code buffers with {} bytes are alive, and the host holds at most {} "
                      "bytes of generated code (0: no limit)",
                      Size, LiveCodeBuffers.load(), LiveCodeBufferBytes.load(), FEXCore::Allocator::JITCodeLimit);
  }
#else
  LOGMAN_THROW_A_FMT(!!Ptr, "Couldn't allocate code buffer");
#endif
  LiveCodeBuffers.fetch_add(1);
  LiveCodeBufferBytes.fetch_add(Size);

  // Protect the guard region at the end of the allocated buffer to trigger SIGSEGV on write access
  uintptr_t LastPageAddr = AlignDown(reinterpret_cast<uintptr_t>(Ptr) + Size - 1, FEXCore::Utils::FEX_GUARD_SIZE);
  if (!FEXCore::Allocator::VirtualProtect(reinterpret_cast<void*>(LastPageAddr), FEXCore::Utils::FEX_GUARD_SIZE,
                                          FEXCore::Allocator::ProtectOptions::None)) {
    LogMan::Msg::EFmt("Failed to mprotect last page of code buffer.");
  }

  if (ShouldBeNamed) {
    FEXCore::Allocator::VirtualName("FEXMemJIT", Ptr, Size);
  }

  // Huge-pages reduce the amount of iTLB misses dramatically when it works.
  FEXCore::Allocator::VirtualTHPControl(Ptr, Size, FEXCore::Allocator::THPControl::Enable);

  LookupCache = fextl::make_unique<GuestToHostMap>();

  CodeBufferEnd = Ptr + UsableSize();
  CodeBufferOffset = Ptr;
}

CodeBuffer::~CodeBuffer() {
  LiveCodeBuffers.fetch_sub(1);
  LiveCodeBufferBytes.fetch_sub(AllocatedSize);
  FEXCore::Allocator::VirtualFree(Ptr, AllocatedSize);
}

SharedCodeBufferManager::SharedCodeBufferManager() {
  FEX_CONFIG_OPT(GlobalJITNaming, GLOBALJITNAMING);
  FEX_CONFIG_OPT(LibraryJITNaming, LIBRARYJITNAMING);
  FEX_CONFIG_OPT(BlockJITNaming, BLOCKJITNAMING);

  // Only name the JIT buffers if perf JIT naming is disabled.
  // `perf top` prefers VMA names over the JIT symbols file for some reason.
  // Breaks memory tracking when naming is enabled, but it's a debug feature so it isn't expected to be enabled by default.
  NameJITBuffers = !(GlobalJITNaming || LibraryJITNaming || BlockJITNaming);
}

fextl::shared_ptr<CodeBuffer> SharedCodeBufferManager::AllocateNew(size_t Size) {
#ifndef _WIN32
  // MDWE (Memory-Deny-Write-Execute) is a new Linux 6.3 feature.
  // It's equivalent to systemd's `MemoryDenyWriteExecute` but implemented entirely in the kernel.
  //
  // MDWE prevents applications from creating RWX memory mappings.
  // This prevents FEX from doing anything JIT related, as FEX uses RWX for JIT memory mappings.
  //
  // A potential workaround to make FEX work with MDWE is to call mprotect every time we need to write or modify code.
  // Alternatively, FEX could use a memory mirror where one half is mapped as RW and the other is RX.
  //
  // Once MDWE is enabled with the prctl, the feature is sealed and it can /NOT/ be turned off.
  //
  // Status of MDWE is queried through prctl using `PR_GET_MDWE`:
  // -1: The kernel doesn't support MDWE
  // 0: MDWE is supported but disabled
  // >0: MDWE is enabled, hence prohibiting RWX mappings
  int MDWE = ::prctl(PR_GET_MDWE, 0, 0, 0, 0);
  if (MDWE != -1 && MDWE != 0) {
    LogMan::Msg::EFmt("MDWE was set to 0x{:x} which means FEX can't allocate executable memory", MDWE);
  }
#endif

  auto Buffer = fextl::make_shared<CodeBuffer>(Size, NameJITBuffers);

  Latest = Buffer;

  OnCodeBufferAllocated(Buffer);

  return Buffer;
}

fextl::shared_ptr<CodeBuffer> SharedCodeBufferManager::GetLatest() {
  if (!Latest) {
    AllocateNew(std::min<size_t>(INITIAL_CODE_SIZE, MaxCodeSize()));
  }
  return Latest;
}

fextl::shared_ptr<CodeBuffer> SharedCodeBufferManager::StartLargerCodeBuffer() {
  if (!Latest) {
    // Allocate initial CodeBuffer and return it
    return GetLatest();
  }

  auto NewCodeBufferSize = GetLatest()->TotalAllocationSize();
  NewCodeBufferSize = std::min<size_t>(NewCodeBufferSize * 2, MaxCodeSize());
  return AllocateNew(NewCodeBufferSize);
}

fextl::shared_ptr<CodeBuffer> SharedCodeBufferManager::StartMaximalCodeBuffer() {
  return AllocateNew(MaxCodeSize());
}
} // namespace FEXCore::CPU
