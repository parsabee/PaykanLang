// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// CodeGen tests: char primitive type end-to-end.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// ============================================================================
// Basic char literals and StrChar conversion
// ============================================================================

TEST(Char, LiteralAndStrChar) {
  auto r = compileAndRun(wrapMain(R"(
    c: char = 'A';
    println(StrChar(c));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "A\n");
}

TEST(Char, EscapeNewline) {
  auto r = compileAndRun(wrapMain(R"(
    c: char = '\n';
    print(StrChar(c));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "\n");
}

TEST(Char, EscapeTab) {
  auto r = compileAndRun(wrapMain(R"(
    c: char = '\t';
    print("a" + StrChar(c) + "b");
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "a\tb");
}

TEST(Char, EscapeBackslash) {
  auto r = compileAndRun(wrapMain(R"(
    c: char = '\\';
    println(StrChar(c));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "\\\n");
}

// ============================================================================
// Comparison operators
// ============================================================================

TEST(Char, LessThan) {
  auto r = compileAndRun(wrapMain(R"(
    lo: char = 'a';
    hi: char = 'z';
    if (lo < hi) { println("true"); }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "true\n");
}

TEST(Char, Equality) {
  auto r = compileAndRun(wrapMain(R"(
    c: char = 'x';
    if (c == 'x') { println("eq"); }
    if (c != 'y') { println("neq"); }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "eq\nneq\n");
}

TEST(Char, GreaterThan) {
  auto r = compileAndRun(wrapMain(R"(
    if ('z' > 'a') { println("true"); }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "true\n");
}

// ============================================================================
// String indexing: s[i] -> char
// ============================================================================

TEST(Char, StringIndexYieldsChar) {
  auto r = compileAndRun(wrapMain(R"(
    s: Str = "hello";
    c: char = s[0];
    println(StrChar(c));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "h\n");
}

TEST(Char, StringIndexLastChar) {
  auto r = compileAndRun(wrapMain(R"(
    s: Str = "paykan";
    println(StrChar(s[5]));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "n\n");
}

TEST(Char, StringIndexCompare) {
  auto r = compileAndRun(wrapMain(R"(
    s: Str = "az";
    if (s[0] < s[1]) { println("a < z"); }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "a < z\n");
}

TEST(Char, IterateStringViaIndex) {
  auto r = compileAndRun(wrapMain(R"(
    s: Str = "hi";
    i: int = 0;
    while (i < s.len()) {
      print(StrChar(s[i]));
      i = i + 1;
    }
    println("");
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "hi\n");
}

// ============================================================================
// StrChar usage in string concatenation
// ============================================================================

TEST(Char, ConcatChars) {
  auto r = compileAndRun(wrapMain(R"(
    sep: char = '-';
    println(StrChar(sep) + StrChar(sep) + StrChar(sep));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "---\n");
}

// ============================================================================
// Regression: multi-char literal must be rejected (task #2)
// ============================================================================

TEST(Char, MultiCharLiteralIsError) {
  // 'ab' should fail to parse or compile — not silently produce a wrong char.
  auto r = compileAndRun(wrapMain("c: char = 'ab';"));
  EXPECT_FALSE(r.CompileOk) << "multi-char literal should not compile";
}
