// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: if/else, while, logical ops, ternary, break/continue

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// ─── if / else / else-if ────────────────────────────────────────────────────

TEST(ControlFlow, IfStmt) {
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

TEST(ControlFlow, IfElseStmt) {
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

TEST(ControlFlow, IfElseIfElse) {
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

TEST(ControlFlow, NestedIf) {
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

// ─── while ──────────────────────────────────────────────────────────────────

TEST(ControlFlow, WhileStmt) {
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

TEST(ControlFlow, WhileTrue) {
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

// ─── logical operators ──────────────────────────────────────────────────────

TEST(ControlFlow, LogicalAnd) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: bool = True && False;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(ControlFlow, LogicalOr) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: bool = True || False;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(ControlFlow, LogicalPrecedence) {
  // && binds tighter than ||
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: bool = True || False && True;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(ControlFlow, LogicalWithRelational) {
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

// ─── ternary ────────────────────────────────────────────────────────────────

TEST(ControlFlow, TernaryBasic) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = if True then 1 else 2;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(ControlFlow, TernaryNested) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = if True then 1 else if False then 2 else 3;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(ControlFlow, TernaryWithRelational) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int = 5;
      x: int = if a > 3 then 10 else 20;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// ─── break / continue ───────────────────────────────────────────────────────

TEST(ControlFlow, BreakInWhile) {
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

TEST(ControlFlow, ContinueInWhile) {
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

TEST(ControlFlow, BreakContinueNested) {
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
