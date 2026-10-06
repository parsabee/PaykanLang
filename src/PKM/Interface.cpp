// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The IFACE blob codec (docs/design/pkm.md §3.2, §3.3, §3.5, §3.7) in its
// prototype form: the part table and record framing are the design's, the
// DECLS payloads carry ModuleInfo's type-name strings.  Every record is
// read through a sub-reader bounded to its length; known fields are read,
// trailing bytes are ignored (a later minor may append fields).

#include "paykan/pkm/Interface.h"

#include "Internal.h"
#include "paykan/support/Bytes.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace paykan::pkm {

using support::ByteReader;
using support::ByteWriter;
using support::StringTable;

namespace {

constexpr size_t kBlobHeader = 16;
constexpr size_t kPartEntry = 24;
constexpr uint32_t kPartRequired = 1;
constexpr uint32_t kMaxParts = 64;
/// MODS and DIAG record tags.
constexpr uint64_t kModuleRec = 1;
constexpr uint64_t kDiagFileRec = 1;
/// Class and enum record flags.
constexpr uint8_t kDeclLocal = 1;
/// Record tags below this are required (§3.2).
constexpr uint64_t kOptionalTag = 0x80;

// --- Writer -----------------------------------------------------------------

/// Appends `uleb tag, uleb len, payload`.
template <typename F> void record(ByteWriter &w, uint64_t tag, F body) {
  ByteWriter v;
  body(v);
  w.uleb(tag);
  w.uleb(v.size());
  w.bytes(v.data());
}

struct PartBytes {
  PartTag Tag;
  std::vector<uint8_t> Bytes;
};

template <typename T>
std::vector<const T *> sortedByName(const std::vector<T> &v) {
  std::vector<const T *> out;
  out.reserve(v.size());
  for (const T &x : v)
    out.push_back(&x);
  std::stable_sort(out.begin(), out.end(),
                   [](const T *a, const T *b) { return a->Name < b->Name; });
  return out;
}

void writeSig(ByteWriter &v, StringTable &strs, const std::string &ret,
              const std::vector<std::string> &params) {
  v.uleb(strs.intern(ret));
  v.uleb(params.size());
  for (const std::string &p : params)
    v.uleb(strs.intern(p));
}

} // namespace

