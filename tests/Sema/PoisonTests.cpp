// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: poisoned binders (#89).  A binder whose declaration fails is
// still bound -- to the internal poison type -- and every use of it is
// absorbed silently, so only the original error is reported: never a
// follow-on "use of undeclared variable" or a type error about the poisoned
// value.  Like every Sema suite, these run once per enabled frontend.

#include "TestUtils.h"
#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace paykan::test;

namespace {

std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// A failing initializer: the conversion's source is written as `int`, but the
// argument is a float.
const std::string kBad = "Str<int>(2.5)";
const std::string kBadMsg = "argument of 'Str<int>' has type 'float'";

// Exactly one error, @p expected, and nothing about an undeclared variable.
void expectOnly(const std::string &src, const std::string &expected) {
  auto r = semaCheck(src);
  EXPECT_FALSE(r.Ok) << src;
  EXPECT_EQ(r.ErrorCount, 1u) << src << "\n" << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(expected), std::string::npos) << src << "\n"
                                                             << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("undeclared variable"), std::string::npos)
      << src << "\n"
      << r.Diagnostics;
}

} // namespace

// ============================================================================
// Inferred declarations: `a = <bad>`
// ============================================================================

// The issue's reproducer.
TEST(Poison, FailedFirstAssignmentReportsOnlyTheRealError) {
  expectOnly("fn main() -> int { n = 2.5; a = Str<int>(n); println(a); "
             "return 0; }",
             kBadMsg);
}

TEST(Poison, SeveralLaterUsesAreSilent) {
  expectOnly(
      wrapMain("a = " + kBad +
               ";\n println(a);\n b = a;\n x: int = a;\n print(Str(x));"),
      kBadMsg);
}

// Poison is absorbing: a value computed from a poisoned one is poisoned too.
TEST(Poison, PoisonPropagatesThroughDerivedVariables) {
  expectOnly(wrapMain("a = " + kBad +
                      ";\n b = a + 1;\n println(b);\n c = b.len();\n"
                      " println(Str(c));"),
             kBadMsg);
}

// Every kind of use of a poisoned variable, one program each.
TEST(Poison, EveryUseOfAPoisonedVariableIsSilent) {
  const std::string classes =
      "class P {\n  x: int;\n  fn __init__(x: int) { self.x = x; }\n}\n"
      "fn f(v: int) -> int { return v; }\n";
  const std::vector<std::string> uses = {
      "a.foo();",                                 // method call
      "y = a.x; println(y);",                     // field access
      "z = a[0]; println(z);",                    // subscript
      "xs = [1, 2]; println(Str(xs[a]));",        // as an array index
      "s = \"abc\"; println(Str(s[a]));",         // as a string index
      "f(a);",                                    // call argument
      "p = P(a); println(Str(p.x));",             // constructor argument
      "println(Str(f(a) + 1));",                  // nested in a call
      "if (a) { println(\"t\"); }",               // if condition
      "while (a < 3) { println(\"w\"); }",        // while condition
      "k = if a then 1 else 2; println(Str(k));", // ternary condition
      "q = -a; r = !a; println(Str(q)); println(Str(r));", // unary ops
      "t = a * 2 + 1 == 3 && a; println(Str(t));",         // binary ops
      "e = a == None; println(Str(e));",                   // comparison
      "a.x = 3;",                                          // member assignment
      "a[1] = 2;",                         // subscript assignment
      "xs = [1]; xs[0] = a;",              // assigned into an array
      "pp = P(1); pp.x = a;",              // assigned into a field
      "t = (a, 1); println(t.0);",         // tuple element
      "u = [a, a]; println(u);",           // array element
      "s = Str(a); println(s);",           // inferred conversion
      "s = Str<int>(a); println(s);",      // explicit conversion
      "m = mov a; println(m);",            // mov
      "match a { _ { println(\"m\"); } }", // match subject
      "o: Obj = a; println(o);",           // typed declaration
      "e: Error = a; println(e);",         // not the user-visible `Error` class
      "u, v = a; println(u); println(v);", // destructuring source
      "println(a.toString().len());",      // chained calls
  };
  for (const auto &use : uses) {
    SCOPED_TRACE(use);
    std::string body = "a = ";
    body += kBad;
    body += ";\n";
    body += use;
    expectOnly(classes + wrapMain(body), kBadMsg);
  }
}

