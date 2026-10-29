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
#include <utility>
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

// A parameter, and a field, is reported at itself, as what it names
// (finding 49: at the function, the class or the struct, as a variable).
const std::string kStrParamMsg =
    "'Str' is a type name and cannot be used as a parameter name";

TEST(BinderName, FunctionParametersRejectReservedNames) {
  expectOneBinderError("fn f(a: int, Str: int) -> int { return 0; }\n" +
                           wrapMain("n = f(1, 2);"),
                       "1:14: error: " + kStrParamMsg);
  expectOneBinderError(kPoint + "fn f(Point: int) -> int { return Point; }\n" +
                           wrapMain("n = f(1);"),
                       "3:6: error: 'Point' is a type name and cannot be used "
                       "as a parameter name");
  // A native function's too, and a lambda's.
  expectOneBinderError("native fn nat(n: int, u8: int) -> int = \"pk_nat\";\n" +
                           wrapMain("n = nat(1, 2);"),
                       "1:23: error: 'u8' is a type name and cannot be used "
                       "as a parameter name");
  expectOneBinderError(wrapMain("f = fn(a: int,\n         Str: int) => a;"),
                       "3:10: error: " + kStrParamMsg);
}

TEST(BinderName, MethodParametersRejectReservedNames) {
  expectOneBinderError("class C { fn m(Str: int) -> int { return 0; } }\n" +
                           wrapMain("c = C(); n = c.m(1);"),
                       "1:16: error: " + kStrParamMsg);
  expectOneBinderError("struct S { n: int; }\n"
                       "impl S { fn m(n: int, Str: int) {} }\n" +
                           wrapMain("s = S(1);"),
                       "2:23: error: " + kStrParamMsg);
}

// A field is checked itself, so whether or not its type writes `__init__`
// (which took one named like a type, finding 49), a variant's and a generic
// type's too.
TEST(BinderName, FieldsRejectReservedNames) {
  const std::string u8Field =
      "error: 'u8' is a type name and cannot be used as a field name";
  expectOneBinderError("struct S { x: int; u8: int; }\n" + wrapMain(""),
                       "1:20: " + u8Field);
  expectOneBinderError("class C { u8: int; }\n" + wrapMain(""),
                       "1:11: " + u8Field);
  expectOneBinderError("class C { u8: int; }\n"
                       "impl C { fn __init__() { self.u8 = 1; } }\n" +
                           wrapMain("c = C(); io::println(c.u8);"),
                       "1:11: " + u8Field);
  expectOneBinderError(kPoint +
                           "struct S {\n  Point: int;\n}\n"
                           "impl S { fn __init__() { self.Point = 2; } }\n" +
                           wrapMain("s = S(); io::println(s.Point);"),
                       "4:3: error: 'Point' is a type name and cannot be used "
                       "as a field name");
  expectOneBinderError("enum E { A, B(n: int, u8: int) }\n" +
                           wrapMain("e = E::B(1, 2);"),
                       "1:23: " + u8Field);
  expectOneBinderError("class Base { n: int; }\nclass D : Base { u8: int; }\n" +
                           wrapMain("d = D(1, 2);"),
                       "2:18: " + u8Field);
  auto generic =
      semaCheck("struct Box<T> { int: T; }\n" + wrapMain("b = Box<int>(1);"));
  EXPECT_FALSE(generic.Ok);
  EXPECT_NE(generic.Diagnostics.find("1:17: error: 'int' is a type name and "
                                     "cannot be used as a field name"),
            std::string::npos)
      << generic.Diagnostics;
}

