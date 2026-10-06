// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen tests: arithmetic, literals, and string primitives.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// -- Arithmetic

TEST(Arith, IntArithmetic) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = 2 + 3 * 4;
    println(Str<int>(x));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "14\n");
}

TEST(Arith, FloatArithmetic) {
  auto r = compileAndRun(wrapMain(R"(
    x: float = 1.5 + 2.5;
    println(Str<float>(x));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "4\n");
}

TEST(Arith, BoolLiterals) {
  auto r = compileAndRun(wrapMain(R"(
    a: bool = True;
    b: bool = False;
    println(Str<bool>(a) + Str<bool>(b));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "TrueFalse\n");
}

// -- String primitives

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
    println(Str<int>(42) + Str<float>(3.14) + Str<bool>(True));
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

// -- Float comparisons with NaN (IEEE 754: `!=` is unordered, the rest ordered)

namespace {

/// A program that prints, one line per (lhs, rhs) pair, the six comparison
/// results as T/F in the order != == < <= > >=.  `n` is a NaN computed at
/// run time (inf - inf), `one` is 1.0.
std::string floatCompareProgram(const std::vector<std::string> &pairs) {
  std::string body = "  inf: float = 1.0e308 * 10.0;\n"
                     "  n: float = inf - inf;\n"
                     "  one: float = 1.0;\n";
  for (size_t i = 0; i + 1 < pairs.size(); i += 2) {
    const std::string &a = pairs[i], &b = pairs[i + 1];
    body += "  println(\"\"";
    for (const char *op : {"!=", "==", "<", "<=", ">", ">="})
      body.append(" + (if ")
          .append(a)
          .append(" ")
          .append(op)
          .append(" ")
          .append(b)
          .append(R"( then "T" else "F"))");
    body += ");\n";
  }
  return wrapMain(body);
}

} // namespace

TEST(FloatCompare, NaNOnBothSides) {
  auto r = compileAndRun(floatCompareProgram({"n", "n"}));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  // Only != holds: NaN is unordered with everything, itself included.
  EXPECT_EQ(r.StdOut, "TFFFFF\n");
}

TEST(FloatCompare, NaNOnOneSide) {
  auto r = compileAndRun(floatCompareProgram({"n", "one", "one", "n"}));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "TFFFFF\nTFFFFF\n");
}

TEST(FloatCompare, NoNaN) {
  auto r = compileAndRun(floatCompareProgram(
      {"one", "one", "one", "2.0", "2.0", "one", "inf", "one", "0.0", "-0.0"}));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "FTFTFT\n"   // 1 vs 1
                      "TFTTFF\n"   // 1 vs 2
                      "TFFFTT\n"   // 2 vs 1
                      "TFFFTT\n"   // inf vs 1
                      "FTFTFT\n"); // 0.0 == -0.0
}

TEST(FloatCompare, NaNNotEqualAsAConditionAndANegation) {
  auto r = compileAndRun(wrapMain(R"(
    inf: float = 1.0e308 * 10.0;
    n: float = inf - inf;
    if (n != n) { println("ne"); } else { println("eq"); }
    b: bool = n != n;
    println(Str<bool>(b));
    println(Str<bool>(!(n != n)));
    println(Str<bool>(!(n == n)));
    i = 0;
    while (n != n && i < 3) { i = i + 1; }
    println(Str<int>(i));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "ne\nTrue\nFalse\nTrue\n3\n");
}

TEST(FloatCompare, NaNThroughTupleBoxedFloatAndMatch) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    inf: float = 1.0e308 * 10.0;
    n: float = inf - inf;
    // Element-wise tuple equality compares the float slots with ==.
    t1: (float, int) = (n, 1);
    t2: (float, int) = (n, 1);
    println(Str<bool>(t1 == t2) + " " + Str<bool>(t1 != t2));
    // A boxed Float's equals compares the values with == (a present
    // `float?` is a Float box; matched as one through `Obj`).
    o: Obj = float<Str>("nan");
    match o {
      f: Float {
        println(Str<bool>(f == f) + " " + Str<bool>(f != f));
      }
      _ { println("no nan"); }
    }
    // A float value match tests each arm with ==: NaN takes the wildcard.
    match n {
      1.0 { println("one"); }
      _ { println("other"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "False True\nFalse True\nother\n");
  g.expectNoLeaks("FloatCompareNaNThroughTupleBoxedFloatAndMatch");
}

// -- Return code

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
