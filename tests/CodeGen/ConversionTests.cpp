// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// CodeGen tests: the conversion constructors `Target<Source>(value)` (#64) —
// `Str<int|float|bool|char>` and the numeric conversions between int, float,
// bool and char, with their edge cases; every pair of the families
// (docs/language/04-strings-and-conversions.md, "Between numbers"): a `bool`
// and a `char` to and from every integer type, identity, an `unsigned`'s
// `toInt()`, and an `f32` of a wide integer rounded once — and Str's
// `toBool()` and the parse of a conversion's text (#88).  The other parses
// (`toInt()`, `toFloat()`) are in ParseTests.cpp; the panics (out-of-range
// `int<float>` / `char<S>`) are death tests in DriverTests.cpp.  Every test
// runs on the backend the suite is built for (llvm and c) and checks zero
// live heap blocks.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>
#include <iterator>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "import ::io; fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// -- Str<...>

TEST(Conversion, StrOfInt) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    io::println(Str<int>(0));
    io::println(Str<int>(-42));
    io::println(Str<int>(9223372036854775807));
    io::println(Str<int>(-9223372036854775807 - 1));
    x = 12;
    io::println("x = " + Str<int>(x * 2));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n-42\n9223372036854775807\n-9223372036854775808\n"
                      "x = 24\n");
  g.expectNoLeaks("Conversion.StrOfInt");
}

// `Str(s)` with a `Str` argument is the `Str` constructor and returns the
// string: the fresh box of a method call, a conversion or a concatenation
// is passed through and released once, like the argument itself would be.
TEST(Conversion, StrOfAFreshStringIsTheString) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    y: float = 0.5;
    io::println(Str(y.toString()));
    io::println(Str(Str(3)));
    io::println(Str("a" + "b"));
    t = (1, "one");
    io::println(Str(t.toString()));
    s = Str(y.toString()) + Str(Str<int>(4));
    io::println(s);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0.5\n3\nab\n(1, one)\n0.54\n");
  g.expectNoLeaks("Conversion.StrOfAFreshStringIsTheString");
}

TEST(Conversion, StrOfFloatKeepsCanonicalSpellings) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    z: float = 0.0;
    io::println(Str<float>(3.14));
    io::println(Str<float>(2.0));
    io::println(Str<float>(-0.0));
    io::println(Str<float>(z / z));
    io::println(Str<float>(1.0 / z));
    io::println(Str<float>(-1.0 / z));
    io::println(Str<float>(1e300 * 10.0));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3.14\n2\n-0\nnan\ninf\n-inf\n1e+301\n");
  g.expectNoLeaks("Conversion.StrOfFloatKeepsCanonicalSpellings");
}

TEST(Conversion, StrOfBoolAndChar) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    t = 1 < 2;
    io::println(Str<bool>(True) + Str<bool>(False) + Str<bool>(t));
    io::println(Str<char>('a') + Str<char>('Z') + Str<char>(' ') + "|");
    s = "hey";
    io::println(Str<char>(s[1]));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "TrueFalseTrue\naZ |\ne\n");
  g.expectNoLeaks("Conversion.StrOfBoolAndChar");
}

// -- int <-> float

TEST(Conversion, IntOfFloatTruncatesTowardZero) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    io::println(Str<int>(int<float>(2.9)));
    io::println(Str<int>(int<float>(-2.9)));
    io::println(Str<int>(int<float>(0.5)));
    io::println(Str<int>(int<float>(-0.5)));
    io::println(Str<int>(int<float>(-0.0)));
    io::println(Str<int>(int<float>(1e18)));
    io::println(Str<int>(int<float>(-9223372036854775808.0)));
    io::println(Str<int>(int<float>(9223372036854774784.0)));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "2\n-2\n0\n0\n0\n1000000000000000000\n"
                      "-9223372036854775808\n9223372036854774784\n");
  g.expectNoLeaks("Conversion.IntOfFloatTruncatesTowardZero");
}

TEST(Conversion, FloatOfIntIsTheNearestDouble) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    io::println(Str<float>(float<int>(3)));
    io::println(Str<float>(float<int>(-7) / 2.0));
    big = 9007199254740993;                       // 2^53 + 1
    io::println(Str<bool>(float<int>(big) == 9007199254740992.0));
    io::println(Str<int>(int<float>(float<int>(-9223372036854775807 - 1))));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3\n-3.5\nTrue\n-9223372036854775808\n");
  g.expectNoLeaks("Conversion.FloatOfIntIsTheNearestDouble");
}

