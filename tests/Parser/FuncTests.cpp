// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: function declarations, calls, parameters

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Func, FunctionWithParams) {
  auto [ok, _] = parse(R"(
    fn add(a: int, b: int) -> int {
      return a + b;
    }
    fn main() -> int { return add(1, 2); }
  )");
  EXPECT_TRUE(ok);
}

TEST(Func, VoidFunction) {
  auto [ok, _] = parse(R"(
    fn greet() {
      out("hi");
    }
    fn main() -> int { greet(); return 0; }
  )");
  EXPECT_TRUE(ok);
}
