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

// A free function and a class cannot share a name (constructor vs function).
TEST(Func, FunctionClassNameCollision) {
  auto r = semaCheck(R"(
    fn Foo() -> int { return 1; }
    class Foo { fn __init__() {} }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("redefinition"), std::string::npos);
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
  EXPECT_NE(r.Diagnostics.find("unknown class type 'FooBar'"), std::string::npos);
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
