// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: arithmetic, literals, variables, basic errors

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Arith, EmptyMain) {
  auto [ok, _] = parse("fn main() -> int { return 0; }");
  EXPECT_TRUE(ok);
}

TEST(Arith, IntegerLiteral) {
  auto [ok, _] = parse("fn main() -> int { return 42; }");
  EXPECT_TRUE(ok);
}

TEST(Arith, FloatLiteral) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: float = 3.14;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, BoolLiterals) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: bool = True;
      b: bool = False;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, StringLiteral) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      s: Str = "hello world";
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, ArithmeticPrecedence) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      return 1 + 2 * 3;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, UnaryOperators) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = -42;
      b: bool = !True;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, ParenthesizedExpression) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      return (1 + 2) * 3;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, OwnershipQualifiers) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: Str = "hello";
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, ParamOwnershipQualifiers) {
  auto [ok, _] = parse(R"(
    fn f1(a: Str) {}
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, RefExpression) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: Str = "hello";
      b: Str = a;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, RelationalOperators) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: bool = 1 < 2;
      b: bool = 3 >= 3;
      c: bool = 4 == 4;
      d: bool = 5 != 6;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, NestedBlocks) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = 1;
      {
        y: int = 2;
        {
          z: int = 3;
        }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, EmptyStatement) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      ;
      ;;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, UnknownTypeAccepted) {
  // Identifiers in type positions are parsed as forward-referenced class types;
  // resolution is deferred to sema.
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: FooBar = 42;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, MissingSemicolon) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = 42
      return 0;
    }
  )");
  EXPECT_FALSE(ok);
}

TEST(Arith, MissingCloseBrace) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      return 0;
  )");
  EXPECT_FALSE(ok);
}
