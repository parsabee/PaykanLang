// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The MANIFEST codec (docs/design/pkm.md §2) and the compatibility verdict
// (§8.2): round trip, canonical ordering, the TLV rules, every post-loop
// check, and each row of the verdict table.

#include "PKMTestUtils.h"

#include "paykan/support/Bytes.h"

#include <algorithm>

using namespace pkmtest;
using paykan::support::ByteWriter;

namespace {

/// `uleb tag, uleb len, value`.
void rec(ByteWriter &w, uint64_t tag, const std::vector<uint8_t> &value) {
  w.uleb(tag);
  w.uleb(value.size());
  w.bytes(value);
}

/// The sample manifest's bytes with one record's value replaced (or added,
/// when @p tag is new) and the records re-sorted: a hand-built TLV.
std::vector<uint8_t> rebuilt(const Manifest &m, uint64_t tag,
                             const std::vector<uint8_t> &value) {
  std::vector<uint8_t> src = encodeManifest(m);
  paykan::support::ByteReader r(src);
  r.skip(8);
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> records;
  while (!r.atEnd()) {
    uint64_t t, len;
    std::span<const uint8_t> v;
    EXPECT_TRUE(r.uleb(t) && r.uleb(len) && r.bytes(size_t(len), v));
    records.emplace_back(t, std::vector<uint8_t>(v.begin(), v.end()));
  }
  bool replaced = false;
  for (auto &[t, v] : records)
    if (t == tag) {
      v = value;
      replaced = true;
    }
  if (!replaced)
    records.emplace_back(tag, value);
  std::sort(records.begin(), records.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });
  ByteWriter w;
  w.bytes(src.data(), 8);
  for (auto &[t, v] : records)
    rec(w, t, v);
  return w.take();
}

std::string decodeError(const std::vector<uint8_t> &b) {
  StatusOr<Manifest> m = decodeManifest(b);
  EXPECT_FALSE(m);
  return m ? "" : m.status().message();
}

TEST(Manifest, RoundTrip) {
  Manifest m = sampleManifest();
  std::vector<uint8_t> b = encodeManifest(m);
  EXPECT_EQ(std::memcmp(b.data(), "PKMM\x01\x00\x00\x00", 8), 0);
  StatusOr<Manifest> back = decodeManifest(b);
  ASSERT_TRUE(back) << back.status().message();
  EXPECT_EQ(*back, m);
  EXPECT_EQ(encodeManifest(*back), b);

  Manifest min = minimalManifest("::sys");
  b = encodeManifest(min);
  back = decodeManifest(b);
  ASSERT_TRUE(back) << back.status().message();
  EXPECT_EQ(*back, min);
  EXPECT_EQ(encodeManifest(*back), b);
}

TEST(Manifest, CanonicalOrdering) {
  // Tags ascend; a default informational record is not written.
  std::vector<uint8_t> b = encodeManifest(sampleManifest());
  paykan::support::ByteReader r(b);
  r.skip(8);
  std::vector<uint64_t> tags;
  while (!r.atEnd()) {
    uint64_t t, len;
    ASSERT_TRUE(r.uleb(t) && r.uleb(len) && r.skip(size_t(len)));
    tags.push_back(t);
  }
  EXPECT_EQ(tags,
            (std::vector<uint64_t>{0x01, 0x03, 0x05, 0x07, 0x09, 0x0B, 0x0D,
                                   0x0F, 0x11, 0x20, 0x22, 0x24, 0x28}));
  b = encodeManifest(minimalManifest());
  r = paykan::support::ByteReader(b);
  r.skip(8);
  tags.clear();
  while (!r.atEnd()) {
    uint64_t t, len;
    ASSERT_TRUE(r.uleb(t) && r.uleb(len) && r.skip(size_t(len)));
    tags.push_back(t);
  }
  EXPECT_EQ(tags, (std::vector<uint64_t>{0x01, 0x03, 0x05, 0x07, 0x09, 0x0B,
                                         0x0D, 0x11}));
}

