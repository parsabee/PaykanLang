// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// CodeGen tests: the fallible parses, Str's `s.toInt()` -> `int?` & co.
// (docs/language/04-strings-and-conversions.md, "Parsing a string").

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>

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

// -- toInt()

TEST(ParseInt, ValidStrings) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kShowInt) + wrapMain(R"(
    io::println(show("0".toInt()));
    io::println(show("42".toInt()));
    io::println(show("-99".toInt()));
    io::println(show("+7".toInt()));
    io::println(show("007".toInt()));
    io::println(show("9223372036854775807".toInt()));
    io::println(show("-9223372036854775808".toInt()));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n42\n-99\n7\n7\n9223372036854775807\n"
                      "-9223372036854775808\n");
  g.expectNoLeaks("ParseInt.ValidStrings");
}

TEST(ParseInt, InvalidStringsAreNone) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kShowInt) + wrapMain(R"(
    io::println(show("".toInt()));
    io::println(show("abc".toInt()));
    io::println(show("12abc".toInt()));
    io::println(show("3.14".toInt()));
    io::println(show(" 12".toInt()));
    io::println(show("12 ".toInt()));
    io::println(show("-".toInt()));
    io::println(show("9223372036854775808".toInt()));
    io::println(show("-9223372036854775809".toInt()));
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
      v = inputs[i].toInt();            // int?
      if v != None { valid = valid + 1; }
      match v {
        n: int { total = total + n; }
        None   { }
      }
      i = i + 1;
    }
    io::println(Str<int>(valid) + " " + Str<int>(total));
    io::println("5".toInt());
    io::println("five".toInt());
    io::println(Str<bool>("5".toInt() == "05".toInt()));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3 60\n5\nNone\nTrue\n");
  g.expectNoLeaks("ParseInt.ResultIsAnOrdinaryIntOptional");
}

// -- toFloat()

TEST(ParseFloat, ValidStrings) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kShowFloat) + wrapMain(R"(
    io::println(show("0".toFloat()));
    io::println(show("2.5".toFloat()));
    io::println(show("-2.718".toFloat()));
    io::println(show("1e2".toFloat()));
    io::println(show("-0.0".toFloat()));
    io::println(show("nan".toFloat()));
    io::println(show("inf".toFloat()));
    io::println(show("-inf".toFloat()));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n2.5\n-2.718\n100\n-0\nnan\ninf\n-inf\n");
  g.expectNoLeaks("ParseFloat.ValidStrings");
}

