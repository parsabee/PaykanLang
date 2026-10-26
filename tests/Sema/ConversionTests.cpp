// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: conversion constructors `Target<Source>(value)` (#64, #88) —
// every specialization of each builtin target and its result type, the
// closed set (unlisted and custom type arguments), the missing-specialization
// hint, a parse written as a conversion (a method of `Str` now), wrong arity
// and argument types, and what became of the removed builtins.

#include "TestUtils.h"
#include <gtest/gtest.h>

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

/// The one error of @p body, whose text is @p message (the whole line after
/// `error: `).
static void expectOneError(const std::string &body,
                           const std::string &message) {
  auto r = semaCheck(inMain(body));
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("error: " + message + "\n"), std::string::npos)
      << r.Diagnostics;
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
                 "are int");
  expectOneError("o = char<Str>(\"a\");",
                 "no specialization of 'char' for 'Str'; its specializations "
                 "are int");
  // An optional string is no `Str`: the ordinary error.
  expectOneError("s: Str? = \"1\"; o = int(s);",
                 "no specialization of 'int' for 'Str?'; its specializations "
                 "are float, bool, char, i8, i16, i32, u8, u16, u32, unsigned, "
                 "f16, f32");
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

// ─── unsupported pairs ──────────────────────────────────────────────────────

TEST(Conversion, UnlistedTypeArgumentsNameTheClosedSet) {
  expectError(inMain("x = int<int>(1);"),
              "no specialization of 'int' for 'int'; its specializations "
              "are float, bool, char");
  expectError(inMain("x = Str<Str>(\"a\");"),
              "no specialization of 'Str' for 'Str'; its specializations "
              "are int, float, bool, char");
  expectError(inMain("x = bool<float>(1.5);"),
              "no specialization of 'bool' for 'float'; its specializations "
              "are int\n");
  expectError(inMain("x = char<Str>(\"a\");"),
              "no specialization of 'char' for 'Str'; its specializations "
              "are int");
  expectError(inMain("x = float<bool>(True);"),
              "no specialization of 'float' for 'bool'; its specializations "
              "are int, i8");
  expectError(inMain("x = Str<Obj>(\"a\");"),
              "no specialization of 'Str' for 'Obj'");
  expectError(inMain("o: int? = 1;\n x = Str<int?>(o);"),
              "no specialization of 'Str' for 'int?'");
  expectError(inMain("x = int<Missing>(1);"),
              "source type of conversion 'int' has unknown class type "
              "'Missing'");
}

TEST(Conversion, CustomTypesAreNeverSpecializations) {
  // A user class can't add specializations; toString() stringifies it.
  const std::string point = "class Point {}\n"
                            "impl Point { fn __init__() { } }\n";
  expectError(point + "fn main() -> int { x = Str<Point>(Point()); return 0; }",
              "no specialization of 'Str' for 'Point'; its specializations "
              "are int, float, bool, char");
  expectError(point + "fn main() -> int { x = int<Point>(Point()); return 0; }",
              "no specialization of 'int' for 'Point'; its specializations "
              "are float, bool, char");
  expectError("enum Color { Red }\n"
              "fn main() -> int { x = Str<Color>(Color::Red); return 0; }",
              "no specialization of 'Str' for 'Color'");
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
  // No widening and no unwrapping: the argument's own type must be a source.
  expectError(inMain("x = float(2.5);"),
              "no specialization of 'float' for 'float'; its specializations "
              "are int, i8");
  expectError(inMain("x = char('a');"),
              "no specialization of 'char' for 'char'; its specializations "
              "are int");
  expectError(inMain("x = bool(1.5);"),
              "no specialization of 'bool' for 'float'; its specializations "
              "are int\n");
  expectError(inMain("o: int? = 1;\n x = Str(o);"),
              "no specialization of 'Str' for 'int?'; its specializations "
              "are int, float, bool, char");
  expectError(inMain("o: int? = 1;\n x = int(o);"),
              "no specialization of 'int' for 'int?'; its specializations "
              "are float, bool, char");
}

TEST(Conversion, InferredFormRejectsEveryOtherType) {
  const std::string decls = "class A {}\n"
                            "impl A { fn __init__() { } }\n"
                            "enum Color { Red }\n";
  auto prog = [&](const std::string &body) {
    return decls + "import ::io; fn main() -> int {\n" + body +
           "\n  return 0;\n}\n";
  };
  const std::string strSet =
      "; its specializations are int, float, bool, char, "
      "i8, i16, i32, u8, u16, u32, unsigned, f16, f32";
  expectError(prog("a = A(); s = Str(a);"),
              "no specialization of 'Str' for 'A'" + strSet);
  expectError(prog("s = Str(Color::Red);"),
              "no specialization of 'Str' for 'Color'" + strSet);
  expectError(prog("o = None; s = Str(o);"),
              "no specialization of 'Str' for 'Obj'" + strSet);
  expectError(prog("s = Str(None);"),
              "no specialization of 'Str' for 'Obj'" + strSet);
  expectError(prog("xs: int[1] = [1]; s = Str(xs);"),
              "no specialization of 'Str' for 'int[1]'" + strSet);
  expectError(prog("a = A(); n = int(a);"),
              "no specialization of 'int' for 'A'; its specializations are "
              "float, bool, char, i8, i16, i32, u8, u16, u32, unsigned, f16, "
              "f32");
  expectError(prog("n = float(None);"),
              "no specialization of 'float' for 'Obj'; its specializations are "
              "int, i8, i16, i32, u8, u16, u32, unsigned, f16, f32");
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
  // `Str(s)` with a `Str` is not a conversion.
  expectOk(inMain(R"(
    s = "abc";
    t: Str = Str(s);
    u: Str = Str("lit");
  )"));
  expectError(inMain("o: Str? = \"a\";\n t = Str(o);"),
              "cannot use optional 'Str?' as 'Str' without unwrapping");
  expectError(inMain("t = Str();"), "function 'Str' expects 1 argument(s)");
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
  expectError(inMain("b: u8 = 1; s = \"c\" + b;"),
              "operator '+' is not defined for types 'Str' and 'u8'");
  expectOk(inMain("n: int = 3; s: Str = \"a\" + Str(n) + \"b\";"));
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
