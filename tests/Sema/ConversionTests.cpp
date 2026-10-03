// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: conversion constructors `Target<Source>(value)` (#64) — every
// supported pair and its result type, unsupported pairs, wrong arity and
// argument types, and what became of the removed builtins.

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
  )"));
}

TEST(Conversion, ParsesReturnOptionals) {
  expectError(inMain("n: int = int<Str>(\"1\");"),
              "cannot use optional 'int?' as 'int' without unwrapping");
  expectError(inMain("f: float = float<Str>(\"1\");"),
              "cannot use optional 'float?' as 'float' without unwrapping");
  expectOk(inMain(R"(
    match int<Str>("12") { n: int { x: int = n; } None { } }
    if (float<Str>("x") == None) { }
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

TEST(Conversion, UnsupportedPairsListTheValidSources) {
  expectError(inMain("x = int<int>(1);"),
              "no conversion from 'int' to 'int'; 'int<...>' converts from "
              "'Str', 'float', 'bool' or 'char'");
  expectError(inMain("x = Str<Str>(\"a\");"),
              "no conversion from 'Str' to 'Str'; 'Str<...>' converts from "
              "'int', 'float', 'bool' or 'char'");
  expectError(inMain("x = bool<float>(1.5);"),
              "no conversion from 'float' to 'bool'; 'bool<...>' converts "
              "from 'int'");
  expectError(inMain("x = char<Str>(\"a\");"),
              "no conversion from 'Str' to 'char'; 'char<...>' converts from "
              "'int'");
  expectError(inMain("x = float<bool>(True);"),
              "no conversion from 'bool' to 'float'; 'float<...>' converts "
              "from 'Str' or 'int'");
  expectError(R"(
    class Node { fn __init__() { } }
    fn main() -> int { x = Str<Node>(Node()); return 0; }
  )",
              "no conversion from 'Node' to 'Str'");
  expectError(inMain("o: int? = 1;\n x = Str<int?>(o);"),
              "no conversion from 'int?' to 'Str'");
  expectError(inMain("x = int<Missing>(1);"),
              "source type of conversion 'int' has unknown class type "
              "'Missing'");
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

TEST(Conversion, TargetWithoutSourceType) {
  expectError(inMain("x = int(2.5);"),
              "a conversion to 'int' names its source type: write "
              "'int<Source>(value)'");
  expectError(inMain("x = char(97);"),
              "a conversion to 'char' names its source type");
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
    class Box<T> { v: T; fn __init__(x: T) { self.v = x; } }
    fn ident<T>(x: T) -> T { return x; }
    fn main() -> int {
      b = Box<int>(int<float>(1.5));
      s: Str = ident<Str>(Str<int>(b.v));
      return 0;
    }
  )");
}
