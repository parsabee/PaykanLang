// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The OBJ blob (docs/design/pkm.md §5.13): round trip, and every way a
// reader rejects one.

#include "paykan/pkm/Object.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace paykan::pkm;

namespace {

Object sample() {
  Object o;
  o.Backend = "mlir";
  o.Format = "llvm-bc";
  o.Target = "x86_64-unknown-linux-gnu";
  o.Producer = "MLIR 22.1.8";
  o.Bytes = {'B', 'C', 0xc0, 0xde, 0, 1, 2, 3};
  return o;
}

std::string error(const std::vector<uint8_t> &bytes) {
  auto o = readObject(bytes);
  return o ? std::string() : o.status().message();
}

} // namespace

TEST(Object, RoundTrips) {
  auto o = readObject(writeObject(sample()));
  ASSERT_TRUE(o) << o.status().message();
  EXPECT_EQ(*o, sample());
  EXPECT_TRUE((*o).sameKind(sample()));
  Object other = sample();
  other.Producer = "MLIR 23.0.0";
  EXPECT_FALSE(other.sameKind(sample()));
}

TEST(Object, RejectsWhatIsNotABlobOfThisRelease) {
  std::vector<uint8_t> bytes = writeObject(sample());
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
  // One byte more, allocated at that size (see TemplatesTests).
  std::vector<uint8_t> trailing(bytes.size() + 1, 0);
  std::copy(bytes.begin(), bytes.end(), trailing.begin());
  EXPECT_NE(error(trailing).find("1 trailing bytes"), std::string::npos);
  EXPECT_FALSE(error({bytes.begin(), bytes.end() - 3}).empty());
  Object nameless = sample();
  nameless.Backend.clear();
  EXPECT_NE(error(writeObject(nameless)).find("names no backend"),
            std::string::npos);
}
