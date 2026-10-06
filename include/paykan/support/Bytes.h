// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Bounds-checked little-endian byte I/O for the binary formats (the .pkm
// container and its sections, docs/design/pkm.md §1.1): fixed-width
// integers, minimal LEB128, raw bytes and a first-use-ordered string table.
// Header-only, standard C++20, no exceptions: every read reports failure
// through its return value and records the first error with its offset.

#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace paykan::support {

/// Appends little-endian integers, LEB128 and raw bytes to a buffer.
class ByteWriter {
public:
  void u8(uint8_t v) { Buf.push_back(v); }
  void u16(uint16_t v) { le(v, 2); }
  void u32(uint32_t v) { le(v, 4); }
  void u64(uint64_t v) { le(v, 8); }

  /// Minimal unsigned LEB128.
  void uleb(uint64_t v) {
    do {
      uint8_t byte = v & 0x7F;
      v >>= 7;
      if (v)
        byte |= 0x80;
      Buf.push_back(byte);
    } while (v);
  }

  /// Minimal signed LEB128.
  void sleb(int64_t v) {
    bool more = true;
    while (more) {
      uint8_t byte = v & 0x7F;
      v >>= 7; // arithmetic shift
      more = !((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40)));
      if (more)
        byte |= 0x80;
      Buf.push_back(byte);
    }
  }

  /// 8 raw IEEE-754 bytes, little-endian.
  void f64(double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    u64(bits);
  }

  void bytes(std::span<const uint8_t> b) {
    Buf.insert(Buf.end(), b.begin(), b.end());
  }
  void bytes(const void *p, size_t n) {
    bytes(std::span<const uint8_t>(static_cast<const uint8_t *>(p), n));
  }
  /// `uleb` length then the UTF-8 bytes (no NUL).
  void str(std::string_view s) {
    uleb(s.size());
    bytes(s.data(), s.size());
  }
  /// Zero bytes up to the next multiple of @p align.
  void align(size_t align) {
    while (Buf.size() % align)
      Buf.push_back(0);
  }

  size_t size() const { return Buf.size(); }
  const std::vector<uint8_t> &data() const { return Buf; }
  std::vector<uint8_t> take() { return std::move(Buf); }
  /// Overwrites @p n bytes at @p offset (for back-patching a fixed-width
  /// field; never a LEB).
  void patchU32(size_t offset, uint32_t v) {
    for (int i = 0; i < 4; ++i)
      Buf[offset + i] = static_cast<uint8_t>(v >> (8 * i));
  }
  void patchU64(size_t offset, uint64_t v) {
    for (int i = 0; i < 8; ++i)
      Buf[offset + i] = static_cast<uint8_t>(v >> (8 * i));
  }

private:
  void le(uint64_t v, int n) {
    for (int i = 0; i < n; ++i)
      Buf.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  std::vector<uint8_t> Buf;
};

/// Reads what ByteWriter writes.  Never throws, never casts bytes to a
/// struct; every read is bounds-checked and a LEB must be minimal, at most
/// 10 bytes and within the target type.  The first failure is remembered
/// with its offset and every later read fails too.
class ByteReader {
public:
  ByteReader() = default;
  explicit ByteReader(std::span<const uint8_t> data) : Data(data) {}

  bool u8(uint8_t &v) {
    if (!need(1))
      return false;
    v = Data[Pos++];
    return true;
  }
  bool u16(uint16_t &v) {
    uint64_t t;
    if (!le(t, 2))
      return false;
    v = static_cast<uint16_t>(t);
    return true;
  }
  bool u32(uint32_t &v) {
    uint64_t t;
    if (!le(t, 4))
      return false;
    v = static_cast<uint32_t>(t);
    return true;
  }
  bool u64(uint64_t &v) { return le(v, 8); }

