// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// CodeGen tests: the conversion constructors `Target<Source>(value)` (#64) —
// `Str<int|float|bool|char>` and the numeric conversions between int, float,
// bool and char, with their edge cases — and the boxed forms and `bool<Str>`
// (#88).  The parses (`int<Str>`,
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

// ============================================================================
// The boxed forms and bool<Str> (#88)
// ============================================================================

TEST(Conversion, StrOfBoxedSourcesFormatsLikeThePrimitive) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn boxed(o: Obj) -> Str {
      match o {
        x: Int { return "Int " + Str<Int>(x); }
        x: Float { return "Float " + Str<Float>(x); }
        x: Bool { return "Bool " + Str<Bool>(x); }
        x: Char { return "Char " + Str<Char>(x) + "|"; }
        _ { return "other"; }
      }
    }
    fn main() -> int {
      i: int? = -42;      println(boxed(i));
      f: float? = 2.0;    println(boxed(f));
      g: float? = 0.1;    println(boxed(g));
      t: bool? = True;    println(boxed(t));
      u: bool? = False;   println(boxed(u));
      c: char? = 'z';     println(boxed(c));
      sp: char? = ' ';    println(boxed(sp));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Int -42\nFloat 2\nFloat 0.1\nBool True\nBool False\n"
                      "Char z|\nChar  |\n");
  g.expectNoLeaks("Conversion.StrOfBoxedSourcesFormatsLikeThePrimitive");
}

TEST(Conversion, StrOfAnOwnedBoxReleasesIt) {
  // The boxed argument is a fresh temporary (a call's result, a parse's
  // unwrapped box): it is released after the formatting.
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn parsed(s: Str) -> Int {
      match Int<Str>(s) { n: Int { return n; } None { return parsed("0"); } }
    }
    fn main() -> int {
      println(Str<Int>(parsed("17")) + Str<Int>(parsed("x")));
      xs: Str[] = [];
      xs.push(Str<Int>(parsed("5")));
      println(xs[0]);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "170\n5\n");
  g.expectNoLeaks("Conversion.StrOfAnOwnedBoxReleasesIt");
}

TEST(Conversion, BoxedParsesGiveTheOptionalBox) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    match Int<Str>("-12") { n: Int { println(n); } None { println("None"); } }
    match Int<Str>("1.5") { n: Int { println(n); } None { println("None"); } }
    match Float<Str>("2.5e1") { f: Float { println(f); } None { println("None"); } }
    match Float<Str>("") { f: Float { println(f); } None { println("None"); } }
    match Bool<Str>("True") { b: Bool { println(b); } None { println("None"); } }
    match Bool<Str>("false") { b: Bool { println(b); } None { println("None"); } }
    println(Int<Str>("9223372036854775808"));
    println(Float<Str>(" 1"));
    println(Bool<Str>("False"));
    kept: Int? = Int<Str>("8");
    println(kept);
    kept = Int<Str>("nope");
    println(kept);
    println(Str<bool>(Int<Str>("3") == None) + Str<bool>(Bool<Str>("") == None));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "-12\nNone\n25\nNone\nTrue\nNone\nNone\nNone\nFalse\n"
                      "8\nNone\nFalseTrue\n");
  g.expectNoLeaks("Conversion.BoxedParsesGiveTheOptionalBox");
}

TEST(Conversion, BoolOfStrParsesExactlyTrueAndFalse) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn show(s: Str) -> Str {
      match bool<Str>(s) {
        b: bool { if (b) { return "yes"; } return "no"; }
        None { return "None"; }
      }
    }
    fn main() -> int {
      println(show("True") + " " + show("False"));
      println(show("true") + " " + show("FALSE") + " " + show("1") + " " +
              show("") + " " + show(" True") + " " + show("False "));
      // The round trip with Str<bool>.
      t = 1 < 2;
      match bool<Str>(Str<bool>(t)) { b: bool { println(Str<bool>(b == t)); } None { } }
      match bool<Str>(Str<bool>(!t)) { b: bool { println(Str<bool>(b == !t)); } None { } }
      println(bool<Str>("True"));
      println(bool<Str>("maybe"));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "yes no\nNone None None None None None\nTrue\nTrue\n"
                      "True\nNone\n");
  g.expectNoLeaks("Conversion.BoolOfStrParsesExactlyTrueAndFalse");
}

TEST(Conversion, BoxedRoundTrips) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    inputs: Str[] = ["0", "-7", "123456789", "x"];
    i = 0;
    while (i < inputs.len()) {
      match Int<Str>(inputs[i]) {
        n: Int { println(Str<bool>(Str<Int>(n) == inputs[i])); }
        None { println("None"); }
      }
      i = i + 1;
    }
    match Float<Str>(Str<float>(0.1)) {
      f: Float { println(Str<Float>(f)); }
      None { }
    }
    match Bool<Str>(Str<bool>(False)) {
      b: Bool { println(Str<Bool>(b)); }
      None { }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\nTrue\nTrue\nNone\n0.1\nFalse\n");
  g.expectNoLeaks("Conversion.BoxedRoundTrips");
}

// ============================================================================
// The inferred form Target(value) (#88)
// ============================================================================

TEST(Conversion, InferredFormMatchesTheExplicitOne) {
  // Each line prints the inferred and the explicit form side by side.
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    i = -42;  f = 2.5;  b = 1 < 2;  c = 'q';
    println(Str(i) + "|" + Str<int>(i));
    println(Str(f) + "|" + Str<float>(f));
    println(Str(b) + "|" + Str<bool>(b));
    println(Str(c) + "|" + Str<char>(c));
    println(Str(1) + Str(0.5) + Str(False) + Str('!'));
    println(Str(int(f)) + " " + Str(float(i) / 4.0) + " " + Str(int(b)) +
            " " + Str(bool(0)) + " " + Str(int(c)) + " " + Str(char(65)));
    s = "Hello";
    t: Str = Str(s);
    println(t);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "-42|-42\n2.5|2.5\nTrue|True\nq|q\n10.5False!\n"
                      "2 -10.5 1 False 113 A\nHello\n");
  g.expectNoLeaks("Conversion.InferredFormMatchesTheExplicitOne");
}

TEST(Conversion, InferredParsesAndBoxedForms) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn show(s: Str) -> Str {
      out = "";
      match int(s) { n: int { out = out + "int " + Str(n); } None { out = out + "-"; } }
      match Float(s) { f: Float { out = out + " Float " + Str(f); } None { out = out + " -"; } }
      match bool(s) { b: bool { out = out + " bool " + Str(b); } None { out = out + " -"; } }
      match Bool(s) { b: Bool { out = out + " Bool " + Str(b); } None { out = out + " -"; } }
      return out;
    }
    fn main() -> int {
      println(show("12"));
      println(show("1.5"));
      println(show("True"));
      println(show("nope"));
      println(Int("x"));
      kept: Int? = Int("7");
      println(kept);
      println(float("1e999"));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "int 12 Float 12 - -\n- Float 1.5 - -\n"
                      "- - bool True Bool True\n- - - -\nNone\n7\nNone\n");
  g.expectNoLeaks("Conversion.InferredParsesAndBoxedForms");
}
