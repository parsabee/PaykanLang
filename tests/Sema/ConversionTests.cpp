// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: conversion constructors `Target<Source>(value)` (#64, #88) —
// every specialization of each builtin target and its result type, the
// closed set (unlisted and custom type arguments), the missing-specialization
// hint, wrong arity and argument types, and what became of the removed
// builtins.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string inMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
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
    i: int = 1;  f: float = 1.5;  b: bool = True;  c: char = 'c';  s: Str = "1";
    s1: Str = Str<int>(i);
    s2: Str = Str<float>(f);
    s3: Str = Str<bool>(b);
    s4: Str = Str<char>(c);
    p1: int? = int<Str>(s);
    p2: float? = float<Str>(s);
    n1: int = int<float>(f);
    n2: float = float<int>(i);
    n3: int = int<bool>(b);
    n4: bool = bool<int>(i);
    n5: int = int<char>(c);
    n6: char = char<int>(i);
    q1: bool? = bool<Str>(s);
  )"));
}

TEST(Conversion, BoolParseReturnsAnOptional) {
  expectError(inMain("x: bool = bool<Str>(\"True\");"),
              "cannot use optional 'bool?' as 'bool' without unwrapping");
  expectOk(inMain(R"(
    match bool<Str>("True") { b: bool { c: bool = !b; } None { } }
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

TEST(Conversion, ParsesReturnOptionals) {
  expectError(inMain("n: int = int<Str>(\"1\");"),
              "cannot use optional 'int?' as 'int' without unwrapping");
  expectError(inMain("f: float = float<Str>(\"1\");"),
              "cannot use optional 'float?' as 'float' without unwrapping");
  expectOk(inMain(R"(
    match int<Str>("12") { n: int { x: int = n; } None { } }
    if float<Str>("x") == None { }
  )"));
}

TEST(Conversion, InferredDeclarationsTakeTheResultType) {
  expectOk(inMain(R"(
    a = int<float>(2.5);
    b: int = a + 1;
    c = int<Str>("4");
    d: int? = c;
  )"));
}

// ─── unsupported pairs ──────────────────────────────────────────────────────

TEST(Conversion, UnlistedTypeArgumentsNameTheClosedSet) {
  expectError(inMain("x = int<int>(1);"),
              "no specialization of 'int' for 'int'; its specializations "
              "are Str, float, bool, char");
  expectError(inMain("x = Str<Str>(\"a\");"),
              "no specialization of 'Str' for 'Str'; its specializations "
              "are int, float, bool, char");
  expectError(inMain("x = bool<float>(1.5);"),
              "no specialization of 'bool' for 'float'; its specializations "
              "are int, Str");
  expectError(inMain("x = char<Str>(\"a\");"),
              "no specialization of 'char' for 'Str'; its specializations "
              "are int");
  expectError(inMain("x = float<bool>(True);"),
              "no specialization of 'float' for 'bool'; its specializations "
              "are Str, int");
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
              "are Str, float, bool, char");
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
  expectError(inMain("x = int<Str>(12);"),
              "argument of 'int<Str>' has type 'int', expected 'Str'");
  // An optional must be unwrapped first.
  expectError(inMain("o: int? = 1;\n x = Str<int>(o);"),
              "cannot use optional 'int?' as 'int' without unwrapping");
}

// ─── the inferred form: Target(value) (#88) ─────────────────────────────────

TEST(Conversion, InferredFormPicksEveryPair) {
  // Each declaration states the result type, so a wrong pick is an error.
  expectOk(inMain(R"(
    i: int = 1;  f: float = 1.5;  b: bool = True;  c: char = 'c';  s: Str = "1";
    s1: Str = Str(i);
    s2: Str = Str(f);
    s3: Str = Str(b);
    s4: Str = Str(c);
    p1: int? = int(s);
    p2: float? = float(s);
    q1: bool? = bool(s);
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
    p: int? = int("12");
    q: bool? = bool("True");
  )"));
}

TEST(Conversion, InferredFormIsTheExplicitOne) {
  // The same result types (and so the same optionals to unwrap).
  expectError(inMain("n: int = int(\"1\");"),
              "cannot use optional 'int?' as 'int' without unwrapping");
  expectError(inMain("x: float? = int(\"1\");"),
              "initializer of type 'int?' does not match declared type "
              "'float?'");
  expectOk(inMain(R"(
    a = int(2.5);
    b: int = a + 1;
    s = Str(b);
    t: Str = s;
    match int("4") { n: int { m: int = n; } None { } }
  )"));
}

TEST(Conversion, InferredFormNeedsAnExactSource) {
  // No widening and no unwrapping: the argument's own type must be a source.
  expectError(inMain("x = float(2.5);"),
              "no specialization of 'float' for 'float'; its specializations "
              "are Str, int");
  expectError(inMain("x = char('a');"),
              "no specialization of 'char' for 'char'; its specializations "
              "are int");
  expectError(inMain("x = bool(1.5);"),
              "no specialization of 'bool' for 'float'; its specializations "
              "are int, Str");
  expectError(inMain("o: int? = 1;\n x = Str(o);"),
              "no specialization of 'Str' for 'int?'; its specializations "
              "are int, float, bool, char");
  expectError(inMain("x = int(int(\"1\"));"),
              "no specialization of 'int' for 'int?'; its specializations "
              "are Str, float, bool, char");
}

TEST(Conversion, InferredFormRejectsEveryOtherType) {
  const std::string decls = "class A {}\n"
                            "impl A { fn __init__() { } }\n"
                            "enum Color { Red }\n";
  auto prog = [&](const std::string &body) {
    return decls + "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
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
              "Str, float, bool, char, i8, i16, i32, u8, u16, u32, unsigned, "
              "f16, f32");
  expectError(prog("n = float(None);"),
              "no specialization of 'float' for 'Obj'; its specializations are "
              "Str, int, i8, i16, i32, u8, u16, u32, unsigned, f16, f32");
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