TEST(Poison, ReturnOfAPoisonedVariableIsSilent) {
  expectOnly("fn g() -> int {\n  a = " + kBad + ";\n  return a;\n}\n" +
                 wrapMain("println(Str(g()));"),
             kBadMsg);
}

// An empty array literal without an annotation fails the declaration too.
TEST(Poison, UninferableEmptyArrayPoisonsTheVariable) {
  expectOnly(wrapMain("a = [];\n println(a);\n a.push(1);"),
             "cannot infer element type of empty array literal");
}

// ============================================================================
// Recovery
// ============================================================================

// A later valid assignment re-declares the variable with the value's type.
TEST(Poison, ValidReassignmentRedeclaresTheVariable) {
  expectOnly(wrapMain("a = " + kBad + ";\n a = 5;\n println(Str(a + 1));"),
             kBadMsg);
}

// ... and from then on it is checked as usual.
TEST(Poison, RedeclaredVariableIsCheckedAgain) {
  auto r = semaCheck(wrapMain("a = " + kBad + ";\n a = 5;\n s: Str = a;"));
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(kBadMsg), std::string::npos) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("initializer of type 'int' does not match "
                               "declared type 'Str'"),
            std::string::npos)
      << r.Diagnostics;
}

// A re-assignment in an inner scope re-declares the variable where it lives.
TEST(Poison, ReassignmentInANestedScopeRedeclaresTheOuterVariable) {
  expectOnly(wrapMain("a = " + kBad +
                      ";\n if (True) { a = 5; }\n println(Str(a + 1));"),
             kBadMsg);
}

// A failing re-assignment keeps the variable poisoned (and silent).
TEST(Poison, FailedReassignmentKeepsThePoison) {
  expectOnly(wrapMain("a = " + kBad + ";\n a = a + 1;\n println(a);"), kBadMsg);
}

// A failed re-assignment of a healthy variable keeps its type.
TEST(Poison, FailedReassignmentOfAHealthyVariableKeepsItsType) {
  auto r = semaCheck(wrapMain("a = 1;\n a = " + kBad + ";\n s: Str = a;"));
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("initializer of type 'int' does not match"),
            std::string::npos)
      << r.Diagnostics;
}

// ============================================================================
// Genuinely undeclared names
// ============================================================================

TEST(Poison, UndeclaredVariableIsStillReported) {
  auto r = semaCheck(wrapMain("println(nope);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("use of undeclared variable 'nope'"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Poison, UndeclaredVariableNextToAPoisonedOneIsStillReported) {
  auto r = semaCheck(
      wrapMain("a = " + kBad + ";\n println(a);\n println(nope + a);"));
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(kBadMsg), std::string::npos) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("use of undeclared variable 'nope'"),
            std::string::npos)
      << r.Diagnostics;
}

// The poison is a binding like any other: it ends with its scope.
TEST(Poison, PoisonDoesNotOutliveItsScope) {
  auto r = semaCheck(
      wrapMain("if (True) { a = " + kBad + "; println(a); }\n println(a);"));
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("use of undeclared variable 'a'"),
            std::string::npos)
      << r.Diagnostics;
}

// ============================================================================
// Typed declarations: `x: T = <bad>`
// ============================================================================

TEST(Poison, UnknownDeclaredTypePoisonsTheVariable) {
  expectOnly(wrapMain("x: Foo = 1;\n println(x);\n y = x + 1;\n"
                      " println(y.len());"),
             "variable 'x' has unknown class type 'Foo'");
}

// An annotation that resolves to an unsupported type poisons the variable
// (it used to be declared as `void`, which reported follow-ons at its uses).
TEST(Poison, UnsupportedDeclaredTypePoisonsTheVariable) {
  expectOnly("enum Color { Red }\n" +
                 wrapMain("t: Color? = None;\n"
                          " match t { x: Color { println(\"c\"); } _ { } }\n"
                          " println(t);"),
             "optional enum types are not supported yet");
}

