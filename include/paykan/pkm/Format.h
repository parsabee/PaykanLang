// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The fixed numbers of the .pkm module file (docs/design/pkm.md §1, §2, §3):
// format versions, section kinds and flags, limits.  One header so that the
// container, the manifest and the interface codecs, and the host identity a
// driver checks a file against, agree by construction.

#pragma once

#include <cstddef>
#include <cstdint>

namespace paykan::pkm {

/// Container format (§1.2).  A reader accepts exactly the majors it knows
/// and any minor.
inline constexpr uint16_t kFormatMajor = 1;
inline constexpr uint16_t kFormatMinor = 0;
/// MANIFEST section format (§2.1).
inline constexpr uint16_t kManifestMajor = 1;
inline constexpr uint16_t kManifestMinor = 0;
/// IFACE blob format (§3.2).
inline constexpr uint16_t kIfaceMajor = 1;
inline constexpr uint16_t kIfaceMinor = 0;

/// Section kinds (§1.4).  Bits 8-15 are the class: 0x00 header, 0x01 sema,
/// 0x02 binary, 0x7F private (never written or required by paykan).
enum class Kind : uint32_t {
  Manifest = 0x0001,
  Iface = 0x0100,
  Tmpl = 0x0101,
  Code = 0x0200,
  SymIdx = 0x0201,
  Debug = 0x0202,
  Payload = 0x0210,
};

/// Section flags (§1.3).
inline constexpr uint32_t kFlagRequired = 1;  ///< unknown kind must reject
inline constexpr uint32_t kFlagInstanced = 2; ///< the kind may repeat
inline constexpr uint32_t kKnownFlags = kFlagRequired | kFlagInstanced;

/// Limits (§1.6).
inline constexpr size_t kHeaderSize = 64;
inline constexpr size_t kEntrySize = 64;
inline constexpr size_t kMaxSections = 1024;
inline constexpr uint64_t kMaxFileSize = uint64_t(1) << 40;
inline constexpr size_t kMaxManifestSize = size_t(1) << 20;
inline constexpr uint64_t kMaxString = uint64_t(1) << 16;
inline constexpr uint64_t kMaxList = uint64_t(1) << 16;

/// The name of a known kind ("MANIFEST", "IFACE", ...), nullptr otherwise.
const char *kindName(uint32_t kind);

} // namespace paykan::pkm
