// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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

// `<`, `<=`, `>` and `>=` order chars by their code, 0..255, as `int(c)`
// reads it: every pair of a few codes on both sides of 128, each a row of
// bits per operator, worked out here.  They compared the bytes signed, so
// 128..255 were below 0.
TEST(Char, OrderingIsByCode) {
  const std::vector<int> codes = {0, 1, 127, 128, 200, 255};
  auto holds = [](int op, int a, int b) {
    switch (op) {
    case 0:
      return a < b;
    case 1:
      return a <= b;
    case 2:
      return a > b;
    default:
      return a >= b;
    }
  };
  std::string expected;
  for (int a : codes) {
    for (int op = 0; op < 4; ++op) {
      for (int b : codes)
        expected += holds(op, a, b) ? '1' : '0';
      expected += op == 3 ? '\n' : ' ';
    }
  }
  auto r = compileAndRun(R"(
    import ::io;
    fn bits(a: char, op: int) -> Str {
      codes: int[6] = [0, 1, 127, 128, 200, 255];
      out = "";
      for i in 0..6 {
        b = char(codes[i]);
        r = a >= b;
        if op == 0 { r = a < b; }
        if op == 1 { r = a <= b; }
        if op == 2 { r = a > b; }
        out = out + (if r then "1" else "0");
      }
      return out;
    }
    fn main() -> int {
      codes: int[6] = [0, 1, 127, 128, 200, 255];
      for i in 0..6 {
        a = char(codes[i]);
        io::println(bits(a, 0) + " " + bits(a, 1) + " " + bits(a, 2) + " " +
            bits(a, 3));
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, expected);
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
