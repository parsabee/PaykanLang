// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// CodeGen tests: array subscript read/write end-to-end.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// ============================================================================
// int[] subscript assignment
// ============================================================================

TEST(ArrayAssign, IntArray) {
  auto r = compileAndRun(wrapMain(R"(
    arr: int[] = [10, 20, 30];
    arr[0] = 99;
    arr[2] = 42;
    println(StrInt(arr[0]));
    println(StrInt(arr[1]));
    println(StrInt(arr[2]));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "99\n20\n42\n");
}

TEST(ArrayAssign, IntArrayLoopOverwrite) {
  auto r = compileAndRun(wrapMain(R"(
    arr: int[] = [0, 0, 0, 0, 0];
    i: int = 0;
    while (i < 5) {
      arr[i] = i * i;
      i = i + 1;
    }
    i = 0;
    while (i < 5) {
      println(StrInt(arr[i]));
      i = i + 1;
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n1\n4\n9\n16\n");
}

// ============================================================================
// float[] subscript assignment
// ============================================================================

TEST(ArrayAssign, FloatArray) {
  auto r = compileAndRun(wrapMain(R"(
    arr: float[] = [1.0, 2.0, 3.0];
    arr[1] = 9.5;
    println(StrFloat(arr[0]));
    println(StrFloat(arr[1]));
    println(StrFloat(arr[2]));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1\n9.5\n3\n");
}

// ============================================================================
// bool[] subscript assignment
// ============================================================================

TEST(ArrayAssign, BoolArray) {
  auto r = compileAndRun(wrapMain(R"(
    arr: bool[] = [True, False, True];
    arr[0] = False;
    arr[2] = False;
    println(StrBool(arr[0]));
    println(StrBool(arr[1]));
    println(StrBool(arr[2]));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "False\nFalse\nFalse\n");
}

// ============================================================================
// Str[] subscript assignment
// ============================================================================

TEST(ArrayAssign, StrArray) {
  auto r = compileAndRun(wrapMain(R"(
    arr: Str[] = ["a", "b", "c"];
    arr[1] = "hello";
    println(arr[0]);
    println(arr[1]);
    println(arr[2]);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "a\nhello\nc\n");
}

TEST(ArrayAssign, StrArrayMultipleWrites) {
  auto r = compileAndRun(wrapMain(R"(
    arr: Str[] = ["x", "y", "z"];
    arr[0] = "first";
    arr[0] = "second";
    println(arr[0]);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "second\n");
}

// ============================================================================
// String subscript read: s[i] -> Str (single char string)
// ============================================================================

TEST(ArraySubscript, StringIndexRead) {
  // s[i] on a Str yields a char — wrap with StrChar to print.
  auto r = compileAndRun(wrapMain(R"(
    s: Str = "paykan";
    println(StrChar(s[0]));
    println(StrChar(s[5]));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "p\nn\n");
}

TEST(ArraySubscript, StringIndexBuildResult) {
  auto r = compileAndRun(wrapMain(R"(
    s: Str = "abcdef";
    result: Str = StrChar(s[2]) + StrChar(s[1]) + StrChar(s[0]);
    println(result);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "cba\n");
}

// ============================================================================
// push/pop still work after subscript assign
// ============================================================================

TEST(ArrayAssign, PushAfterAssign) {
  auto r = compileAndRun(wrapMain(R"(
    arr: int[] = [1, 2, 3];
    arr[0] = 10;
    arr.push(99);
    println(StrInt(arr[0]));
    println(StrInt(arr.len()));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "10\n4\n");
}