TEST(ParseFloat, InvalidStringsAreNone) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kShowFloat) + wrapMain(R"(
    io::println(show("".toFloat()));
    io::println(show("xyz".toFloat()));
    io::println(show("1.2.3".toFloat()));
    io::println(show(" 1.5".toFloat()));
    io::println(show("1.5 ".toFloat()));
    io::println(show("1e999".toFloat()));
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
    io::println(show("1e-310".toFloat()));
    io::println(show("5e-324".toFloat()));
    io::println(show("-5e-324".toFloat()));
    io::println(show("2.2250738585072014e-308".toFloat()));
    io::println(show("0e-999".toFloat()));
    io::println(show("1e-400".toFloat()));
    io::println(show("-1e999".toFloat()));
    x = 5e-324;
    y = 1e-310;
    io::println(Str<float>(x) + " " + Str<float>(y) + " " + Str<float>(x * 2.0));
    io::println(Str<bool>(x > 0.0) + " " + Str<bool>(x / 2.0 == 0.0));
    match "1e-310".toFloat() {
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

// -- Str's parse methods

namespace {

/// A text and what printing its parse prints.
using ParseCase = std::pair<const char *, const char *>;

/// Run `io::println(<text>.<method>())` for each case of @p cases, where the
/// text is spelled as a Paykan string literal, and expect each line.
void expectParses(const char *method, const std::vector<ParseCase> &cases) {
  LeakGuard g;
  std::string body;
  std::string expected;
  for (const auto &[text, out] : cases) {
    body +=
        std::string("    io::println(\"") + text + "\"." + method + "());\n";
    expected += std::string(out) + "\n";
  }
  auto r = compileAndRun(wrapMain(body));
  ASSERT_TRUE(r.CompileOk) << method << ": " << r.StdErr;
  EXPECT_EQ(r.StdOut, expected) << method;
  g.expectNoLeaks(method);
}

/// What no integer parse accepts: whitespace around the digits, the empty
/// string, a lone sign, hexadecimal, a digit separator, a fraction and an
/// exponent.
const std::vector<ParseCase> kNoInteger = {
    {"", "None"},      {" 1", "None"},    {"1 ", "None"},  {"\\t1", "None"},
    {"1\\n", "None"},  {"+", "None"},     {"-", "None"},   {"--1", "None"},
    {"0x10", "None"},  {"1_0", "None"},   {"1.0", "None"}, {"1e3", "None"},
    {"12abc", "None"}, {"1\\02", "None"},
};

std::vector<ParseCase> with(std::vector<ParseCase> cases,
                            const std::vector<ParseCase> &more) {
  cases.insert(cases.end(), more.begin(), more.end());
  return cases;
}

} // namespace

TEST(StrParse, SignedIntegers) {
  expectParses("toInt", with({{"0", "0"},
                              {"42", "42"},
                              {"+7", "7"},
                              {"-99", "-99"},
                              {"007", "7"},
                              {"-0", "0"},
                              {"9223372036854775807", "9223372036854775807"},
                              {"-9223372036854775808", "-9223372036854775808"},
                              {"9223372036854775808", "None"},
                              {"-9223372036854775809", "None"}},
                             kNoInteger));
  expectParses("toI8", with({{"127", "127"},
                             {"-128", "-128"},
                             {"128", "None"},
                             {"-129", "None"},
                             {"+5", "5"},
                             {"-0", "0"},
                             {"0", "0"}},
                            kNoInteger));
  expectParses("toI16", with({{"32767", "32767"},
                              {"-32768", "-32768"},
                              {"32768", "None"},
                              {"-32769", "None"},
                              {"+0", "0"}},
                             kNoInteger));
  expectParses("toI32", with({{"2147483647", "2147483647"},
                              {"-2147483648", "-2147483648"},
                              {"2147483648", "None"},
                              {"-2147483649", "None"},
                              {"-1", "-1"}},
                             kNoInteger));
}

// Every unsigned width refuses a sign, `+` and `-0` included (finding 43:
// `u8`, `u16` and `u32` accepted them, `unsigned` did not).
TEST(StrParse, UnsignedIntegersRefuseEverySign) {
  const std::vector<ParseCase> signs = {
      {"+1", "None"}, {"-0", "None"}, {"+0", "None"}, {"-1", "None"}};
  expectParses("toUnsigned",
               with(with({{"0", "0"},
                          {"18446744073709551615", "18446744073709551615"},
                          {"9223372036854775808", "9223372036854775808"},
                          {"18446744073709551616", "None"}},
                         signs),
                    kNoInteger));
  expectParses("toU8", with(with({{"255", "255"},
                                  {"0", "0"},
                                  {"007", "7"},
                                  {"256", "None"},
                                  {"9223372036854775808", "None"},
                                  {"18446744073709551615", "None"}},
                                 signs),
                            kNoInteger));
  expectParses(
      "toU16",
      with(with({{"65535", "65535"}, {"65536", "None"}}, signs), kNoInteger));
  expectParses(
      "toU32",
      with(with({{"4294967295", "4294967295"}, {"4294967296", "None"}}, signs),
           kNoInteger));
}

TEST(StrParse, Floats) {
  const std::vector<ParseCase> none = {
      {"", "None"},      {" 1", "None"},     {"1 ", "None"},
      {"1_0", "None"},   {"x", "None"},      {"1.5f", "None"},
      {"1e999", "None"}, {"1e-400", "None"}, {"1\\02", "None"}};
  expectParses("toFloat", with({{"2.5", "2.5"},
                                {"+1.5", "1.5"},
                                {"-0.0", "-0"},
                                {".5", "0.5"},
                                {"5.", "5"},
                                {"1e3", "1000"},
                                {"0x10", "16"},
                                {"inf", "inf"},
                                {"-Infinity", "-inf"},
                                {"nan", "nan"},
                                {"NaN", "nan"},
                                {"nan(123)", "nan"},
                                {"5e-324", "4.94066e-324"},
                                {"-1e999", "None"}},
                               none));
  // Finding 11: finite text that rounds to infinity in the type is None, as
  // its literal is an error; text below the range rounds to 0, as the
  // literal does.
  expectParses("toF32", with({{"0.1", "0.1"},
                              {"3.4028235e38", "3.40282e+38"},
                              {"3.4028235677973366e38", "None"},
                              {"1e39", "None"},
                              {"-1e39", "None"},
                              {"1e300", "None"},
                              {"1e-46", "0"},
                              {"-1e-50", "-0"},
                              {"0x10", "16"},
                              {"-0", "-0"},
                              {"inf", "inf"},
                              {"nan", "nan"}},
                             none));
  expectParses("toF16", with({{"65504", "65504"},
                              {"65519", "65504"},
                              {"65520", "None"},
                              {"70000", "None"},
                              {"-70000", "None"},
                              {"1e-9", "0"},
                              {"-1e-9", "-0"},
                              {"0.1", "0.0999756"},
                              {"inf", "inf"},
                              {"-inf", "-inf"},
                              {"nan", "nan"}},
                             none));
}

// The value of an f32 or f16 text is the float nearest the text rounded to
// the type (C's `(float)strtod`), and an integer text is that integer
// rounded once, at any size (2^63 + 2^39 + 1 and 2^64 + 2^40 + 1, whose
// f64s are midpoints, as f32(u) rounds one).
TEST(StrParse, SizedFloatRounding) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    match "1.0000000596046448".toF32() { v: f32 { io::println(float(v) == 1.0); } None { } }
    match "1152921573326323713".toF32() {
      v: f32 { io::println(float(v) == 1152921642045800448.0); }
      None { }
    }
    match "9223372586610589697".toF32() {
      v: f32 {
        match "9223372586610589697".toUnsigned() {
          u: unsigned { io::println(v == f32(u) && float(v) == 9223373136366403584.0); }
          None { }
        }
      }
      None { }
    }
    match "-18446745173221179393".toF32() {
      v: f32 { io::println(float(v) == -18446746272732807168.0); }
      None { }
    }
    match "16777217".toF32() { v: f32 { io::println(float(v) == 16777216.0); } None { } }
    match "1.00048828125".toF16() { v: f16 { io::println(float(v) == 1.0); } None { } }
    match "2049".toF16() { v: f16 { io::println(float(v) == 2048.0); } None { } }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\nTrue\nTrue\nTrue\nTrue\nTrue\nTrue\n");
  g.expectNoLeaks("StrParse.SizedFloatRounding");
}

TEST(StrParse, Bools) {
  expectParses("toBool", {{"True", "True"},
                          {"False", "False"},
                          {"true", "None"},
                          {"FALSE", "None"},
                          {"1", "None"},
                          {" True", "None"},
                          {"True ", "None"},
                          {"", "None"},
                          {"True\\0", "None"}});
}

// The receiver is borrowed for the parse wherever it is, and a temporary
// one is released after it.
TEST(StrParse, EveryKindOfReceiver) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    import ::io;
    import ::collections;
    struct Field { text: Str; }
    class Holder {
      text: Str;
      view fn parsed() -> int? { return self.text.toInt(); }
    }
    impl Holder {
      fn __init__(t: Str) { self.text = t; }
    }
    fn fromView(s: view Str) -> u8? { return s.toU8(); }
    fn digits() -> Str { return "4" + "2"; }
    fn main() -> int {
      io::println("17".toInt());
      local = "-3.5";
      io::println(local.toFloat());
      f = Field("200");
      io::println(f.text.toU8());
      h = Holder("65535");
      io::println(h.text.toU16());
      io::println(h.parsed());
      io::println(fromView("255"));
      io::println(fromView("256"));
      xs = collections::List<Str>();
      xs.push("True");
      xs.push("-129");
      i: unsigned = 0;
      io::println(xs.at(i).toBool());
      io::println(xs.at(unsigned(1)).toI8());
      io::println(("1" + "2" + "8").toI16());
      io::println(digits().toI32());
      io::println(Str(7).toUnsigned());
      io::println(h.text.toF16());
      io::println(f.text.toF32());
      io::println(xs.at(i).toU32());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "17\n-3.5\n200\n65535\n65535\n255\nNone\nTrue\nNone\n128\n"
            "42\n7\nNone\n200\nNone\n");
  g.expectNoLeaks("StrParse.EveryKindOfReceiver");
}

