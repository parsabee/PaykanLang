// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The .pkm container (docs/design/pkm.md §1): the writer's layout, the
// reader's checks of §1.3/§1.7, derived hashes, hostile input and files.

#include "PKMTestUtils.h"

#include <filesystem>

using namespace pkmtest;

namespace {

TEST(Container, WriterLayoutAndRoundTrip) {
  std::vector<uint8_t> b = sampleFileBytes();
  ASSERT_GE(b.size(), 64u);
  EXPECT_EQ(std::memcmp(b.data(), "\x89PKM\r\n\x1a\n", 8), 0);
  EXPECT_EQ(b[8], 1); // format 1.0
  EXPECT_EQ(b[10], 0);
  EXPECT_EQ(getU32(b, 12), 64u);
  EXPECT_EQ(getU32(b, 16), 6u);
  EXPECT_EQ(getU32(b, 20), 64u);

  StatusOr<File> f = File::read(b);
  ASSERT_TRUE(f) << f.status().message();
  const auto &s = (*f).sections();
  ASSERT_EQ(s.size(), 6u);
  // Sorted by (kind, offset), 8-aligned, past the table, non-overlapping.
  std::vector<uint32_t> kinds;
  for (size_t i = 0; i < s.size(); ++i) {
    kinds.push_back(s[i].Kind);
    EXPECT_EQ(s[i].Offset % 8, 0u);
    EXPECT_GE(s[i].Offset, 64u + 64u * 6);
    if (i > 0) {
      EXPECT_GE(s[i].Offset, s[i - 1].Offset + s[i - 1].Size);
    }
    EXPECT_EQ(support::sha256((*f).section(i)), s[i].Hash);
  }
  EXPECT_EQ(kinds, (std::vector<uint32_t>{0x0001, 0x0100, 0x0200, 0x0201,
                                          0x0210, 0x0210}));
  EXPECT_EQ((*f).count(Kind::Payload), 2u);
  EXPECT_EQ((*f).count(Kind::Tmpl), 0u);
  EXPECT_EQ(vec((*f).section(Kind::Payload, 0)), bytesOf("payload one"));
  EXPECT_EQ(vec((*f).section(Kind::Payload, 1)),
            bytesOf("payload two, longer"));
  EXPECT_TRUE((*f).section(Kind::Payload, 2).empty());
  EXPECT_TRUE((*f).section(Kind::Debug).empty());
  EXPECT_EQ((*f).manifest(), sampleManifest());
  EXPECT_EQ((*f).formatMajor(), 1);
  EXPECT_EQ((*f).formatMinor(), 0);
}

TEST(Container, DerivedHashes) {
  StatusOr<File> f = File::read(sampleFileBytes());
  ASSERT_TRUE(f);
  EXPECT_EQ((*f).ifaceHash(), support::sha256((*f).section(Kind::Iface)));
  EXPECT_EQ((*f).codeHash(), support::sha256((*f).section(Kind::Code)));
  EXPECT_EQ((*f).tmplHash(), support::Hash256{});
  support::Sha256 h;
  for (Kind k : {Kind::Manifest, Kind::Iface, Kind::Tmpl, Kind::Code}) {
    support::Hash256 part =
        k == Kind::Tmpl ? support::Hash256{} : support::sha256((*f).section(k));
    h.update(part.data(), part.size());
  }
  EXPECT_EQ((*f).moduleHash(), h.finish());

  // Adding or stripping a payload leaves the module hash unchanged.
  Writer w;
  w.add(Kind::Manifest, kFlagRequired, encodeManifest(sampleManifest()));
  w.add(Kind::Iface, kFlagRequired, writeInterface(sampleInterface()));
  w.add(Kind::Code, kFlagRequired, bytesOf("binary pir bytes here"));
  StatusOr<File> g = File::read(*w.finish());
  ASSERT_TRUE(g);
  EXPECT_EQ((*g).moduleHash(), (*f).moduleHash());
}

TEST(Container, Deterministic) {
  EXPECT_EQ(sampleFileBytes(), sampleFileBytes());
  EXPECT_EQ(minimalFileBytes(), minimalFileBytes());
}

TEST(Container, WriterRefusals) {
  Writer none;
  EXPECT_FALSE(none.finish());
  Writer noManifest;
  noManifest.add(Kind::Iface, kFlagRequired, bytesOf("x"));
  EXPECT_FALSE(noManifest.finish());
  Writer twice;
  twice.add(Kind::Manifest, kFlagRequired, encodeManifest(minimalManifest()));
  twice.add(Kind::Code, kFlagRequired, bytesOf("a"));
  twice.add(Kind::Code, kFlagRequired, bytesOf("b"));
  EXPECT_FALSE(twice.finish());
  Writer twoManifests;
  twoManifests.add(Kind::Manifest, kFlagRequired, bytesOf("a"));
  twoManifests.add(Kind::Manifest, kFlagRequired, bytesOf("b"));
  EXPECT_FALSE(twoManifests.finish());
  Writer badFlags;
  badFlags.add(Kind::Manifest, 8, bytesOf("a"));
  EXPECT_FALSE(badFlags.finish());
  // A private kind, kept in the table and never interpreted.
  Writer priv;
  priv.add(Kind::Manifest, kFlagRequired, encodeManifest(minimalManifest()));
  priv.add(0x7F01u, 0, bytesOf("tool data"));
  StatusOr<std::vector<uint8_t>> b = priv.finish();
  ASSERT_TRUE(b);
  StatusOr<File> f = File::read(*b);
  ASSERT_TRUE(f) << f.status().message();
  EXPECT_EQ((*f).sections().size(), 2u);
  EXPECT_EQ(vec((*f).section(1)), bytesOf("tool data"));
}

TEST(Container, HeaderErrors) {
  std::vector<uint8_t> b = minimalFileBytes();
  {
    std::vector<uint8_t> t(b.begin(), b.begin() + 20);
    EXPECT_EQ(readError(t).Code, ErrorCode::Truncated);
  }
  {
    std::vector<uint8_t> t = b;
    t[1] = 'Q';
    EXPECT_EQ(readError(t).Code, ErrorCode::NotAPkm);
  }
  {
    std::vector<uint8_t> t = b;
    t[8] = 2;
    Error e = readError(t);
    EXPECT_EQ(e.Code, ErrorCode::UnsupportedFormat);
    EXPECT_EQ(e.str(), "format 2.0; this reader knows format 1.x");
  }
  {
    // A higher minor is accepted.
    std::vector<uint8_t> t = b;
    t[10] = 5;
    EXPECT_TRUE(File::read(t));
  }
  {
    std::vector<uint8_t> t = b;
    putU32(t, 12, 48);
    EXPECT_EQ(readError(t).Code, ErrorCode::BadHeader);
  }
  for (uint32_t count : {0u, 1025u}) {
    std::vector<uint8_t> t = b;
    putU32(t, 16, count);
    EXPECT_EQ(readError(t).Code, ErrorCode::BadHeader);
  }
  {
    std::vector<uint8_t> t = b;
    putU32(t, 20, 32);
    EXPECT_EQ(readError(t).Code, ErrorCode::BadHeader);
  }
  {
    std::vector<uint8_t> t = b;
    putU64(t, 24, 60);
    EXPECT_EQ(readError(t).Code, ErrorCode::BadHeader);
  }
  {
    std::vector<uint8_t> t = b;
    putU64(t, 24, 1ull << 62); // overflow-safe
    EXPECT_EQ(readError(t).Code, ErrorCode::Truncated);
  }
  {
    std::vector<uint8_t> t = b;
    t[40] ^= 1;
    Error e = readError(t);
    EXPECT_EQ(e.Code, ErrorCode::HashMismatch);
    EXPECT_EQ(e.Section, -1);
  }
}

TEST(Container, TableErrors) {
  std::vector<uint8_t> b = minimalFileBytes();
  auto patched = [&](auto edit) {
    std::vector<uint8_t> t = b;
    edit(t);
    rehash(t);
    return t;
  };
  // Unsorted: swap the two entries.
  {
    std::vector<uint8_t> t = patched([](std::vector<uint8_t> &t) {
      std::vector<uint8_t> e0(t.begin() + 64, t.begin() + 128);
      std::copy(t.begin() + 128, t.begin() + 192, t.begin() + 64);
      std::copy(e0.begin(), e0.end(), t.begin() + 128);
    });
    Error e = readError(t);
    EXPECT_EQ(e.Code, ErrorCode::BadSectionTable);
    EXPECT_EQ(e.Section, 1);
    EXPECT_EQ(e.Kind, 0x0001u);
  }
  // Overlap: the IFACE entry starts inside the manifest.
  {
    uint64_t manifestOff = 64 + 64 * 2;
    std::vector<uint8_t> t = patched([&](std::vector<uint8_t> &t) {
      putU64(t, entryAt(1) + 8, manifestOff);
    });
    Error e = readError(t);
    EXPECT_EQ(e.Code, ErrorCode::BadSectionTable);
    EXPECT_EQ(e.Section, 1);
    EXPECT_EQ(e.Kind, 0x0100u);
    EXPECT_EQ(e.str().substr(0, 20), "section IFACE (#1) a");
  }
  // Alignment.
  {
    std::vector<uint8_t> t = patched([&](std::vector<uint8_t> &t) {
      putU64(t, entryAt(1) + 8, getU64(t, entryAt(1) + 8) + 4);
    });
    EXPECT_EQ(readError(t).Code, ErrorCode::BadSectionTable);
  }
  // Offset inside the table.
  {
    std::vector<uint8_t> t = patched(
        [&](std::vector<uint8_t> &t) { putU64(t, entryAt(0) + 8, 64); });
    EXPECT_EQ(readError(t).Code, ErrorCode::BadSectionTable);
  }
  // Size past the end (and overflow).
  for (uint64_t size : {uint64_t(1) << 20, ~uint64_t(0)}) {
    std::vector<uint8_t> t = patched(
        [&](std::vector<uint8_t> &t) { putU64(t, entryAt(1) + 16, size); });
    EXPECT_EQ(readError(t).Code, ErrorCode::BadSectionTable);
  }
  // Unknown required kind.
  {
    std::vector<uint8_t> t = patched(
        [&](std::vector<uint8_t> &t) { putU32(t, entryAt(1), 0x7F00); });
    EXPECT_EQ(readError(t).Code, ErrorCode::UnknownRequiredSection);
  }
  // Unknown optional kind is kept; but then IFACE is missing from the
  // manifest's point of view only when CODE is claimed -- here it reads.
  {
    std::vector<uint8_t> t = patched([&](std::vector<uint8_t> &t) {
      putU32(t, entryAt(1), 0x7F00);
      putU32(t, entryAt(1) + 4, 0);
    });
    StatusOr<File> f = File::read(t);
    ASSERT_TRUE(f) << f.status().message();
    EXPECT_EQ((*f).sections()[1].Kind, 0x7F00u);
    EXPECT_TRUE((*f).section(Kind::Iface).empty());
  }
  // Duplicate kind: the IFACE entry relabelled MANIFEST (offset order kept).
  {
    std::vector<uint8_t> t = patched(
        [&](std::vector<uint8_t> &t) { putU32(t, entryAt(1), 0x0001); });
    EXPECT_EQ(readError(t).Code, ErrorCode::DuplicateSection);
  }
  // No MANIFEST: relabel it IFACE and the IFACE a payload.
  {
    std::vector<uint8_t> t = patched([&](std::vector<uint8_t> &t) {
      putU32(t, entryAt(0), 0x0100);
      putU32(t, entryAt(1), 0x0210);
      putU32(t, entryAt(1) + 4, kFlagInstanced);
    });
    EXPECT_EQ(readError(t).Code, ErrorCode::MissingManifest);
  }
  // INSTANCED on a kind that may not repeat; unknown flag bits.
  {
    std::vector<uint8_t> t = patched([&](std::vector<uint8_t> &t) {
      putU32(t, entryAt(1) + 4, kFlagRequired | kFlagInstanced);
    });
    EXPECT_EQ(readError(t).Code, ErrorCode::BadSectionTable);
    t = patched(
        [&](std::vector<uint8_t> &t) { putU32(t, entryAt(1) + 4, 0x10); });
    EXPECT_EQ(readError(t).Code, ErrorCode::BadSectionTable);
  }
  // Reserved != 0.
  {
    std::vector<uint8_t> t =
        patched([&](std::vector<uint8_t> &t) { t[entryAt(0) + 56] = 1; });
    EXPECT_EQ(readError(t).Code, ErrorCode::BadSectionTable);
  }
  // Section hash mismatch, with the documented message.
  {
    std::vector<uint8_t> t = b;
    size_t ifaceOff =
        static_cast<size_t>(File::read(b).value().sections()[1].Offset);
    t[ifaceOff + 4] ^= 0xFF;
    Error e = readError(t);
    EXPECT_EQ(e.Code, ErrorCode::HashMismatch);
    EXPECT_EQ(e.Section, 1);
    char expect[64];
    std::snprintf(expect, sizeof expect,
                  "section IFACE (#1) at offset 0x%zx: hash mismatch",
                  ifaceOff);
    EXPECT_EQ(e.str(), expect);
    // The test hook skips step 4.
    ReadOptions lax;
    lax.VerifyHashes = false;
    EXPECT_TRUE(File::read(t, lax));
  }
  // A manifest that disagrees with the table (HAS_CODE without CODE).
  {
    Manifest m = minimalManifest();
    m.Contents |= kContentsHasCode;
    Writer w;
    w.add(Kind::Manifest, kFlagRequired, encodeManifest(m));
    Interface i;
    i.Module = "m";
    w.add(Kind::Iface, kFlagRequired, writeInterface(i));
    Error e = readError(*w.finish());
    EXPECT_EQ(e.Code, ErrorCode::BadManifest);
    EXPECT_EQ(e.Section, 0);
  }
  {
    Writer w;
    w.add(Kind::Manifest, kFlagRequired, bytesOf("PKMMxxxx"));
    EXPECT_EQ(readError(*w.finish()).Code, ErrorCode::BadManifest);
  }
}

TEST(Container, TruncationAndBitFlipsNeverCrash) {
  std::vector<uint8_t> b = minimalFileBytes();
  for (size_t n = 0; n < b.size(); ++n) {
    std::vector<uint8_t> t(b.begin(), b.begin() + n);
    EXPECT_FALSE(File::read(t)) << "truncated to " << n;
  }
  // Bytes no check covers: the format minor (a higher minor is accepted)
  // and the zero padding between sections.
  std::vector<bool> unchecked(b.size(), false);
  unchecked[10] = unchecked[11] = true;
  StatusOr<File> f = File::read(b);
  ASSERT_TRUE(f);
  uint64_t end = 64 + 64 * (*f).sections().size();
  for (const SectionEntry &e : (*f).sections()) {
    for (uint64_t p = end; p < e.Offset; ++p)
      unchecked[p] = true;
    end = e.Offset + e.Size;
  }
  for (size_t at = 0; at < b.size(); ++at)
    for (uint8_t mask : {uint8_t(1), uint8_t(0x80), uint8_t(0xFF)}) {
      std::vector<uint8_t> t = b;
      t[at] ^= mask;
      StatusOr<File> r = File::read(t);
      if (!unchecked[at]) {
        EXPECT_FALSE(r) << "flip at " << at << " mask " << unsigned(mask);
      }
    }
}

TEST(Container, Files) {
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / "paykan_pkm_tests";
  fs::remove_all(dir);
  fs::path file = dir / "a" / "b.pkm";
  std::vector<uint8_t> b = minimalFileBytes();
  Status s = writeFileAtomically(file, b);
  ASSERT_TRUE(s) << s.message();
  StatusOr<std::vector<uint8_t>> back = readFileBytes(file);
  ASSERT_TRUE(back);
  EXPECT_EQ(*back, b);
  // No temporary left behind.
  size_t entries = 0;
  for (const auto &e : fs::directory_iterator(dir / "a")) {
    (void)e;
    ++entries;
  }
  EXPECT_EQ(entries, 1u);
  EXPECT_FALSE(readFileBytes(dir / "missing.pkm"));
  fs::remove_all(dir);
}

} // namespace