// -- int <-> bool

TEST(Conversion, IntOfBoolAndBoolOfInt) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    io::println(Str<int>(int<bool>(True)) + Str<int>(int<bool>(False)));
    n = 3;
    io::println(Str<int>(int<bool>(n > 2) + int<bool>(n > 5)));
    io::println(Str<bool>(bool<int>(0)));
    io::println(Str<bool>(bool<int>(1)));
    io::println(Str<bool>(bool<int>(-9223372036854775807 - 1)));
    if bool<int>(n) { io::println("nonzero"); }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "10\n1\nFalse\nTrue\nTrue\nnonzero\n");
  g.expectNoLeaks("Conversion.IntOfBoolAndBoolOfInt");
}

// -- int <-> char

TEST(Conversion, IntOfCharIsTheByteCode) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    io::println(Str<int>(int<char>('a')));
    io::println(Str<int>(int<char>('0')));
    io::println(Str<int>(int<char>('\n')));
    s = "é";                                     // UTF-8: 0xC3 0xA9
    io::println(Str<int>(int<char>(s[0])) + " " + Str<int>(int<char>(s[1])));
    d = int<char>('7') - int<char>('0');
    io::println(Str<int>(d));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "97\n48\n10\n195 169\n7\n");
  g.expectNoLeaks("Conversion.IntOfCharIsTheByteCode");
}

TEST(Conversion, CharOfIntRoundTripsEveryCode) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    io::println(Str<char>(char<int>(98)));
    io::println(Str<char>(char<int>(int<char>('a') + 25)));
    ok = True;
    i = 0;
    while i < 256 {
      if int<char>(char<int>(i)) != i { ok = False; }
      i = i + 1;
    }
    io::println(Str<bool>(ok));
    s = "é";
    io::println(Str<bool>(char<int>(195) == s[0]));
    io::println(Str<char>(char<int>(195)) + Str<char>(char<int>(169)));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "b\nz\nTrue\nTrue\né\n");
  g.expectNoLeaks("Conversion.CharOfIntRoundTripsEveryCode");
}

// -- Conversions as ordinary expressions

TEST(Conversion, NestedAndInsideOtherExpressions) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Box<T> { v: T; }
    impl Box<T> { fn __init__(x: T) { self.v = x; } }
    fn twice(n: int) -> int { return n * 2; }
    fn main() -> int {
      b = Box<int>(int<float>(4.75));          // a real generic class still works
      io::println(Str<int>(b.v));
      io::println(Str<float>(float<int>(twice(int<bool>(True))) + 0.5));
      xs: int[3] = [int<char>('a'), int<float>(2.5), int<bool>(False)];
      io::println(Str<int>(xs[0] + xs[1] + xs[2]));
      t = (Str<int>(1), char<int>(65));
      io::println(t.0 + Str<char>(t.1));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "4\n2.5\n99\n1A\n");
  g.expectNoLeaks("Conversion.NestedAndInsideOtherExpressions");
}

// -- toBool() and the round trips through a text (#88)

TEST(Conversion, ToBoolParsesExactlyTrueAndFalse) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn show(s: Str) -> Str {
      match s.toBool() {
        b: bool { if b { return "yes"; } return "no"; }
        None { return "None"; }
      }
    }
    fn main() -> int {
      io::println(show("True") + " " + show("False"));
      io::println(show("true") + " " + show("FALSE") + " " + show("1") + " " +
              show("") + " " + show(" True") + " " + show("False "));
      // The round trip with Str<bool>.
      t = 1 < 2;
      match Str<bool>(t).toBool() { b: bool { io::println(Str<bool>(b == t)); } None { } }
      match Str<bool>(!t).toBool() { b: bool { io::println(Str<bool>(b == !t)); } None { } }
      io::println("True".toBool());
      io::println("maybe".toBool());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "yes no\nNone None None None None None\nTrue\nTrue\n"
                      "True\nNone\n");
  g.expectNoLeaks("Conversion.ToBoolParsesExactlyTrueAndFalse");
}

