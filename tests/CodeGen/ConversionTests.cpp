// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// CodeGen tests: the conversion constructors `Target<Source>(value)` (#64) —
// `Str<int|float|bool|char>` and the numeric conversions between int, float,
// bool and char, with their edge cases.  The parses (`int<Str>`,
// `float<Str>`) are in BoxedPrimitiveTests.cpp; the panics (out-of-range
// `int<float>` / `char<int>`) are death tests in DriverTests.cpp.  Every test
// runs on the backend the suite is built for (llvm and c) and checks zero
// live heap blocks.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// ============================================================================
// Str<...>
// ============================================================================

TEST(Conversion, StrOfInt) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    println(Str<int>(0));
    println(Str<int>(-42));
    println(Str<int>(9223372036854775807));
    println(Str<int>(-9223372036854775807 - 1));
    x = 12;
    println("x = " + Str<int>(x * 2));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n-42\n9223372036854775807\n-9223372036854775808\n"
                      "x = 24\n");
  g.expectNoLeaks("Conversion.StrOfInt");
}

TEST(Conversion, StrOfFloatKeepsCanonicalSpellings) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    z: float = 0.0;
    println(Str<float>(3.14));
    println(Str<float>(2.0));
    println(Str<float>(-0.0));
    println(Str<float>(z / z));
    println(Str<float>(1.0 / z));
    println(Str<float>(-1.0 / z));
    println(Str<float>(1e300 * 10.0));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3.14\n2\n-0\nnan\ninf\n-inf\n1e+301\n");
  g.expectNoLeaks("Conversion.StrOfFloatKeepsCanonicalSpellings");
}

TEST(Conversion, StrOfBoolAndChar) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    t = 1 < 2;
    println(Str<bool>(True) + Str<bool>(False) + Str<bool>(t));
    println(Str<char>('a') + Str<char>('Z') + Str<char>(' ') + "|");
    s = "hey";
    println(Str<char>(s[1]));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "TrueFalseTrue\naZ |\ne\n");
  g.expectNoLeaks("Conversion.StrOfBoolAndChar");
}

// ============================================================================
// int <-> float
// ============================================================================

TEST(Conversion, IntOfFloatTruncatesTowardZero) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    println(Str<int>(int<float>(2.9)));
    println(Str<int>(int<float>(-2.9)));
    println(Str<int>(int<float>(0.5)));
    println(Str<int>(int<float>(-0.5)));
    println(Str<int>(int<float>(-0.0)));
    println(Str<int>(int<float>(1e18)));
    println(Str<int>(int<float>(-9223372036854775808.0)));
    println(Str<int>(int<float>(9223372036854774784.0)));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "2\n-2\n0\n0\n0\n1000000000000000000\n"
                      "-9223372036854775808\n9223372036854774784\n");
  g.expectNoLeaks("Conversion.IntOfFloatTruncatesTowardZero");
}

TEST(Conversion, FloatOfIntIsTheNearestDouble) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    println(Str<float>(float<int>(3)));
    println(Str<float>(float<int>(-7) / 2.0));
    big = 9007199254740993;                       // 2^53 + 1
    println(Str<bool>(float<int>(big) == 9007199254740992.0));
    println(Str<int>(int<float>(float<int>(-9223372036854775807 - 1))));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3\n-3.5\nTrue\n-9223372036854775808\n");
  g.expectNoLeaks("Conversion.FloatOfIntIsTheNearestDouble");
}

// ============================================================================
// int <-> bool
// ============================================================================

TEST(Conversion, IntOfBoolAndBoolOfInt) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    println(Str<int>(int<bool>(True)) + Str<int>(int<bool>(False)));
    n = 3;
    println(Str<int>(int<bool>(n > 2) + int<bool>(n > 5)));
    println(Str<bool>(bool<int>(0)));
    println(Str<bool>(bool<int>(1)));
    println(Str<bool>(bool<int>(-9223372036854775807 - 1)));
    if (bool<int>(n)) { println("nonzero"); }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "10\n1\nFalse\nTrue\nTrue\nnonzero\n");
  g.expectNoLeaks("Conversion.IntOfBoolAndBoolOfInt");
}

// ============================================================================
// int <-> char
// ============================================================================

TEST(Conversion, IntOfCharIsTheByteCode) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    println(Str<int>(int<char>('a')));
    println(Str<int>(int<char>('0')));
    println(Str<int>(int<char>('\n')));
    s = "é";                                     // UTF-8: 0xC3 0xA9
    println(Str<int>(int<char>(s[0])) + " " + Str<int>(int<char>(s[1])));
    d = int<char>('7') - int<char>('0');
    println(Str<int>(d));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "97\n48\n10\n195 169\n7\n");
  g.expectNoLeaks("Conversion.IntOfCharIsTheByteCode");
}

TEST(Conversion, CharOfIntRoundTripsEveryCode) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    println(Str<char>(char<int>(98)));
    println(Str<char>(char<int>(int<char>('a') + 25)));
    ok = True;
    i = 0;
    while (i < 256) {
      if (int<char>(char<int>(i)) != i) { ok = False; }
      i = i + 1;
    }
    println(Str<bool>(ok));
    s = "é";
    println(Str<bool>(char<int>(195) == s[0]));
    println(Str<char>(char<int>(195)) + Str<char>(char<int>(169)));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "b\nz\nTrue\nTrue\né\n");
  g.expectNoLeaks("Conversion.CharOfIntRoundTripsEveryCode");
}

// ============================================================================
// Conversions as ordinary expressions
// ============================================================================

TEST(Conversion, NestedAndInsideOtherExpressions) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Box<T> { v: T; fn __init__(x: T) { self.v = x; } }
    fn twice(n: int) -> int { return n * 2; }
    fn main() -> int {
      b = Box<int>(int<float>(4.75));          // a real generic class still works
      println(Str<int>(b.v));
      println(Str<float>(float<int>(twice(int<bool>(True))) + 0.5));
      xs: int[] = [int<char>('a'), int<float>(2.5), int<bool>(False)];
      println(Str<int>(xs[0] + xs[1] + xs[2]));
      t = (Str<int>(1), char<int>(65));
      println(t.0 + Str<char>(t.1));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "4\n2.5\n99\n1A\n");
  g.expectNoLeaks("Conversion.NestedAndInsideOtherExpressions");
}
