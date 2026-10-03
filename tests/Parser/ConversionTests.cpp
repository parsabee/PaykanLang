// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: conversion constructors `Target<Source>(value)` (#64).  A
// conversion is an ordinary call with one explicit type argument whose
// callee is a type name (`Str`, or a builtin type such as `int`); Sema tells
// it apart from a generic constructor call.  Every input is also parsed by
// the other built frontends and the ASTs compared (TestUtils.h).

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::ast;
using namespace paykan::test;

// The value of every `x = <expr>;` statement in main, in order.
static std::vector<Expr *> assignedValues(TranslationUnit *tu) {
  std::vector<Expr *> out;
  for (auto *fn : tu->getFuncDecls())
    if (fn->getName() == "main")
      for (auto *s : fn->getBody()->getStatements())
        if (auto *as = dyn_cast<AssignStmt>(s))
          out.push_back(as->getValue());
  return out;
}

TEST(Conversion, EveryPairParsesAsACallWithOneTypeArgument) {
  auto [ok, driver] = parse(R"(
    fn main() -> int {
      a = Str<int>(1);
      b = Str<float>(1.5);
      c = Str<bool>(True);
      d = Str<char>('c');
      e = int<Str>("1");
      f = float<Str>("1.5");
      g = int<float>(1.5);
      h = float<int>(1);
      i = int<bool>(True);
      j = bool<int>(1);
      k = int<char>('c');
      l = char<int>(99);
      return 0;
    }
  )");
  ASSERT_TRUE(ok);
  auto values = assignedValues(driver->getRoot());
  const char *const callees[] = {"Str", "Str",   "Str", "Str",  "int", "float",
                                 "int", "float", "int", "bool", "int", "char"};
  const char *const sources[] = {"int",  "float", "bool",  "char",
                                 "Str",  "Str",   "float", "int",
                                 "bool", "int",   "char",  "int"};
  ASSERT_EQ(values.size(), 12u);
  for (size_t i = 0; i < values.size(); ++i) {
    auto *call = dyn_cast<CallExpr>(values[i]);
    ASSERT_NE(call, nullptr) << i;
    EXPECT_EQ(call->getCalleeName(), callees[i]) << i;
    ASSERT_EQ(call->getTypeArgs().size(), 1u) << i;
    EXPECT_EQ(typeName(call->getTypeArgs()[0]), sources[i]) << i;
    EXPECT_EQ(call->getNumArguments(), 1u) << i;
  }
}

TEST(Conversion, NestedAndInsideExpressions) {
  auto [ok, driver] = parse(R"(
    fn main() -> int {
      a = 3;
      b = 4;
      s = Str<float>(float<int>(a) * 2.0) + Str<bool>(a < b);
      n = int<float>(2.5) + int<bool>(a > b) * int<char>('a');
      m = Str<int>(n).len();
      t = (int<Str>("1"), char<int>(65));
      xs = [int<float>(1.0), a < b];
      if (a < b && b > a) { }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Conversion, ComparisonsAreNotTypeArguments) {
  // `a < b` and `a > (b)` stay relational: only `IDENT < types > (` with a
  // matching `>` directly before `(` is a type-argument list.
  auto [ok, driver] = parse(R"(
    fn main() -> int {
      a = 1;
      b = 2;
      c = a < b;
      d = a > (b);
      e = (a < b) == (b > a);
      return 0;
    }
  )");
  ASSERT_TRUE(ok);
  auto values = assignedValues(driver->getRoot());
  ASSERT_EQ(values.size(), 5u);
  for (size_t i = 2; i < values.size(); ++i)
    EXPECT_EQ(dyn_cast<CallExpr>(values[i]), nullptr) << i;
}

TEST(Conversion, GenericClassesAreUnaffected) {
  auto [ok, driver] = parse(R"(
    class Box<T> { v: T; fn __init__(x: T) { self.v = x; } }
    class Pair<K, V> { k: K; v: V; fn __init__(a: K, b: V) { self.k = a; self.v = b; } }
    fn main() -> int {
      b = Box<int>(int<float>(1.5));
      p: Pair<Str, int> = Pair<Str, int>(Str<int>(1), 2);
      return 0;
    }
  )");
  ASSERT_TRUE(ok);
  auto values = assignedValues(driver->getRoot());
  ASSERT_EQ(values.size(), 1u);
  auto *box = dyn_cast<CallExpr>(values[0]);
  ASSERT_NE(box, nullptr);
  EXPECT_EQ(box->getCalleeName(), "Box");
  ASSERT_EQ(box->getNumArguments(), 1u);
  auto *inner = dyn_cast<CallExpr>(box->getArguments()[0]);
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(inner->getCalleeName(), "int");
}

TEST(Conversion, MalformedConversionsAreSyntaxErrors) {
  EXPECT_FALSE(parse("fn main() -> int { x = int<>(1); return 0; }").Ok);
  EXPECT_FALSE(parse("fn main() -> int { x = Str<int>; return 0; }").Ok);
}
