// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// CodeGen tests: IntStr / FloatStr builtins, match on boxed types, Error paths.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// ============================================================================
// IntStr — valid parses
// ============================================================================

TEST(IntStr, ParseZero) {
  auto r = compileAndRun(wrapMain(R"(
    match IntStr("0") {
      n: Int { println(n.toString()); }
      _      { println("error"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n");
}

TEST(IntStr, ParsePositive) {
  auto r = compileAndRun(wrapMain(R"(
    match IntStr("42") {
      n: Int { println(n.toString()); }
      _      { println("error"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "42\n");
}

TEST(IntStr, ParseNegative) {
  auto r = compileAndRun(wrapMain(R"(
    match IntStr("-99") {
      n: Int { println(n.toString()); }
      _      { println("error"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "-99\n");
}

// ============================================================================
// IntStr — invalid input yields Error
// ============================================================================

TEST(IntStr, ParseAlpha) {
  auto r = compileAndRun(wrapMain(R"(
    match IntStr("abc") {
      n: Int     { println("ok"); }
      err: Error { println("error"); }
      _          {}
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "error\n");
}

TEST(IntStr, ParseEmpty) {
  auto r = compileAndRun(wrapMain(R"(
    match IntStr("") {
      n: Int     { println("ok"); }
      err: Error { println("error"); }
      _          {}
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "error\n");
}

TEST(IntStr, ParseFloat) {
  // "3.14" is not a valid integer.
  auto r = compileAndRun(wrapMain(R"(
    match IntStr("3.14") {
      n: Int     { println("ok"); }
      err: Error { println("error"); }
      _          {}
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "error\n");
}

// ============================================================================
// FloatStr — valid parses
// ============================================================================

TEST(FloatStr, ParseInteger) {
  auto r = compileAndRun(wrapMain(R"(
    match FloatStr("0") {
      f: Float { println(f.toString()); }
      _        { println("error"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n");
}

TEST(FloatStr, ParseDecimal) {
  auto r = compileAndRun(wrapMain(R"(
    match FloatStr("2.5") {
      f: Float { println(f.toString()); }
      _        { println("error"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "2.5\n");
}

TEST(FloatStr, ParseScientific) {
  auto r = compileAndRun(wrapMain(R"(
    match FloatStr("1e2") {
      f: Float { println(f.toString()); }
      _        { println("error"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "100\n");
}

// ============================================================================
// FloatStr — invalid input yields Error
// ============================================================================

TEST(FloatStr, ParseAlpha) {
  auto r = compileAndRun(wrapMain(R"(
    match FloatStr("xyz") {
      f: Float   { println("ok"); }
      err: Error { println("error"); }
      _          {}
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "error\n");
}

TEST(FloatStr, ParseEmpty) {
  auto r = compileAndRun(wrapMain(R"(
    match FloatStr("") {
      f: Float   { println("ok"); }
      err: Error { println("error"); }
      _          {}
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "error\n");
}

// ============================================================================
// equals on boxed values
// ============================================================================

TEST(BoxedEquals, IntEqualsSameValue) {
  auto r = compileAndRun(wrapMain(R"(
    match IntStr("7") {
      a: Int {
        match IntStr("7") {
          b: Int { println(StrBool(a.equals(b))); }
          _ {}
        }
      }
      _ {}
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\n");
}

TEST(BoxedEquals, IntNotEqualDifferentValue) {
  auto r = compileAndRun(wrapMain(R"(
    match IntStr("3") {
      a: Int {
        match IntStr("4") {
          b: Int { println(StrBool(a.equals(b))); }
          _ {}
        }
      }
      _ {}
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "False\n");
}

// ============================================================================
// Accumulation with IntStr in a loop
// ============================================================================

TEST(IntStr, SumValidInList) {
  auto r = compileAndRun(wrapMain(R"(
    inputs: Str[] = ["10", "bad", "20", "x", "30"];
    total: int = 0;
    i: int = 0;
    while (i < inputs.len()) {
      match IntStr(inputs[i]) {
        n: Int { total = total + 1; }
        _      {}
      }
      i = i + 1;
    }
    println(StrInt(total));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3\n");
}