TEST(Manifest, Framing) {
  Manifest m = sampleManifest();
  std::vector<uint8_t> good = encodeManifest(m);
  EXPECT_NE(decodeError({}).find("truncated"), std::string::npos);
  {
    std::vector<uint8_t> b = good;
    b[0] = 'X';
    EXPECT_NE(decodeError(b).find("magic"), std::string::npos);
  }
  {
    std::vector<uint8_t> b = good;
    b[4] = 2;
    EXPECT_NE(decodeError(b).find("manifest format 2.0"), std::string::npos);
    b[4] = 1;
    b[6] = 9; // a newer minor is accepted
    EXPECT_TRUE(decodeManifest(b));
  }
  // Unknown critical tag rejects; unknown non-critical is skipped.
  EXPECT_NE(decodeError(rebuilt(m, 0x31, {1, 2, 3})).find("critical"),
            std::string::npos);
  StatusOr<Manifest> skipped = decodeManifest(rebuilt(m, 0x30, {1, 2, 3}));
  ASSERT_TRUE(skipped) << skipped.status().message();
  EXPECT_EQ(*skipped, m);
  // Trailing bytes in a known record.
  {
    ByteWriter v;
    v.u32(m.RuntimeAbi);
    v.u8(0);
    EXPECT_NE(decodeError(rebuilt(m, 0x09, v.take())).find("trailing"),
              std::string::npos);
  }
  // A short record.
  EXPECT_NE(decodeError(rebuilt(m, 0x09, {7, 0})).find("runtime_abi"),
            std::string::npos);
  // Out of order / repeated tags.
  {
    ByteWriter w;
    w.bytes(good.data(), good.size());
    rec(w, 0x01, {1, 'x'});
    EXPECT_NE(decodeError(w.take()).find("out of order"), std::string::npos);
  }
  // A record longer than the manifest.
  {
    ByteWriter w;
    w.bytes(good.data(), good.size());
    w.uleb(0x30);
    w.uleb(100);
    EXPECT_NE(decodeError(w.take()).find("longer"), std::string::npos);
  }
  // Truncation anywhere fails cleanly, except exactly between records once
  // every required record is in (the trailing records are informational).
  size_t requiredEnd = 0;
  std::vector<size_t> boundaries;
  {
    paykan::support::ByteReader r(good);
    r.skip(8);
    while (!r.atEnd()) {
      uint64_t t, len;
      ASSERT_TRUE(r.uleb(t) && r.uleb(len) && r.skip(size_t(len)));
      boundaries.push_back(r.offset());
      if (t == 0x11)
        requiredEnd = r.offset();
    }
  }
  for (size_t n = 0; n < good.size(); ++n) {
    bool ok = bool(decodeManifest(std::span<const uint8_t>(good.data(), n)));
    bool boundary =
        std::find(boundaries.begin(), boundaries.end(), n) != boundaries.end();
    EXPECT_EQ(ok, n >= requiredEnd && boundary) << n;
  }
  // Bad UTF-8 and NUL in strings.
  {
    ByteWriter v;
    v.str("geo\xff");
    EXPECT_NE(decodeError(rebuilt(m, 0x01, v.take())).find("UTF-8"),
              std::string::npos);
    ByteWriter v2;
    v2.str(std::string("a\0b", 3));
    EXPECT_FALSE(decodeManifest(rebuilt(m, 0x01, v2.take())));
  }
  // A list count larger than the bytes left.
  {
    ByteWriter v;
    v.uleb(1000);
    EXPECT_NE(decodeError(rebuilt(m, 0x0D, v.take())).find("deps"),
              std::string::npos);
  }
}

TEST(Manifest, PostLoopChecks) {
  Manifest m = sampleManifest();
  auto reject = [&](const Manifest &bad, const char *needle) {
    std::string msg = decodeError(encodeManifest(bad));
    EXPECT_NE(msg.find(needle), std::string::npos) << msg;
  };
  // Missing required record: strip the target tag by rebuilding without it.
  {
    std::vector<uint8_t> src = encodeManifest(m);
    paykan::support::ByteReader r(src);
    r.skip(8);
    ByteWriter w;
    w.bytes(src.data(), 8);
    while (!r.atEnd()) {
      uint64_t t, len;
      std::span<const uint8_t> v;
      ASSERT_TRUE(r.uleb(t) && r.uleb(len) && r.bytes(size_t(len), v));
      if (t != 0x0B)
        rec(w, t, std::vector<uint8_t>(v.begin(), v.end()));
    }
    EXPECT_NE(decodeError(w.take()).find("missing required record target"),
              std::string::npos);
  }
  Manifest bad = m;
  bad.Module = "a::::b";
  reject(bad, "canonical");
  bad.Module = "";
  reject(bad, "canonical");
  bad = m;
  bad.Deps[0] = bad.Deps[1];
  reject(bad, "sorted");
  bad = m;
  std::swap(bad.Deps[0], bad.Deps[1]);
  reject(bad, "sorted");
  bad = m;
  bad.Deps[1].Name = m.Module;
  reject(bad, "itself");
  bad = m;
  bad.Deps[0].Flags = 0;
  reject(bad, "SYSTEM");
  bad = m;
  bad.Libraries.clear();
  reject(bad, "RUNTIME");
  bad = m;
  bad.Libraries[0].Abi = 6;
  reject(bad, "ABI");
  bad = m;
  bad.Libraries.push_back(bad.Libraries[0]);
  reject(bad, "RUNTIME");
  bad = m;
  bad.Libraries[0].Kind = 9;
  reject(bad, "kind");
}

