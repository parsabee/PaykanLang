// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: functions and scoping.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// ============================================================================
// Function declaration checks
// ============================================================================

TEST(Func, UndeclaredFunction) {
  auto r = semaCheck(wrapMain("foo();"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("undeclared function"), std::string::npos);
}

TEST(Func, FunctionRedefinition) {
  auto r = semaCheck(R"(
    fn foo() {}
    fn foo() {}
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("redefinition"), std::string::npos);
}

// Functions are forward-declared, so a function may call another that is
// defined later in the module (and mutually-recursive functions resolve).
TEST(Func, CallFunctionDefinedLater) {
  auto r = semaCheck(R"(
    fn first() -> int { return second(); }
    fn second() -> int { return 1; }
    fn main() -> int { return first(); }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// A class method may call a module-level free function (forward-declared).
TEST(Func, MethodCallsFreeFunction) {
  auto r = semaCheck(R"(
    class C {
      fn __init__() {}
      fn get() -> int { return helper(); }
    }
    fn helper() -> int { return 7; }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// A free function and a class cannot share a name (constructor vs function),
// whichever is declared first.
TEST(Func, FunctionClassNameCollision) {
  auto r = semaCheck(R"(
    fn Foo() -> int { return 1; }
    class Foo { fn __init__() {} }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'Foo' is already declared as a class"),
            std::string::npos)
      << r.Diagnostics;

  r = semaCheck(R"(
    class Foo { fn __init__() {} }
    fn Foo() -> int { return 1; }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'Foo' is already declared as a class"),
            std::string::npos)
      << r.Diagnostics;
}

// ============================================================================
// Builtin names are reserved: a free function may not redeclare one
// ============================================================================

// Every builtin function registered by Sema::run(), including those a user
// might plausibly want to "override".
TEST(Func, FunctionShadowsBuiltinFunctionRejected) {
  for (const char *name :
       {"print", "println", "printerr", "printerrln", "StrInt", "StrFloat",
        "StrBool", "StrChar", "open", "IntStr", "FloatStr"}) {
    auto r = semaCheck(std::string("fn ") + name +
                       "(x: int) -> int { return x; }\n" + wrapMain(""));
    EXPECT_FALSE(r.Ok) << name;
    EXPECT_NE(r.Diagnostics.find(std::string("'") + name +
                                 "' is a builtin function and cannot be "
                                 "redeclared"),
              std::string::npos)
        << r.Diagnostics;
  }
}

// The collision is the only diagnostic: the rest of the rejected signature
// and its body are not checked (neither against the builtin's entry nor on
// their own), and the builtin keeps its original signature afterwards.
TEST(Func, FunctionShadowsBuiltinReportsCollisionOnly) {
  auto r = semaCheck("fn print(x: NoSuchType) { return x + 1; }\n" +
                     wrapMain("println(\"still one Obj argument\");"));
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(
                "'print' is a builtin function and cannot be redeclared"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("unknown class type"), std::string::npos)
      << r.Diagnostics;
}

TEST(Func, FunctionShadowsBuiltinClassRejected) {
  for (const char *name : {"Obj", "Str", "File", "Error", "Int"}) {
    auto r = semaCheck(std::string("fn ") + name + "() -> int { return 0; }\n" +
                       wrapMain(""));
    EXPECT_FALSE(r.Ok) << name;
    EXPECT_NE(
        r.Diagnostics.find(std::string("'") + name +
                           "' is a builtin class and cannot be redeclared"),
        std::string::npos)
        << r.Diagnostics;
  }
}

TEST(Func, FunctionShadowsEnumRejected) {
  auto r = semaCheck("enum Color { Red }\nfn Color() -> int { return 0; }\n" +
                     wrapMain(""));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'Color' is already declared as an enum"),
            std::string::npos)
      << r.Diagnostics;
}

// A name that merely resembles a builtin is fine.
TEST(Func, FunctionNamedNearBuiltinOk) {
  auto r = semaCheck(R"(
    fn print2(x: int) { println(StrInt(x)); }
    fn Print(x: int) { print2(x); }
    fn main() -> int { Print(1); return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Func, ArgumentCountMismatch) {
  auto r = semaCheck(R"(
    fn add(a: int, b: int) -> int { return a + b; }
    fn main() -> int { return add(1); }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Func, ReturnTypeMismatch) {
  auto r = semaCheck(R"(
    fn foo() -> int { return "hello"; }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Func, UnknownParamType) {
  auto r = semaCheck(R"(
    fn foo(x: FooBar) -> int { return 0; }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("unknown class type 'FooBar'"),
            std::string::npos);
}

TEST(Func, ClassTypeAsParam) {
  auto r = semaCheck(R"(
    class A {}
    fn foo(a: A) -> bool { return a == a; }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// Scoping
// ============================================================================

TEST(Func, BlockScopeIsolation) {
  auto r = semaCheck(wrapMain(R"(
    {
      x: int = 1;
    }
    println(StrInt(x));
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("undeclared"), std::string::npos);
}

TEST(Func, InnerScopeCanAccessOuter) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 1;
    {
      y: int = x + 1;
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// The print family is single-argument (no variadic functions / overloading)
// ============================================================================

TEST(Func, PrintlnSingleArgOk) {
  auto r = semaCheck(wrapMain(R"(println("hello" + " world");)"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Func, PrintlnRejectsMultipleArgs) {
  auto r = semaCheck(wrapMain(R"(println("hello", "world");)"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("expects 1 argument"), std::string::npos)
      << r.Diagnostics;
}

TEST(Func, PrintRejectsMultipleArgs) {
  auto r = semaCheck(wrapMain(R"(print("a", StrInt(1), "b");)"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("expects 1 argument"), std::string::npos)
      << r.Diagnostics;
}
