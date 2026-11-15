// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>

namespace FEXCore::Utils {
// FEX assumes an operating page size of 4096
// To work around build systems that build on a 16k/64k page size, define our page size here
// Don't use the system provided PAGE_SIZE define because of this.
constexpr size_t FEX_PAGE_SIZE = 4096;
constexpr size_t FEX_PAGE_SHIFT = 12;
constexpr size_t FEX_PAGE_MASK = ~(FEX_PAGE_SIZE - 1);

// A guard region after a buffer is only enforced if it covers whole host pages. Wine on Apple
// silicon runs Windows code on 16 KiB host pages while it reports 4 KiB ones, so Windows builds
// use guard regions of 16 KiB, which work on 4 KiB hosts as well.
#ifdef _WIN32
constexpr size_t FEX_GUARD_SIZE = 16384;
#else
constexpr size_t FEX_GUARD_SIZE = FEX_PAGE_SIZE;
#endif
} // namespace FEXCore::Utils
