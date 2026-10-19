// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// CodeGen tests: the sized numeric types (docs/design/sized-types.md) —
// arithmetic wrapping to the width, unsigned division, remainder and
// ordering, f32 / f16 rounding after each operation, the conversions
// between every numeric pair (the low bits, truncation with the range
// guard, rounding), printing and parsing, hashing, and sized values in
// structs, tuples, arrays, optionals, generics and match.  Every test runs
// on the backend the suite is built for (llvm and c) and checks zero live
// heap blocks.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "import ::io; fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

TEST(SizedTypes, IntegerArithmeticWrapsToTheWidth) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    a: u8 = 250;  io::println(Str(a + 10));          // 4
    b: u8 = 3;    io::println(Str(b - 5));           // 254
    c: i8 = -128; io::println(Str(c - 1));           // 127
    d: i8 = 127;  io::println(Str(d + 1));           // -128
    e: i16 = 30000; io::println(Str(e * 2));         // -5536
    f: u16 = 65535; io::println(Str(f * f));         // 1
    g: u32 = 4000000000; io::println(Str(g + 1000000000)); // 705032704
    h: i32 = 2147483647; io::println(Str(h + 1));    // -2147483648
    n: i8 = -128; io::println(Str(-n));              // -128
    io::println(Str(n / -1));                        // -128: no overflow panic
    io::println(Str(n % -1));                        // 0
    u: u8 = 200; io::println(Str(u * 2 / 3));        // 144 (400 wraps to 144 first)
    v: u8 = 7; io::println(Str(v / 2) + " " + Str(v % 4));  // 3 3
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "4\n254\n127\n-128\n-5536\n1\n705032704\n-2147483648\n"
                      "-128\n-128\n0\n48\n3 3\n");
  g.expectNoLeaks("SizedTypes.IntegerArithmeticWrapsToTheWidth");
}

TEST(SizedTypes, UnsignedIsReadUnsigned) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    u: unsigned = unsigned(-1);
    io::println(Str(u));                     // 18446744073709551615
    io::println(Str(u / 2));                 // 9223372036854775807
    io::println(Str(u % 10));                // 5
    io::println(Str(u > 1) + " " + Str(u < 1) + " " + Str(u >= u) + " " + Str(1 <= u));
    io::println(Str(u == unsigned(-1)) + " " + Str(u != 0));
    io::println(Str(int(u)));                // -1
    io::println(Str(float(u)));              // 1.84467e+19
    io::println(Str(u + 2));                 // 1 (wraps)
    v: unsigned = 9223372036854775807;
    io::println(Str(v + v + 1));             // 18446744073709551615
    io::println(Str(unsigned(1.5)) + " " + Str(unsigned(18446744073709549568.0)));
    io::println(u.toString());
    o: unsigned? = u;
    io::println(o);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "18446744073709551615\n9223372036854775807\n5\n"
                      "True False True True\nTrue True\n-1\n1.84467e+19\n1\n"
                      "18446744073709551615\n1 18446744073709549568\n"
                      "18446744073709551615\n18446744073709551615\n");
  g.expectNoLeaks("SizedTypes.UnsignedIsReadUnsigned");
}

TEST(SizedTypes, NarrowFloatsRoundAfterEachOperation) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    x: f32 = 0.1;
    io::println(Str(x));                       // 0.1
    io::println(Str(float(x)));                // 0.1 (%g shows six digits)
    io::println(Str(float(x) == 0.1));         // False: the f32 is not the f64
    y: f32 = 16777216.0;
    io::println(Str(y + 1));                   // 1.67772e+07: the f32 cannot hold 16777217
    io::println(Str(y + 1 == y));              // True
    z: f16 = 0.1;
    io::println(Str(z));                       // 0.0999756
    w: f16 = 2048.0;
    io::println(Str(w + 1));                   // 2048 (the ulp is 2)
    io::println(Str(w + 2));                   // 2050
    io::println(Str(f16(65519.0)) + " " + Str(f16(65520.0)) + " " + Str(f16(-70000.0)));
    io::println(Str(f16(0.00000006)));         // 5.96046e-08: the smallest subnormal
    io::println(Str(f16(0.00000002)));         // 0
    io::println(Str(f32(16777217)));           // 1.67772e+07
    n: f16 = -1.5;
    io::println(Str(-n) + " " + Str(n * n));   // 1.5 2.25
    i: f32 = 3;
    io::println(Str(i));                       // 3
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0.1\n0.1\nFalse\n1.67772e+07\nTrue\n0.0999756\n2048\n"
                      "2050\n65504 inf -inf\n5.96046e-08\n0\n1.67772e+07\n"
                      "1.5 2.25\n3\n");
  g.expectNoLeaks("SizedTypes.NarrowFloatsRoundAfterEachOperation");
}

