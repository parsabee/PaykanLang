// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: conversion constructors `Target<Source>(value)` (#64, #88) —
// every specialization of each builtin target and its result type, the
// sources of each family (every target and source, both forms), identity,
// the rewrite the error of a pair the families leave out names, an
// `unsigned`'s `toInt()`, the closed set (unlisted and custom type
// arguments), the missing-specialization hint, a parse written as a
// conversion (a method of `Str` now), `Str` of every value with a text,
// wrong arity and argument types, and what became of the removed builtins.

#include "TestUtils.h"
#include <gtest/gtest.h>
#include <iterator>
#include <vector>

using namespace paykan::test;

static std::string inMain(const std::string &body) {
  return "import ::io; fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

static void expectOk(const std::string &src) {
  auto r = semaCheck(src);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

static void expectError(const std::string &src, const std::string &needle) {
  auto r = semaCheck(src);
  EXPECT_FALSE(r.Ok) << "expected an error containing: " << needle;
  EXPECT_NE(r.Diagnostics.find(needle), std::string::npos)
      << "diagnostics were:\n"
      << r.Diagnostics;
}

// ─── every pair, with its exact result type ─────────────────────────────────

TEST(Conversion, EveryPairHasItsResultType) {
  // Each declaration states the result type, so a wrong one is an error.
  expectOk(inMain(R"(
    i: int = 1;  f: float = 1.5;  b: bool = True;  c: char = 'c';
    s1: Str = Str<int>(i);
    s2: Str = Str<float>(f);
    s3: Str = Str<bool>(b);
    s4: Str = Str<char>(c);
    n1: int = int<float>(f);
    n2: float = float<int>(i);
    n3: int = int<bool>(b);
    n4: bool = bool<int>(i);
    n5: int = int<char>(c);
    n6: char = char<int>(i);
  )"));
}

TEST(Conversion, BoxedPrimitiveClassesAreGone) {
  // Int, Float, Bool and Char were removed with #96: optionals are values.
  expectError(inMain("x = Int<Str>(\"1\");"),
              "call to undeclared generic function or class 'Int'");
  expectError(inMain("x = Float(\"1.5\");"),
              "call to undeclared function 'Float'");
  expectError(inMain("b: Bool? = None;"),
              "variable 'b' has unknown class type 'Bool'");
  expectError(inMain("x = Str<Char>('c');"),
              "source type of conversion 'Str' has unknown class type 'Char'");
}

TEST(Conversion, InferredDeclarationsTakeTheResultType) {
  expectOk(inMain(R"(
    a = int<float>(2.5);
    b: int = a + 1;
    c = bool<int>(b);
    d: bool = c;
  )"));
}

// ─── a parse is a method of Str (docs/language/04-strings-and-conversions.md,
// ─── "Parsing a string")

/// The one error of the program @p src, whose text is @p message (the whole
/// line after `error: `).
static void expectOneErrorIn(const std::string &src,
                             const std::string &message) {
  auto r = semaCheck(src);
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("error: " + message + "\n"), std::string::npos)
      << r.Diagnostics;
}
/// expectOneErrorIn of `main` with the statements @p body.
static void expectOneError(const std::string &body,
                           const std::string &message) {
  expectOneErrorIn(inMain(body), message);
}

TEST(Conversion, APrimitiveDoesNotParseAStr) {
  const std::string s = "s: Str = \"1\"; ";
  // The inferred and the explicit form alike name the method.
  expectOneError(s + "o = int(s);",
                 "no specialization of 'int' for 'Str': parse a 'Str' with its "
                 "method 'toInt()', which returns 'int?'");
  expectOneError(s + "o = int<Str>(s);",
                 "no specialization of 'int' for 'Str': parse a 'Str' with its "
                 "method 'toInt()', which returns 'int?'");
  expectOneError(s + "o = i64(s);",
                 "no specialization of 'int' for 'Str': parse a 'Str' with its "
                 "method 'toInt()', which returns 'int?'");
  expectOneError(s + "o = float(s);",
                 "no specialization of 'float' for 'Str': parse a 'Str' with "
                 "its method 'toFloat()', which returns 'float?'");
  expectOneError(s + "o = f64<Str>(s);",
                 "no specialization of 'float' for 'Str': parse a 'Str' with "
                 "its method 'toFloat()', which returns 'float?'");
  expectOneError(s + "o = bool(s);",
                 "no specialization of 'bool' for 'Str': parse a 'Str' with "
                 "its method 'toBool()', which returns 'bool?'");
  expectOneError(s + "o = bool<Str>(\"True\");",
                 "no specialization of 'bool' for 'Str': parse a 'Str' with "
                 "its method 'toBool()', which returns 'bool?'");
  // The explicit form names the source: an argument of another type is not
  // a second error.
  expectOneError("o = int<Str>(12);",
                 "no specialization of 'int' for 'Str': parse a 'Str' with its "
                 "method 'toInt()', which returns 'int?'");
  // `char` has no parse: the ordinary error, with its list.
  expectOneError(s + "o = char(s);",
                 "no specialization of 'char' for 'Str'; its specializations "
                 "are int, i8, i16, i32, u8, u16, u32, unsigned, char");
  expectOneError("o = char<Str>(\"a\");",
                 "no specialization of 'char' for 'Str'; its specializations "
                 "are int, i8, i16, i32, u8, u16, u32, unsigned, char");
  // An optional string is no `Str`: the ordinary error.
  expectOneError("s: Str? = \"1\"; o = int(s);",
                 "no specialization of 'int' for 'Str?'; its specializations "
                 "are int, float, i8, i16, i32, u8, u16, u32, f16, f32, bool, "
                 "char");
}

// A program written for the older parse gets one error at each of its old
// parses, and none from what surrounds it.
TEST(Conversion, AParseWrittenAsAConversionIsOneError) {
  const std::string e1 = "no specialization of 'int' for 'Str': parse a 'Str' "
                         "with its method 'toInt()', which returns 'int?'";
  expectOneError("n: int = int(\"1\");", e1);
  expectOneError("s = \"7\"; m: int? = int(s);", e1);
  expectOneError(R"(
    s = "7";
    match int(s) { n: int { m: int = n + 1; } None { } }
  )",
                 e1);
  expectOneError(R"(
    s = "7";
    o = int<Str>(s);
    if o == None { }
    io::println(o);
  )",
                 e1);
  expectOneError("t: Str = Str(int(\"1\")) + \"x\";", e1);
  expectOneError("x = int(\"1\") + 1;", e1);
  // Each old parse is its own error.
  auto r = semaCheck(inMain("a = int(\"1\"); b = float<Str>(\"2\");"));
  EXPECT_NE(r.Diagnostics.find("method 'toInt()'"), std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("method 'toFloat()'"), std::string::npos)
      << r.Diagnostics;
}

// ─── the families (docs/language/04-strings-and-conversions.md, "Between
// ─── numbers")

namespace {

/// The primitives, in the order a diagnostic lists them, each with a value
/// of its type.
struct Primitive {
  const char *Name;
  const char *Value;
};
constexpr Primitive kPrimitives[] = {
    {"int", "1"},      {"float", "1.5"}, {"i8", "1"},    {"i16", "1"},
    {"i32", "1"},      {"u8", "1"},      {"u16", "1"},   {"u32", "1"},
    {"unsigned", "1"}, {"f16", "1.5"},   {"f32", "1.5"}, {"bool", "True"},
    {"char", "'c'"}};

/// What `target(v)` is for each target (a row) and the source `v` has (a
/// column), both in kPrimitives' order: '.' a conversion, '2' the error of
/// an `unsigned` into the signed family, '3' of a float into the unsigned
/// family, '4' of a `bool` or a `char` into the float family, '0' the error
/// that lists the target's sources.
constexpr const char *kFamilies[] = {
    "........2....", // int
    "...........44", // float
    "........2....", // i8
    "........2....", // i16
    "........2....", // i32
    ".3.......33..", // u8
    ".3.......33..", // u16
    ".3.......33..", // u32
    ".3.......33..", // unsigned
    "...........44", // f16
    "...........44", // f32
    ".0.......00.0", // bool
    ".0.......000.", // char
};

/// The method of `Str` that parses each primitive (char: none).
constexpr const char *kParseMethods[] = {
    "toInt", "toFloat",    "toI8",  "toI16", "toI32",  "toU8", "toU16",
    "toU32", "toUnsigned", "toF16", "toF32", "toBool", nullptr};

/// The sources of the target @p t, as an error lists them.
std::string sourceList(size_t t) {
  std::string list;
  for (size_t i = 0; i < std::size(kPrimitives); ++i)
    if (kFamilies[t][i] == '.')
      list += (list.empty() ? "" : ", ") + std::string(kPrimitives[i].Name);
  return list;
}

/// The error of `target(v)` for the rejected cell (@p t, @p s).
std::string familyError(size_t t, size_t s) {
  const std::string target = kPrimitives[t].Name;
  const std::string source = kPrimitives[s].Name;
  const std::string miss =
      "no specialization of '" + target + "' for '" + source + "'";
  const std::string bits = target == "unsigned" ? "64" : target.substr(1);
  switch (kFamilies[t][s]) {
  case '2':
    if (target == "int")
      return miss + ": an 'unsigned' can be larger than any 'int'; its "
                    "method 'toInt()' returns 'int?', None above "
                    "9223372036854775807";
    return miss + ": an 'unsigned' can be larger than any '" + target + "'; '" +
           target + "(u" + bits + "(...))' keeps its low " + bits + " bits";
  case '3':
    return miss + ": convert the '" + source + "' to an 'int' first: '" +
           target + "(int(...))' truncates toward zero, then keeps the low " +
           bits + " bits";
  case '4':
    return miss + ": convert the '" + source + "' to an 'int' first: '" +
           target + "(int(...))'";
  default:
    return miss + "; its specializations are " + sourceList(t);
  }
}

/// The inferred and the explicit form of a conversion to @p target of
/// @p arg, of type @p source.
std::vector<std::string> conversionForms(const std::string &target,
                                         const std::string &arg,
                                         const std::string &source) {
  return {target + "(" + arg + ")", target + "<" + source + ">(" + arg + ")"};
}

/// A program that declares `v: source = value` and then @p statement.
std::string withValue(const std::string &source, const std::string &value,
                      const std::string &statement) {
  return "fn main() -> int {\n  v: " + source + " = " + value + ";\n  " +
         statement + "\n  return 0;\n}\n";
}

} // namespace

// Every (target, source) pair of the primitives, in the inferred and the
// explicit form: a conversion whose result is the target, or the one error
// of its cell.
TEST(Conversion, SourceSetsAreTheFamilies) {
  for (size_t t = 0; t < std::size(kPrimitives); ++t) {
    for (size_t s = 0; s < std::size(kPrimitives); ++s) {
      const std::string target = kPrimitives[t].Name;
      const std::string source = kPrimitives[s].Name;
      for (const std::string &call : conversionForms(target, "v", source)) {
        std::string trace = call;
        SCOPED_TRACE(trace.append(" with v: ").append(source));
        std::string declaration = "x: ";
        declaration.append(target).append(" = ").append(call).append(";");
        auto r =
            semaCheck(withValue(source, kPrimitives[s].Value, declaration));
        if (kFamilies[t][s] == '.') {
          EXPECT_TRUE(r.Ok) << r.Diagnostics;
          continue;
        }
        EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
        EXPECT_NE(r.Diagnostics.find("error: " + familyError(t, s) + "\n"),
                  std::string::npos)
            << r.Diagnostics;
      }
    }
  }
}

// `Str` takes every primitive; a primitive takes no `Str`: the error names
// its parse, or lists the sources of `char`, which has none.
TEST(Conversion, StrAndThePrimitives) {
  for (size_t t = 0; t < std::size(kPrimitives); ++t) {
    const std::string name = kPrimitives[t].Name;
    SCOPED_TRACE(name);
    const std::vector<std::string> shows = conversionForms("Str", "v", name);
    std::string texts = "a: Str = ";
    texts.append(shows[0]).append(" + ").append(shows[1]).append(";");
    expectOk(withValue(name, kPrimitives[t].Value, texts));
    std::string error = "no specialization of '";
    error.append(name).append("' for 'Str'");
    if (const char *method = kParseMethods[t])
      error.append(": parse a 'Str' with its method '")
          .append(method)
          .append("()', which returns '")
          .append(name)
          .append("?'");
    else
      error.append("; its specializations are ").append(sourceList(t));
    for (const std::string &call : conversionForms(name, "v", "Str")) {
      std::string parse = "x = ";
      parse.append(call).append(";");
      auto r = semaCheck(withValue("Str", "\"1\"", parse));
      EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
      EXPECT_NE(r.Diagnostics.find("error: " + error + "\n"), std::string::npos)
          << r.Diagnostics;
    }
  }
}

// The errors of the pairs the families leave out, as the language reference
// gives them.
TEST(Conversion, TheFamiliesNameTheRewrite) {
  expectOneError("u: unsigned = 1; n = int(u);",
                 "no specialization of 'int' for 'unsigned': an 'unsigned' can "
                 "be larger than any 'int'; its method 'toInt()' returns "
                 "'int?', None above 9223372036854775807");
  expectOneError("u: unsigned = 1; n = i32<unsigned>(u);",
                 "no specialization of 'i32' for 'unsigned': an 'unsigned' can "
                 "be larger than any 'i32'; 'i32(u32(...))' keeps its low 32 "
                 "bits");
  expectOneError("u: u64 = 1; n = i16(u);",
                 "no specialization of 'i16' for 'unsigned': an 'unsigned' can "
                 "be larger than any 'i16'; 'i16(u16(...))' keeps its low 16 "
                 "bits");
  expectOneError("n = u8(255.5);",
                 "no specialization of 'u8' for 'float': convert the 'float' "
                 "to an 'int' first: 'u8(int(...))' truncates toward zero, "
                 "then keeps the low 8 bits");
  expectOneError("f: f64 = 2.5; n = u64<f64>(f);",
                 "no specialization of 'unsigned' for 'float': convert the "
                 "'float' to an 'int' first: 'unsigned(int(...))' truncates "
                 "toward zero, then keeps the low 64 bits");
  expectOneError("h: f16 = 2.5; n = u32(h);",
                 "no specialization of 'u32' for 'f16': convert the 'f16' to "
                 "an 'int' first: 'u32(int(...))' truncates toward zero, then "
                 "keeps the low 32 bits");
  expectOneError("x = float(True);",
                 "no specialization of 'float' for 'bool': convert the 'bool' "
                 "to an 'int' first: 'float(int(...))'");
  expectOneError("x = f32<char>('a');",
                 "no specialization of 'f32' for 'char': convert the 'char' to "
                 "an 'int' first: 'f32(int(...))'");
  // One error at each, and none from what is built on it.
  expectOneError("u: unsigned = 1; n: int = int(u) + 1;",
                 "no specialization of 'int' for 'unsigned': an 'unsigned' can "
                 "be larger than any 'int'; its method 'toInt()' returns "
                 "'int?', None above 9223372036854775807");
}

// `T(x)` of a `T` is the value itself, under any spelling of the type.
TEST(Conversion, IdentityIsTheValue) {
  expectOk(inMain(R"(
    n: int = 5;  b: u8 = 7;  k: f32 = 1.5;  u: unsigned = 2;
    a1: int = int(5);  a2: float = float(2.5);  a3: bool = bool(True);
    a4: char = char('x');  a5: int = i64(n);  a6: u8 = u8(b);
    a7: f32 = f32<f32>(k);  a8: int = int<int>(1);  a9: unsigned = u64(u);
    a10: float = f64<float>(2.5);
  )"));
}

// `u.toInt()`: an `unsigned`'s value as an `int?`, a builtin method like
// `hash()` with no arguments, on an `unsigned` only.
TEST(Conversion, AnUnsignedHasToInt) {
  expectOk(inMain(R"(
    u: unsigned = 7;
    a: int? = u.toInt();
    b: int? = unsigned(-1).toInt();
    w: u64 = 3;
    c: int? = w.toInt();
    match u.toInt() { n: int { m: int = n + 1; } None { } }
  )"));
  expectOneError("u: unsigned = 7; n: int = u.toInt();",
                 "cannot use optional 'int?' as 'int' without unwrapping (use "
                 "match)");
  expectOneError("u: unsigned = 7; n = u.toInt(1);",
                 "method 'toInt' expects 0 argument(s), got 1");
  expectOneError("o: unsigned? = 7; n = o.toInt();",
                 "cannot use optional 'unsigned?' as 'unsigned' without "
                 "unwrapping (use match)");
  expectOneError("a: u32 = 7; n = a.toInt();",
                 "method call on non-class type 'u32'");
  expectOneError("n = 7; m = n.toInt();",
                 "method call on non-class type 'int'");
}

// ─── unsupported pairs ──────────────────────────────────────────────────────

TEST(Conversion, UnlistedTypeArgumentsNameTheClosedSet) {
  expectError(inMain("x = bool<float>(1.5);"),
              "no specialization of 'bool' for 'float'; its specializations "
              "are int, i8, i16, i32, u8, u16, u32, unsigned, bool\n");
  expectError(inMain("x = char<Str>(\"a\");"),
              "no specialization of 'char' for 'Str'; its specializations "
              "are int, i8, i16, i32, u8, u16, u32, unsigned, char\n");
  expectError(inMain("x = char<float>(1.5);"),
              "no specialization of 'char' for 'float'; its specializations "
              "are int, i8, i16, i32, u8, u16, u32, unsigned, char\n");
  expectError(inMain("x = int<Missing>(1);"),
              "source type of conversion 'int' has unknown class type "
              "'Missing'");
}

TEST(Conversion, CustomTypesAreNeverSpecializations) {
  // A user type can't add specializations to a primitive; `Str` takes it
  // as its text (StrTakesEveryTypeWithAText).
  const std::string point = "class Point {}\n"
                            "impl Point { fn __init__() { } }\n";
  expectError(point + "fn main() -> int { x = int<Point>(Point()); return 0; }",
              "no specialization of 'int' for 'Point'; its specializations "
              "are int, float, i8, i16, i32, u8, u16, u32, f16, f32, bool, "
              "char\n");
  expectError("enum Color { Red }\n"
              "fn main() -> int { x = bool<Color>(Color::Red); return 0; }",
              "no specialization of 'bool' for 'Color'; its specializations "
              "are int, i8, i16, i32, u8, u16, u32, unsigned, bool\n");
  expectOk(point + "fn main() -> int { s: Str = Point().toString(); "
                   "return 0; }");
}

// ─── arity and argument types ───────────────────────────────────────────────

TEST(Conversion, ExactlyOneSourceTypeAndOneArgument) {
  expectError(inMain("x = int<float, int>(1.5);"),
              "conversion 'int<...>' takes exactly one source type, got 2");
  expectError(inMain("x = Str<int>();"),
              "conversion 'Str<int>' takes exactly one argument, got 0");
  expectError(inMain("x = Str<int>(1, 2);"),
              "conversion 'Str<int>' takes exactly one argument, got 2");
}

TEST(Conversion, ArgumentMustHaveExactlyTheSourceType) {
  // No int -> float promotion: the source type is spelled out.
  expectError(inMain("x = int<float>(3);"),
              "argument of 'int<float>' has type 'int', expected 'float'");
  expectError(inMain("x = Str<float>(3);"),
              "argument of 'Str<float>' has type 'int', expected 'float'");
  expectError(inMain("x = float<int>(1.5);"),
              "argument of 'float<int>' has type 'float', expected 'int'");
  expectError(inMain("x = Str<char>(\"c\");"),
              "argument of 'Str<char>' has type 'Str', expected 'char'");
  expectError(inMain("x = bool<int>(1.5);"),
              "argument of 'bool<int>' has type 'float', expected 'int'");
  // An optional must be unwrapped first.
  expectError(inMain("o: int? = 1;\n x = Str<int>(o);"),
              "cannot use optional 'int?' as 'int' without unwrapping");
}

// ─── the inferred form: Target(value) (#88) ─────────────────────────────────

TEST(Conversion, InferredFormPicksEveryPair) {
  // Each declaration states the result type, so a wrong pick is an error.
  expectOk(inMain(R"(
    i: int = 1;  f: float = 1.5;  b: bool = True;  c: char = 'c';
    s1: Str = Str(i);
    s2: Str = Str(f);
    s3: Str = Str(b);
    s4: Str = Str(c);
    n1: int = int(f);
    n2: float = float(i);
    n3: int = int(b);
    n4: bool = bool(i);
    n5: int = int(c);
    n6: char = char(i);
  )"));
}

TEST(Conversion, InferredFormTakesLiterals) {
  expectOk(inMain(R"(
    a: Str = Str(1) + Str(2.5) + Str(True) + Str('c');
    n: int = int(2.5);
    f: float = float(3);
    c: char = char(97);
    b: bool = bool(0);
  )"));
}

TEST(Conversion, InferredFormIsTheExplicitOne) {
  // The same result types.
  expectError(inMain("n: bool = int(2.5);"),
              "initializer of type 'int' does not match declared type 'bool'");
  expectError(inMain("x: int? = bool(1);"),
              "initializer of type 'bool' does not match declared type 'int?'");
  expectOk(inMain(R"(
    a = int(2.5);
    b: int = a + 1;
    s = Str(b);
    t: Str = s;
  )"));
}

TEST(Conversion, InferredFormNeedsAnExactSource) {
  // No widening and no unwrapping: the argument's own type must be a source
  // (an optional's text is its own: StrTakesEveryTypeWithAText).
  expectError(inMain("x = bool(1.5);"),
              "no specialization of 'bool' for 'float'; its specializations "
              "are int, i8, i16, i32, u8, u16, u32, unsigned, bool\n");
  expectError(inMain("o: int? = 1;\n x = int(o);"),
              "no specialization of 'int' for 'int?'; its specializations "
              "are int, float, i8, i16, i32, u8, u16, u32, f16, f32, bool, "
              "char\n");
}

TEST(Conversion, InferredFormRejectsEveryOtherType) {
  const std::string decls = "class A {}\n"
                            "impl A { fn __init__() { } }\n"
                            "enum Color { Red }\n";
  auto prog = [&](const std::string &body) {
    return decls + "import ::io; fn main() -> int {\n" + body +
           "\n  return 0;\n}\n";
  };
  expectError(prog("a = A(); n = int(a);"),
              "no specialization of 'int' for 'A'; its specializations are "
              "int, float, i8, i16, i32, u8, u16, u32, f16, f32, bool, char\n");
  expectError(prog("n = float(None);"),
              "no specialization of 'float' for 'Obj'; its specializations are "
              "int, float, i8, i16, i32, u8, u16, u32, unsigned, f16, f32\n");
  expectError(prog("n = char(Color::Red);"),
              "no specialization of 'char' for 'Color'; its specializations "
              "are int, i8, i16, i32, u8, u16, u32, unsigned, char\n");
}

TEST(Conversion, InferredFormTakesExactlyOneArgument) {
  expectError(inMain("x = int();"),
              "conversion 'int' takes exactly one argument, got 0");
  expectError(inMain("x = bool(1, 2);"),
              "conversion 'bool' takes exactly one argument, got 2");
  expectError(inMain("x = char(97, 98, 99);"),
              "conversion 'char' takes exactly one argument, got 3");
}

TEST(Conversion, InferredFormInsideGenericsAndCalls) {
  expectOk(R"(
    class Box<T> { v: T; }
    impl Box<T> { fn __init__(x: T) { self.v = x; } }
    fn ident<T>(x: T) -> T { return x; }
    fn show<T>(x: T) -> Str { return "?"; }
    fn main() -> int {
      b = Box<int>(int(1.5));
      s: Str = ident<Str>(Str(b.v));
      t: Str = ident(Str(int(2.5)));
      return 0;
    }
  )");
}

TEST(Conversion, StrCopyConstructorStillWorks) {
  // `Str(s)` with a `Str` is not a conversion: `s` itself, as `Str<Str>(s)`
  // is.  A `Str?` has a text, "None" or the string.
  expectOk(inMain(R"(
    s = "abc";
    t: Str = Str(s);
    u: Str = Str("lit");
    v: Str = Str<Str>(s);
    w: Str = Str<Str>("lit");
    o: Str? = "a";
    x: Str = Str(o);
    y: Str = Str<Str?>(o);
  )"));
  expectOneError("t = Str<Str>(3);",
                 "argument of 'Str<Str>' has type 'int', expected 'Str'");
  expectOneError("o: Str? = \"a\"; t = Str<Str>(o);",
                 "cannot use optional 'Str?' as 'Str' without unwrapping "
                 "(use match)");
  expectError(inMain("t = Str();"), "function 'Str' expects 1 argument(s)");
}

// ─── Str of every value with a text (docs/language/04-strings-and-
// ─── conversions.md, "To a string")

// `Str(x)` is `x.toString()` for every type with a text: an optional, a
// tuple, a struct, an enum, an object, `None`; in the explicit form too.
TEST(Conversion, StrTakesEveryTypeWithAText) {
  const std::string decls = R"(
    class P { x: int; view fn toString() -> Str { return "P"; } }
    class Q { }
    struct S { a: int; b: Str; }
    enum E { A, B(n: int) }
    enum Color { Red, Green }
    fn show<T>(x: T) -> Str { return Str(x); }
  )";
  expectOk(decls + inMain(R"(
    o: int? = None;  so: Str? = "s";  po: P? = P(1);
    a: Str = Str(o);
    b: Str = Str(so);
    c: Str = Str(po);
    d: Str = Str((1, "a", 2.5));
    e: Str = Str(S(1, "b"));
    f: Str = Str(E::B(4));
    g: Str = Str(E::A);
    h: Str = Str(Color::Red);
    i: Str = Str(P(2));
    j: Str = Str(Q());
    ob: Obj = P(3);
    k: Str = Str(ob);
    l: Str = Str(None);
    m: Str = show(P(4)) + show((1, 2)) + show(o) + show(S(2, "c"));
    t = (P(5), Color::Green);
    n: Str = Str(t);
  )"));
  expectOk(decls + inMain(R"(
    o: int? = 3;  p = P(1);
    a: Str = Str<int?>(o);
    b: Str = Str<P>(p);
    c: Str = Str<(int, Str)>((1, "a"));
    d: Str = Str<S>(S(1, "b"));
    e: Str = Str<Color>(Color::Red);
  )"));
  // The argument has exactly the type written.
  expectOneErrorIn(decls + inMain("x = Str<P>(3);"),
                   "argument of 'Str<P>' has type 'int', expected 'P'");
  expectOneErrorIn(decls + inMain("p = P(1); x = Str<P?>(p);"),
                   "argument of 'Str<P?>' has type 'P', expected 'P?'");
  expectOneErrorIn(decls + inMain("x = Str<Obj>(\"a\");"),
                   "argument of 'Str<Obj>' has type 'Str', expected 'Obj'");
}

// A value without a text (a function value, a fixed-size array, a slice, or
// an aggregate holding one) is no `Str`: the error names the part.
TEST(Conversion, StrOfAValueWithoutATextNamesThePart) {
  const std::string decls = R"(
    struct F { n: int; f: (int) -> int; }
    enum G { A, B(f: () -> void) }
    fn show<T>(x: T) -> Str { return Str(x); }
  )";
  auto only = [&](const std::string &body, const std::string &message) {
    expectOneErrorIn(decls + inMain(body), message);
  };
  only("x = Str(fn(n: int) => n);",
       "'Str' cannot convert a value of type '(int) -> int', which does not "
       "implement trait 'Show'");
  only("x = Str(F(1, fn(n: int) => n));",
       "'Str' cannot convert a value of type 'F', which does not implement "
       "trait 'Show': its field 'f' has type '(int) -> int', which does not "
       "implement it");
  only("x = Str<F>(F(1, fn(n: int) => n));",
       "'Str' cannot convert a value of type 'F', which does not implement "
       "trait 'Show': its field 'f' has type '(int) -> int', which does not "
       "implement it");
  only("xs: int[3] = [1, 2, 3]; x = Str(xs);",
       "'Str' cannot convert a value of type 'int[3]', which does not "
       "implement trait 'Show'");
  only("x = Str((1, fn() {}));",
       "'Str' cannot convert a value of type '(int, () -> void)', which does "
       "not implement trait 'Show': its element 1 has type '() -> void', "
       "which does not implement it");
  only("x = Str(G::A);",
       "'Str' cannot convert a value of type 'G', which does not implement "
       "trait 'Show': its field 'f' of variant 'B' has type '() -> void', "
       "which does not implement it");
  expectOneErrorIn("fn s(v: view int[]) -> Str { return Str(v); }\n" +
                       inMain(""),
                   "'Str' cannot convert a value of type 'int[]', which does "
                   "not implement trait 'Show'");
  // Checked per instantiation: the note names it.
  auto g = semaCheck(decls + inMain("x = show(fn() {});"));
  EXPECT_EQ(g.ErrorCount, 1u) << g.Diagnostics;
  EXPECT_NE(g.Diagnostics.find("error: 'Str' cannot convert a value of type "
                               "'() -> void', which does not implement trait "
                               "'Show'\n"),
            std::string::npos)
      << g.Diagnostics;
  EXPECT_NE(g.Diagnostics.find("note: in instantiation of 'show<() -> void>'"),
            std::string::npos)
      << g.Diagnostics;
}

// ─── the removed builtins ───────────────────────────────────────────────────

TEST(Conversion, RemovedBuiltinsAreUnknownFunctions) {
  // No dedicated diagnostic: an old name is just an undeclared function.
  expectError(inMain("s = StrInt(1);"), "call to undeclared function 'StrInt'");
  expectError(inMain("o = IntStr(\"1\");"),
              "call to undeclared function 'IntStr'");
}

TEST(Conversion, RemovedNamesAreOrdinaryIdentifiersAgain) {
  // A program may now declare its own function with an old name.
  expectOk(R"(
    fn StrInt(n: int) -> Str { return Str<int>(n); }
    fn main() -> int { s: Str = StrInt(3); return 0; }
  )");
}

TEST(Conversion, GenericCallsAreStillGenericCalls) {
  expectOk(R"(
    class Box<T> { v: T; }
    impl Box<T> { fn __init__(x: T) { self.v = x; } }
    fn ident<T>(x: T) -> T { return x; }
    fn main() -> int {
      b = Box<int>(int<float>(1.5));
      s: Str = ident<Str>(Str<int>(b.v));
      return 0;
    }
  )");
}

// -- `''` is one error on every frontend (#119)

TEST(CharLiteral, EmptyCharLiteralIsOneError) {
  const std::string src = "fn main() -> int { c = ''; return 0; }\n";
  auto path = writeTempFile(src);
  for (const std::string &fe : paykan::frontend::Registry::get().names()) {
    paykan::parser::ParserDriver drv(fe);
    std::ostringstream os;
    paykan::sema::DiagEngine diag(os);
    diag.setSourceInfo("t.pkn", &drv.getSourceLines());
    drv.setDiagEngine(&diag);
    EXPECT_NE(drv.parseFile(path), 0) << fe;
    EXPECT_EQ(diag.getErrorCount(), 1u) << fe << "\n" << os.str();
    EXPECT_NE(os.str().find("t.pkn:1:24: error: empty character literal"),
              std::string::npos)
        << fe << "\n"
        << os.str();
  }
  std::filesystem::remove(path);
}

// `+` joins two strings or adds two numbers; a string and a number need the
// number converted first.
TEST(Conversion, StrPlusANumberNeedsStr) {
  expectError(inMain("n: int = 3; s = \"a\" + n;"),
              "operator '+' is not defined for types 'Str' and 'int' (convert "
              "the 'int' with 'Str(...)')");
  expectError(inMain("s = 2.5 + \"b\";"),
              "operator '+' is not defined for types 'float' and 'Str' "
              "(convert the 'float' with 'Str(...)')");
  expectOneError("b: u8 = 1; s = \"c\" + b;",
                 "operator '+' is not defined for types 'Str' and 'u8' "
                 "(convert the 'u8' with 'Str(...)')");
  expectOk(inMain("n: int = 3; s: Str = \"a\" + Str(n) + \"b\";"));
}

// So does any other value with a text; a value without one gets no hint.
TEST(Conversion, StrPlusAValueWithATextNeedsStr) {
  const std::string p = "class P { }\n";
  expectOneErrorIn(p + inMain("x = \"a\" + P();"),
                   "operator '+' is not defined for types 'Str' and 'P' "
                   "(convert the 'P' with 'Str(...)')");
  expectOneErrorIn(p + inMain("x = P() + \"a\";"),
                   "operator '+' is not defined for types 'P' and 'Str' "
                   "(convert the 'P' with 'Str(...)')");
  expectOneError("x = \"a\" + (1, 2);",
                 "operator '+' is not defined for types 'Str' and '(int, int)' "
                 "(convert the '(int, int)' with 'Str(...)')");
  expectOneError("xs: int[3] = [1, 2, 3]; x = \"a\" + xs;",
                 "operator '+' is not defined for types 'Str' and 'int[3]'");
  expectOneError("f = fn(n: int) => n; x = \"a\" + f;",
                 "operator '+' is not defined for types 'Str' and "
                 "'(int) -> int'");
  expectOk(p + inMain("x: Str = \"a\" + Str(P()) + Str((1, 2));"));
}

// ─── Str's parse methods (docs/language/04-strings-and-conversions.md,
// ─── "Parsing a string")

TEST(StrParse, EachMethodGivesTheOptionalOfItsType) {
  // Each declaration states the result type, so a wrong one is an error.
  expectOk(inMain(R"(
    s: Str = "1";
    a: int? = s.toInt();
    b: i8? = s.toI8();
    c: i16? = s.toI16();
    d: i32? = s.toI32();
    e: unsigned? = s.toUnsigned();
    f: u8? = s.toU8();
    g: u16? = s.toU16();
    h: u32? = s.toU32();
    i: float? = s.toFloat();
    j: f16? = s.toF16();
    k: f32? = s.toF32();
    l: bool? = s.toBool();
    m: i64? = "2".toInt();
    n: u64? = "3".toUnsigned();
    o: f64? = "4".toFloat();
  )"));
  expectError(inMain(R"(n: int = "1".toInt();)"),
              "cannot use optional 'int?' as 'int' without unwrapping");
  expectError(inMain(R"(n: u8? = "1".toU16();)"),
              "initializer of type 'u16?' does not match declared type 'u8?'");
  expectOk(inMain(R"(
    match "7".toU8() { n: u8 { m: u8 = n + 1; } None { } }
  )"));
}

TEST(StrParse, TheMethodsTakeNoArgument) {
  expectError(inMain(R"(o = "1".toInt(2);)"),
              "method 'toInt' expects 0 argument(s), got 1");
  expectError(inMain(R"(o = "1".toF32("x", 2);)"),
              "method 'toF32' expects 0 argument(s), got 2");
}

TEST(StrParse, AnOptionalReceiverIsUnwrappedFirst) {
  expectError(inMain(R"(s: Str? = "1"; o = s.toInt();)"),
              "cannot use optional 'Str?' as 'Str' without unwrapping (use "
              "match)");
}

TEST(StrParse, OnlyAStrHasThem) {
  expectError(inMain("o = (3).toInt();"),
              "method call on non-class type 'int'");
  expectError("class P { }\n" + inMain("p = P(); o = p.toInt();"),
              "no method 'toInt' on type 'P'");
  // A class of the program may have its own method of that name.
  expectOk("class P { view fn toInt() -> int { return 1; } }\n" +
           inMain("p = P(); n: int = p.toInt();"));
}

TEST(StrParse, AnotherSpellingOfTheTypeNamesTheMethod) {
  expectError(inMain(R"(o = "1".toI64();)"),
              "no method 'toI64' on type 'Str' (an 'i64' is an 'int': parse "
              "it with 'toInt()')");
  expectError(inMain(R"(o = "1".toU64();)"),
              "no method 'toU64' on type 'Str' (a 'u64' is an 'unsigned': "
              "parse it with 'toUnsigned()')");
  expectError(inMain(R"(o = "1".toF64();)"),
              "no method 'toF64' on type 'Str' (an 'f64' is a 'float': parse "
              "it with 'toFloat()')");
  auto r = semaCheck(inMain(R"(o = "1".toChar();)"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("no method 'toChar' on type 'Str'\n"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(StrParse, InGenericBodiesAndOnViews) {
  expectOk(R"(
    fn first<T>(x: T) -> int? { return x.toInt(); }
    fn lenient(s: view Str) -> u8? { return s.toU8(); }
    fn main() -> int {
      a: int? = first("12");
      b: u8? = lenient("7");
      c: float? = "2.5".toFloat();
      return 0;
    }
  )");
  // Checked per instantiation: an `int` has no `toInt()`.
  expectError(R"(
    fn first<T>(x: T) -> int? { return x.toInt(); }
    fn main() -> int { a: int? = first(12); return 0; }
  )",
              "method call on non-class type 'int'");
}
