// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen tests: arithmetic, literals, and string primitives.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// ============================================================================
// Arithmetic
// ============================================================================

TEST(Arith, IntArithmetic) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = 2 + 3 * 4;
    println(StrInt(x));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "14\n");
}

TEST(Arith, FloatArithmetic) {
  auto r = compileAndRun(wrapMain(R"(
    x: float = 1.5 + 2.5;
    println(StrFloat(x));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "4\n");
}

TEST(Arith, BoolLiterals) {
  auto r = compileAndRun(wrapMain(R"(
    a: bool = True;
    b: bool = False;
    println(StrBool(a) + StrBool(b));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "TrueFalse\n");
}

// ============================================================================
// String primitives
// ============================================================================

TEST(Arith, StringLiteral) {
  auto r = compileAndRun(wrapMain(R"(println("hello world");)"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "hello world\n");
}

TEST(Arith, StringConcat) {
  auto r = compileAndRun(wrapMain(R"(
    a: Str = "hello";
    b: Str = " world";
    c: Str = a + b;
    println(c);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "hello world\n");
}

TEST(Arith, StringBuiltins) {
  auto r = compileAndRun(wrapMain(R"(
    println(StrInt(42) + StrFloat(3.14) + StrBool(True));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "423.14True\n");
}

TEST(Arith, StringVarBasic) {
  auto r = compileAndRun(wrapMain(R"(
    a: Str = "alpha";
    println(a);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "alpha\n");
}

TEST(Arith, StringReassign) {
  auto r = compileAndRun(wrapMain(R"(
    a: Str = "alpha";
    a = "beta";
    println(a);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "beta\n");
}

// ============================================================================
// Return code
// ============================================================================

TEST(Arith, ReturnCode) {
  auto r = compileAndRun("fn main() -> int { return 42; }");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 42);
}

TEST(Arith, ReturnZero) {
  auto r = compileAndRun("fn main() -> int { return 0; }");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
}
