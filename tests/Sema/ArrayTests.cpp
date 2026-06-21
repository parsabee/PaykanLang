// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: array type declarations, subscript access, and type checking.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// ============================================================================
// Array declarations
// ============================================================================

TEST(Array, IntArrayLiteral) {
  auto r = semaCheck(wrapMain("a: int[] = [1, 2, 3];"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, FloatArrayLiteral) {
  auto r = semaCheck(wrapMain("a: float[] = [1.0, 2.0];"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, BoolArrayLiteral) {
  auto r = semaCheck(wrapMain("a: bool[] = [True, False];"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, StrArrayLiteral) {
  auto r = semaCheck(wrapMain(R"(a: Str[] = ["x", "y"];)"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, EmptyArrayWithAnnotation) {
  auto r = semaCheck(wrapMain("a: int[] = [];"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, ObjArrayEmpty) {
  auto r = semaCheck(wrapMain("a: Obj[] = [];"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// Type mismatches
// ============================================================================

TEST(Array, WrongElementType) {
  // Mixing int and Str in a literal should fail type checking.
  auto r = semaCheck(wrapMain(R"(a: int[] = [1, "two", 3];)"));
  EXPECT_FALSE(r.Ok);
}

TEST(Array, AssignWrongArrayType) {
  // Assigning a Str[] to an int[] variable.
  auto r = semaCheck(wrapMain(R"(
    a: int[] = [1];
    b: Str[] = ["x"];
    a = b;
  )"));
  EXPECT_FALSE(r.Ok);
}

TEST(Array, DeclTypeMismatch) {
  // Annotated as int[] but initialised with Str literal elements.
  auto r = semaCheck(wrapMain(R"(a: int[] = ["hello"];)"));
  EXPECT_FALSE(r.Ok);
}

// ============================================================================
// Subscript access
// ============================================================================

TEST(Array, IntSubscriptRead) {
  auto r = semaCheck(wrapMain(R"(
    a: int[] = [10, 20];
    x: int = a[0];
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, StrSubscriptRead) {
  auto r = semaCheck(wrapMain(R"(
    a: Str[] = ["hi"];
    s: Str = a[0];
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, StringCharSubscript) {
  // s[i] on a Str yields char, not Str.
  auto r = semaCheck(wrapMain(R"(
    s: Str = "hello";
    c: char = s[0];
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, SubscriptAssignInt) {
  auto r = semaCheck(wrapMain(R"(
    a: int[] = [1, 2, 3];
    a[1] = 99;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, SubscriptAssignWrongType) {
  auto r = semaCheck(wrapMain(R"(
    a: int[] = [1, 2];
    a[0] = "bad";
  )"));
  EXPECT_FALSE(r.Ok);
}

// ============================================================================
// Array in function parameters and return types
// ============================================================================

TEST(Array, ArrayParam) {
  auto r = semaCheck(R"(
    fn sum(vals: int[]) -> int {
      return 0;
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, ArrayReturnType) {
  auto r = semaCheck(R"(
    fn makeArr() -> int[] {
      a: int[] = [1, 2];
      return a;
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, PassArrayToFunction) {
  auto r = semaCheck(R"(
    fn first(a: int[]) -> int { return a[0]; }
    fn main() -> int {
      arr: int[] = [5, 6];
      x: int = first(arr);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, WrongArrayParamType) {
  auto r = semaCheck(R"(
    fn takesIntArr(a: int[]) -> int { return 0; }
    fn main() -> int {
      s: Str[] = ["x"];
      takesIntArr(s);
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
}

// ============================================================================
// push / len / pop method type-checking
// ============================================================================

TEST(Array, PushCorrectType) {
  auto r = semaCheck(wrapMain(R"(
    a: int[] = [];
    a.push(42);
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, LenReturnsInt) {
  auto r = semaCheck(wrapMain(R"(
    a: int[] = [1, 2, 3];
    n: int = a.len();
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// Str indexing type
// ============================================================================

TEST(Array, StrIndexIsChar) {
  // Assigning s[i] to Str should fail — the result is char.
  auto r = semaCheck(wrapMain(R"(
    s: Str = "hi";
    bad: Str = s[0];
  )"));
  EXPECT_FALSE(r.Ok);
}
