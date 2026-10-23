// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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
  return "import ::io; fn main() -> int {\n" + body + "\n  return 0;\n}\n";
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

// -- Inferred declarations: `a = <bad>`

// The issue's reproducer.
TEST(Poison, FailedFirstAssignmentReportsOnlyTheRealError) {
  expectOnly("import ::io; fn main() -> int { n = 2.5; a = Str<int>(n); "
             "io::println(a); "
             "return 0; }",
             kBadMsg);
}

TEST(Poison, SeveralLaterUsesAreSilent) {
  expectOnly(
      wrapMain(
          "a = " + kBad +
          ";\n io::println(a);\n b = a;\n x: int = a;\n io::print(Str(x));"),
      kBadMsg);
}

// Poison is absorbing: a value computed from a poisoned one is poisoned too.
TEST(Poison, PoisonPropagatesThroughDerivedVariables) {
  expectOnly(wrapMain("a = " + kBad +
                      ";\n b = a + 1;\n io::println(b);\n c = b.len();\n"
                      " io::println(Str(c));"),
             kBadMsg);
}

// Every kind of use of a poisoned variable, one program each.
TEST(Poison, EveryUseOfAPoisonedVariableIsSilent) {
  const std::string classes =
      "class P {\n  x: int;\n}\n"
      "impl P {\n  fn __init__(x: int) { self.x = x; }\n}\n"
      "fn f(v: int) -> int { return v; }\n";
  const std::vector<std::string> uses = {
      "a.foo();",                                     // method call
      "y = a.x; io::println(y);",                     // field access
      "z = a[0]; io::println(z);",                    // subscript
      "xs = [1, 2]; io::println(Str(xs[a]));",        // as an array index
      "s = \"abc\"; io::println(Str(s[a]));",         // as a string index
      "f(a);",                                        // call argument
      "p = P(a); io::println(Str(p.x));",             // constructor argument
      "io::println(Str(f(a) + 1));",                  // nested in a call
      "if a { io::println(\"t\"); }",                 // if condition
      "while a < 3 { io::println(\"w\"); }",          // while condition
      "k = if a then 1 else 2; io::println(Str(k));", // ternary condition
      "q = -a; r = !a; io::println(Str(q)); io::println(Str(r));", // unary ops
      "t = a * 2 + 1 == 3 && a; io::println(Str(t));",             // binary ops
      "e = a == None; io::println(Str(e));",                       // comparison
      "a.x = 3;",                              // member assignment
      "a[1] = 2;",                             // subscript assignment
      "xs = [1]; xs[0] = a;",                  // assigned into an array
      "pp = P(1); pp.x = a;",                  // assigned into a field
      "t = (a, 1); io::println(t.0);",         // tuple element
      "u = [a, a]; io::println(Str(u[1]));",   // array element
      "s = Str(a); io::println(s);",           // inferred conversion
      "s = Str<int>(a); io::println(s);",      // explicit conversion
      "m = a; io::println(m);",                // copy
      "match a { _ { io::println(\"m\"); } }", // match subject
      "o: Obj = a; io::println(o);",           // typed declaration
      "e: Error = a; io::println(e);", // not the user-visible `Error` class
      "u, v = a; io::println(u); io::println(v);", // destructuring source
      "io::println(a.toString().len());",          // chained calls
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
                 wrapMain("io::println(Str(g()));"),
             kBadMsg);
}

// An empty array literal fails the declaration too.
TEST(Poison, EmptyArrayLiteralPoisonsTheVariable) {
  expectOnly(wrapMain("a = [];\n n = a.len();\n x: int = a[0];"),
             "an empty array literal has no type: an array has at least one "
             "element");
}

// -- Recovery

// A later valid assignment re-declares the variable with the value's type.
TEST(Poison, ValidReassignmentRedeclaresTheVariable) {
  expectOnly(wrapMain("a = " + kBad + ";\n a = 5;\n io::println(Str(a + 1));"),
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
                      ";\n if True { a = 5; }\n io::println(Str(a + 1));"),
             kBadMsg);
}