// A resolved annotation still types the variable, so it keeps being checked.
TEST(Poison, ResolvedDeclaredTypeIsKeptAfterABadInitializer) {
  expectOnly(wrapMain("x: int = " + kBad + ";\n println(Str(x + 1));"),
             kBadMsg);
  auto r = semaCheck(wrapMain("x: int = " + kBad + ";\n s: Str = x;"));
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
}

// ============================================================================
// Destructuring
// ============================================================================

TEST(Poison, FailedDestructuringBindsBothTargets) {
  // The value is in error.
  expectOnly(wrapMain("a, b = " + kBad +
                      ";\n println(a);\n println(b + a);\n c = a.len();"),
             kBadMsg);
  // The value is not a tuple.
  expectOnly(wrapMain("a, b = 5;\n println(a);\n println(b + a);"),
             "only tuples can be destructured");
  // The arity does not match.
  expectOnly(wrapMain("a, b = (1, 2, 3);\n println(a);\n println(b);"),
             "into 2 targets (it has 3 elements)");
}

// An annotated target whose annotation resolves keeps its type.
TEST(Poison, FailedDestructuringKeepsResolvedAnnotations) {
  expectOnly(wrapMain("a, b: int = 5;\n println(a);\n println(Str(b + 1));"),
             "only tuples can be destructured");
  auto r = semaCheck(wrapMain("a, b: int = 5;\n s: Str = b;"));
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
}

// A target whose annotation does not match its element is still declared.
TEST(Poison, MismatchedDestructuringTargetIsStillDeclared) {
  expectOnly(wrapMain("p: Str, q = (1, 2);\n println(Str(p.len()));\n"
                      " println(Str(q));"),
             "does not match declared type 'Str' for target 'p'");
}

// A target with an unknown annotation is poisoned.
TEST(Poison, UnknownDestructuringAnnotationPoisonsTheTarget) {
  expectOnly(wrapMain("p: Nope, q = (1, 2);\n println(p);\n"
                      " println(Str(q));"),
             "unknown class type 'Nope'");
}

// A visible variable named as a target keeps its type; a poisoned one is
// re-declared with its element's type.
TEST(Poison, DestructuringRedeclaresAPoisonedVariable) {
  expectOnly(
      wrapMain("a = " + kBad + ";\n a, b = (1, 2);\n println(Str(a + b));"),
      kBadMsg);
}

// ============================================================================
// Other binders
// ============================================================================

TEST(Poison, MatchArmWithUnknownTypeBindsItsName) {
  expectOnly("class A { }\n" +
                 wrapMain("o: Obj = A();\n"
                          " match o { q: Nope { println(q); } _ { } }"),
             "match arm has unknown class type 'Nope'");
}

TEST(Poison, OptionalMatchArmInErrorBindsItsName) {
  expectOnly("class A { }\n" +
                 wrapMain("m: A? = None;\n"
                          " match m { r: Str { println(r.len()); } _ { } }"),
             "is not a subclass of 'A'");
}

TEST(Poison, EnumMatchArmBindingIsSilentInItsBody) {
  expectOnly("enum Color { Red, Green }\n" +
                 wrapMain("c = Color::Red;\n"
                          " match c { x: Red { println(x); } _ { } }"),
             "enum-match arm cannot bind a variable");
}

// A function whose signature failed is not declared; calls by its name are
// follow-ons of that error, and so are the values they produce.
TEST(Poison, CallsToAFunctionWithABadSignatureAreSilent) {
  expectOnly("fn f(x: Foo) -> int { return 1; }\n" +
                 wrapMain("v = f(1);\n println(Str(v + 1));"),
             "parameter 'x' has unknown class type 'Foo'");
  expectOnly("fn g() -> Foo { return 1; }\n" +
                 wrapMain("v = g();\n println(v.len());"),
             "unknown class type 'Foo'");
}

// A function whose name collides keeps the name's owner checked as usual.
TEST(Poison, ColliderDoesNotSilenceTheOriginal) {
  auto r = semaCheck("fn h(x: int) -> int { return x; }\n"
                     "fn h(x: Nope) -> int { return 1; }\n" +
                     wrapMain("h(\"not an int\");"));
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("argument 1 of 'h' has type 'Str'"),
            std::string::npos)
      << r.Diagnostics;
}