TEST(SizedTypes, ConversionsBetweenIntegersKeepTheLowBits) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    io::println(Str(u8(300)) + " " + Str(u8(-1)) + " " + Str(i8(200)) + " " + Str(i8(-129)));
    io::println(Str(u16(65536)) + " " + Str(i16(32768)) + " " + Str(u32(-1)) + " " + Str(i32(2147483648)));
    io::println(Str(unsigned(-1)) + " " + Str(int(unsigned(-1))));
    a: i8 = -1;
    io::println(Str(u8(a)) + " " + Str(u16(a)) + " " + Str(u32(a)) + " " + Str(unsigned(a)) + " " + Str(int(a)));
    b: u8 = 255;
    io::println(Str(i8(b)) + " " + Str(i16(b)) + " " + Str(int(b)) + " " + Str(unsigned(b)));
    c: u32 = 4294967295;
    io::println(Str(i32(c)) + " " + Str(u16(c)) + " " + Str(int(c)));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "44 255 -56 127\n0 -32768 4294967295 -2147483648\n"
                      "18446744073709551615 -1\n"
                      "255 65535 4294967295 18446744073709551615 -1\n"
                      "-1 255 255 255\n-1 65535 4294967295\n");
  g.expectNoLeaks("SizedTypes.ConversionsBetweenIntegersKeepTheLowBits");
}

TEST(SizedTypes, ConversionsWithFloatsTruncateAndRound) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    io::println(Str(i8(-128.9)) + " " + Str(i8(127.9)) + " " + Str(u8(255.5)) + " " + Str(u8(0.5)));
    io::println(Str(i32(-1.5)) + " " + Str(u16(65535.9)) + " " + Str(int(-2.9)));
    f: f32 = 100.75;
    io::println(Str(i8(f)) + " " + Str(u8(f)) + " " + Str(int(f)));
    h: f16 = -3.5;
    io::println(Str(i16(h)) + " " + Str(int(h)));
    io::println(Str(f32(16777217)) + " " + Str(f16(2049)) + " " + Str(float(i8(-5))));
    a: u8 = 200;  b: i32 = -7;  u: unsigned = 3;
    io::println(Str(float(a)) + " " + Str(f32(b)) + " " + Str(f16(u)) + " " + Str(float(u)));
    io::println(Str(f16(f)) + " " + Str(f32(h)) + " " + Str(float(f32(0.1))));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "-128 127 255 0\n-1 65535 -2\n100 100 100\n-3 -3\n"
                      "1.67772e+07 2048 -5\n200 -7 3 3\n100.75 -3.5 0.1\n");
  g.expectNoLeaks("SizedTypes.ConversionsWithFloatsTruncateAndRound");
}