std::vector<uint8_t> writeInterface(const Interface &iface) {
  StringTable strs;
  std::vector<PartBytes> parts;

  // MODS: this module, then dependencies sorted by canonical name.
  ByteWriter mods;
  std::vector<const ModuleRef *> deps;
  deps.reserve(iface.Mods.size());
  for (const ModuleRef &m : iface.Mods)
    deps.push_back(&m);
  std::stable_sort(deps.begin(), deps.end(),
                   [](const ModuleRef *a, const ModuleRef *b) {
                     return a->Canonical < b->Canonical;
                   });
  mods.uleb(deps.size() + 1);
  ModuleRef self{iface.Module, iface.System, {}, {}};
  deps.insert(deps.begin(), &self);
  for (const ModuleRef *m : deps)
    record(mods, kModuleRec, [&](ByteWriter &v) {
      v.uleb(strs.intern(m->Canonical));
      v.u8(m->System ? 1 : 0);
      detail::writeHash(v, m->IfaceHash);
      detail::writeHash(v, m->TmplHash);
    });

  ByteWriter types;
  types.uleb(0);

  // DECLS sorted by (tag, name): ENUM, CLASS, FUNC.
  ByteWriter decls;
  decls.uleb(iface.Enums.size() + iface.Classes.size() +
             iface.Functions.size());
  for (const EnumRec *e : sortedByName(iface.Enums))
    record(decls, static_cast<uint64_t>(DeclTag::Enum), [&](ByteWriter &v) {
      v.uleb(strs.intern(e->Name));
      v.u8(e->IsLocal ? kDeclLocal : 0);
      v.uleb(strs.intern(e->OriginModule));
      v.uleb(e->Variants.size());
      for (const std::string &name : e->Variants)
        v.uleb(strs.intern(name));
    });
  for (const ClassRec *c : sortedByName(iface.Classes))
    record(decls, static_cast<uint64_t>(DeclTag::Class), [&](ByteWriter &v) {
      v.uleb(strs.intern(c->Name));
      v.uleb(strs.intern(c->SuperClassName));
      v.u8(c->IsLocal ? kDeclLocal : 0);
      v.uleb(strs.intern(c->OriginModule));
      v.uleb(c->Fields.size());
      for (const ClassRec::Field &f : c->Fields) {
        v.uleb(strs.intern(f.FieldName));
        v.uleb(strs.intern(f.TypeName));
      }
      v.uleb(c->Methods.size());
      for (const ClassRec::Method &m : c->Methods) {
        v.uleb(strs.intern(m.Name));
        writeSig(v, strs, m.ReturnTypeName, m.ParamTypeNames);
        v.u8(m.Flags);
      }
    });
  for (const FuncRec *f : sortedByName(iface.Functions))
    record(decls, static_cast<uint64_t>(DeclTag::Func), [&](ByteWriter &v) {
      v.uleb(strs.intern(f->Name));
      writeSig(v, strs, f->ReturnTypeName, f->ParamTypeNames);
    });

  ByteWriter inst;
  inst.uleb(0);

  ByteWriter diag;
  if (!iface.DisplayFile.empty()) {
    diag.uleb(1);
    record(diag, kDiagFileRec,
           [&](ByteWriter &v) { v.uleb(strs.intern(iface.DisplayFile)); });
  }

  // STRS is complete only now.
  ByteWriter strsBytes;
  strs.write(strsBytes);
  parts.push_back({PartTag::Strs, strsBytes.take()});
  parts.push_back({PartTag::Mods, mods.take()});
  parts.push_back({PartTag::Types, types.take()});
  parts.push_back({PartTag::Decls, decls.take()});
  parts.push_back({PartTag::Inst, inst.take()});
  if (!iface.DisplayFile.empty())
    parts.push_back({PartTag::Diag, diag.take()});

  ByteWriter w;
  w.bytes("PKMI", 4);
  w.u16(kIfaceMajor);
  w.u16(kIfaceMinor);
  w.u32(iface.System ? kIfaceSystem : 0);
  w.u32(static_cast<uint32_t>(parts.size()));
  uint64_t offset = kBlobHeader + parts.size() * kPartEntry;
  for (const PartBytes &p : parts) {
    w.u32(static_cast<uint32_t>(p.Tag));
    w.u32(p.Tag == PartTag::Diag ? 0u : kPartRequired);
    w.u64(offset);
    w.u64(p.Bytes.size());
    offset += (p.Bytes.size() + 7) & ~uint64_t(7);
  }
  for (const PartBytes &p : parts) {
    w.bytes(p.Bytes);
    w.align(8);
  }
  return w.take();
}

// --- Reader -----------------------------------------------------------------

namespace {

struct PartView {
  uint32_t Tag = 0, Flags = 0;
  std::span<const uint8_t> Bytes;
};

class Reader {
public:
  Reader(std::span<const uint8_t> bytes, std::string_view expected)
      : Blob(bytes), Expected(expected) {}

  Status run(Interface &out);

private:
  Status fail(std::string msg) { return Status::error(std::move(msg)); }
  Status failAt(const char *part, const ByteReader &r) {
    return fail(std::string(part) + ": " + detail::readerError(r));
  }
  /// Resolves string index @p i, failing when out of range.
  bool str(ByteReader &r, std::string &out) {
    uint64_t i;
    if (!r.uleb(i, Strings.empty() ? 0 : Strings.size() - 1))
      return false;
    out = Strings[static_cast<size_t>(i)];
    return true;
  }
  bool strList(ByteReader &r, std::vector<std::string> &out) {
    uint64_t n;
    if (!detail::readCount(r, n, 1))
      return false;
    out.resize(static_cast<size_t>(n));
    for (std::string &s : out)
      if (!str(r, s))
        return false;
    return true;
  }
  /// `uleb count` then records; @p body gets (tag, payload reader) for a
  /// record and returns false (after r.fail) to reject.
  template <typename F>
  Status records(const char *part, std::span<const uint8_t> bytes, F body) {
    ByteReader r(bytes);
    uint64_t n;
    if (!detail::readCount(r, n, 2))
      return failAt(part, r);
    for (uint64_t i = 0; i < n; ++i) {
      uint64_t tag = 0, len = 0;
      if (!r.uleb(tag) || !r.uleb(len) || len > r.remaining())
        return failAt(part, r);
      ByteReader v = r.sub(static_cast<size_t>(len));
      bool known = body(tag, v);
      if (!v.ok())
        return failAt(part, v);
      if (!known && tag < kOptionalTag)
        return fail(std::string(part) + ": unknown required record tag " +
                    detail::hex(tag));
    }
    if (!r.atEnd())
      return fail(std::string(part) + ": trailing bytes after the records");
    return Status::ok();
  }
  Status readParts();
  Status readStrs();
  Status readMods(Interface &out);
  Status readDecls(Interface &out);
  Status readDiag(Interface &out);
  Status readEmpty(const char *part, PartTag tag);
  const PartView *part(PartTag tag) const {
    auto it = Parts.find(static_cast<uint32_t>(tag));
    return it == Parts.end() ? nullptr : &it->second;
  }