// A failing re-assignment keeps the variable poisoned (and silent).
TEST(Poison, FailedReassignmentKeepsThePoison) {
  expectOnly(wrapMain("a = " + kBad + ";\n a = a + 1;\n io::println(a);"),
             kBadMsg);
}

// A failed re-assignment of a healthy variable keeps its type.
TEST(Poison, FailedReassignmentOfAHealthyVariableKeepsItsType) {
  auto r = semaCheck(wrapMain("a = 1;\n a = " + kBad + ";\n s: Str = a;"));
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("initializer of type 'int' does not match"),
            std::string::npos)
      << r.Diagnostics;
}

// -- Genuinely undeclared names

TEST(Poison, UndeclaredVariableIsStillReported) {
  auto r = semaCheck(wrapMain("io::println(nope);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("use of undeclared variable 'nope'"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Poison, UndeclaredVariableNextToAPoisonedOneIsStillReported) {
  auto r = semaCheck(
      wrapMain("a = " + kBad + ";\n io::println(a);\n io::println(nope + a);"));
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(kBadMsg), std::string::npos) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("use of undeclared variable 'nope'"),
            std::string::npos)
      << r.Diagnostics;
}

// The poison is a binding like any other: it ends with its scope.
TEST(Poison, PoisonDoesNotOutliveItsScope) {
  auto r = semaCheck(wrapMain("if True { a = " + kBad +
                              "; io::println(a); }\n io::println(a);"));
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("use of undeclared variable 'a'"),
            std::string::npos)
      << r.Diagnostics;
}

// -- Typed declarations: `x: T = <bad>`

TEST(Poison, UnknownDeclaredTypePoisonsTheVariable) {
  expectOnly(wrapMain("x: Foo = 1;\n io::println(x);\n y = x + 1;\n"
                      " io::println(y.len());"),
             "variable 'x' has unknown class type 'Foo'");
}

// An annotation that resolves to an unsupported type poisons the variable
// (it used to be declared as `void`, which reported follow-ons at its uses).
TEST(Poison, UnsupportedDeclaredTypePoisonsTheVariable) {
  expectOnly(wrapMain("t: Missing? = None;\n"
                      " match t { x: Missing { io::println(\"c\"); } _ { } }\n"
                      " io::println(t);"),
             "unknown class type 'Missing'");
}

// A resolved annotation still types the variable, so it keeps being checked.
TEST(Poison, ResolvedDeclaredTypeIsKeptAfterABadInitializer) {
  expectOnly(wrapMain("x: int = " + kBad + ";\n io::println(Str(x + 1));"),
             kBadMsg);
  auto r = semaCheck(wrapMain("x: int = " + kBad + ";\n s: Str = x;"));
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
}

// -- Destructuring

TEST(Poison, FailedDestructuringBindsBothTargets) {
  // The value is in error.
  expectOnly(
      wrapMain("a, b = " + kBad +
               ";\n io::println(a);\n io::println(b + a);\n c = a.len();"),
      kBadMsg);
  // The value is not a tuple.
  expectOnly(wrapMain("a, b = 5;\n io::println(a);\n io::println(b + a);"),
             "only tuples can be destructured");
  // The arity does not match.
  expectOnly(wrapMain("a, b = (1, 2, 3);\n io::println(a);\n io::println(b);"),
             "into 2 targets (it has 3 elements)");
}

// An annotated target whose annotation resolves keeps its type.
TEST(Poison, FailedDestructuringKeepsResolvedAnnotations) {
  expectOnly(
      wrapMain("a, b: int = 5;\n io::println(a);\n io::println(Str(b + 1));"),
      "only tuples can be destructured");
  auto r = semaCheck(wrapMain("a, b: int = 5;\n s: Str = b;"));
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
}

// A target whose annotation does not match its element is still declared.
TEST(Poison, MismatchedDestructuringTargetIsStillDeclared) {
  expectOnly(wrapMain("p: Str, q = (1, 2);\n io::println(Str(p.len()));\n"
                      " io::println(Str(q));"),
             "does not match declared type 'Str' for target 'p'");
}