TEST(Conversion, ParseRoundTrips) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    inputs: Str[4] = ["0", "-7", "123456789", "x"];
    i = 0;
    while i < inputs.len() {
      match inputs[i].toInt() {
        n: int { io::println(Str<bool>(Str<int>(n) == inputs[i])); }
        None { io::println("None"); }
      }
      i = i + 1;
    }
    match Str<float>(0.1).toFloat() {
      f: float { io::println(Str<float>(f)); }
      None { }
    }
    match Str<bool>(False).toBool() {
      b: bool { io::println(Str<bool>(b)); }
      None { }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\nTrue\nTrue\nNone\n0.1\nFalse\n");
  g.expectNoLeaks("Conversion.ParseRoundTrips");
}

// Every parse of the same text, and a parse's optional kept and printed.
TEST(Conversion, EachParseOfOneText) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn show(s: Str) -> Str {
      out = "";
      match s.toInt() { n: int { out = out + "int " + Str(n); } None { out = out + "-"; } }
      match s.toFloat() { f: float { out = out + " float " + Str(f); } None { out = out + " -"; } }
      match s.toBool() { b: bool { out = out + " bool " + Str(b); } None { out = out + " -"; } }
      return out;
    }
    fn main() -> int {
      io::println(show("12"));
      io::println(show("1.5"));
      io::println(show("True"));
      io::println(show("nope"));
      io::println("x".toInt());
      kept: int? = "7".toInt();
      io::println(kept);
      io::println("1e999".toFloat());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "int 12 float 12 -\n- float 1.5 -\n"
                      "- - bool True\n- - -\nNone\n7\nNone\n");
  g.expectNoLeaks("Conversion.EachParseOfOneText");
}

// -- The inferred form Target(value) (#88)

TEST(Conversion, InferredFormMatchesTheExplicitOne) {
  // Each line prints the inferred and the explicit form side by side.
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    i = -42;  f = 2.5;  b = 1 < 2;  c = 'q';
    io::println(Str(i) + "|" + Str<int>(i));
    io::println(Str(f) + "|" + Str<float>(f));
    io::println(Str(b) + "|" + Str<bool>(b));
    io::println(Str(c) + "|" + Str<char>(c));
    io::println(Str(1) + Str(0.5) + Str(False) + Str('!'));
    io::println(Str(int(f)) + " " + Str(float(i) / 4.0) + " " + Str(int(b)) +
            " " + Str(bool(0)) + " " + Str(int(c)) + " " + Str(char(65)));
    s = "Hello";
    t: Str = Str(s);
    io::println(t);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "-42|-42\n2.5|2.5\nTrue|True\nq|q\n10.5False!\n"
                      "2 -10.5 1 False 113 A\nHello\n");
  g.expectNoLeaks("Conversion.InferredFormMatchesTheExplicitOne");
}

// -- The families (docs/language/04-strings-and-conversions.md, "Between
// -- numbers")

// Every pair the families have, inferred, on one value of each source: none
// is left unlowered.  The table is tests/Sema/ConversionTests.cpp's ('.' a
// conversion).
TEST(Conversion, EveryPairOfTheFamiliesRuns) {
  struct Primitive {
    const char *Name;
    const char *Value;
  };
  static constexpr Primitive kPrimitives[] = {
      {"int", "65"},      {"float", "65.5"}, {"i8", "65"},    {"i16", "65"},
      {"i32", "65"},      {"u8", "65"},      {"u16", "65"},   {"u32", "65"},
      {"unsigned", "65"}, {"f16", "65.5"},   {"f32", "65.5"}, {"bool", "True"},
      {"char", "'A'"}};
  static constexpr const char *kFamilies[] = {
      "........2....", "...........44", "........2....", "........2....",
      "........2....", ".3.......33..", ".3.......33..", ".3.......33..",
      ".3.......33..", "...........44", "...........44", ".0.......00.0",
      ".0.......000.",
  };
  auto isFloat = [](const std::string &t) {
    return t == "float" || t == "f16" || t == "f32";
  };
  std::string body, expected;
  for (size_t s = 0; s < std::size(kPrimitives); ++s)
    body += "v" + std::to_string(s) + ": " + kPrimitives[s].Name + " = " +
            kPrimitives[s].Value + ";\n";
  for (size_t t = 0; t < std::size(kPrimitives); ++t) {
    const std::string target = kPrimitives[t].Name;
    for (size_t s = 0; s < std::size(kPrimitives); ++s) {
      if (kFamilies[t][s] != '.')
        continue;
      const std::string source = kPrimitives[s].Name;
      body += "io::println(Str(" + target + "(v" + std::to_string(s) + ")));\n";
      if (target == "bool")
        expected += "True\n";
      else if (target == "char")
        expected += "A\n";
      else if (source == "bool")
        expected += "1\n";
      else if (isFloat(target) && isFloat(source))
        expected += "65.5\n";
      else
        expected += "65\n";
    }
  }
  LeakGuard g;
  auto r = compileAndRun(wrapMain(body));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, expected);
  g.expectNoLeaks("Conversion.EveryPairOfTheFamiliesRuns");
}