// A text parses exactly when it is a valid literal of the type, and gives
// the literal's value (docs/language/04-strings-and-conversions.md, "Parsing
// a string"): for each text and type, either `x: T = <text>;` compiles and
// `"<text>".toT()` is that value, or the literal is an error and the parse
// is None.  A sign is a literal's only for a signed or a float type: an
// unsigned type refuses one (UnsignedIntegersRefuseEverySign).  An integer
// beyond 2^53 is rounded once into an f32, as a literal and as a text.
TEST(StrParse, LiteralsAndParsesAgree) {
  struct Type {
    const char *Name;
    const char *Method;
    bool Signed;
  };
  const Type ints[] = {
      {"int", "toInt", true},
      {"i8", "toI8", true},
      {"i16", "toI16", true},
      {"i32", "toI32", true},
      {"unsigned", "toUnsigned", false},
      {"u8", "toU8", false},
      {"u16", "toU16", false},
      {"u32", "toU32", false},
  };
  const char *intTexts[] = {
      "0",
      "1",
      "127",
      "128",
      "255",
      "256",
      "32767",
      "32768",
      "65535",
      "65536",
      "2147483647",
      "2147483648",
      "4294967295",
      "4294967296",
      "9223372036854775807",
      "-1",
      "-128",
      "-129",
      "-32768",
      "-32769",
      "-2147483648",
      "-2147483649",
      "-9223372036854775807",
  };
  const Type floats[] = {{"float", "toFloat", true},
                         {"f32", "toF32", true},
                         {"f16", "toF16", true}};
  const char *floatTexts[] = {
      "1.0000000596046448",
      "3.4028235e38",
      "3.4028235677973366e38",
      "1e39",
      "1e-46",
      "1e-9",
      "5e-324",
      "0.1",
      "-0.0",
      "65504.0",
      "65519.0",
      "65520.0",
      "1.00048828125",
      "2.5",
      "1e300",
      "65519",
      "65520",
      "16777217",
      "9007199254740993",
      "1152921573326323713",
      "-1152921573326323713",
      "-7",
      "0",
  };
  std::string helpers;
  std::string body;
  std::string expected;
  size_t accepted = 0, refused = 0;
  auto table = [&](const Type &t, const char *text) {
    if (!t.Signed && text[0] == '-')
      return;
    const bool fits = checks(std::string("fn main() -> int { x: ") + t.Name +
                             " = " + text + "; return 0; }");
    if (fits) {
      // The literal flows into the parameter's `T` slot.
      body += std::string("    io::println(same_") + t.Name + "(" + text +
              ", \"" + text + "\"." + t.Method + "()));\n";
      expected += "same\n";
      ++accepted;
    } else {
      body += std::string("    io::println(\"") + text + "\"." + t.Method +
              "());\n";
      expected += "None\n";
      ++refused;
    }
  };
  for (const Type &t : ints) {
    helpers += std::string("fn same_") + t.Name + "(x: " + t.Name +
               ", o: " + t.Name + "?) -> Str {\n  match o {\n    v: " + t.Name +
               " { if v == x { return \"same\"; } return Str(v) + \" is not \" "
               "+ Str(x); }\n    None { return \"None\"; }\n  }\n}\n";
    for (const char *text : intTexts)
      table(t, text);
  }
  for (const Type &t : floats) {
    // Equal, and the same text, which tells -0 from 0.
    helpers += std::string("fn same_") + t.Name + "(x: " + t.Name +
               ", o: " + t.Name + "?) -> Str {\n  match o {\n    v: " + t.Name +
               " { if v == x && Str(v) == Str(x) { return \"same\"; } return "
               "Str(v) + \" is not \" + Str(x); }\n    None { return "
               "\"None\"; }\n  }\n}\n";
    for (const char *text : floatTexts)
      table(t, text);
  }
  // Both kinds occur, among them the boundaries each family has.
  EXPECT_GT(accepted, 100u);
  EXPECT_GT(refused, 40u);
  LeakGuard g;
  auto r = compileAndRun("import ::io;\n" + helpers + wrapMain(body));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, expected) << body;
  g.expectNoLeaks("StrParse.LiteralsAndParsesAgree");
}

