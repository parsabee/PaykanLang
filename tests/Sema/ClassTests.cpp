// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: class declarations, inheritance, methods, fields, match.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string withClasses(const std::string &classDefs,
                                const std::string &body = "return 0;") {
  return classDefs + "\nfn main() -> int {\n" + body + "\n}\n";
}

// ============================================================================
// Basic class structure
// ============================================================================

TEST(Class, ClassEmpty) {
  auto r = semaCheck(withClasses("class A {}"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, ClassWithFields) {
  auto r = semaCheck(withClasses(R"(
    class Point { x: int; y: int; }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, ClassWithMethod) {
  auto r = semaCheck(withClasses(R"(
    class Counter {
      count: int;
      fn getCount() -> int { return 0; }
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, ClassMethodUseSelf) {
  auto r = semaCheck(withClasses(R"(
    class Counter {
      count: int;
      fn getCount() -> int { return self.count; }
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, ClassInitSetField) {
  auto r = semaCheck(withClasses(R"(
    class Point {
      x: int; y: int;
      fn __init__(x: int, y: int) { self.x = x; self.y = y; }
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, ImplicitObjBaseEqNeq) {
  auto r = semaCheck(R"(
    class A {}
    fn eq(a: A, b: A) -> bool { return a == b; }
    fn neq(a: A, b: A) -> bool { return a != b; }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// Duplicate definitions
// ============================================================================

TEST(Class, DuplicateName) {
  auto r = semaCheck(withClasses("class A {}\nclass A {}"));
  EXPECT_FALSE(r.Ok);
}

TEST(Class, DuplicateField) {
  auto r = semaCheck(withClasses(R"(
    class A { x: int; x: float; }
  )"));
  EXPECT_FALSE(r.Ok);
}

TEST(Class, DuplicateMethod) {
  auto r = semaCheck(withClasses(R"(
    class A { fn foo() {} fn foo() {} }
  )"));
  EXPECT_FALSE(r.Ok);
}

// ============================================================================
// Unknown types
// ============================================================================

TEST(Class, FieldUnknownType) {
  auto r = semaCheck(withClasses(R"(
    class A { x: NoSuchType; }
  )"));
  EXPECT_FALSE(r.Ok);
}

TEST(Class, MethodUnknownReturnType) {
  auto r = semaCheck(withClasses(R"(
    class A { fn foo() -> NoSuchType { return 0; } }
  )"));
  EXPECT_FALSE(r.Ok);
}

TEST(Class, MethodUnknownParamType) {
  auto r = semaCheck(withClasses(R"(
    class A { fn foo(x: NoSuchType) {} }
  )"));
  EXPECT_FALSE(r.Ok);
}

// ============================================================================
// Field access & type errors
// ============================================================================

TEST(Class, WrongFieldAssignType) {
  auto r = semaCheck(withClasses(R"(
    class A { x: int; fn setX(v: float) { self.x = v; } }
  )"));
  EXPECT_FALSE(r.Ok);
}

TEST(Class, AccessNonExistentField) {
  auto r = semaCheck(withClasses(R"(
    class A { x: int; fn bad() -> int { return self.y; } }
  )"));
  EXPECT_FALSE(r.Ok);
}

TEST(Class, AssignNonExistentField) {
  auto r = semaCheck(withClasses(R"(
    class A { fn bad() { self.nosuchfield = 1; } }
  )"));
  EXPECT_FALSE(r.Ok);
}

// ============================================================================
// Method return type checks
// ============================================================================

TEST(Class, MethodReturnTypeMismatch) {
  auto r = semaCheck(withClasses(R"(
    class A { fn foo() -> int { return True; } }
  )"));
  EXPECT_FALSE(r.Ok);
}

TEST(Class, MethodMissingReturn) {
  auto r = semaCheck(withClasses(R"(
    class A { fn foo() -> int { x: int = 1; } }
  )"));
  EXPECT_FALSE(r.Ok);
}

// ============================================================================
// Inheritance
// ============================================================================

TEST(Class, Inheritance) {
  auto r = semaCheck(withClasses(R"(
    class Animal { name: Str; }
    class Dog : Animal { breed: Str; }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, InheritedFieldAccess) {
  auto r = semaCheck(withClasses(R"(
    class Animal { name: Str; }
    class Dog : Animal { fn getName() -> Str { return self.name; } }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, UndefinedSuperclass) {
  auto r = semaCheck(withClasses("class Dog : Animal {}"));
  EXPECT_FALSE(r.Ok);
}

TEST(Class, CyclicInheritance) {
  auto r = semaCheck(withClasses("class A : B {}\nclass B : A {}"));
  EXPECT_FALSE(r.Ok);
}

TEST(Class, FieldShadowingSuperclass) {
  auto r = semaCheck(withClasses(R"(
    class Animal { name: Str; }
    class Dog : Animal { name: Str; }
  )"));
  EXPECT_FALSE(r.Ok);
}

TEST(Class, ForwardReference) {
  auto r = semaCheck(withClasses(R"(
    class Dog : Animal { fn getName() -> Str { return self.name; } }
    class Animal { name: Str; }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// __super__
// ============================================================================

TEST(Class, SuperCallValid) {
  auto r = semaCheck(withClasses(R"(
    class Animal {
      name: Str;
      fn __init__(n: Str) { self.name = n; }
    }
    class Dog : Animal {
      breed: Str;
      fn __init__(n: Str, b: Str) { __super__(n); self.breed = b; }
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, SuperCallMissing) {
  auto r = semaCheck(withClasses(R"(
    class Animal { name: Str; fn __init__(n: Str) { self.name = n; } }
    class Dog : Animal { breed: Str; fn __init__(n: Str, b: Str) { self.breed = b; } }
  )"));
  EXPECT_FALSE(r.Ok);
}

TEST(Class, SuperCallWrongArgType) {
  auto r = semaCheck(withClasses(R"(
    class Animal { fn __init__(x: int) {} }
    class Dog : Animal { fn __init__() { __super__(True); } }
  )"));
  EXPECT_FALSE(r.Ok);
}

TEST(Class, SuperCallOutsideInit) {
  auto r = semaCheck(withClasses(R"(
    class A { fn foo() { __super__(); } }
  )"));
  EXPECT_FALSE(r.Ok);
}

TEST(Class, SuperCallNoSuperclass) {
  auto r = semaCheck(withClasses(R"(
    class A { fn __init__() { __super__(); } }
  )"));
  EXPECT_FALSE(r.Ok);
}

// ============================================================================
// match statement
// ============================================================================

TEST(Class, MatchBasicValid) {
  auto r = semaCheck(R"(
    class Dog {}
    class Cat {}
    fn main() -> int {
      x: Obj = Dog();
      match x { Dog {} Cat {} _ {} }
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, MatchBindingInScope) {
  auto r = semaCheck(R"(
    class Dog { name: Str; }
    fn main() -> int {
      x: Obj = Dog();
      match x { d: Dog { d.name = "Rex"; } _ {} }
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, MatchWildcardValid) {
  auto r = semaCheck(R"(
    class Dog {}
    fn main() -> int {
      x: Obj = Dog();
      match x { Dog {} _ {} }
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, MatchSubjectNotClassType) {
  auto r = semaCheck(R"(
    fn main() -> int {
      x: int = 1;
      match x { _ {} }
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Class, MatchArmUnknownType) {
  auto r = semaCheck(R"(
    fn main() -> int {
      x: Obj = 0;
      match x { NoSuchClass {} }
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Class, MatchArmNotSubclass) {
  auto r = semaCheck(R"(
    class Animal {}
    class Car {}
    fn main() -> int {
      x: Animal = Animal();
      match x { Car {} }
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Class, MatchWildcardNotLast) {
  auto r = semaCheck(R"(
    class Dog {}
    fn main() -> int {
      x: Obj = Dog();
      match x { _ {} Dog {} }
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Class, MatchBindingRedeclaration) {
  auto r = semaCheck(R"(
    class Dog {}
    fn main() -> int {
      x: Obj = Dog();
      match x { d: Dog { d: int = 1; } }
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Class, MatchArmBodyTypechecked) {
  auto r = semaCheck(R"(
    class Dog {}
    fn main() -> int {
      x: Obj = Dog();
      match x { Dog { y: int = true; } }
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Class, MatchDeepInheritance) {
  auto r = semaCheck(R"(
    class Animal {}
    class Dog : Animal {}
    class Labrador : Dog {}
    fn main() -> int {
      x: Animal = Animal();
      match x { Labrador {} Dog {} _ {} }
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, MatchMissingWildcardWithFallthrough) {
  auto r = semaCheck(R"(
    class Dog {}
    fn main() -> int {
      x: Obj = Dog();
      match x { Dog { return 0; } }
      return 1;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, MatchMissingWildcardNoFallthrough) {
  auto r = semaCheck(R"(
    class Dog {}
    fn main() -> int {
      x: Obj = Dog();
      match x { Dog { return 0; } }
    }
  )");
  EXPECT_FALSE(r.Ok);
}
