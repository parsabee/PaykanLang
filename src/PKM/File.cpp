// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The .pkm container reader and writer (docs/design/pkm.md §1).  The reader
// follows §1.7 in order and stops at the first fault with a structured
// Error; every integer is assembled byte by byte and every bound is checked
// in uint64_t before it is used.

#include "paykan/pkm/File.h"

#include "Internal.h"
#include "paykan/support/Bytes.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>

namespace paykan::pkm {

using support::ByteReader;
using support::ByteWriter;
using support::Hash256;

namespace {

constexpr uint8_t kMagic[8] = {0x89, 'P', 'K', 'M', '\r', '\n', 0x1a, '\n'};

bool known(uint32_t kind) { return kindName(kind) != nullptr; }

/// The section bytes of @p e in @p bytes, which the table checks have made
/// safe.
std::span<const uint8_t> view(std::span<const uint8_t> bytes,
                              const SectionEntry &e) {
  return bytes.subspan(static_cast<size_t>(e.Offset),
                       static_cast<size_t>(e.Size));
}

std::optional<Error> fail(ErrorCode code, std::string msg, int section = -1,
                          uint32_t kind = 0, uint64_t offset = 0) {
  return Error{code, std::move(msg), section, kind, offset};
}

/// §1.7 steps 1-5.
std::optional<Error> readImpl(std::span<const uint8_t> bytes,
                              const ReadOptions &opts,
                              std::vector<SectionEntry> &sections,
                              uint16_t &major, uint16_t &minor,
                              Hash256 &tableHash, Manifest &manifest) {
  // 1. Header.
  if (bytes.size() < kHeaderSize)
    return fail(ErrorCode::Truncated, "file is " +
                                          std::to_string(bytes.size()) +
                                          " bytes; the header needs 64");
  if (bytes.size() > kMaxFileSize)
    return fail(ErrorCode::BadHeader, "file larger than 2^40 bytes");
  if (std::memcmp(bytes.data(), kMagic, 8) != 0)
    return fail(ErrorCode::NotAPkm, "not a .pkm file (bad magic)");
  ByteReader r(bytes);
  r.skip(8);
  uint32_t headerSize, count, entrySize;
  uint64_t tableOffset;
  r.u16(major);
  r.u16(minor);
  r.u32(headerSize);
  r.u32(count);
  r.u32(entrySize);
  r.u64(tableOffset);
  if (!r.ok())
    return fail(ErrorCode::Truncated, "truncated header");
  if (major != kFormatMajor)
    return fail(ErrorCode::UnsupportedFormat,
                "format " + std::to_string(major) + "." +
                    std::to_string(minor) + "; this reader knows format " +
                    std::to_string(kFormatMajor) + ".x");
  if (headerSize != kHeaderSize)
    return fail(ErrorCode::BadHeader,
                "header_size " + std::to_string(headerSize) + ", expected 64");
  if (count == 0 || count > kMaxSections)
    return fail(ErrorCode::BadHeader, "section_count " + std::to_string(count) +
                                          " is not in 1..1024");
  if (entrySize != kEntrySize)
    return fail(ErrorCode::BadHeader, "section_entry_size " +
                                          std::to_string(entrySize) +
                                          ", expected 64");
  if (tableOffset < kHeaderSize || tableOffset % 8 != 0)
    return fail(ErrorCode::BadHeader, "section_table_offset " +
                                          detail::hex(tableOffset) +
                                          " is below the header or unaligned");
  uint64_t tableSize = uint64_t(count) * kEntrySize;
  if (tableOffset > bytes.size() || tableSize > bytes.size() - tableOffset)
    return fail(ErrorCode::Truncated,
                "section table at " + detail::hex(tableOffset) + " (" +
                    std::to_string(tableSize) + " bytes) is past the end");
  uint64_t tableEnd = tableOffset + tableSize;

  // 2. Table hash.
  std::span<const uint8_t> stored;
  r.bytes(32, stored);
  std::copy(stored.begin(), stored.end(), tableHash.begin());
  std::span<const uint8_t> table = bytes.subspan(
      static_cast<size_t>(tableOffset), static_cast<size_t>(tableSize));
  if (support::sha256(table) != tableHash)
    return fail(ErrorCode::HashMismatch, "section table hash mismatch", -1, 0,
                tableOffset);

  // 3. Entries.
  ByteReader t(table);
  sections.clear();
  sections.reserve(count);
  size_t manifests = 0;
  for (uint32_t i = 0; i < count; ++i) {
    SectionEntry e;
    uint64_t reserved;
    std::span<const uint8_t> h;
    t.u32(e.Kind);
    t.u32(e.Flags);
    t.u64(e.Offset);
    t.u64(e.Size);
    t.bytes(32, h);
    t.u64(reserved);
    std::copy(h.begin(), h.end(), e.Hash.begin());
    int idx = static_cast<int>(i);
    auto bad = [&](std::string msg) {
      return fail(ErrorCode::BadSectionTable, std::move(msg), idx, e.Kind,
                  e.Offset);
    };
    if (e.Flags & ~kKnownFlags)
      return bad("unknown flag bits " + detail::hex(e.Flags & ~kKnownFlags));
    if (reserved != 0)
      return bad("reserved field is not zero");
    if (e.Offset % 8 != 0)
      return bad("offset is not a multiple of 8");
    if (e.Offset < tableEnd)
      return bad("offset is inside the header or section table");
    if (e.Offset > bytes.size() || e.Size > bytes.size() - e.Offset)
      return bad("section of " + std::to_string(e.Size) +
                 " bytes is past the end of the file");
    if (i > 0) {
      const SectionEntry &p = sections.back();
      if (e.Kind < p.Kind || (e.Kind == p.Kind && e.Offset <= p.Offset))
        return bad("section table is not sorted by (kind, offset)");
      if (e.Offset < p.Offset + p.Size || e.Offset <= p.Offset)
        return bad("section overlaps the previous one");
      if (e.Kind == p.Kind && !(e.Flags & kFlagInstanced))
        return fail(ErrorCode::DuplicateSection,
                    "kind appears twice and is not INSTANCED", idx, e.Kind,
                    e.Offset);
    }
    if (!known(e.Kind) && (e.Flags & kFlagRequired))
      return fail(ErrorCode::UnknownRequiredSection,
                  "unknown section kind marked REQUIRED", idx, e.Kind,
                  e.Offset);
    if (known(e.Kind) && e.Kind != static_cast<uint32_t>(Kind::Payload) &&
        (e.Flags & kFlagInstanced))
      return bad("only PAYLOAD may be INSTANCED");
    if (e.Kind == static_cast<uint32_t>(Kind::Manifest)) {
      ++manifests;
      if (e.Size > kMaxManifestSize)
        return fail(ErrorCode::BadManifest, "manifest larger than 1 MiB", idx,
                    e.Kind, e.Offset);
    }
    sections.push_back(e);
  }
  if (manifests != 1)
    return fail(
        manifests ? ErrorCode::DuplicateSection : ErrorCode::MissingManifest,
        manifests ? "more than one MANIFEST section" : "no MANIFEST section");

  // 4. Section hashes, eagerly, in table order.
  if (opts.VerifyHashes)
    for (size_t i = 0; i < sections.size(); ++i)
      if (support::sha256(view(bytes, sections[i])) != sections[i].Hash)
        return fail(ErrorCode::HashMismatch, "hash mismatch",
                    static_cast<int>(i), sections[i].Kind, sections[i].Offset);

  // 5. Manifest, and its agreement with the table (§2.5).
  size_t mi = 0;
  while (sections[mi].Kind != static_cast<uint32_t>(Kind::Manifest))
    ++mi;
  auto badManifest = [&](std::string msg) {
    return fail(ErrorCode::BadManifest, std::move(msg), static_cast<int>(mi),
                sections[mi].Kind, sections[mi].Offset);
  };
  StatusOr<Manifest> m = decodeManifest(view(bytes, sections[mi]));
  if (!m)
    return badManifest(m.status().message());
  manifest = std::move(*m);
  auto has = [&](Kind k) {
    return std::any_of(sections.begin(), sections.end(), [&](const auto &e) {
      return e.Kind == static_cast<uint32_t>(k);
    });
  };
  if (bool(manifest.Contents & kContentsHasCode) != has(Kind::Code))
    return badManifest("contents HAS_CODE disagrees with the section table");
  if (bool(manifest.Contents & kContentsHasTemplates) != has(Kind::Tmpl))
    return badManifest(
        "contents HAS_TEMPLATES disagrees with the section table");
  if (bool(manifest.Contents & kContentsSystem) !=
      manifest.Module.starts_with("::"))
    return badManifest("contents SYSTEM disagrees with the module name");
  return std::nullopt;
}

} // namespace

const char *kindName(uint32_t kind) {
  switch (static_cast<Kind>(kind)) {
  case Kind::Manifest:
    return "MANIFEST";
  case Kind::Iface:
    return "IFACE";
  case Kind::Tmpl:
    return "TMPL";
  case Kind::Code:
    return "CODE";
  case Kind::SymIdx:
    return "SYMIDX";
  case Kind::Debug:
    return "DEBUG";
  case Kind::Payload:
    return "PAYLOAD";
  }
  return nullptr;
}

std::string Error::str() const {
  if (Section < 0)
    return Message;
  const char *name = kindName(Kind);
  return "section " + (name ? std::string(name) : detail::hex(Kind)) + " (#" +
         std::to_string(Section) + ") at offset " + detail::hex(Offset) + ": " +
         Message;
}

StatusOr<File> File::read(std::span<const uint8_t> bytes,
                          const ReadOptions &opts, Error *err) {
  File f;
  std::optional<Error> e =
      readImpl(bytes, opts, f.Sections, f.Major, f.Minor, f.TableHash, f.M);
  if (e) {
    if (err)
      *err = *e;
    return Status::error(e->str());
  }
  f.Bytes.assign(bytes.begin(), bytes.end());
  return f;
}

const SectionEntry *File::find(Kind kind, size_t instance) const {
  for (const SectionEntry &e : Sections)
    if (e.Kind == static_cast<uint32_t>(kind) && instance-- == 0)
      return &e;
  return nullptr;
}

std::span<const uint8_t> File::section(Kind kind, size_t instance) const {
  const SectionEntry *e = find(kind, instance);
  return e ? view(Bytes, *e) : std::span<const uint8_t>();
}

std::span<const uint8_t> File::section(size_t index) const {
  return index < Sections.size() ? view(Bytes, Sections[index])
                                 : std::span<const uint8_t>();
}

size_t File::count(Kind kind) const {
  return static_cast<size_t>(
      std::count_if(Sections.begin(), Sections.end(), [&](const auto &e) {
        return e.Kind == static_cast<uint32_t>(kind);
      }));
}

Hash256 File::hashOf(Kind kind) const {
  const SectionEntry *e = find(kind, 0);
  return e ? e->Hash : Hash256{};
}

Hash256 File::moduleHash() const {
  support::Sha256 h;
  for (Kind k : {Kind::Manifest, Kind::Iface, Kind::Tmpl, Kind::Code}) {
    Hash256 part = hashOf(k);
    h.update(part.data(), part.size());
  }
  return h.finish();
}

// --- Writer -----------------------------------------------------------------

void Writer::add(uint32_t kind, uint32_t flags, std::vector<uint8_t> bytes) {
  Pendings.push_back({kind, flags, std::move(bytes)});
}

StatusOr<std::vector<uint8_t>> Writer::finish() const {
  std::vector<const Pending *> order;
  for (const Pending &p : Pendings)
    order.push_back(&p);
  // Stable: instances of one kind keep their insertion order.
  std::stable_sort(
      order.begin(), order.end(),
      [](const Pending *a, const Pending *b) { return a->Kind < b->Kind; });
  if (order.empty() || order.size() > kMaxSections)
    return Status::error("a .pkm holds 1..1024 sections, not " +
                         std::to_string(order.size()));
  size_t manifests = 0;
  for (size_t i = 0; i < order.size(); ++i) {
    const Pending &p = *order[i];
    if (p.Flags & ~kKnownFlags)
      return Status::error("unknown section flags " +
                           detail::hex(p.Flags & ~kKnownFlags));
    if (p.Kind == static_cast<uint32_t>(Kind::Manifest)) {
      ++manifests;
      if (p.Bytes.size() > kMaxManifestSize)
        return Status::error("manifest larger than 1 MiB");
    }
    if (i > 0 && order[i - 1]->Kind == p.Kind && !(p.Flags & kFlagInstanced))
      return Status::error(
          std::string("section kind ") +
          (kindName(p.Kind) ? kindName(p.Kind) : detail::hex(p.Kind).c_str()) +
          " added twice and not INSTANCED");
  }
  if (manifests != 1)
    return Status::error(manifests ? "more than one MANIFEST section"
                                   : "no MANIFEST section");

  uint64_t tableOffset = kHeaderSize;
  uint64_t offset = tableOffset + order.size() * kEntrySize;
  ByteWriter table;
  ByteWriter body;
  for (const Pending *p : order) {
    body.align(8);
    uint64_t at = offset + body.size();
    body.bytes(p->Bytes);
    table.u32(p->Kind);
    table.u32(p->Flags);
    table.u64(at);
    table.u64(p->Bytes.size());
    detail::writeHash(table, support::sha256(p->Bytes));
    table.u64(0);
  }
  ByteWriter out;
  out.bytes(kMagic, 8);
  out.u16(kFormatMajor);
  out.u16(kFormatMinor);
  out.u32(static_cast<uint32_t>(kHeaderSize));
  out.u32(static_cast<uint32_t>(order.size()));
  out.u32(static_cast<uint32_t>(kEntrySize));
  out.u64(tableOffset);
  detail::writeHash(out, support::sha256(table.data()));
  out.bytes(table.data());
  out.bytes(body.data());
  return out.take();
}

// --- Files ------------------------------------------------------------------

Status writeFileAtomically(const std::filesystem::path &path,
                           std::span<const uint8_t> bytes) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path dir = path.parent_path();
  if (!dir.empty()) {
    fs::create_directories(dir, ec);
    if (ec)
      return Status::error("cannot create directory '" + dir.string() +
                           "': " + ec.message());
  }
  // Unique per process and call, so concurrent writers of one cache entry
  // never share a temporary (a crash leaves a stray .tmp, never a partial
  // module file).
  static std::atomic<unsigned> counter{0};
  std::random_device rd;
  fs::path tmp = path;
  tmp += ".tmp" + std::to_string(rd()) + "-" + std::to_string(counter++);
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
      fs::remove(tmp, ec);
      return Status::error("cannot write '" + tmp.string() + "'");
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    std::error_code ignored;
    fs::remove(tmp, ignored);
    return Status::error("cannot rename '" + tmp.string() + "' to '" +
                         path.string() + "': " + ec.message());
  }
  return Status::ok();
}

StatusOr<std::vector<uint8_t>>
readFileBytes(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return Status::error("cannot open '" + path.string() + "'");
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  if (in.bad())
    return Status::error("cannot read '" + path.string() + "'");
  return bytes;
}

} // namespace paykan::pkm
