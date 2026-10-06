// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The IFACE blob codec (docs/design/pkm.md §3): round trip, determinism,
// the part table and record framing, and the §3.7 invariants the prototype
// records support.

#include "PKMTestUtils.h"

#include "paykan/support/Bytes.h"

#include <algorithm>

using namespace pkmtest;
using paykan::support::ByteReader;
using paykan::support::ByteWriter;

namespace {

/// The sample interface as the reader returns it: dependencies and records
/// sorted by name.
Interface sortedSample() {
  Interface i = sampleInterface();
  auto byName = [](const auto &a, const auto &b) { return a.Name < b.Name; };
  std::sort(i.Mods.begin(), i.Mods.end(), [](const auto &a, const auto &b) {
    return a.Canonical < b.Canonical;
  });
  std::sort(i.Functions.begin(), i.Functions.end(), byName);
  std::sort(i.Classes.begin(), i.Classes.end(), byName);
  std::sort(i.Enums.begin(), i.Enums.end(), byName);
  return i;
}

std::string readError(const std::vector<uint8_t> &b,
                      std::string_view module = "geometry::shapes") {
  StatusOr<Interface> r = readInterface(b, module);
  EXPECT_FALSE(r);
  return r ? "" : r.status().message();
}

struct Part {
  uint32_t Tag, Flags;
  uint64_t Offset, Size;
};

std::vector<Part> parts(const std::vector<uint8_t> &b) {
  ByteReader r(b);
  r.skip(12);
  uint32_t n;
  EXPECT_TRUE(r.u32(n));
  std::vector<Part> out(n);
  for (Part &p : out)
    EXPECT_TRUE(r.u32(p.Tag) && r.u32(p.Flags) && r.u64(p.Offset) &&
                r.u64(p.Size));
  return out;
}

/// Rebuilds a blob from parts, so a test can replace one part's bytes.
std::vector<uint8_t>
rebuild(const std::vector<uint8_t> &src,
        std::vector<std::pair<Part, std::vector<uint8_t>>> replaced) {
  ByteWriter w;
  w.bytes(src.data(), 12);
  w.u32(static_cast<uint32_t>(replaced.size()));
  uint64_t offset = 16 + 24 * replaced.size();
  for (auto &[p, bytes] : replaced) {
    w.u32(p.Tag);
    w.u32(p.Flags);
    w.u64(offset);
    w.u64(bytes.size());
    offset += (bytes.size() + 7) & ~uint64_t(7);
  }
  for (auto &[p, bytes] : replaced) {
    w.bytes(bytes);
    w.align(8);
  }
  return w.take();
}

std::vector<std::pair<Part, std::vector<uint8_t>>>
partsWithBytes(const std::vector<uint8_t> &b) {
  std::vector<std::pair<Part, std::vector<uint8_t>>> out;
  for (const Part &p : parts(b))
    out.emplace_back(p, std::vector<uint8_t>(b.begin() + p.Offset,
                                             b.begin() + p.Offset + p.Size));
  return out;
}

/// The blob with part @p tag's bytes replaced.
std::vector<uint8_t> withPart(const std::vector<uint8_t> &b, uint32_t tag,
                              std::vector<uint8_t> bytes) {
  auto ps = partsWithBytes(b);
  for (auto &[p, v] : ps)
    if (p.Tag == tag)
      v = std::move(bytes);
  return rebuild(b, ps);
}

TEST(Interface, RoundTrip) {
  Interface i = sampleInterface();
  std::vector<uint8_t> b = writeInterface(i);
  EXPECT_EQ(std::memcmp(b.data(), "PKMI\x01\x00\x00\x00", 8), 0);
  std::vector<Part> ps = parts(b);
  ASSERT_EQ(ps.size(), 6u);
  std::vector<uint32_t> tags, flags;
  for (const Part &p : ps) {
    tags.push_back(p.Tag);
    flags.push_back(p.Flags);
    EXPECT_EQ(p.Offset % 8, 0u);
    EXPECT_LE(p.Offset + p.Size, b.size());
  }
  EXPECT_EQ(tags, (std::vector<uint32_t>{1, 2, 3, 4, 5, 7}));
  EXPECT_EQ(flags, (std::vector<uint32_t>{1, 1, 1, 1, 1, 0}));
  // TYPES and INST are empty record lists.
  EXPECT_EQ(ps[2].Size, 1u);
  EXPECT_EQ(b[ps[2].Offset], 0);
  EXPECT_EQ(ps[4].Size, 1u);

  StatusOr<Interface> back = readInterface(b, "geometry::shapes");
  ASSERT_TRUE(back) << back.status().message();
  EXPECT_EQ(*back, sortedSample());
  EXPECT_EQ(writeInterface(*back), b);
  EXPECT_EQ(writeInterface(i), writeInterface(sortedSample()));

  // Without DIAG, and a system module.
  Interface s;
  s.Module = "::io";
  s.System = true;
  b = writeInterface(s);
  EXPECT_EQ(parts(b).size(), 5u);
  EXPECT_EQ(b[8], 2); // flags bit1 system
  back = readInterface(b, "::io");
  ASSERT_TRUE(back) << back.status().message();
  EXPECT_EQ(*back, s);
}

TEST(Interface, Deterministic) {
  EXPECT_EQ(writeInterface(sampleInterface()),
            writeInterface(sampleInterface()));
}

TEST(Interface, Framing) {
  std::vector<uint8_t> good = writeInterface(sampleInterface());
  EXPECT_NE(readError({}).find("truncated"), std::string::npos);
  {
    std::vector<uint8_t> b = good;
    b[0] = 'x';
    EXPECT_NE(readError(b).find("magic"), std::string::npos);
    b = good;
    b[4] = 2;
    EXPECT_NE(readError(b).find("interface format 2.0"), std::string::npos);
    b = good;
    b[6] = 3; // newer minor accepted
    EXPECT_TRUE(readInterface(b, "geometry::shapes"));
  }
  EXPECT_NE(readError(good, "geometry::other").find("not 'geometry::other'"),
            std::string::npos);
  // Part table faults: unaligned, overlapping, past the end, repeated,
  // unknown required, missing required.
  auto ps = partsWithBytes(good);
  {
    std::vector<uint8_t> b = rebuild(good, ps);
    putU64(b, 16 + 24 * 1 + 8, getU64(b, 16 + 24 * 1 + 8) + 4);
    EXPECT_NE(readError(b).find("unaligned"), std::string::npos);
    b = rebuild(good, ps);
    putU64(b, 16 + 24 * 2 + 8, getU64(b, 16 + 24 * 1 + 8));
    EXPECT_NE(readError(b).find("overlapping"), std::string::npos);
    b = rebuild(good, ps);
    putU64(b, 16 + 24 * 5 + 16, 1u << 20);
    EXPECT_NE(readError(b).find("past the end"), std::string::npos);
    b = rebuild(good, ps);
    putU32(b, 16 + 24 * 5, 1);
    EXPECT_NE(readError(b).find("repeats"), std::string::npos);
    b = rebuild(good, ps);
    putU32(b, 16 + 24 * 5, 9);
    putU32(b, 16 + 24 * 5 + 4, 1);
    EXPECT_NE(readError(b).find("unknown and required"), std::string::npos);
    // Unknown optional part: skipped.
    putU32(b, 16 + 24 * 5 + 4, 0);
    StatusOr<Interface> r = readInterface(b, "geometry::shapes");
    ASSERT_TRUE(r) << r.status().message();
    EXPECT_TRUE((*r).DisplayFile.empty());
  }
  {
    auto missing = ps;
    missing.erase(missing.begin() + 2); // TYPES
    EXPECT_NE(readError(rebuild(good, missing)).find("missing part 3"),
              std::string::npos);
  }
  {
    std::vector<uint8_t> b = good;
    putU32(b, 12, 0);
    EXPECT_NE(readError(b).find("part count"), std::string::npos);
    putU32(b, 12, 1000);
    EXPECT_NE(readError(b).find("part count"), std::string::npos);
  }
  // Truncation anywhere fails cleanly (the final alignment padding is the
  // only part of the blob no part needs).
  size_t lastEnd = 0;
  for (const Part &p : parts(good))
    lastEnd = std::max(lastEnd, size_t(p.Offset + p.Size));
  for (size_t n = 0; n < good.size(); ++n) {
    bool ok = bool(readInterface(std::span<const uint8_t>(good.data(), n),
                                 "geometry::shapes"));
    EXPECT_EQ(ok, n >= lastEnd) << n;
  }
  // Single-byte flips never crash; the string table and records are
  // validated so most are rejected (some flips are benign: a renamed
  // string, an ignored trailing byte).
  for (size_t at = 0; at < good.size(); ++at) {
    std::vector<uint8_t> b = good;
    b[at] ^= 0xFF;
    (void)readInterface(b, "geometry::shapes");
  }
}

TEST(Interface, Records) {
  Interface i = sampleInterface();
  std::vector<uint8_t> good = writeInterface(i);
  // STRS: entry 0 not empty; invalid UTF-8; trailing bytes.
  {
    ByteWriter w;
    w.uleb(1);
    w.str("x");
    EXPECT_NE(readError(withPart(good, 1, w.take())).find("STRS"),
              std::string::npos);
    ByteWriter w2;
    w2.uleb(2);
    w2.str("");
    w2.str("\xC0\x80");
    EXPECT_NE(readError(withPart(good, 1, w2.take())).find("UTF-8"),
              std::string::npos);
  }
  // A string index out of range: MODS entry naming string 999.
  {
    ByteWriter w;
    w.uleb(1);
    ByteWriter v;
    v.uleb(999);
    v.u8(0);
    for (int k = 0; k < 64; ++k)
      v.u8(0);
    w.uleb(1);
    w.uleb(v.size());
    w.bytes(v.data());
    EXPECT_NE(readError(withPart(good, 2, w.take())).find("MODS"),
              std::string::npos);
  }
  // Record framing: unknown tag < 0x80 rejects, >= 0x80 skipped; a
  // record's trailing payload bytes are ignored; a record longer than the
  // part rejects; a count larger than the bytes rejects.
  {
    auto diag = [&](uint64_t tag, std::vector<uint8_t> payload) {
      ByteWriter w;
      w.uleb(1);
      w.uleb(tag);
      w.uleb(payload.size());
      w.bytes(payload);
      return withPart(good, 7, w.take());
    };
    EXPECT_NE(readError(diag(2, {0})).find("unknown required record tag"),
              std::string::npos);
    StatusOr<Interface> r = readInterface(diag(0x80, {1, 2, 3}), i.Module);
    ASSERT_TRUE(r) << r.status().message();
    EXPECT_TRUE((*r).DisplayFile.empty());
    // FILE record with trailing bytes: the display file is read, the rest
    // ignored.  String index of the display file in the sample table:
    // find it by writing a fresh blob and reading the index back.
    std::vector<uint8_t> strsBytes = partsWithBytes(good)[0].second;
    ByteReader sr(strsBytes);
    std::vector<std::string> strs;
    ASSERT_TRUE(paykan::support::StringTable::read(sr, strs));
    size_t idx =
        std::find(strs.begin(), strs.end(), i.DisplayFile) - strs.begin();
    ByteWriter payload;
    payload.uleb(idx);
    payload.u8(0xAA);
    payload.u8(0xBB);
    r = readInterface(diag(1, payload.take()), i.Module);
    ASSERT_TRUE(r) << r.status().message();
    EXPECT_EQ((*r).DisplayFile, i.DisplayFile);
    ByteWriter longRec;
    longRec.uleb(1);
    longRec.uleb(1);
    longRec.uleb(50);
    EXPECT_FALSE(readInterface(withPart(good, 7, longRec.take()), i.Module));
    ByteWriter bigCount;
    bigCount.uleb(1000);
    EXPECT_NE(readError(withPart(good, 7, bigCount.take())).find("DIAG"),
              std::string::npos);
    ByteWriter trailing;
    trailing.uleb(0);
    trailing.u8(0);
    EXPECT_NE(readError(withPart(good, 7, trailing.take())).find("trailing"),
              std::string::npos);
  }
  // TYPES with a type record: not supported by the prototype reader.
  {
    ByteWriter w;
    w.uleb(1);
    w.uleb(1); // T_INT
    w.uleb(0);
    EXPECT_NE(readError(withPart(good, 3, w.take())).find("TYPES"),
              std::string::npos);
  }
  // MODS: self-dependency, unsorted, duplicate, system flag disagreement.
  {
    Interface bad = i;
    bad.Mods.push_back({i.Module, false, {}, {}});
    EXPECT_NE(readError(writeInterface(bad), i.Module).find("itself"),
              std::string::npos);
    bad = i;
    bad.Mods.push_back(bad.Mods[0]);
    EXPECT_NE(readError(writeInterface(bad), i.Module).find("sorted"),
              std::string::npos);
    std::vector<uint8_t> b = good;
    putU32(b, 8, kIfaceSystem);
    EXPECT_NE(readError(b).find("system flag"), std::string::npos);
  }
  // DECLS: duplicate names per kind reject; the same name across kinds is
  // allowed (a constructor function and its class).
  {
    Interface bad = i;
    bad.Enums.push_back(bad.Enums[0]);
    EXPECT_NE(readError(writeInterface(bad), i.Module).find("sorted"),
              std::string::npos);
    bad = i;
    bad.Classes.push_back(bad.Classes[0]);
    EXPECT_FALSE(readInterface(writeInterface(bad), i.Module));
    bad = i;
    bad.Functions.push_back(bad.Functions[0]);
    EXPECT_FALSE(readInterface(writeInterface(bad), i.Module));
    // Out of order: FUNC before ENUM, hand-built from the good DECLS.
    auto ps = partsWithBytes(good);
    ByteReader r(ps[3].second);
    uint64_t n;
    ASSERT_TRUE(r.uleb(n));
    std::vector<std::vector<uint8_t>> recs;
    for (uint64_t k = 0; k < n; ++k) {
      size_t start = r.offset();
      uint64_t tag, len;
      ASSERT_TRUE(r.uleb(tag) && r.uleb(len) && r.skip(size_t(len)));
      recs.emplace_back(ps[3].second.begin() + start,
                        ps[3].second.begin() + r.offset());
    }
    std::rotate(recs.begin(), recs.begin() + 1, recs.end());
    ByteWriter w;
    w.uleb(n);
    for (auto &rec : recs)
      w.bytes(rec);
    EXPECT_NE(readError(withPart(good, 4, w.take())).find("sorted"),
              std::string::npos);
  }
}

} // namespace