// A generic type's field named like a type is reported at its template,
// once: one never instantiated (a library's export compiled cleanly), one
// instantiated twice (reported per instantiation), one instantiated by a
// field declared before it; of a struct, a class and an enum.  A field
// named like the template's own type parameter is not a type's name.
TEST(BinderName, ATemplatesFieldsAreCheckedAtTheTemplate) {
  const std::string field = "is a type name and cannot be used as a field name";
  auto r = semaCheck("struct Box<T> { u8: T; }\nclass G<T> { Str: T; }\n"
                     "enum M<T> { J(int: T), N }\n" +
                     wrapMain(""));
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
  for (const char *at :
       {"1:17: error: 'u8' ", "2:14: error: 'Str' ", "3:15: error: 'int' "})
    EXPECT_NE(r.Diagnostics.find(at + field), std::string::npos)
        << at << "\n"
        << r.Diagnostics;
  for (const auto &[src, at] :
       {std::pair<std::string, std::string>{
            "struct Box<T> { u8: T; }\n" +
                wrapMain("a = Box<int>(1); b = Box<Str>(\"x\");"),
            "1:17: error: 'u8' "},
        {"class G<T> { u8: T; }\n" +
             wrapMain("a = G<int>(1); b = G<Str>(\"x\");"),
         "1:14: error: 'u8' "},
        {"enum M<T> { J(u8: T), N }\n" +
             wrapMain("a = M::J(1); b = M::J(\"x\");"),
         "1:15: error: 'u8' "},
        {"class H { b: Box<int>; }\nstruct Box<T> { u8: T; }\n" +
             wrapMain("h = H(Box<int>(1));"),
         "2:17: error: 'u8' "}})
    expectOneBinderError(src, at + field);
  auto own = semaCheck("struct Box<T> { T: int; v: T; }\n" +
                       wrapMain("b = Box<int>(1, 2); io::println(b.T);"));
  EXPECT_TRUE(own.Ok) << own.Diagnostics;
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
  // Finding 95: the assignments and destructuring targets that bind the
  // rejected name again are its follow-ons, in an inner block too; each
  // reported again before.
  expectOneBinderError(kFunc + wrapMain("func = 1; func = 2; func = 3;"),
                       kFnMsg);
  expectOneBinderError(kFunc + wrapMain("func = 1;\n if True { func = 2; }\n"
                                        " func, b = (3, 4); a, func = (5, 6);\n"
                                        " io::println(func + a + b);"),
                       kFnMsg);
  expectOneBinderError(
      kFunc + wrapMain("func, b = (1, 2); func = 3; io::println(b);"), kFnMsg);
}

// The binding that poisons a function's name is reported though its value
// has an error of its own (`func = nope;` reported only `nope`, and the
// follow-on rule then hid the name's error at every later binding), once,
// at the first binding: an assignment, a destructuring target annotated
// or not (an annotated one bound the name), and one of a value that is
// no tuple.
TEST(BinderName, AFunctionsNameIsReportedWhenTheValueFails) {
  const std::string nope = "use of undeclared variable 'nope'";
  auto countOf = [](const std::string &text, const std::string &what) {
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos;
         at = text.find(what, at + what.size()))
      ++n;
    return n;
  };
  for (const char *body :
       {"func = nope; func = 2;", "func = nope; func = nope; func, b = (1, 2);",
        "func = nope;\n if True { func = 2; }", "func, b = nope; func = 2;",
        "func: int, b: int = nope; func = 3;", "a, func = 5; func = 6;"}) {
    auto r = semaCheck(kFunc + wrapMain(body));
    EXPECT_FALSE(r.Ok) << body;
    EXPECT_EQ(countOf(r.Diagnostics, kFnMsg), 1u) << body << "\n"
                                                  << r.Diagnostics;
    const size_t column = std::string(body).find("func") + 1;
    EXPECT_NE(r.Diagnostics.find("3:" + std::to_string(column) +
                                 ": error: " + kFnMsg),
              std::string::npos)
        << body << "\n"
        << r.Diagnostics;
  }
  auto r = semaCheck(kFunc + wrapMain("func = nope; func = nope;"));
  EXPECT_EQ(countOf(r.Diagnostics, nope), 2u) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
}

TEST(BinderName, ParametersAndBindingsRejectFunctionNames) {
  const std::string paramMsg = "'func' is a function of this module and "
                               "cannot be used as a parameter name";
  expectOneBinderError(kFunc + "fn g(func: int) -> int { return func; }\n" +
                           wrapMain("n = g(1);"),
                       "2:6: error: " + paramMsg);
  expectOneBinderError(kFunc + wrapMain("f = fn(func: int) => func * 2;"),
                       paramMsg);
  // A method's, declared before the function.
  expectOneBinderError("class C { fn m(func: int) -> int { return func; } }\n" +
                           kFunc + wrapMain("n = C().m(1);"),
                       "1:16: error: " + paramMsg);
  expectOneBinderError(
      "struct S { a: int; }\n"
      "impl S { view fn m(func: int) -> int { return func; } }\n" +
          kFunc + wrapMain("n = func(2);"),
      paramMsg);
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