TEST(SizedTypes, PrintingAndParsing) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    a: i8 = -5;  b: u16 = 65535;  c: f32 = 0.25;  d: f16 = 0.5;  u: unsigned = 7;
    io::println(Str(a) + " " + Str(b) + " " + Str(c) + " " + Str(d) + " " + Str(u));
    io::println(a.toString() + " " + Str<u16>(b) + " " + c.toString() + " " + Str<unsigned>(u));
    p: u8? = u8("255");  q: u8? = u8("256");  r: i8? = i8("-128");  s: i8? = i8("-129");
    io::print(p); io::print(" "); io::print(q); io::print(" "); io::print(r); io::print(" "); io::println(s);
    t: unsigned? = unsigned("18446744073709551615");  v: unsigned? = unsigned("18446744073709551616");
    w: unsigned? = unsigned("-1");  x: unsigned? = unsigned("+1");  y: unsigned? = unsigned("");
    io::print(t); io::print(" "); io::print(v); io::print(" "); io::print(w); io::print(" "); io::print(x); io::print(" "); io::println(y);
    z: f32? = f32("0.1");  zz: f16? = f16("70000");  zi: i32? = i32(" 5");
    io::print(z); io::print(" "); io::print(zz); io::print(" "); io::println(zi);
    match u16("65535") { n: u16 { io::println(Str(n + 1)); } None { io::println("none"); } }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "-5 65535 0.25 0.5 7\n-5 65535 0.25 7\n"
                      "255 None -128 None\n"
                      "18446744073709551615 None None None None\n"
                      "0.1 inf None\n0\n");
  g.expectNoLeaks("SizedTypes.PrintingAndParsing");
}

TEST(SizedTypes, HashingAndEquality) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    a: u8 = 250;  b: u8 = 250;  c: i32 = -1;  d: int = -1;  u: unsigned = unsigned(-1);
    io::println(Str(a.hash() == b.hash()) + " " + Str(c.hash() == d.hash()) + " " + Str(u.hash() == d.hash()));
    io::println(Str(a == b) + " " + Str(a != b) + " " + Str(c == -1));
    o: u8? = 250;  p: u8? = None;
    io::println(Str(o == p) + " " + Str(p == None) + " " + Str(o != None));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True True True\nTrue False True\nFalse True True\n");
  g.expectNoLeaks("SizedTypes.HashingAndEquality");
}

TEST(SizedTypes, SizedValuesInAggregatesGenericsAndMatch) {
  LeakGuard g;
  auto r =
      compileAndRun("struct Pixel { r: u8; g: u8; b: u8; a: f32; }\n"
                    "class Box<T> { v: T; }\n"
                    "impl Box<T> { fn __init__(v: T) { self.v = v; } "
                    "view fn get() -> T { return self.v; } }\n"
                    "fn sum(xs: view u16[3]) -> u16 { t: u16 = 0; i = 0; "
                    "while i < 3 { t = t + xs[i]; i = i + 1; } return t; }\n"
                    "fn bump(n: inout u8) { n = n + 1; }\n" +
                    wrapMain(R"(
    p = Pixel(255, 128, 0, 0.5);
    io::println(p.toString() + " " + Str(p == Pixel(255, 128, 0, 0.5)));
    p.r = p.r + 1;
    io::println(Str(p.r));
    t = (unsigned(-1), i8(-5), f16(0.1));
    io::println(t.toString() + " " + Str(t == (unsigned(-1), i8(-5), f16(0.1))));
    xs: u16[3] = [65535, 1, 2];
    io::println(Str(sum(xs)));
    ys: f16[2] = [0.1, 0.2];
    io::println(Str(ys[0] + ys[1]));
    b = Box<u8>(200);
    io::println(Str(b.get() + 100));
    d = Box<unsigned>(unsigned(-1));
    io::println(Str(d.get()));
    n: u8 = 255;
    bump(n);
    io::println(Str(n));
    k: u32 = 7;
    match k { 7 { io::println("seven"); } _ { io::println("other"); } }
    m: i8 = -1;
    match -m { 1 { io::println("minus one"); } _ { io::println("other"); } }
    f: f32 = 0.5;
    match f { 0.5 { io::println("half"); } _ { io::println("other"); } }
    o: i16? = None;
    io::print(o); io::print(" "); o = -300; io::println(o);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Pixel(r: 255, g: 128, b: 0, a: 0.5) True\n0\n"
                      "(18446744073709551615, -5, 0.0999756) True\n2\n"
                      "0.299805\n44\n18446744073709551615\n0\nseven\n"
                      "minus one\nhalf\nNone -300\n");
  g.expectNoLeaks("SizedTypes.SizedValuesInAggregatesGenericsAndMatch");
}
