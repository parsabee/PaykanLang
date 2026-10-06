// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The MANIFEST section (docs/design/pkm.md §2): what a module file depends
// on and which toolchain produced it, as a flat TLV so fields can be added
// without touching readers of older files.  The compatibility verdict (§8.2)
// is computed from the manifest and a HostIdentity alone, so a rejected file
// costs one diagnostic before anything else is decoded.

#pragma once

#include "paykan/Status.h"
#include "paykan/pkm/Format.h"
#include "paykan/support/Sha256.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace paykan::pkm {

/// `contents` bits (tag 0x03).
inline constexpr uint32_t kContentsHasCode = 1 << 0;
inline constexpr uint32_t kContentsHasTemplates = 1 << 1;
inline constexpr uint32_t kContentsHasMain = 1 << 2;
inline constexpr uint32_t kContentsSystem = 1 << 3;
inline constexpr uint32_t kContentsHasStaticInit = 1 << 4;
inline constexpr uint32_t kContentsExportsC = 1 << 5;

/// `Dep.flags` bits (§2.3).
inline constexpr uint32_t kDepSystem = 1 << 0;
inline constexpr uint32_t kDepInstantiated = 1 << 1;

/// `Lib.kind` (§2.4).
enum class LibKind : uint8_t {
  Runtime = 1,
  Allocator = 2,
  CLibrary = 3,
  PaykanLibrary = 4
};
/// `Lib.flags` bits.
inline constexpr uint32_t kLibRequiredAtLink = 1 << 0;
inline constexpr uint32_t kLibPreferStatic = 1 << 1;

/// `code_encoding` values (§2.2, tag 0x07).
inline constexpr uint32_t kCodeNone = 0;
inline constexpr uint32_t kCodePirText = 1;
inline constexpr uint32_t kCodeBinaryPir1 = 2;

/// One record per tag of §2.2.
struct Manifest {
  struct CoreInfo {
    std::string Version;
    uint32_t Major = 0, Minor = 0, Patch = 0;
    std::string Prerelease;
    std::string Build;
    bool operator==(const CoreInfo &) const = default;
  };
  struct FormatVersions {
    uint32_t IfaceMajor = 0, IfaceMinor = 0, TmplVersion = 0, PirVersion = 0,
             CodeEncoding = 0, DebugVersion = 0;
    bool operator==(const FormatVersions &) const = default;
  };
  /// The abstract target PIR assumes (`8, 8, 1, 64, 64` for LP64 LE).
  struct TargetInfo {
    uint8_t PointerSize = 0, SlotSize = 0, Endianness = 0, IntWidth = 0,
            FloatWidth = 0;
    bool operator==(const TargetInfo &) const = default;
  };
  struct Dep {
    std::string Name; ///< canonical name of the imported module
    uint32_t Flags = 0;
    support::Hash256 IfaceHash{};
    support::Hash256 TmplHash{};
    std::string CoreVersion; ///< informational
    bool operator==(const Dep &) const = default;
  };
  struct SourceInfo {
    support::Hash256 Sha256{};
    uint64_t Size = 0;
    bool operator==(const SourceInfo &) const = default;
  };
  struct Lib {
    uint8_t Kind = 0; ///< LibKind
    std::string Name;
    std::string Version;
    uint32_t Abi = 0;
    uint32_t Flags = 0;
    bool operator==(const Lib &) const = default;
  };
  struct FrontendInfo {
    std::string Name, Version, Plugin;
    bool operator==(const FrontendInfo &) const = default;
  };
  struct ProducerInfo {
    std::string Tool, Version;
    bool operator==(const ProducerInfo &) const = default;
  };

  // Gating (required).
  std::string Module;
  uint32_t Contents = 0;
  CoreInfo Core;
  FormatVersions Formats;
  uint32_t RuntimeAbi = 0;
  TargetInfo Target;
  std::vector<Dep> Deps; ///< direct imports, sorted by name
  // Gating (optional).
  std::optional<SourceInfo> Source;
  std::vector<Lib> Libraries;
  // Informational; a default value is not written.
  FrontendInfo Frontend;
  ProducerInfo Producer;
  uint32_t PluginApi = 0;
  std::string OptPipeline;
  std::vector<std::string> Attributes;

  bool operator==(const Manifest &) const = default;
};

/// The canonical encoding (§2.1): records sorted by tag, informational
/// records at their default value omitted.
std::vector<uint8_t> encodeManifest(const Manifest &m);

/// §2.5.  The message names the record and what is wrong, without a file
/// name or section (the container reader prefixes those).  Only the checks
/// that need the section table (`contents`) are left to File::read.
StatusOr<Manifest> decodeManifest(std::span<const uint8_t> bytes);

/// What the running toolchain is, for checkCompatibility.  The integrator
/// fills it from Version.h, PluginCompat.h, pir::kPIRVersion and Runtime.h.
struct HostIdentity {
  uint32_t IfaceMajor = kIfaceMajor;
  uint32_t IfaceMinor = kIfaceMinor;
  uint32_t PirVersion = 0;
  uint32_t RuntimeAbi = 0;
  Manifest::TargetInfo Target;
  std::string CoreVersion; ///< kVersion
  std::string CoreBuild;   ///< PAYKAN_BUILD_ID
  /// PAYKAN_PKM_COMPATIBLE_VERSIONS: the producer versions a prebuilt file
  /// may carry (the running version included).
  std::vector<std::string> CompatibleVersions;
};

/// Cache: a `.paykan_cache` entry whose source is at hand (a mismatch is
/// Stale and rebuilt).  Prebuilt: a distributed file (a mismatch rejects).
enum class Policy { Cache, Prebuilt };

struct Verdict {
  enum class Kind { Usable, Stale, Rejected };
  enum class Reason { None, Format, Interface, PIR, ABI, Target, Toolchain };
  Kind Outcome = Kind::Usable;
  Reason Cause = Reason::None;
  std::string Message; ///< what differs; "" when Usable
};

/// The rows of §8.2 that depend on the manifest and host alone, in order.
/// Container corruption is File::read's; the source hash and the dependency
/// hashes are the caller's (sourceMatches helps with the former).
Verdict checkCompatibility(const Manifest &m, const HostIdentity &host,
                           Policy policy);

/// True when the manifest records a source and @p sourceBytes is exactly it.
bool sourceMatches(const Manifest &m, std::span<const uint8_t> sourceBytes);

} // namespace paykan::pkm
