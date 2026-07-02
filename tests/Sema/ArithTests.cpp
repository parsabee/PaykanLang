// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: literals, type checking, variables, operators, builtins.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// ============================================================================
// Literals & type-checking
// ============================================================================

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

// ============================================================================
// Variable lifecycle
// ============================================================================

TEST(Arith, UndeclaredVariable) {
  auto r = semaCheck(wrapMain("println(x);"));
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

// ============================================================================
// Operator type checks
// ============================================================================

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

// ============================================================================
// Builtins
// ============================================================================

TEST(Arith, OutAcceptsMultipleArgs) {
  auto r = semaCheck(wrapMain(R"(
    a: Str = "hello";
    println(a + StrInt(42));
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Arith, OutAcceptsLiterals) {
  auto r = semaCheck(wrapMain("println(\"hello\");"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}
