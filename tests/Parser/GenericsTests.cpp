// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: generic class / function declarations, type arguments in
// every type position, nested arguments, and explicit call type arguments.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::ast;
using namespace paykan::test;

// ─── declarations ───────────────────────────────────────────────────────────

TEST(Generics, GenericClassDecl) {
  auto [ok, driver] = parse(R"(
    class Box<T> {
      v: T;
      fn __init__(v: T) { self.v = v; }
      fn get() -> T { return self.v; }
    }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(ok);
  auto *tu = driver->getRoot();
  // Templates are kept apart from concrete classes.
  ASSERT_EQ(tu->getGenericClassDecls().size(), 1u);
  EXPECT_TRUE(tu->getClassDecls().empty());
  auto *cd = tu->getGenericClassDecls()[0];
  EXPECT_TRUE(cd->isGeneric());
  ASSERT_EQ(cd->getTypeParams().size(), 1u);
  EXPECT_EQ(*cd->getTypeParams()[0], "T");
  // A type parameter in a field annotation parses as an (unknown) class stub
  // that Sema substitutes.
  ASSERT_EQ(cd->getFields().size(), 1u);
  auto *fieldTy = dyn_cast<ClassType>(cd->getFields()[0]->getType());
  ASSERT_NE(fieldTy, nullptr);
  EXPECT_EQ(fieldTy->getName(), "T");
}

TEST(Generics, GenericClassMultipleParamsAndSuperclass) {
  auto [ok, driver] = parse(R"(
    class Base {}
    class Pair<K, V> : Base { k: K; v: V; }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(ok);
  auto *tu = driver->getRoot();
  ASSERT_EQ(tu->getGenericClassDecls().size(), 1u);
  auto *cd = tu->getGenericClassDecls()[0];
  ASSERT_EQ(cd->getTypeParams().size(), 2u);
  EXPECT_EQ(*cd->getTypeParams()[0], "K");
  EXPECT_EQ(*cd->getTypeParams()[1], "V");
  EXPECT_EQ(cd->getSuperClassName(), "Base");
  EXPECT_EQ(tu->getClassDecls().size(), 1u);
}

TEST(Generics, GenericFuncDecl) {
  auto [ok, driver] = parse(R"(
    fn first<T>(xs: T[]) -> T { return xs[0]; }
    fn swap<A, B>(a: A, b: B) { }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(ok);
  auto *tu = driver->getRoot();
  ASSERT_EQ(tu->getGenericFuncDecls().size(), 2u);
  ASSERT_EQ(tu->getFuncDecls().size(), 1u); // main only
  auto *first = tu->getGenericFuncDecls()[0];
  EXPECT_TRUE(first->isGeneric());
  ASSERT_EQ(first->getTypeParams().size(), 1u);
  EXPECT_EQ(*first->getTypeParams()[0], "T");
  // Parameter `xs: T[]` is an array of the stub `T`.
  auto *arr = dyn_cast<ArrayType>(first->getParams()[0].ParamType);
  ASSERT_NE(arr, nullptr);
  auto *elem = dyn_cast<ClassType>(arr->getElementType());
  ASSERT_NE(elem, nullptr);
  EXPECT_EQ(elem->getName(), "T");
  EXPECT_EQ(tu->getGenericFuncDecls()[1]->getTypeParams().size(), 2u);
}

TEST(Generics, EmptyTypeParamListIsSyntaxError) {
  auto [ok, _] = parse("class Box<> {}\nfn main() -> int { return 0; }");
  EXPECT_FALSE(ok);
}

// ─── type arguments in type positions ───────────────────────────────────────

// Find the single VarDecl of main's body.
static VarDecl *firstVarDecl(TranslationUnit *tu) {
  for (auto *fn : tu->getFuncDecls())
    if (fn->getName() == "main")
      for (auto *s : fn->getBody()->getStatements())
        if (auto *ds = dyn_cast<DeclStmt>(s))
          return cast<VarDecl>(ds->getDecl());
  return nullptr;
}

TEST(Generics, TypeArgsInVariableAnnotation) {
  auto [ok, driver] = parse(R"(
    fn main() -> int { b: Box<int> = None; return 0; }
  )");
  ASSERT_TRUE(ok);
  auto *vd = firstVarDecl(driver->getRoot());
  ASSERT_NE(vd, nullptr);
  auto *gt = dyn_cast<GenericType>(vd->getType());
  ASSERT_NE(gt, nullptr);
  EXPECT_EQ(gt->getName(), "Box");
  ASSERT_EQ(gt->getNumArgs(), 1u);
  EXPECT_TRUE(isa<BuiltinType>(gt->getArgs()[0]));
}

TEST(Generics, NestedAndMultipleTypeArgs) {
  auto [ok, driver] = parse(R"(
    fn main() -> int { p: Pair<Str, Box<Box<int>>> = None; return 0; }
  )");
  ASSERT_TRUE(ok);
  auto *vd = firstVarDecl(driver->getRoot());
  ASSERT_NE(vd, nullptr);
  auto *pair = dyn_cast<GenericType>(vd->getType());
  ASSERT_NE(pair, nullptr);
  EXPECT_EQ(pair->getName(), "Pair");
  ASSERT_EQ(pair->getNumArgs(), 2u);
  EXPECT_TRUE(isa<ClassType>(pair->getArgs()[0])); // Str (known at parse time)
  auto *outer = dyn_cast<GenericType>(pair->getArgs()[1]);
  ASSERT_NE(outer, nullptr);
  EXPECT_EQ(outer->getName(), "Box");
  ASSERT_EQ(outer->getNumArgs(), 1u);
  auto *inner = dyn_cast<GenericType>(outer->getArgs()[0]);
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(inner->getName(), "Box");
  EXPECT_TRUE(isa<BuiltinType>(inner->getArgs()[0]));
}

TEST(Generics, ArrayOfInstantiationAndInstantiationOfArray) {
  auto [ok, driver] = parse(R"(
    fn main() -> int { a: Box<int>[] = []; return 0; }
  )");
  ASSERT_TRUE(ok);
  auto *vd = firstVarDecl(driver->getRoot());
  ASSERT_NE(vd, nullptr);
  auto *arr = dyn_cast<ArrayType>(vd->getType());
  ASSERT_NE(arr, nullptr);
  EXPECT_TRUE(isa<GenericType>(arr->getElementType()));

  auto [ok2, driver2] = parse(R"(
    fn main() -> int { b: Box<int[]> = None; return 0; }
  )");
  ASSERT_TRUE(ok2);
  auto *vd2 = firstVarDecl(driver2->getRoot());
  ASSERT_NE(vd2, nullptr);
  auto *gt = dyn_cast<GenericType>(vd2->getType());
  ASSERT_NE(gt, nullptr);
  EXPECT_TRUE(isa<ArrayType>(gt->getArgs()[0]));
}

TEST(Generics, TypeArgsInParamsReturnFieldsAndMatchArms) {
  auto [ok, driver] = parse(R"(
    class Holder { b: Box<int>; }
    fn take(b: Box<Str>, p: Pair<int, int>) -> Box<int> {
      match b {
        x: Box<Str> { return None; }
        Box<int> { return None; }
        _ { return None; }
      }
    }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(ok);
  auto *tu = driver->getRoot();
  ASSERT_EQ(tu->getClassDecls().size(), 1u);
  EXPECT_TRUE(
      isa<GenericType>(tu->getClassDecls()[0]->getFields()[0]->getType()));
  auto *take = tu->getFuncDecls()[0];
  ASSERT_EQ(take->getName(), "take");
  EXPECT_TRUE(isa<GenericType>(take->getParams()[0].ParamType));
  EXPECT_TRUE(isa<GenericType>(take->getParams()[1].ParamType));
  EXPECT_TRUE(isa<GenericType>(take->getReturnType()));
  auto *ms = dyn_cast<MatchStmt>(take->getBody()->getStatements()[0]);
  ASSERT_NE(ms, nullptr);
  ASSERT_EQ(ms->getArms().size(), 3u);
  EXPECT_TRUE(isa<GenericType>(ms->getArms()[0]->getArmType()));
  EXPECT_EQ(ms->getArms()[0]->getBinding(), "x");
  EXPECT_TRUE(isa<GenericType>(ms->getArms()[1]->getArmType()));
}

TEST(Generics, QualifiedGenericTypeParses) {
  auto [ok, driver] = parse(R"(
    fn main() -> int { b: lib::Box<int> = None; return 0; }
  )");
  ASSERT_TRUE(ok);
  auto *vd = firstVarDecl(driver->getRoot());
  ASSERT_NE(vd, nullptr);
  auto *gt = dyn_cast<GenericType>(vd->getType());
  ASSERT_NE(gt, nullptr);
  EXPECT_EQ(gt->getName(), "lib::Box");
}

// ─── explicit type arguments on calls ───────────────────────────────────────

// Find the first CallExpr in main (as an initializer or expression statement).
static CallExpr *firstCall(TranslationUnit *tu) {
  for (auto *fn : tu->getFuncDecls())
    if (fn->getName() == "main")
      for (auto *s : fn->getBody()->getStatements()) {
        if (auto *ds = dyn_cast<DeclStmt>(s))
          return dyn_cast<CallExpr>(
              cast<VarDecl>(ds->getDecl())->getInitExpr());
        if (auto *es = dyn_cast<ExprStmt>(s))
          return dyn_cast<CallExpr>(es->getExpr());
        if (auto *as = dyn_cast<AssignStmt>(s))
          return dyn_cast<CallExpr>(as->getValue());
      }
  return nullptr;
}

TEST(Generics, ExplicitCallTypeArgs) {
  auto [ok, driver] = parse(R"(
    fn main() -> int { first<int>(xs); return 0; }
  )");
  ASSERT_TRUE(ok);
  auto *call = firstCall(driver->getRoot());
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(call->getCalleeName(), "first");
  ASSERT_TRUE(call->hasTypeArgs());
  ASSERT_EQ(call->getTypeArgs().size(), 1u);
  EXPECT_TRUE(isa<BuiltinType>(call->getTypeArgs()[0]));
  EXPECT_EQ(call->getNumArguments(), 1u);
}

TEST(Generics, ExplicitCtorTypeArgsNested) {
  auto [ok, driver] = parse(R"(
    fn main() -> int { b = Box<Box<int>>(inner); return 0; }
  )");
  ASSERT_TRUE(ok);
  auto *call = firstCall(driver->getRoot());
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(call->getCalleeName(), "Box");
  ASSERT_EQ(call->getTypeArgs().size(), 1u);
  auto *inner = dyn_cast<GenericType>(call->getTypeArgs()[0]);
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(inner->getName(), "Box");
}

TEST(Generics, ExplicitCallTypeArgsMultipleAndQualified) {
  auto [ok, driver] = parse(R"(
    fn main() -> int { p = Pair<Str, int>("a", 1); return 0; }
  )");
  ASSERT_TRUE(ok);
  auto *call = firstCall(driver->getRoot());
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(call->getTypeArgs().size(), 2u);
  EXPECT_EQ(call->getNumArguments(), 2u);

  auto [ok2, driver2] = parse(R"(
    fn main() -> int { b = lib::Box<int>(1); return 0; }
  )");
  ASSERT_TRUE(ok2);
  auto *call2 = firstCall(driver2->getRoot());
  ASSERT_NE(call2, nullptr);
  EXPECT_EQ(call2->getCalleeName(), "lib::Box");
  EXPECT_EQ(call2->getTypeArgs().size(), 1u);
}

// A '<' after an identifier that is NOT followed by `... > (` is still the
// relational operator: comparisons keep parsing as before.
TEST(Generics, LessThanStaysRelational) {
  auto [ok, driver] = parse(R"(
    fn main() -> int {
      i: int = 0; n: int = 3;
      while (i < n) { i = i + 1; }
      b: bool = i < n;
      c: bool = (i < n) && (n > i);
      if (i < n) { return 1; }
      return 0;
    }
  )");
  ASSERT_TRUE(ok);
  auto *tu = driver->getRoot();
  // No call in main carries type arguments.
  for (auto *s : tu->getFuncDecls()[0]->getBody()->getStatements())
    if (auto *ds = dyn_cast<DeclStmt>(s))
      if (auto *ce =
              dyn_cast<CallExpr>(cast<VarDecl>(ds->getDecl())->getInitExpr()))
        EXPECT_FALSE(ce->hasTypeArgs());
}

// `a < f<int>(x)`: the outer '<' is relational, the inner one opens type
// arguments (the lexer re-examines tokens it queued while looking ahead).
TEST(Generics, RelationalAgainstGenericCall) {
  auto [ok, driver] = parse(R"(
    fn main() -> int { b: bool = a < f<int>(x); return 0; }
  )");
  ASSERT_TRUE(ok);
  auto *vd = firstVarDecl(driver->getRoot());
  ASSERT_NE(vd, nullptr);
  auto *cmp = dyn_cast<BinaryExpr>(vd->getInitExpr());
  ASSERT_NE(cmp, nullptr);
  EXPECT_EQ(cmp->getOpcode(), BinaryOpcode::Lt);
  auto *call = dyn_cast<CallExpr>(cmp->getRHS());
  ASSERT_NE(call, nullptr);
  EXPECT_TRUE(call->hasTypeArgs());
}

// ─── type arguments that are tuple / optional types (#4, #5) ────────────────

// The lookahead that tells `f<...>(` from a comparison must scan over the
// tokens of tuple and optional types too.
TEST(Generics, TupleAndOptionalTypeArgsOnCalls) {
  auto [ok, driver] = parse(R"(
    fn main() -> int { b = Box<(int, Str)>(t); return 0; }
  )");
  ASSERT_TRUE(ok);
  auto *call = firstCall(driver->getRoot());
  ASSERT_NE(call, nullptr);
  ASSERT_EQ(call->getTypeArgs().size(), 1u);
  auto *tt = dyn_cast<TupleType>(call->getTypeArgs()[0]);
  ASSERT_NE(tt, nullptr);
  EXPECT_EQ(tt->getArity(), 2u);

  auto [ok2, driver2] = parse(R"(
    fn main() -> int { m = ident<Node?>(n); return 0; }
  )");
  ASSERT_TRUE(ok2);
  auto *call2 = firstCall(driver2->getRoot());
  ASSERT_NE(call2, nullptr);
  ASSERT_EQ(call2->getTypeArgs().size(), 1u);
  EXPECT_TRUE(isa<OptionalType>(call2->getTypeArgs()[0]));

  auto [ok3, driver3] = parse(R"(
    fn main() -> int { p = Pair<Node?[], (int, Str)?>(a, b); return 0; }
  )");
  ASSERT_TRUE(ok3);
  auto *call3 = firstCall(driver3->getRoot());
  ASSERT_NE(call3, nullptr);
  EXPECT_EQ(call3->getTypeArgs().size(), 2u);
}

// Comparisons whose right operand is parenthesised are still comparisons.
TEST(Generics, ParenthesisedComparisonStaysRelational) {
  auto [ok, driver] = parse(R"(
    fn main() -> int { b: bool = a < (c + 1); return 0; }
  )");
  ASSERT_TRUE(ok);
  auto *vd = firstVarDecl(driver->getRoot());
  ASSERT_NE(vd, nullptr);
  auto *cmp = dyn_cast<BinaryExpr>(vd->getInitExpr());
  ASSERT_NE(cmp, nullptr);
  EXPECT_EQ(cmp->getOpcode(), BinaryOpcode::Lt);
}
