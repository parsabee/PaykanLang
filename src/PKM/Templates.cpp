// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The TMPL blob:
//
//   "PKMT" u32 version u32 flags(0)
//   Str file
//   uleb n ; Str uses[n]
//   uleb n ; { u8 owner kind (1 class, 2 struct); uleb owner index }[n]
//   Str ast
//
// Str is a uleb length and UTF-8 bytes.  Nothing may follow.

#include "paykan/pkm/Templates.h"
#include "Internal.h"

#include <cstring>

namespace paykan::pkm {

using support::ByteReader;
using support::ByteWriter;

std::vector<uint8_t> writeTemplates(const Templates &t) {
  ByteWriter w;
  w.bytes("PKMT", 4);
  w.u32(kTmplVersion);
  w.u32(0);
  w.str(t.File);
  w.uleb(t.Uses.size());
  for (const std::string &u : t.Uses)
    w.str(u);
  w.uleb(t.Impls.size());
  for (const Templates::Impl &im : t.Impls) {
    w.u8(static_cast<uint8_t>(im.Kind));
    w.uleb(im.Owner);
  }
  w.str(t.Ast);
  return w.take();
}

StatusOr<Templates> readTemplates(std::span<const uint8_t> bytes) {
  ByteReader r(bytes);
  auto fail = [&](const std::string &why) -> StatusOr<Templates> {
    return Status::error("TMPL: " + why);
  };
  std::span<const uint8_t> magic;
  uint32_t version = 0, flags = 0;
  if (!r.bytes(4, magic) || !r.u32(version) || !r.u32(flags))
    return fail("truncated header");
  if (std::memcmp(magic.data(), "PKMT", 4) != 0)
    return fail("bad magic");
  if (version != kTmplVersion)
    return fail("format " + versionString(version) + "; this reader knows " +
                versionString(kTmplVersion));
  if (flags != 0)
    return fail("unknown flags " + detail::hex(flags));
  Templates t;
  uint64_t n = 0;
  if (!detail::readStr(r, t.File) || !r.count(n))
    return fail(detail::readerError(r));
  t.Uses.resize(n);
  for (std::string &u : t.Uses)
    if (!detail::readStr(r, u))
      return fail(detail::readerError(r));
  if (!r.count(n, 2))
    return fail(detail::readerError(r));
  t.Impls.resize(n);
  for (Templates::Impl &im : t.Impls) {
    uint8_t kind = 0;
    if (!r.u8(kind) || !r.uleb32(im.Owner))
      return fail(detail::readerError(r));
    if (kind != 1 && kind != 2)
      return fail("bad impl owner kind " + std::to_string(kind));
    im.Kind = static_cast<Templates::OwnerKind>(kind);
  }
  // The bodies are as long as a module's source: no string limit.
  if (!detail::readStr(r, t.Ast, r.remaining()))
    return fail(detail::readerError(r));
  if (r.remaining() != 0)
    return fail(std::to_string(r.remaining()) + " trailing bytes");
  return t;
}

} // namespace paykan::pkm