// A target with an unknown annotation is poisoned.
TEST(Poison, UnknownDestructuringAnnotationPoisonsTheTarget) {
  expectOnly(wrapMain("p: Nope, q = (1, 2);\n io::println(p);\n"
                      " io::println(Str(q));"),
             "unknown class type 'Nope'");
}

// A visible variable named as a target keeps its type; a poisoned one is
// re-declared with its element's type.
TEST(Poison, DestructuringRedeclaresAPoisonedVariable) {
  expectOnly(
      wrapMain("a = " + kBad + ";\n a, b = (1, 2);\n io::println(Str(a + b));"),
      kBadMsg);
}

// -- Other binders

TEST(Poison, MatchArmWithUnknownTypeBindsItsName) {
  expectOnly("class A { }\n" +
                 wrapMain("o: Obj = A();\n"
                          " match o { q: Nope { io::println(q); } _ { } }"),
             "match arm has unknown class type 'Nope'");
}

TEST(Poison, OptionalMatchArmInErrorBindsItsName) {
  expectOnly(
      "class A { }\n" +
          wrapMain("m: A? = None;\n"
                   " match m { r: Str { io::println(r.len()); } _ { } }"),
      "is not a subclass of 'A'");
}

TEST(Poison, EnumMatchArmBindingIsSilentInItsBody) {
  expectOnly("enum Color { Red, Green }\n" +
                 wrapMain("c = Color::Red;\n"
                          " match c { x: Red { io::println(x); } _ { } }"),
             "variant 'Red' of enum 'Color' has no fields to bind");
}

