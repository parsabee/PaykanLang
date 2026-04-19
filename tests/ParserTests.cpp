// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser unit tests

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// ============================================================================
// Successful parsing
// ============================================================================

TEST(Parser, EmptyMain) {
  auto [ok, _] = parse("fn main() -> int { return 0; }");
  EXPECT_TRUE(ok);
}

TEST(Parser, IntegerLiteral) {
  auto [ok, _] = parse("fn main() -> int { return 42; }");
  EXPECT_TRUE(ok);
}

TEST(Parser, FloatLiteral) {
  auto [ok, drv] = parse(R"(
    fn main() -> int {
      x: float = 3.14;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, BoolLiterals) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: bool = True;
      b: bool = False;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, StringLiteral) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      s: String = "hello world";
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, ArithmeticPrecedence) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      return 1 + 2 * 3;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, UnaryOperators) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = -42;
      b: bool = !True;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, ParenthesizedExpression) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      return (1 + 2) * 3;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, FunctionWithParams) {
  auto [ok, _] = parse(R"(
    fn add(a: int, b: int) -> int {
      return a + b;
    }
    fn main() -> int { return add(1, 2); }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, VoidFunction) {
  auto [ok, _] = parse(R"(
    fn greet() {
      out("hi");
    }
    fn main() -> int { greet(); return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, OwnershipQualifiers) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: String = "unique";
      b: shared String = "shared";
      r: String& = &a;
      c: const String = "constant";
      d: const String& = &a;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, ParamOwnershipQualifiers) {
  auto [ok, _] = parse(R"(
    fn f1(a: String) {}
    fn f2(a: shared String) {}
    fn f3(a: String&) {}
    fn f4(a: const String) {}
    fn f5(a: const String&) {}
    fn f6(a: const shared String) {}
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, MovExpression) {
  auto [ok, _] = parse(R"(
    fn take(s: String) {}
    fn main() -> int {
      a: String = "hello";
      take(mov a);
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, RefExpression) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: String = "hello";
      r: String& = &a;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, RelationalOperators) {
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

TEST(Parser, NestedBlocks) {
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

TEST(Parser, EmptyStatement) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      ;
      ;;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// ============================================================================
// Parse errors
// ============================================================================

TEST(Parser, UnknownType) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: FooBar = 42;
      return 0;
    }
  )");
  EXPECT_FALSE(ok);
}

TEST(Parser, MissingSemicolon) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = 42
      return 0;
    }
  )");
  EXPECT_FALSE(ok);
}

TEST(Parser, MissingCloseBrace) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      return 0;
  )");
  EXPECT_FALSE(ok);
}

// ============================================================================
// if / else / else-if
// ============================================================================

TEST(Parser, IfStmt) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      if (True) {
        out("yes");
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, IfElseStmt) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = 10;
      if (x > 5) {
        out("big");
      } else {
        out("small");
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, IfElseIfElse) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = 42;
      if (x > 100) {
        out("large");
      } else if (x > 10) {
        out("medium");
      } else {
        out("small");
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, NestedIf) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = 5;
      if (x > 0) {
        if (x < 10) {
          out("single digit positive");
        }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// ============================================================================
// while
// ============================================================================

TEST(Parser, WhileStmt) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      i: int = 0;
      while (i < 5) {
        i = i + 1;
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, WhileTrue) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      while (True) {
        return 0;
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// ============================================================================
// Logical operators && ||
// ============================================================================

TEST(Parser, LogicalAnd) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: bool = True && False;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, LogicalOr) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: bool = True || False;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, LogicalPrecedence) {
  // && binds tighter than ||
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: bool = True || False && True;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, LogicalWithRelational) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = 5;
      y: int = 10;
      a: bool = x > 0 && y < 20;
      b: bool = x == 5 || y != 10;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// ============================================================================
// Ternary expressions
// ============================================================================

TEST(Parser, TernaryBasic) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = if True then 1 else 2;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, TernaryNested) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = if True then 1 else if False then 2 else 3;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, TernaryWithRelational) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int = 5;
      x: int = if a > 3 then 10 else 20;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// ============================================================================
// break / continue
// ============================================================================

TEST(Parser, BreakInWhile) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      while (True) {
        break;
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, ContinueInWhile) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      i: int = 0;
      while (i < 10) {
        i = i + 1;
        continue;
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Parser, BreakContinueNested) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      while (True) {
        while (True) {
          break;
        }
        continue;
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}