TEST(Manifest, Verdicts) {
  using K = Verdict::Kind;
  using R = Verdict::Reason;
  Manifest m = sampleManifest();
  HostIdentity host;
  host.IfaceMajor = 1;
  host.IfaceMinor = 0;
  host.PirVersion = 2;
  host.RuntimeAbi = 7;
  host.Target = {8, 8, 1, 64, 64};
  host.CoreVersion = "0.2.0-rc1";
  host.CoreBuild = "abc123";
  host.CompatibleVersions = {"0.2.0-rc1", "0.2.0"};
  for (Policy p : {Policy::Cache, Policy::Prebuilt}) {
    Verdict v = checkCompatibility(m, host, p);
    EXPECT_EQ(v.Outcome, K::Usable);
    EXPECT_EQ(v.Cause, R::None);
    EXPECT_TRUE(v.Message.empty());
  }
  auto expect = [&](const Manifest &x, Policy p, K k, R r) {
    Verdict v = checkCompatibility(x, host, p);
    EXPECT_EQ(v.Outcome, k) << v.Message;
    EXPECT_EQ(v.Cause, r) << v.Message;
    EXPECT_FALSE(v.Message.empty());
  };
  Manifest x = m;
  x.Formats.IfaceMajor = 2;
  expect(x, Policy::Cache, K::Rejected, R::Format);
  x = m;
  x.Formats.IfaceMinor = 1;
  expect(x, Policy::Cache, K::Stale, R::Interface);
  expect(x, Policy::Prebuilt, K::Rejected, R::Interface);
  x = m;
  x.Formats.PirVersion = 1;
  expect(x, Policy::Prebuilt, K::Rejected, R::PIR);
  x = m;
  x.RuntimeAbi = 6;
  expect(x, Policy::Cache, K::Rejected, R::ABI);
  x = m;
  x.Target.PointerSize = 4;
  expect(x, Policy::Cache, K::Rejected, R::Target);
  // core.version: two tiers.
  x = m;
  x.Core.Version = "0.2.0";
  expect(x, Policy::Cache, K::Stale, R::Toolchain);
  EXPECT_EQ(checkCompatibility(x, host, Policy::Prebuilt).Outcome, K::Usable);
  x.Core.Version = "0.1.1";
  expect(x, Policy::Prebuilt, K::Rejected, R::Toolchain);
  x = m;
  x.Core.Build = "other";
  expect(x, Policy::Cache, K::Stale, R::Toolchain);
  EXPECT_EQ(checkCompatibility(x, host, Policy::Prebuilt).Outcome, K::Usable);
  // The first failing row wins.
  x = m;
  x.RuntimeAbi = 6;
  x.Formats.PirVersion = 1;
  expect(x, Policy::Cache, K::Rejected, R::PIR);
}

TEST(Manifest, SourceMatches) {
  Manifest m = sampleManifest();
  std::string src = "fn main() {}";
  m.Source = Manifest::SourceInfo{support::sha256(src), src.size()};
  std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t *>(src.data()),
                                 src.size());
  EXPECT_TRUE(sourceMatches(m, bytes));
  EXPECT_FALSE(sourceMatches(m, bytes.subspan(1)));
  m.Source->Size = 1;
  EXPECT_FALSE(sourceMatches(m, bytes));
  m.Source.reset();
  EXPECT_FALSE(sourceMatches(m, bytes));
}

} // namespace