  /// Minimal unsigned LEB128, rejected when it exceeds @p max.
  bool uleb(uint64_t &v, uint64_t max = UINT64_MAX) {
    uint64_t result = 0;
    unsigned shift = 0;
    size_t start = Pos;
    for (int i = 0; i < 10; ++i) {
      uint8_t byte;
      if (!u8(byte))
        return false;
      if (i == 9 && (byte & 0x7E))
        return fail("LEB128 overflows 64 bits", start);
      result |= static_cast<uint64_t>(byte & 0x7F) << shift;
      if (!(byte & 0x80)) {
        if (i > 0 && byte == 0)
          return fail("non-minimal LEB128", start);
        if (result > max)
          return fail("value out of range", start);
        v = result;
        return true;
      }
      shift += 7;
    }
    return fail("LEB128 longer than 10 bytes", start);
  }
  /// A `uleb` that must fit a uint32_t.
  bool uleb32(uint32_t &v, uint32_t max = UINT32_MAX) {
    uint64_t t;
    if (!uleb(t, max))
      return false;
    v = static_cast<uint32_t>(t);
    return true;
  }
  /// Minimal signed LEB128.
  bool sleb(int64_t &v) {
    int64_t result = 0;
    unsigned shift = 0;
    size_t start = Pos;
    uint8_t byte = 0;
    for (int i = 0; i < 10; ++i) {
      if (!u8(byte))
        return false;
      if (shift < 64)
        result |= static_cast<int64_t>(static_cast<uint64_t>(byte & 0x7F)
                                       << shift);
      shift += 7;
      if (!(byte & 0x80)) {
        if (shift < 64 && (byte & 0x40))
          result |= -(static_cast<int64_t>(1) << shift);
        // Minimality: a trailing 0x00 after a byte without 0x40, or a
        // trailing 0x7F after a byte with 0x40, is redundant.
        if (i > 0) {
          uint8_t prev = Data[start + i - 1];
          if ((byte == 0 && !(prev & 0x40)) || (byte == 0x7F && (prev & 0x40)))
            return fail("non-minimal LEB128", start);
        }
        v = result;
        return true;
      }
    }
    return fail("LEB128 longer than 10 bytes", start);
  }
  bool f64(double &v) {
    uint64_t bits;
    if (!u64(bits))
      return false;
    std::memcpy(&v, &bits, 8);
    return true;
  }
  /// A view of the next @p n bytes, no copy.
  bool bytes(size_t n, std::span<const uint8_t> &out) {
    if (!need(n))
      return false;
    out = Data.subspan(Pos, n);
    Pos += n;
    return true;
  }
  /// `uleb` length + bytes, into a string (UTF-8 is not validated here).
  bool str(std::string &out, uint64_t maxLen = UINT32_MAX) {
    uint64_t n;
    if (!uleb(n, maxLen))
      return false;
    std::span<const uint8_t> b;
    if (!bytes(static_cast<size_t>(n), b))
      return false;
    out.assign(reinterpret_cast<const char *>(b.data()), b.size());
    return true;
  }
  /// A count of elements each at least @p minBytesPerElem long: rejected
  /// before any allocation when the remaining bytes cannot hold them.
  bool count(uint64_t &n, size_t minBytesPerElem = 1) {
    size_t start = Pos;
    if (!uleb(n))
      return false;
    if (minBytesPerElem && n > remaining() / minBytesPerElem)
      return fail("count exceeds the remaining bytes", start);
    return true;
  }
  /// Skips to the next multiple of @p align (from the start of this reader's
  /// data).
  bool align(size_t align) {
    size_t pad = (align - Pos % align) % align;
    return need(pad) ? (Pos += pad, true) : false;
  }
  bool skip(size_t n) {
    if (!need(n))
      return false;
    Pos += n;
    return true;
  }
  /// A reader bounded to the next @p length bytes, which this reader skips.
  /// The sub-reader's offsets are relative to its own start.
  ByteReader sub(size_t length) {
    std::span<const uint8_t> b;
    if (!bytes(length, b)) {
      ByteReader r;
      r.fail("sub-reader past the end", Pos);
      return r;
    }
    return ByteReader(b);
  }

  size_t offset() const { return Pos; }
  size_t size() const { return Data.size(); }
  size_t remaining() const { return Data.size() - Pos; }
  bool atEnd() const { return Pos == Data.size(); }
  bool ok() const { return Err.empty(); }
  /// The first failure, "" when none.
  const std::string &error() const { return Err; }
  size_t errorOffset() const { return ErrPos; }
  /// Records a failure found by the caller (a bad tag, a reserved code).
  bool fail(std::string msg, size_t at) {
    if (Err.empty()) {
      Err = std::move(msg);
      ErrPos = at;
    }
    return false;
  }
  bool fail(std::string msg) { return fail(std::move(msg), Pos); }

private:
  bool need(size_t n) {
    if (!Err.empty())
      return false;
    if (n > remaining())
      return fail("unexpected end of data", Pos);
    return true;
  }
  bool le(uint64_t &v, int n) {
    if (!need(static_cast<size_t>(n)))
      return false;
    v = 0;
    for (int i = 0; i < n; ++i)
      v |= static_cast<uint64_t>(Data[Pos + i]) << (8 * i);
    Pos += n;
    return true;
  }
  std::span<const uint8_t> Data;
  size_t Pos = 0;
  std::string Err;
  size_t ErrPos = 0;
};

/// A string table in first-use order; index 0 is always "".
class StringTable {
public:
  StringTable() { intern(""); }
  /// The index of @p s, adding it on first use.
  uint32_t intern(std::string_view s) {
    auto it = Index.find(std::string(s));
    if (it != Index.end())
      return it->second;
    uint32_t id = static_cast<uint32_t>(Strings.size());
    Strings.emplace_back(s);
    Index.emplace(Strings.back(), id);
    return id;
  }
  const std::vector<std::string> &strings() const { return Strings; }
  /// `uleb count` then each string.
  void write(ByteWriter &w) const {
    w.uleb(Strings.size());
    for (const auto &s : Strings)
      w.str(s);
  }
  /// Reads a table written by write(); entry 0 must be "".
  static bool read(ByteReader &r, std::vector<std::string> &out,
                   uint64_t maxStrings = 1u << 24) {
    uint64_t n;
    if (!r.count(n) || n > maxStrings)
      return r.fail("string table too large");
    out.clear();
    out.reserve(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) {
      std::string s;
      if (!r.str(s))
        return false;
      if (s.find('\0') != std::string::npos)
        return r.fail("NUL in string");
      out.push_back(std::move(s));
    }
    if (out.empty() || !out[0].empty())
      return r.fail("string table entry 0 is not empty");
    return true;
  }

private:
  std::vector<std::string> Strings;
  std::unordered_map<std::string, uint32_t> Index;
};

} // namespace paykan::support
