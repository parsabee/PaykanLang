// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: the `mov` move expression.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Mov, MoveVariable) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int = 3;
      b = mov a;
      return b;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Mov, MoveString) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      s: Str = "hi";
      t = mov s;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Mov, MoveTemporaryCall) {
  auto [ok, _] = parse(R"(
    fn make() -> Str { return "x"; }
    fn main() -> int {
      t = mov make();
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Mov, MoveInReturn) {
  auto [ok, _] = parse(R"(
    fn take(s: Str) -> Str { return mov s; }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}