TEST(Conversion, BoolAndCharToAndFromEveryIntegerType) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    io::println(Str(u8('a')) + " " + Str(i8(char(195))) + " " + Str(u8(char(195))));
    io::println(Str(i16(char(255))) + " " + Str(unsigned('A')) + " " + Str(u32('z')));
    io::println(Str(unsigned(True)) + " " + Str(u16(False)) + " " + Str(i8(True)));
    io::println(Str(bool(u8(3))) + " " + Str(bool(unsigned(0))) + " " +
        Str(bool(unsigned(-1))) + " " + Str(bool(i8(-128))));
    io::println(Str(char(u8(65))) + Str(char(i32(66))) + Str(char(unsigned(67))) +
        Str(char(i8(68))) + Str(char(u16(69))));
    y: u8 = 200;
    io::println(Str(int(char(y))) + " " + Str(u8(char(y)) == y));
    s = "é";
    io::println(Str(char(u8(195)) == s[0]) + " " + Str(i8(s[0])));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "97 -61 195\n255 65 122\n1 0 1\n"
                      "True False True True\nABCDE\n200 True\nTrue -61\n");
  g.expectNoLeaks("Conversion.BoolAndCharToAndFromEveryIntegerType");
}

TEST(Conversion, IdentityIsTheValue) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    b: u8 = 255;  n = -7;  k: f32 = 0.1;  h: f16 = 0.1;  u = unsigned(-1);
    io::println(Str(int(5)) + " " + Str(float(2.5)) + " " + Str(bool(True)) +
        " " + Str(char('x')) + " " + Str(u8(b)) + " " + Str(i64(n)));
    io::println(Str(f32(k) == k) + " " + Str(f16(h)) + " " + Str(u64(u)) +
        " " + Str(int<int>(n)) + " " + Str(f64<float>(0.5)));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "5 2.5 True x 255 -7\n"
                      "True 0.0999756 18446744073709551615 -7 0.5\n");
  g.expectNoLeaks("Conversion.IdentityIsTheValue");
}

TEST(Conversion, AnUnsignedsToIntIsItsValueOrNone) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    io::println(unsigned(7).toInt());
    io::println(unsigned(-1).toInt());
    m: unsigned = 9223372036854775807;
    io::println(m.toInt());
    io::println((m + 1).toInt());
    match unsigned(41).toInt() {
      n: int { io::println(Str(n + 1)); }
      None { io::println("none"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "7\nNone\n9223372036854775807\nNone\n42\n");
  g.expectNoLeaks("Conversion.AnUnsignedsToIntIsItsValueOrNone");
}

// `f32(n)` of an `int` or an `unsigned` is the f32 nearest n, rounded once
// (new-F3c-2), for a variable, a constant and an integer literal in an f32
// slot alike: 2^60 + 2^36 + 1 is 2^60 + 2^37, where the f64 (2^60 + 2^36)
// rounded again gives 2^60; 2^63 + 2^39 + 1 is 2^63 + 2^40, not 2^63.
TEST(Conversion, F32OfAWideIntegerIsRoundedOnce) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    n = 1152921573326323713;
    x: f32 = 1152921573326323713;
    io::println(Str(int(f32(n))) + " " + Str(int(f32(1152921573326323713))) +
        " " + Str(int(x)));
    io::println(Str(int(f32(-n))) + " " + Str(int(f32(float(n)))));
    u = unsigned(-9223371487098961919);
    io::println(Str(f32(u) == f32(9223373136366403584.0)) + " " +
        Str(f32(unsigned(-9223371487098961919)) == f32(9223373136366403584.0)));
    io::println(Str(f32(unsigned(-1)) == f32(18446744073709551616.0)) + " " +
        Str(int(f32(i32(16777217)))));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1152921642045800448 1152921642045800448 "
                      "1152921642045800448\n"
                      "-1152921642045800448 1152921504606846976\n"
                      "True True\nTrue 16777216\n");
  g.expectNoLeaks("Conversion.F32OfAWideIntegerIsRoundedOnce");
}