  std::span<const uint8_t> Blob;
  std::string_view Expected;
  uint32_t Flags = 0;
  std::map<uint32_t, PartView> Parts;
  std::vector<std::string> Strings;
};

Status Reader::readParts() {
  ByteReader r(Blob);
  std::span<const uint8_t> magic;
  uint16_t major, minor;
  uint32_t n;
  if (!r.bytes(4, magic) || !r.u16(major) || !r.u16(minor) || !r.u32(Flags) ||
      !r.u32(n))
    return fail("truncated interface header");
  if (std::memcmp(magic.data(), "PKMI", 4) != 0)
    return fail("bad interface magic");
  if (major != kIfaceMajor)
    return fail("interface format " + std::to_string(major) + "." +
                std::to_string(minor) + "; this reader knows " +
                std::to_string(kIfaceMajor) + ".x");
  if (n == 0 || n > kMaxParts || uint64_t(n) * kPartEntry > r.remaining())
    return fail("bad part count " + std::to_string(n));
  uint64_t tableEnd = kBlobHeader + uint64_t(n) * kPartEntry;
  uint64_t prevEnd = tableEnd;
  for (uint32_t i = 0; i < n; ++i) {
    PartView p;
    uint64_t offset = 0, size = 0;
    r.u32(p.Tag);
    r.u32(p.Flags);
    r.u64(offset);
    r.u64(size);
    std::string where =
        "part #" + std::to_string(i) + " (tag " + std::to_string(p.Tag) + ")";
    if (offset % 8 != 0 || offset < prevEnd)
      return fail(where + " is unaligned, out of order or overlapping");
    if (offset > Blob.size() || size > Blob.size() - offset)
      return fail(where + " is past the end of the blob");
    if (Parts.count(p.Tag))
      return fail(where + " repeats a part");
    bool known = false;
    for (PartTag t : {PartTag::Strs, PartTag::Mods, PartTag::Types,
                      PartTag::Decls, PartTag::Inst, PartTag::Diag})
      known |= static_cast<uint32_t>(t) == p.Tag;
    if (!known && (p.Flags & kPartRequired))
      return fail(where + " is unknown and required");
    p.Bytes =
        Blob.subspan(static_cast<size_t>(offset), static_cast<size_t>(size));
    prevEnd = offset + size;
    Parts.emplace(p.Tag, p);
  }
  for (PartTag t : {PartTag::Strs, PartTag::Mods, PartTag::Types,
                    PartTag::Decls, PartTag::Inst})
    if (!part(t))
      return fail("missing part " + std::to_string(static_cast<uint32_t>(t)));
  return Status::ok();
}

Status Reader::readStrs() {
  ByteReader r(part(PartTag::Strs)->Bytes);
  if (!StringTable::read(r, Strings))
    return failAt("STRS", r);
  if (!r.atEnd())
    return fail("STRS: trailing bytes after the strings");
  for (const std::string &s : Strings)
    if (s.size() > kMaxString || !detail::validUtf8(s))
      return fail("STRS: a string is not valid UTF-8 or is too long");
  return Status::ok();
}

Status Reader::readMods(Interface &out) {
  std::vector<ModuleRef> mods;
  Status s = records("MODS", part(PartTag::Mods)->Bytes,
                     [&](uint64_t tag, ByteReader &v) {
                       if (tag != kModuleRec)
                         return false;
                       ModuleRef m;
                       uint8_t flags;
                       if (str(v, m.Canonical) && v.u8(flags) &&
                           detail::readHash(v, m.IfaceHash) &&
                           detail::readHash(v, m.TmplHash)) {
                         m.System = flags & 1;
                         mods.push_back(std::move(m));
                       }
                       return true;
                     });
  if (!s)
    return s;
  if (mods.empty())
    return fail("MODS: no entry for the module itself");
  if (mods[0].Canonical != Expected)
    return fail("MODS: the blob describes module '" + mods[0].Canonical +
                "', not '" + std::string(Expected) + "'");
  if (mods[0].System != bool(Flags & kIfaceSystem))
    return fail("MODS: the system flag disagrees with the blob header");
  for (size_t i = 1; i < mods.size(); ++i) {
    if (mods[i].Canonical == mods[0].Canonical)
      return fail("MODS: the module depends on itself");
    if (i > 1 && !(mods[i - 1].Canonical < mods[i].Canonical))
      return fail("MODS: dependencies are not sorted and unique");
  }
  out.Module = mods[0].Canonical;
  out.System = mods[0].System;
  out.Mods.assign(mods.begin() + 1, mods.end());
  return Status::ok();
}

Status Reader::readDecls(Interface &out) {
  // (tag, name) of the previous record, for the sort and uniqueness check.
  uint64_t lastTag = 0;
  std::string lastName;
  Status order = Status::ok();
  auto ordered = [&](uint64_t tag, const std::string &name) {
    if (lastTag != 0 &&
        (tag < lastTag || (tag == lastTag && !(lastName < name)))) {
      order = fail("DECLS: records are not sorted by (tag, name) with unique "
                   "names ('" +
                   name + "')");
      return false;
    }
    lastTag = tag;
    lastName = name;
    return true;
  };
  Status s = records(
      "DECLS", part(PartTag::Decls)->Bytes, [&](uint64_t tag, ByteReader &v) {
        switch (static_cast<DeclTag>(tag)) {
        case DeclTag::Enum: {
          EnumRec e;
          uint8_t flags;
          if (!str(v, e.Name) || !v.u8(flags) || !str(v, e.OriginModule) ||
              !strList(v, e.Variants) || !ordered(tag, e.Name))
            return true;
          e.IsLocal = flags & kDeclLocal;
          out.Enums.push_back(std::move(e));
          return true;
        }
        case DeclTag::Class: {
          ClassRec c;
          uint8_t flags;
          uint64_t n;
          if (!str(v, c.Name) || !str(v, c.SuperClassName) || !v.u8(flags) ||
              !str(v, c.OriginModule) || !detail::readCount(v, n, 2))
            return true;
          c.IsLocal = flags & kDeclLocal;
          c.Fields.resize(static_cast<size_t>(n));
          for (ClassRec::Field &f : c.Fields)
            if (!str(v, f.FieldName) || !str(v, f.TypeName))
              return true;
          if (!detail::readCount(v, n, 4))
            return true;
          c.Methods.resize(static_cast<size_t>(n));
          for (ClassRec::Method &m : c.Methods)
            if (!str(v, m.Name) || !str(v, m.ReturnTypeName) ||
                !strList(v, m.ParamTypeNames) || !v.u8(m.Flags))
              return true;
          if (ordered(tag, c.Name))
            out.Classes.push_back(std::move(c));
          return true;
        }
        case DeclTag::Func: {
          FuncRec f;
          if (str(v, f.Name) && str(v, f.ReturnTypeName) &&
              strList(v, f.ParamTypeNames) && ordered(tag, f.Name))
            out.Functions.push_back(std::move(f));
          return true;
        }
        }
        return false;
      });
  if (!s)
    return s;
  return order;
}

Status Reader::readDiag(Interface &out) {
  const PartView *p = part(PartTag::Diag);
  if (!p)
    return Status::ok();
  return records("DIAG", p->Bytes, [&](uint64_t tag, ByteReader &v) {
    if (tag != kDiagFileRec)
      return false;
    str(v, out.DisplayFile);
    return true;
  });
}

/// TYPES and INST: the prototype writes no records; a required record of
/// a later writer rejects, an optional one is skipped.
Status Reader::readEmpty(const char *name, PartTag tag) {
  return records(name, part(tag)->Bytes,
                 [](uint64_t, ByteReader &) { return false; });
}

Status Reader::run(Interface &out) {
  if (Status s = readParts(); !s)
    return s;
  if (Status s = readStrs(); !s)
    return s;
  if (Status s = readMods(out); !s)
    return s;
  if (Status s = readEmpty("TYPES", PartTag::Types); !s)
    return s;
  if (Status s = readDecls(out); !s)
    return s;
  if (Status s = readEmpty("INST", PartTag::Inst); !s)
    return s;
  return readDiag(out);
}

} // namespace

StatusOr<Interface> readInterface(std::span<const uint8_t> bytes,
                                  std::string_view expectedModule) {
  Interface out;
  Reader reader(bytes, expectedModule);
  if (Status s = reader.run(out); !s)
    return s;
  return out;
}

} // namespace paykan::pkm
