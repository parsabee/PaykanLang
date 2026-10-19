// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// CodeGen tests: char primitive type end-to-end.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "import ::io; fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// -- Basic char literals and Str<char> conversion

TEST(Char, LiteralAndStrChar) {
  auto r = compileAndRun(wrapMain(R"(
    c: char = 'A';
    io::println(Str<char>(c));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "A\n");
}

TEST(Char, EscapeNewline) {
  auto r = compileAndRun(wrapMain(R"(
    c: char = '\n';
    io::print(Str<char>(c));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "\n");
}

TEST(Char, EscapeTab) {
  auto r = compileAndRun(wrapMain(R"(
    c: char = '\t';
    io::print("a" + Str<char>(c) + "b");
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "a\tb");
}

TEST(Char, EscapeBackslash) {
  auto r = compileAndRun(wrapMain(R"(
    c: char = '\\';
    io::println(Str<char>(c));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "\\\n");
}

// -- Comparison operators

TEST(Char, LessThan) {
  auto r = compileAndRun(wrapMain(R"(
    lo: char = 'a';
    hi: char = 'z';
    if lo < hi { io::println("true"); }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "true\n");
}

TEST(Char, Equality) {
  auto r = compileAndRun(wrapMain(R"(
    c: char = 'x';
    if c == 'x' { io::println("eq"); }
    if c != 'y' { io::println("neq"); }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "eq\nneq\n");
}

TEST(Char, GreaterThan) {
  auto r = compileAndRun(wrapMain(R"(
    if 'z' > 'a' { io::println("true"); }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "true\n");
}

// -- String indexing: s[i] -> char

TEST(Char, StringIndexYieldsChar) {
  auto r = compileAndRun(wrapMain(R"(
    s: Str = "hello";
    c: char = s[0];
    io::println(Str<char>(c));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "h\n");
}

TEST(Char, StringIndexLastChar) {
  auto r = compileAndRun(wrapMain(R"(
    s: Str = "paykan";
    io::println(Str<char>(s[5]));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "n\n");
}

TEST(Char, StringIndexCompare) {
  auto r = compileAndRun(wrapMain(R"(
    s: Str = "az";
    if s[0] < s[1] { io::println("a < z"); }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "a < z\n");
}

TEST(Char, IterateStringViaIndex) {
  auto r = compileAndRun(wrapMain(R"(
    s: Str = "hi";
    i: int = 0;
    while i < s.len() {
      io::print(Str<char>(s[i]));
      i = i + 1;
    }
    io::println("");
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "hi\n");
}

// -- Str<char> usage in string concatenation

TEST(Char, ConcatChars) {
  auto r = compileAndRun(wrapMain(R"(
    sep: char = '-';
    io::println(Str<char>(sep) + Str<char>(sep) + Str<char>(sep));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "---\n");
}

// -- Regression: multi-char literal must be rejected (task #2)

TEST(Char, MultiCharLiteralIsError) {
  // 'ab' should fail to parse or compile — not silently produce a wrong char.
  auto r = compileAndRun(wrapMain("c: char = 'ab';"));
  EXPECT_FALSE(r.CompileOk) << "multi-char literal should not compile";
}
