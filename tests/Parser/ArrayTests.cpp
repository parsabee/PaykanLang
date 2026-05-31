// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: array types, literals, subscript, match arms

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// ─── type annotations ───────────────────────────────────────────────────────

TEST(Array, IntArrayType) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[] = [];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, FloatArrayType) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: float[] = [];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, StrArrayType) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: Str[] = [];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, ClassArrayType) {
  auto [ok, _] = parse(R"(
    class Point { x: int; y: int; }
    fn main() -> int {
      pts: Point[] = [];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, ArrayTypeAsParam) {
  auto [ok, _] = parse(R"(
    fn sum(nums: int[]) -> int { return 0; }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, ArrayTypeAsReturnType) {
  auto [ok, _] = parse(R"(
    fn makeArr() -> int[] { return []; }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

// ─── array literals ─────────────────────────────────────────────────────────

TEST(Array, EmptyLiteral) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[] = [];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, IntLiteral) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[] = [1, 2, 3];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, FloatLiteral) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: float[] = [1.0, 2.5, 3.14];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, StringLiteral) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: Str[] = ["hello", "world"];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, SingleElementLiteral) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[] = [42];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, LiteralAsObjDecl) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: Obj = ["hello", "world"];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, LiteralWithExpressions) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = 1;
      a: int[] = [x, x + 1, x * 2];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// ─── subscript ──────────────────────────────────────────────────────────────

TEST(Array, SubscriptRead) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[] = [10, 20, 30];
      x: int = a[0];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, SubscriptWithExprIndex) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[] = [1, 2, 3];
      i: int = 1;
      x: int = a[i + 1];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, SubscriptInExpression) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[] = [3, 7];
      return a[0] + a[1];
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, SubscriptPassedToCall) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: Str[] = ["hi"];
      println(a[0]);
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, SubscriptChained) {
  // Parsing only — arr[0][1] is syntactically valid
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[] = [1];
      x: int = a[0];
      return x;
    }
  )");
  EXPECT_TRUE(ok);
}

// ─── push / len builtins (parsed as regular calls) ──────────────────────────

TEST(Array, PushCall) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: Str[] = [];
      push(a, "hello");
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, LenCall) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[] = [1, 2, 3];
      n: int = len(a);
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// ─── match on array types ────────────────────────────────────────────────────

TEST(Array, MatchArrayArm) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: Obj = ["hello", "world"];
      match x {
        Str[] { }
        _ { }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, MatchArrayArmWithBinding) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: Obj = ["hello", "world"];
      match x {
        arr: Str[] { println(arr[0]); }
        _          { }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, MatchMultipleArrayArms) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: Obj = [1, 2];
      match x {
        int[]  { }
        Str[]  { }
        _      { }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, MatchArrayWithBody) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: Obj = ["a", "b", "c"];
      match x {
        arr: Str[] {
          n: int = len(arr);
          if (n > 0) { println(arr[0]); }
        }
        _ { }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// ─── parse errors ────────────────────────────────────────────────────────────

TEST(Array, MissingCloseBracketInLiteral) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[] = [1, 2;
      return 0;
    }
  )");
  EXPECT_FALSE(ok);
}

TEST(Array, MissingCloseBracketInSubscript) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[] = [1, 2];
      x: int = a[0;
      return 0;
    }
  )");
  EXPECT_FALSE(ok);
}
