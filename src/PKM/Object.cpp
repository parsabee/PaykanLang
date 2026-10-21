// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The OBJ blob:
//
//   "PKMO" u32 version u32 flags(0)
//   Str backend ; Str format ; Str target ; Str producer
//   uleb n ; u8 bytes[n]
//
// Str is a uleb length and UTF-8 bytes.  Nothing may follow.

#include "paykan/pkm/Object.h"
#include "Internal.h"

#include <cstring>

namespace paykan::pkm {

using support::ByteReader;
using support::ByteWriter;

std::vector<uint8_t> writeObject(const Object &o) {
  ByteWriter w;
  w.bytes("PKMO", 4);
  w.u32(kObjectVersion);
  w.u32(0);
  w.str(o.Backend);
  w.str(o.Format);
  w.str(o.Target);
  w.str(o.Producer);
  w.uleb(o.Bytes.size());
  w.bytes(o.Bytes);
  return w.take();
}

StatusOr<Object> readObject(std::span<const uint8_t> bytes) {
  ByteReader r(bytes);
  auto fail = [&](const std::string &why) -> StatusOr<Object> {
    return Status::error("OBJ: " + why);
  };
  std::span<const uint8_t> magic;
  uint32_t version = 0, flags = 0;
  if (!r.bytes(4, magic) || !r.u32(version) || !r.u32(flags))
    return fail("truncated header");
  if (std::memcmp(magic.data(), "PKMO", 4) != 0)
    return fail("bad magic");
  if (version != kObjectVersion)
    return fail("format " + versionString(version) + "; this reader knows " +
                versionString(kObjectVersion));
  if (flags != 0)
    return fail("unknown flags " + detail::hex(flags));
  Object o;
  uint64_t n = 0;
  std::span<const uint8_t> code;
  if (!detail::readStr(r, o.Backend) || !detail::readStr(r, o.Format) ||
      !detail::readStr(r, o.Target) || !detail::readStr(r, o.Producer) ||
      !r.count(n) || !r.bytes(static_cast<size_t>(n), code))
    return fail(detail::readerError(r));
  if (o.Backend.empty() || o.Format.empty())
    return fail("names no backend or format");
  o.Bytes.assign(code.begin(), code.end());
  if (r.remaining() != 0)
    return fail(std::to_string(r.remaining()) + " trailing bytes");
  return o;
}

} // namespace paykan::pkm
