// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The PIR version: bumped on any change to the types, opcodes or semantics
// of PIR.h (docs/design/pkm.md §11).  The binary codec stores it in every
// CODE blob and the text form carries it as PAYKAN_PIR_TEXT_VERSION in the C
// plugin API; the two are one number (tests/PIR/BinaryTests.cpp checks).

#pragma once

#include <cstdint>

namespace paykan::pir {

// 2: local.addr, field.addr, ptr.load, ptr.store (`inout` parameters).
inline constexpr uint32_t kPIRVersion = 2;

} // namespace paykan::pir
