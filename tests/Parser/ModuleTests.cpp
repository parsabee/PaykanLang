// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: import statements (all syntactic forms)

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Module, ImportUser) {
  auto [ok, _] = parse(R"(
    import foo;
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Module, ImportSystem) {
  auto [ok, _] = parse(R"(
    import ::io;
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Module, ImportNestedPath) {
  auto [ok, _] = parse(R"(
    import math::arith;
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Module, ImportFrom) {
  auto [ok, _] = parse(R"(
    import math::{add};
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Module, ImportFromSystem) {
  auto [ok, _] = parse(R"(
    import ::{io};
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Module, ImportMultiple) {
  auto [ok, _] = parse(R"(
    import foo;
    import bar::baz;
    import qux::{thing};
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}
