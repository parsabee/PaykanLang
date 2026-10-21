// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// CodeGen tests: the fallible parses `int<Str>(s)` -> `int?` and
// `float<Str>(s)` -> `float?` (#64).

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "import ::io; fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

static const char *const kShowInt = R"(
fn show(o: int?) -> Str {
  match o {
    n: int { return Str<int>(n); }
    None   { return "None"; }
  }
}
)";

static const char *const kShowFloat = R"(
fn show(o: float?) -> Str {
  match o {
    f: float { return Str<float>(f); }
    None     { return "None"; }
  }
}
)";

// -- int<Str>

TEST(ParseInt, ValidStrings) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kShowInt) + wrapMain(R"(
    io::println(show(int<Str>("0")));
    io::println(show(int<Str>("42")));
    io::println(show(int<Str>("-99")));
    io::println(show(int<Str>("+7")));
    io::println(show(int<Str>("007")));
    io::println(show(int<Str>("9223372036854775807")));
    io::println(show(int<Str>("-9223372036854775808")));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n42\n-99\n7\n7\n9223372036854775807\n"
                      "-9223372036854775808\n");
  g.expectNoLeaks("ParseInt.ValidStrings");
}

TEST(ParseInt, InvalidStringsAreNone) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kShowInt) + wrapMain(R"(
    io::println(show(int<Str>("")));
    io::println(show(int<Str>("abc")));
    io::println(show(int<Str>("12abc")));
    io::println(show(int<Str>("3.14")));
    io::println(show(int<Str>(" 12")));
    io::println(show(int<Str>("12 ")));
    io::println(show(int<Str>("-")));
    io::println(show(int<Str>("9223372036854775808")));
    io::println(show(int<Str>("-9223372036854775809")));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "None\nNone\nNone\nNone\nNone\nNone\nNone\nNone\nNone\n");
  g.expectNoLeaks("ParseInt.InvalidStringsAreNone");
}

TEST(ParseInt, ResultIsAnOrdinaryIntOptional) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    inputs: Str[5] = ["10", "bad", "20", "x", "30"];
    total: int = 0;
    valid: int = 0;
    i: int = 0;
    while i < inputs.len() {
      v = int<Str>(inputs[i]);            // int?
      if v != None { valid = valid + 1; }
      match v {
        n: int { total = total + n; }
        None   { }
      }
      i = i + 1;
    }
    io::println(Str<int>(valid) + " " + Str<int>(total));
    io::println(int<Str>("5"));
    io::println(int<Str>("five"));
    io::println(Str<bool>(int<Str>("5") == int<Str>("05")));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3 60\n5\nNone\nTrue\n");
  g.expectNoLeaks("ParseInt.ResultIsAnOrdinaryIntOptional");
}

// -- float<Str>

TEST(ParseFloat, ValidStrings) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kShowFloat) + wrapMain(R"(
    io::println(show(float<Str>("0")));
    io::println(show(float<Str>("2.5")));
    io::println(show(float<Str>("-2.718")));
    io::println(show(float<Str>("1e2")));
    io::println(show(float<Str>("-0.0")));
    io::println(show(float<Str>("nan")));
    io::println(show(float<Str>("inf")));
    io::println(show(float<Str>("-inf")));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n2.5\n-2.718\n100\n-0\nnan\ninf\n-inf\n");
  g.expectNoLeaks("ParseFloat.ValidStrings");
}

TEST(ParseFloat, InvalidStringsAreNone) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kShowFloat) + wrapMain(R"(
    io::println(show(float<Str>("")));
    io::println(show(float<Str>("xyz")));
    io::println(show(float<Str>("1.2.3")));
    io::println(show(float<Str>(" 1.5")));
    io::println(show(float<Str>("1.5 ")));
    io::println(show(float<Str>("1e999")));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "None\nNone\nNone\nNone\nNone\nNone\n");
  g.expectNoLeaks("ParseFloat.InvalidStringsAreNone");
}

// Subnormal strings parse (strtod also flags them with ERANGE); a value too
// large (rounds to +-inf) or too small (nonzero, rounds to 0) is None (#73).
// Subnormal literals compile and agree with the parse.
TEST(ParseFloat, SubnormalsParseAndOutOfRangeIsNone) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kShowFloat) + wrapMain(R"(
    io::println(show(float<Str>("1e-310")));
    io::println(show(float<Str>("5e-324")));
    io::println(show(float<Str>("-5e-324")));
    io::println(show(float<Str>("2.2250738585072014e-308")));
    io::println(show(float<Str>("0e-999")));
    io::println(show(float<Str>("1e-400")));
    io::println(show(float<Str>("-1e999")));
    x = 5e-324;
    y = 1e-310;
    io::println(Str<float>(x) + " " + Str<float>(y) + " " + Str<float>(x * 2.0));
    io::println(Str<bool>(x > 0.0) + " " + Str<bool>(x / 2.0 == 0.0));
    match float<Str>("1e-310") {
      v: float { io::println(Str<bool>(v == y)); }
      None { io::println("None"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1e-310\n4.94066e-324\n-4.94066e-324\n2.22507e-308\n0\n"
                      "None\nNone\n"
                      "4.94066e-324 1e-310 9.88131e-324\n"
                      "True True\n"
                      "True\n");
  g.expectNoLeaks("ParseFloat.SubnormalsParseAndOutOfRangeIsNone");
}
