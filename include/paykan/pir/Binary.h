// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Binary PIR: the CODE section of a .pkm module file (docs/design/pkm.md §5)
// and its symbol index (§6.1).  A one-to-one encoding of PIR.h, so the same
// verifier and backends consume a decoded module: encode never fails for a
// verified module and is deterministic; decode is bounds-checked, rejects
// reserved codes, non-minimal LEBs, slack bytes and excess nesting, and does
// not verify (run pir::verify on the result as on a parsed module).

#pragma once

#include "paykan/Status.h"
#include "paykan/pir/PIR.h"
#include "paykan/support/Sha256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace paykan::pir::binary {

inline constexpr uint16_t kCodecMajor = 1, kCodecMinor = 0;

/// Blob flags (§5.2).
inline constexpr uint32_t kFlagNamesStripped = 1u << 0;
inline constexpr uint32_t kFlagDefinesMain = 1u << 1;

struct EncodeOptions {
  /// Write every value and local name empty (flag bit 0): smaller, same
  /// semantics, different `--emit-pir` text.
  bool StripNames = false;
};

/// The CODE blob of @p m: layout §5.2, records §5.5-§5.7.
std::vector<uint8_t> encode(const Module &m, const EncodeOptions & = {});

struct DecodeOptions {
  uint32_t MaxNesting = 512;       // blocks inside blocks, the body is 1
  size_t MaxFunctions = 1u << 20;  // records in the function table
  size_t MaxStatements = 1u << 26; // across one decode call
};

/// The module of a blob written by encode().  Not verified.
StatusOr<Module> decode(std::span<const uint8_t> blob,
                        const DecodeOptions & = {});

/// SYMIDX kinds (§6.1).
enum class SymbolKind : uint8_t {
  Function = 0,
  ExternFunction = 1,
  Class = 2,
  ExternClass = 3,
  CStr = 4,
  Data = 5,
  Bytes = 6,
  ExternGlobal = 7,
  Slot = 8,     // reserved (#170)
  ClassObj = 9, // reserved (#160)
};

/// One record of the blob: the bytes [Offset, Offset + Length) are exactly
/// that record (for a function: its length prefix included).
struct ItemRef {
  SymbolKind Kind;
  std::string Name;
  size_t Offset = 0;
  size_t Length = 0;
};

/// The header, string table and record map of a blob, without decoding any
/// function body: what a reader needs to find a record or decode one
/// function on its own.
struct BlobInfo {
  uint16_t CodecMajor = 0, CodecMinor = 0;
  uint32_t PIRVersion = 0;
  uint32_t Flags = 0;
  std::string ModuleName;
  std::vector<std::string> Strings;
  /// Offset of each item table's count, in §5.2 order: extern globals,
  /// cstrs, datas, bytes, slots, class objects, classes, functions.
  std::array<size_t, 8> TableOffsets{};
  /// Every record in blob order.
  std::vector<ItemRef> Items;
  size_t TrailerOffset = 0;
};

StatusOr<BlobInfo> inspect(std::span<const uint8_t> blob);

/// The function whose record is blob[offset, offset + length) (an ItemRef or
/// SymbolEntry of kind Function / ExternFunction).
StatusOr<Function> decodeFunction(std::span<const uint8_t> blob,
                                  const BlobInfo &info, size_t offset,
                                  size_t length, const DecodeOptions & = {});

/// One SYMIDX entry (§6.1).  Offset/Length slice the record in the CODE
/// blob exactly as ItemRef does; Hash is the SHA-256 of those bytes.
struct SymbolEntry {
  std::string Name;
  uint8_t Kind = 0;    // SymbolKind
  uint8_t Linkage = 0; // 0 external (§9.3; the only value written today)
  uint64_t Offset = 0;
  uint64_t Length = 0;
  support::Hash256 Hash{};
};

/// The symbol index of a blob, sorted by (Kind, Name); empty when the blob
/// does not inspect().
std::vector<SymbolEntry> index(std::span<const uint8_t> blob);

/// The SYMIDX section bytes of @p entries (sorted on the way).
std::vector<uint8_t> encodeSymbolIndex(std::span<const SymbolEntry> entries);
StatusOr<std::vector<SymbolEntry>>
decodeSymbolIndex(std::span<const uint8_t> section);

} // namespace paykan::pir::binary
