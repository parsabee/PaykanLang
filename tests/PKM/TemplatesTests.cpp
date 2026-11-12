// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The TMPL blob (docs/design/pkm.md §4.3): round trip, and every way a
// reader rejects one.

#include "paykan/pkm/Templates.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace paykan::pkm;

namespace {

Templates sample() {
  Templates t;
  t.File = "lib/gen.pkn";
  t.Uses = {"::result", "lib::util"};
  t.Impls = {{Templates::OwnerKind::Class, 0},
             {Templates::OwnerKind::Struct, 1},
             {Templates::OwnerKind::Class, 0},
             {Templates::OwnerKind::Type, 0}};
  t.Ast = "(paykan-ast 0.2.0\n  (unit))\n";
  return t;
}

std::string error(const std::vector<uint8_t> &bytes) {
  auto t = readTemplates(bytes);
  return t ? std::string() : t.status().message();
}

} // namespace

TEST(Templates, RoundTrips) {
  auto t = readTemplates(writeTemplates(sample()));
  ASSERT_TRUE(t) << t.status().message();
  EXPECT_EQ(*t, sample());
  Templates empty;
  auto e = readTemplates(writeTemplates(empty));
  ASSERT_TRUE(e) << e.status().message();
  EXPECT_EQ(*e, empty);
}

TEST(Templates, RejectsWhatIsNotABlobOfThisRelease) {
  std::vector<uint8_t> bytes = writeTemplates(sample());
  EXPECT_NE(error({bytes.begin(), bytes.begin() + 6}).find("truncated header"),
            std::string::npos);
  auto magic = bytes;
  magic[0] = 'X';
  EXPECT_NE(error(magic).find("bad magic"), std::string::npos);
  auto version = bytes;
  version[4] ^= 1;
  EXPECT_NE(error(version).find("this reader knows"), std::string::npos);
  auto flags = bytes;
  flags[8] = 1;
  EXPECT_NE(error(flags).find("unknown flags"), std::string::npos);
  // One byte more, allocated at that size (GCC 13's -O3 misreads a
  // push_back on a copy as an overflow).
  std::vector<uint8_t> trailing(bytes.size() + 1, 0);
  std::copy(bytes.begin(), bytes.end(), trailing.begin());
  EXPECT_NE(error(trailing).find("1 trailing bytes"), std::string::npos);
  EXPECT_FALSE(error({bytes.begin(), bytes.end() - 3}).empty());
}

TEST(Templates, RejectsABadImplOwner) {
  Templates t = sample();
  std::vector<uint8_t> bytes = writeTemplates(t);
  // The first impl record's owner kind follows the header, the file and
  // the uses, then the impl count.
  size_t at = 12 + 1 + t.File.size() + 1;
  for (const std::string &u : t.Uses)
    at += 1 + u.size();
  at += 1;
  ASSERT_EQ(bytes[at], 1);
  auto kind = bytes;
  kind[at] = 4;
  EXPECT_NE(error(kind).find("bad impl owner kind 4"), std::string::npos);
  kind[at] = 0;
  EXPECT_NE(error(kind).find("bad impl owner kind 0"), std::string::npos);
  // A block of a type that is not generic (#353) names no template: the
  // second record's, `struct 1`, read as one.
  auto type = bytes;
  ASSERT_EQ(type[at + 2], 2);
  ASSERT_EQ(type[at + 3], 1);
  type[at + 2] = 3;
  EXPECT_NE(error(type).find("an impl block of a type names template 1"),
            std::string::npos);
}
