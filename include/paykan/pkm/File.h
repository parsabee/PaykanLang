// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The .pkm container (docs/design/pkm.md §1): a fixed header, a hashed
// section table and self-delimiting sections.  File::read performs steps 1-5
// of §1.7 (framing, table, hashes, manifest) and nothing more, so a corrupt
// or incompatible file costs one diagnostic; Writer lays sections out,
// aligns and hashes them.  Standard C++ only: no Sema, no PIR.

#pragma once

#include "paykan/Status.h"
#include "paykan/pkm/Format.h"
#include "paykan/pkm/Manifest.h"
#include "paykan/support/Sha256.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace paykan::pkm {

/// One decoded section table entry (§1.3).
struct SectionEntry {
  uint32_t Kind = 0;
  uint32_t Flags = 0;
  uint64_t Offset = 0;
  uint64_t Size = 0;
  support::Hash256 Hash{};
};

/// Why a file was rejected (§1.7).
enum class ErrorCode {
  Truncated,
  NotAPkm,
  UnsupportedFormat,
  BadHeader,
  BadSectionTable,
  UnknownRequiredSection,
  DuplicateSection,
  MissingManifest,
  HashMismatch,
  BadManifest,
};

struct Error {
  ErrorCode Code = ErrorCode::Truncated;
  std::string Message;
  int Section = -1;    ///< table index, -1 before the table is known
  uint32_t Kind = 0;   ///< that section's kind
  uint64_t Offset = 0; ///< of the section, or of the fault
  /// One line, no file name (the caller prefixes the path):
  /// `section IFACE (#2) at offset 0x1a40: hash mismatch`.
  std::string str() const;
};

struct ReadOptions {
  /// Test hook: skip step 4 of §1.7 (every section hash).  The table hash is
  /// always verified.
  bool VerifyHashes = true;
};

class File {
public:
  /// §1.7 steps 1-5.  On failure the Status message is Error::str(), and
  /// the structured error is also stored in @p err when given.
  static StatusOr<File> read(std::span<const uint8_t> bytes,
                             const ReadOptions &opts = {},
                             Error *err = nullptr);

  uint16_t formatMajor() const { return Major; }
  uint16_t formatMinor() const { return Minor; }
  const support::Hash256 &tableHash() const { return TableHash; }
  /// Table order: sorted by (kind, offset).
  const std::vector<SectionEntry> &sections() const { return Sections; }
  /// The bytes of the @p instance-th section of @p kind; empty when absent.
  std::span<const uint8_t> section(Kind kind, size_t instance = 0) const;
  /// The bytes of table entry @p index.
  std::span<const uint8_t> section(size_t index) const;
  size_t count(Kind kind) const;
  const Manifest &manifest() const { return M; }
  /// The whole file, which the File owns.
  std::span<const uint8_t> bytes() const { return Bytes; }

  /// Derived hashes (§1.5); zero when the section is absent.
  support::Hash256 ifaceHash() const { return hashOf(Kind::Iface); }
  support::Hash256 tmplHash() const { return hashOf(Kind::Tmpl); }
  support::Hash256 codeHash() const { return hashOf(Kind::Code); }
  support::Hash256 moduleHash() const;

private:
  support::Hash256 hashOf(Kind kind) const;
  const SectionEntry *find(Kind kind, size_t instance) const;

  std::vector<uint8_t> Bytes;
  std::vector<SectionEntry> Sections;
  Manifest M;
  uint16_t Major = 0, Minor = 0;
  support::Hash256 TableHash{};
};

/// Builds a container: sections are sorted by (kind, insertion order),
/// 8-aligned and hashed.  finish() refuses a missing MANIFEST, a repeated
/// non-INSTANCED kind and the limits of §1.6.
class Writer {
public:
  void add(Kind kind, uint32_t flags, std::vector<uint8_t> bytes) {
    add(static_cast<uint32_t>(kind), flags, std::move(bytes));
  }
  void add(uint32_t kind, uint32_t flags, std::vector<uint8_t> bytes);
  StatusOr<std::vector<uint8_t>> finish() const;

private:
  struct Pending {
    uint32_t Kind, Flags;
    std::vector<uint8_t> Bytes;
  };
  std::vector<Pending> Pendings;
};

/// Writes through a temporary file in the same directory and a rename
/// (creating the parent directories), so a reader never sees a partial
/// file.
Status writeFileAtomically(const std::filesystem::path &path,
                           std::span<const uint8_t> bytes);
StatusOr<std::vector<uint8_t>> readFileBytes(const std::filesystem::path &path);

} // namespace paykan::pkm
