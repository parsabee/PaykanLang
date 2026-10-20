// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The PIR version: the release's, like every format (kVersionNumber).  The
// binary codec stores it in every CODE blob and the text form carries it as
// PAYKAN_PIR_TEXT_VERSION in the C plugin API.

#pragma once

#include <Version.h>

#include <cstdint>

namespace paykan::pir {

inline constexpr uint32_t kPIRVersion = kVersionNumber;

} // namespace paykan::pir