// `Str(x).toT()` gives back `x` for every value of an integer type and of
// `bool`: the bounds of each type, compared as optionals.
TEST(StrParse, TheTextOfAValueParsesBack) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
    a: int? = -9223372036854775807 - 1;  b: int? = 9223372036854775807;
    c: i8? = -128;  d: i16? = 32767;  e: i32? = -2147483648;
    u: unsigned? = unsigned(-1);  v: u8? = 255;  w: u16? = 0;  x: u32? = 4294967295;
    t: bool? = True;  f: bool? = False;  h: f16? = 65504.0;  k: f32? = 0.5;
    io::println(Str(-9223372036854775807 - 1).toInt() == a);
    io::println(Str(9223372036854775807).toInt() == b);
    io::println(Str(i8(-128)).toI8() == c);
    io::println(Str(i16(32767)).toI16() == d);
    io::println(Str(i32(-2147483648)).toI32() == e);
    io::println(Str(unsigned(-1)).toUnsigned() == u);
    io::println(Str(u8(255)).toU8() == v);
    io::println(Str(u16(0)).toU16() == w);
    io::println(Str(u32(4294967295)).toU32() == x);
    io::println(Str(True).toBool() == t);
    io::println(Str(False).toBool() == f);
    io::println(Str(f16(65504.0)).toF16() == h);
    io::println(Str(f32(0.5)).toF32() == k);
    io::println(Str(-0.0).toFloat());
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\nTrue\nTrue\nTrue\nTrue\nTrue\nTrue\nTrue\nTrue\n"
                      "True\nTrue\nTrue\nTrue\n-0\n");
  g.expectNoLeaks("StrParse.TheTextOfAValueParsesBack");
}
