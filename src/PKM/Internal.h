// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Helpers shared by the .pkm codecs: the `str` and `hash` primitives of
// docs/design/pkm.md §1.1 on top of support::ByteReader, UTF-8 validation
// and hex formatting for messages.

#pragma once

#include "paykan/pkm/Format.h"
#include "paykan/support/Bytes.h"
#include "paykan/support/Sha256.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace paykan::pkm::detail {

/// Well-formed UTF-8 (no overlongs, no surrogates, ≤ U+10FFFF) and no NUL.
inline bool validUtf8(std::string_view s) {
  size_t i = 0, n = s.size();
  while (i < n) {
    unsigned char c = s[i];
    if (c == 0)
      return false;
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t len;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) {
      len = 2;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + len > n)
      return false;
    for (size_t k = 1; k < len; ++k) {
      unsigned char cc = s[i + k];
      if ((cc & 0xC0) != 0x80)
        return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
        (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    i += len;
  }
  return true;
}

/// A `str` (§1.1): length-bounded, valid UTF-8, no NUL.
inline bool readStr(support::ByteReader &r, std::string &out,
                    uint64_t maxLen = kMaxString) {
  size_t at = r.offset();
  if (!r.str(out, maxLen))
    return false;
  if (!validUtf8(out))
    return r.fail("string is not valid UTF-8", at);
  return true;
}

inline bool readHash(support::ByteReader &r, support::Hash256 &out) {
  std::span<const uint8_t> b;
  if (!r.bytes(32, b))
    return false;
  std::copy(b.begin(), b.end(), out.begin());
  return true;
}

inline void writeHash(support::ByteWriter &w, const support::Hash256 &h) {
  w.bytes(h.data(), h.size());
}

/// `uleb count` bounded by the list limit and the bytes left.
inline bool readCount(support::ByteReader &r, uint64_t &n,
                      size_t minBytesPerElem = 1) {
  size_t at = r.offset();
  if (!r.count(n, minBytesPerElem))
    return false;
  if (n > kMaxList)
    return r.fail("list longer than 65536 entries", at);
  return true;
}

inline std::string hex(uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}

/// The reader's error with its offset: "<msg> (at byte 12)".
inline std::string readerError(const support::ByteReader &r) {
  return r.error() + " (at byte " + std::to_string(r.errorOffset()) + ")";
}

} // namespace paykan::pkm::detail
