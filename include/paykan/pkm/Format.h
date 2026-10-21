// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The fixed numbers of the .pkm module file (docs/design/pkm.md §1, §2, §3):
// format versions, section kinds and flags, limits.  One header so that the
// container, the manifest and the interface codecs, and the host identity a
// driver checks a file against, agree by construction.

#pragma once

#include <Version.h>

#include <cstddef>
#include <cstdint>

namespace paykan::pkm {

/// The versions of the container (§1.2), the MANIFEST section (§2.1) and the
/// IFACE blob (§3.2): the release's, like every format (kVersionNumber).  A
/// reader accepts exactly its own.
inline constexpr uint32_t kFormatVersion = kVersionNumber;
inline constexpr uint32_t kManifestVersion = kVersionNumber;
inline constexpr uint32_t kIfaceVersion = kVersionNumber;

/// Section kinds (§1.4).  Bits 8-15 are the class: 0x00 header, 0x01 sema,
/// 0x02 binary, 0x7F private (never written or required by paykan).
enum class Kind : uint32_t {
  Manifest = 0x0001,
  Iface = 0x0100,
  Tmpl = 0x0101,
  Code = 0x0200,
  SymIdx = 0x0201,
  Debug = 0x0202,
  Object = 0x0203,
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
