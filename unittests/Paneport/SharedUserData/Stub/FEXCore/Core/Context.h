// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 recelilious and the Paneport contributors
#pragma once

// Stands in for FEXCore/include/FEXCore/Core/Context.h when SharedUserDataTest.cpp builds the decoder, so that the test needs
// nothing else of FEXCore. It holds only the two constants that the decoder takes from that header, with the same values.
#include <cstdint>

namespace FEXCore::Context {
constexpr uint64_t SharedUserDataFixedAddress = 0x7ffe0000;
constexpr uint64_t SharedUserDataSize = 0x1000;
} // namespace FEXCore::Context