// A function whose signature failed is not declared; calls by its name are
// follow-ons of that error, and so are the values they produce.
TEST(Poison, CallsToAFunctionWithABadSignatureAreSilent) {
  expectOnly("fn f(x: Foo) -> int { return 1; }\n" +
                 wrapMain("v = f(1);\n io::println(Str(v + 1));"),
             "parameter 'x' has unknown class type 'Foo'");
  expectOnly("fn g() -> Foo { return 1; }\n" +
                 wrapMain("v = g();\n io::println(v.len());"),
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

// Reserved binder names (#126): every binder rejects a type name (a builtin
// one, `Str`, or the program's) with one error, through the same guard.

namespace {

// Exactly one error, @p expected.
void expectOneBinderError(const std::string &src, const std::string &expected) {
  auto r = semaCheck(src);
  EXPECT_FALSE(r.Ok) << src;
  EXPECT_EQ(r.ErrorCount, 1u) << src << "\n" << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(expected), std::string::npos) << src << "\n"
                                                             << r.Diagnostics;
}

const std::string kStrMsg =
    "'Str' is a type name and cannot be used as a variable";
const std::string kPointMsg =
    "'Point' is a type name and cannot be used as a variable";
const std::string kPoint = "class Point { x: int; }\n"
                           "impl Point { fn __init__() { self.x = 0; } }\n";

} // namespace

TEST(BinderName, InferredAssignmentRejectsReservedNames) {
  expectOneBinderError(wrapMain("Str = 1;"), kStrMsg);
  expectOneBinderError(kPoint + wrapMain("Point = 1;"), kPointMsg);
}

TEST(BinderName, TypedDeclarationRejectsReservedNames) {
  expectOneBinderError(kPoint + wrapMain("Point: int = 1;"), kPointMsg);
  expectOneBinderError(wrapMain("Str: int = 1;"), kStrMsg);
}

TEST(BinderName, DestructuringTargetsRejectReservedNames) {
  expectOneBinderError(wrapMain("Str, b = (1, 2);"), kStrMsg);
  expectOneBinderError(wrapMain("a, Str: int = (1, 2);"), kStrMsg);
  expectOneBinderError(kPoint + wrapMain("Point: int, b = (1, 2);"), kPointMsg);
}

TEST(BinderName, FunctionParametersRejectReservedNames) {
  expectOneBinderError(
      "fn f(Str: int) -> int { return 0; }\n" + wrapMain("n = f(1);"), kStrMsg);
  expectOneBinderError(kPoint + "fn f(Point: int) -> int { return Point; }\n" +
                           wrapMain("n = f(1);"),
                       kPointMsg);
}

TEST(BinderName, MethodParametersRejectReservedNames) {
  expectOneBinderError("class C { fn m(Str: int) -> int { return 0; } }\n" +
                           wrapMain("c = C(); n = c.m(1);"),
                       kStrMsg);
}

TEST(BinderName, MatchBindingsRejectReservedNames) {
  expectOneBinderError(
      wrapMain("o: Obj = \"s\";\n match o { Str: Str { io::println(Str); } "
               "_ { } }"),
      kStrMsg);
  expectOneBinderError(
      kPoint + wrapMain("o: Obj = Point();\n match o { Point: Point { "
                        "io::println(Str(Point.x)); } _ { } }"),
      kPointMsg);
  expectOneBinderError(
      wrapMain("s: Str? = None;\n match s { Str: Str { } None { } }"), kStrMsg);
}

// A local never takes the name of a function of the module (a generic or a
// native one too, declared before or after it): the name always means the
// function.  The one error stands alone; the uses that follow are silent.
const std::string kFnMsg =
    "'func' is a function of this module and cannot be used as a variable";
const std::string kFunc = "fn func(x: int) -> int { return x + 2; }\n";

TEST(BinderName, LocalsRejectFunctionNames) {
  expectOneBinderError("fn func<T>(x: T) => x + 2;\n" +
                           wrapMain("func = fn(x: int, y: float) => x + y;\n"
                                    " io::println(func(1, 2.5));"),
                       kFnMsg);
  expectOneBinderError(kFunc + wrapMain("func: int = 3; io::println(func);"),
                       kFnMsg);
  expectOneBinderError(kFunc + wrapMain("let func = 3; io::println(func);"),
                       kFnMsg);
  expectOneBinderError(
      kFunc + wrapMain("func, b = (1, 2); io::println(func + b);"), kFnMsg);
  expectOneBinderError(
      kFunc + wrapMain("for func in 0..2 { io::println(func + 1); }"), kFnMsg);
  // Declared after its use, and a native function.
  expectOneBinderError(wrapMain("func = 1; io::println(func);") +
                           "fn func(x: int) -> int { return x; }\n",
                       kFnMsg);
  expectOneBinderError("native fn func(x: int) -> int = \"pk_func\";\n" +
                           wrapMain("func = 1;"),
                       kFnMsg);
}

TEST(BinderName, ParametersAndBindingsRejectFunctionNames) {
  expectOneBinderError(kFunc + "fn g(func: int) -> int { return func; }\n" +
                           wrapMain("n = g(1);"),
                       kFnMsg);
  expectOneBinderError(kFunc + wrapMain("f = fn(func: int) => func * 2;"),
                       kFnMsg);
  // A method's, declared before the function.
  expectOneBinderError("class C { fn m(func: int) -> int { return func; } }\n" +
                           kFunc + wrapMain("n = C().m(1);"),
                       kFnMsg);
  expectOneBinderError(
      "struct S { a: int; }\n"
      "impl S { view fn m(func: int) -> int { return func; } }\n" +
          kFunc + wrapMain("n = func(2);"),
      kFnMsg);
  expectOneBinderError(
      kPoint + kFunc +
          wrapMain("o: Obj = Point();\n match o { func: Point { "
                   "io::println(Str(func.x)); } _ { } }"),
      kFnMsg);
}

// What no code of the program uses may still take a function's name: a
// field (and the default constructor's parameter for it), a native
// function's parameter.  So may another module's function's, which is
// always qualified.
TEST(BinderName, FieldsAndNativeParametersMayShareFunctionNames) {
  auto r = semaCheck(kFunc +
                     "class C { func: int; }\n"
                     "struct S { func: int; }\n"
                     "native fn nat(func: int) -> int = \"pk_nat\";\n" +
                     wrapMain("c = C(1); s = S(2); println = c.func + s.func;"
                              " io::println(func(println));"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(BinderName, OrdinaryNamesAreStillAccepted) {
  auto r = semaCheck(kPoint + "fn f(print: int) -> int { return print; }\n" +
                     wrapMain("point = Point(); stdin = 1; a, b: int = (1, 2);"
                              " n = f(a + b);"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}
