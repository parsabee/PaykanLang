// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: literals, type checking, variables, operators, builtins.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "import ::io; fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// -- Literals & type-checking

TEST(Arith, IntLiteral) {
  auto r = semaCheck(wrapMain("x: int = 42;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Arith, FloatLiteral) {
  auto r = semaCheck(wrapMain("x: float = 3.14;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Arith, BoolLiteral) {
  auto r = semaCheck(wrapMain("x: bool = True;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Arith, StringLiteral) {
  auto r = semaCheck(wrapMain("x: Str = \"hello\";"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Arith, IntToFloatPromotion) {
  auto r = semaCheck(wrapMain("x: float = 42;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Arith, TypeMismatchInit) {
  auto r = semaCheck(wrapMain("x: int = \"hello\";"));
  EXPECT_FALSE(r.Ok);
}

TEST(Arith, TypeMismatchAssign) {
  auto r = semaCheck(wrapMain("x: int = 1;\n  x = \"hello\";"));
  EXPECT_FALSE(r.Ok);
}

// -- Variable lifecycle

TEST(Arith, UndeclaredVariable) {
  auto r = semaCheck(wrapMain("io::println(x);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("undeclared"), std::string::npos);
}

TEST(Arith, DuplicateDeclaration) {
  auto r = semaCheck(wrapMain("x: int = 1;\n  x: int = 2;"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("redeclaration"), std::string::npos);
}

TEST(Arith, StringReassignAllowed) {
  auto r = semaCheck(wrapMain("a: Str = \"x\";\n  a = \"y\";"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Arith, UnknownTypeInVarDecl) {
  auto r = semaCheck(R"(
    fn main() -> int {
      x: FooBar = 42;
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("unknown class type 'FooBar'"),
            std::string::npos);
}

// -- Operator type checks

TEST(Arith, ArithmeticOnBool) {
  auto r = semaCheck(wrapMain("x: bool = True + False;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Arith, NegateOnBool) {
  auto r = semaCheck(wrapMain("x: int = -True;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Arith, NotOnInt) {
  auto r = semaCheck(wrapMain("x: bool = !42;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Arith, StringConcatenation) {
  auto r = semaCheck(wrapMain(R"(
    a: Str = "hello";
    b: Str = "world";
    c: Str = a + b;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Arith, EqualityTypeMismatch) {
  auto r = semaCheck(wrapMain("x: bool = 1 == \"hello\";"));
  EXPECT_FALSE(r.Ok);
}

// `==` and `!=` take an `int` with a `float`, either way round, as `<` and
// the arithmetic do: the int is converted to float.  They were `operands of
// '==' have mismatched types 'int' and 'float'`.
TEST(Arith, EqualityPromotesIntWithFloat) {
  auto r = semaCheck(wrapMain(R"(
    x = 2;
    y = 2.0;
    a: bool = x == y;
    b: bool = y != x;
    c: bool = 1 == 1.0;
    d: bool = 2.5 != x;
    e: bool = x <= y;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0) << r.Diagnostics;
}

// The promotion is between two operands of `int` and `float` only: a tuple
// whose elements differ so is another type, and a sized type has none (a
// literal takes its type instead).
TEST(Arith, EqualityPromotesNoOtherPair) {
  auto r = semaCheck(wrapMain(R"(
    t = (1, 2) == (1, 2.0);
    n: i32 = 2;
    y = 1.0;
    z = n == y;
    f: f32 = 1.5;
    w = f != 2;
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 2) << r.Diagnostics;
  for (const char *e :
       {"operands of '==' have mismatched types '(int, int)' and '(int, "
        "float)'",
        "operator '==' is not defined for types 'i32' and 'float'"})
    EXPECT_NE(r.Diagnostics.find(e), std::string::npos) << e << "\n"
                                                        << r.Diagnostics;
}

// A char orders against a char only, as it equals one only: `c < 100` and
// `c < n` were accepted and failed PIR verification (`cmp operands have
// different types: char and i64`), while `c == 100` was this error.
TEST(Arith, CharComparesWithACharOnly) {
  auto r = semaCheck(wrapMain(R"(
    c = 'a';
    n = 100;
    u: u8 = 7;
    a = c < 100;
    b = c < n;
    d = 100 > c;
    e = c <= n;
    f = c >= 3;
    g = c > u;
    h = c < 1.5;
    i = c == n;
  )"));
  EXPECT_FALSE(r.Ok);
  for (const char *e : {"operands of '<' have mismatched types 'char' and "
                        "'int'",
                        "operands of '>' have mismatched types 'int' and "
                        "'char'",
                        "operands of '<=' have mismatched types 'char' and "
                        "'int'",
                        "operands of '>=' have mismatched types 'char' and "
                        "'int'",
                        "operands of '>' have mismatched types 'char' and "
                        "'u8'",
                        "operands of '<' have mismatched types 'char' and "
                        "'float'",
                        "operands of '==' have mismatched types 'char' and "
                        "'int'"})
    EXPECT_NE(r.Diagnostics.find(e), std::string::npos) << e << "\n"
                                                        << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 8u) << r.Diagnostics;
  auto ok = semaCheck(wrapMain(R"(
    c = 'a';
    a: bool = c < 'd' && 'z' >= c && int(c) < 100 && 100 > int(c);
  )"));
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;
}

// -- Builtins

TEST(Arith, OutAcceptsMultipleArgs) {
  auto r = semaCheck(wrapMain(R"(
    a: Str = "hello";
    io::println(a + Str<int>(42));
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Arith, OutAcceptsLiterals) {
  auto r = semaCheck(wrapMain("io::println(\"hello\");"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Compound assignment: `t op= v` is checked as `t = t op v`

TEST(Arith, CompoundAssignment) {
  auto r = semaCheck(wrapMain(R"(
    n = 10;
    n += 5; n -= 3; n *= 4; n /= 6; n %= 5;
    s = "a";
    s += "b";
    b: u8 = 250;
    b += 10;
    xs = [1, 2];
    xs[0] *= 3;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Arith, CompoundAssignmentErrors) {
  auto r = semaCheck("fn f(x: view int) { x += 1; }\n" + wrapMain(R"(
    let c = 1;
    c += 1;
    s = "a";
    s -= "x";
    zz += 1;
    for i in 0..3 { i *= 2; }
  )"));
  EXPECT_FALSE(r.Ok);
  for (const char *e :
       {"cannot assign to 'view' parameter 'x'",
        "'c' is declared with 'let' and cannot be reassigned",
        "operator '-' is not defined for types 'Str' and 'Str'",
        "use of undeclared variable 'zz'",
        "'i' is the variable of a 'for' loop and cannot be assigned"})
    EXPECT_NE(r.Diagnostics.find(e), std::string::npos) << e << "\n"
                                                        << r.Diagnostics;
}
