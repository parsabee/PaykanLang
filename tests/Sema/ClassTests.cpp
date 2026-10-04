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
// destroy is the compiler-generated destructor: final and not callable
// ============================================================================

TEST(Class, DestroyDirectCallRejected) {
  auto r = semaCheck(
      withClasses("class Box { v: int; fn __init__(x: int) { self.v = x; } }",
                  "b: Box = Box(5); b.destroy(); return 0;"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("destroy"), std::string::npos) << r.Diagnostics;
}

TEST(Class, DestroyDirectCallOnObjRejected) {
  auto r = semaCheck(withClasses("", "o: Obj = Obj(); o.destroy(); return 0;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Class, DestroyOverrideRejected) {
  auto r = semaCheck(
      withClasses("class Box { v: int; fn __init__(x: int) { self.v = x; }\n"
                  "  fn destroy() { } }"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("destroy"), std::string::npos) << r.Diagnostics;
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
// Top-level names are shared with the builtins: a class name is also its
// constructor, so it may not reuse a builtin function, builtin class, or enum.
// ============================================================================

// Regression: `class print` used to silently replace the builtin `print`.
TEST(Class, NameShadowsBuiltinFunctionRejected) {
  auto r = semaCheck(withClasses("class print { fn __init__() {} }",
                                 "print(\"hi\"); return 0;"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find(
                "'print' is a builtin function and cannot be redeclared"),
            std::string::npos)
      << r.Diagnostics;
}

// The conversion builtins removed by #64 no longer reserve their names.
TEST(Class, RemovedConversionNamesAreFree) {
  auto r = semaCheck(withClasses("class StrInt { fn __init__() {} }"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, NameShadowsBuiltinClassRejected) {
  for (const char *name :
       {"Obj", "Str", "Array", "File", "Error", "Int", "Float", "Bool"}) {
    auto r = semaCheck(withClasses(std::string("class ") + name + " {}"));
    EXPECT_FALSE(r.Ok) << name;
    EXPECT_NE(
        r.Diagnostics.find(std::string("'") + name +
                           "' is a builtin class and cannot be redeclared"),
        std::string::npos)
        << r.Diagnostics;
  }
}

TEST(Class, NameShadowsEnumRejected) {
  auto r = semaCheck(withClasses("enum Color { Red }\nclass Color {}"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find(
                "class 'Color' conflicts with an enum of the same name"),
            std::string::npos)
      << r.Diagnostics;
}

// Fields and methods have their own per-class namespace: they may reuse a
// builtin function name, and the builtin stays callable.
TEST(Class, MemberNamedLikeBuiltinOk) {
  auto r = semaCheck(withClasses(R"(
    class Logger {
      open: int;
      fn __init__() { self.open = 0; }
      fn print(msg: Str) { println(msg); }
      fn StrInt(n: int) -> Str { return "n"; }
    }
  )",
                                 R"(
    l: Logger = Logger();
    l.print("a");
    print(l.StrInt(l.open) + Str<int>(2));
    return 0;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
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

TEST(Class, ExplicitObjSuperclassAllowed) {
  auto r = semaCheck(withClasses("class A: Obj {}"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, InheritFromStrRejected) {
  auto r = semaCheck(withClasses("class MyStr: Str {}"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("class is final"), std::string::npos);
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

// A primitive subject now selects value-mode matching; a type-named arm there
// is an error (value-mode requires literal patterns).
TEST(Class, MatchValueModeRejectsTypeArm) {
  auto r = semaCheck(R"(
    class Animal {}
    fn main() -> int {
      x: int = 1;
      match x { Animal {} _ {} }
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
}

// A wildcard-only match over a primitive subject is valid (value-mode).
TEST(Class, MatchValueModeWildcardOnly) {
  auto r = semaCheck(R"(
    fn main() -> int {
      x: int = 1;
      match x { _ {} }
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok);
}

// Value-mode literal arms must match the subject's type.
TEST(Class, MatchValueModeLiteralTypeMismatch) {
  auto r = semaCheck(R"(
    fn main() -> int {
      x: int = 1;
      match x { "a" {} _ {} }
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
}

// Value-mode int match with matching literal arms is valid.
TEST(Class, MatchValueModeIntLiterals) {
  auto r = semaCheck(R"(
    fn main() -> int {
      x: int = 1;
      match x { 1 {} 2 {} _ {} }
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok);
}

// Str subject selects value-mode; string literal arms are valid.
TEST(Class, MatchValueModeStrLiterals) {
  auto r = semaCheck(R"(
    fn main() -> int {
      s: Str = "hi";
      match s { "a" {} "b" {} _ {} }
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok);
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

// ============================================================================
// Check 3 — override signature mismatch (strict equality, no covariance)
// ============================================================================

TEST(Class, OverrideReturnTypeMismatchRejected) {
  auto r = semaCheck(R"(
    class Animal {
      fn __init__() {}
      fn describe() -> int { return 1; }
    }
    class Dog : Animal {
      fn __init__() { __super__(); }
      fn describe() -> Str { return "dog"; }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("incompatible signature"), std::string::npos)
      << r.Diagnostics;
}

TEST(Class, OverrideParamTypeMismatchRejected) {
  auto r = semaCheck(R"(
    class Base {
      fn __init__() {}
      fn take(x: int) -> int { return x; }
    }
    class Derived : Base {
      fn __init__() { __super__(); }
      fn take(x: Str) -> int { return 0; }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("incompatible signature"), std::string::npos)
      << r.Diagnostics;
}

TEST(Class, OverrideParamCountMismatchRejected) {
  auto r = semaCheck(R"(
    class Base {
      fn __init__() {}
      fn f(x: int) -> int { return x; }
    }
    class Derived : Base {
      fn __init__() { __super__(); }
      fn f() -> int { return 0; }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Class, OverrideMatchingSignatureAccepted) {
  auto r = semaCheck(R"(
    class Animal {
      fn __init__() {}
      fn describe() -> int { return 1; }
    }
    class Dog : Animal {
      fn __init__() { __super__(); }
      fn describe() -> int { return 2; }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, OverrideToStringAndEqualsAccepted) {
  // Legitimate overrides of the Obj root methods must still type-check.
  auto r = semaCheck(R"(
    class Person {
      name: Str;
      fn __init__(n: Str) { self.name = n; }
      fn toString() -> Str { return self.name; }
      fn equals(other: Obj) -> bool { return True; }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, OverrideThreeLevelMismatchRejected) {
  auto r = semaCheck(R"(
    class A { fn __init__() {} fn kind() -> int { return 0; } }
    class B : A { fn __init__() { __super__(); } fn kind() -> int { return 1; } }
    class C : B { fn __init__() { __super__(); } fn kind() -> Str { return "c"; } }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
}

// ============================================================================
// Check 2 — derived __init__ must call __super__() as its first statement
// ============================================================================

TEST(Class, SuperRequiredForParameterizedBase) {
  auto r = semaCheck(R"(
    class Base {
      val: int;
      fn __init__(v: int) { self.val = v; }
    }
    class Derived : Base {
      fn __init__() { }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("__super__"), std::string::npos)
      << r.Diagnostics;
}

TEST(Class, SuperRequiredForZeroParamBase) {
  // Per the language reference, __super__() is required even when the base
  // __init__ takes no parameters.
  auto r = semaCheck(R"(
    class Base { fn __init__() {} }
    class Child : Base {
      x: int;
      fn __init__(x: int) { self.x = x; }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("__super__"), std::string::npos)
      << r.Diagnostics;
}

TEST(Class, SuperMustBeFirstStatement) {
  auto r = semaCheck(R"(
    class Base {
      val: int;
      fn __init__(v: int) { self.val = v; }
    }
    class Derived : Base {
      y: int;
      fn __init__(v: int) {
        self.y = 1;
        __super__(v);
      }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("first statement"), std::string::npos)
      << r.Diagnostics;
}

TEST(Class, DerivedMustDeclareInitWhenBaseHasOne) {
  auto r = semaCheck(R"(
    class Base {
      val: int;
      fn __init__(v: int) { self.val = v; }
    }
    class Derived : Base {
      y: int;
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Class, SuperFirstStatementAccepted) {
  auto r = semaCheck(R"(
    class Base { fn __init__() {} }
    class Child : Base {
      x: int;
      fn __init__(x: int) { __super__(); self.x = x; }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, SuperWithArgsFirstStatementAccepted) {
  auto r = semaCheck(R"(
    class Base {
      val: int;
      fn __init__(v: int) { self.val = v; }
    }
    class Derived : Base {
      y: int;
      fn __init__(v: int) { __super__(v); self.y = v; }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// Check 1 — __init__ must definitely assign every field on every path
// ============================================================================

TEST(Class, InitMissingFieldRejected) {
  auto r = semaCheck(R"(
    class Bad {
      x: int;
      y: int;
      fn __init__(a: int) { self.x = a; }
      fn toString() -> Str { return Str<int>(self.x) + Str<int>(self.y); }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("not assigned on every path"), std::string::npos)
      << r.Diagnostics;
}

TEST(Class, InitAllFieldsAssignedAccepted) {
  auto r = semaCheck(R"(
    class Point {
      x: int;
      y: int;
      fn __init__(a: int, b: int) { self.x = a; self.y = b; }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, InitFieldAssignedOnBothBranchesAccepted) {
  auto r = semaCheck(R"(
    class C {
      x: int;
      fn __init__(a: int) {
        if (a > 0) { self.x = 1; } else { self.x = 2; }
      }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, InitFieldAssignedOnOnlyOneBranchRejected) {
  auto r = semaCheck(R"(
    class C {
      x: int;
      fn __init__(a: int) {
        if (a > 0) { self.x = 1; }
      }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Class, InitFieldAssignedBeforeEarlyReturnAccepted) {
  auto r = semaCheck(R"(
    class C {
      x: int;
      fn __init__(a: int) {
        self.x = a;
        if (a > 0) { return; }
        self.x = 0;
      }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, InitMissingFieldBeforeEarlyReturnRejected) {
  auto r = semaCheck(R"(
    class C {
      x: int;
      y: int;
      fn __init__(a: int) {
        self.x = a;
        if (a > 0) { return; }
        self.y = a;
      }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Class, InitWhileLoopDoesNotGuaranteeAssignment) {
  auto r = semaCheck(R"(
    class C {
      x: int;
      fn __init__(a: int) {
        while (a > 0) { self.x = a; }
      }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Class, InitNoFieldsNoInitAccepted) {
  auto r = semaCheck(R"(
    class A {}
    class B { fn foo() -> int { return 1; } }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, InitDerivedOwnFieldAloneAccepted) {
  // Base fields are covered by __super__; the derived __init__ need only assign
  // its own declared fields.
  auto r = semaCheck(R"(
    class Base {
      a: int;
      fn __init__(a: int) { self.a = a; }
    }
    class Derived : Base {
      b: int;
      fn __init__(a: int, b: int) { __super__(a); self.b = b; }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// `self` as a parameter name is rejected
// ============================================================================

TEST(Class, SelfParameterNameRejected) {
  auto r = semaCheck(R"(
    class A {
      fn __init__() {}
      fn m(self: int) -> int { return 1; }
    }
    fn main() -> int {
      a: A = A();
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'self' cannot be used as a parameter name"),
            std::string::npos);
}

TEST(Class, SelfParameterNameInInitRejected) {
  auto r = semaCheck(R"(
    class A {
      fn __init__(self: int) {}
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'self' cannot be used as a parameter name"),
            std::string::npos);
}

// ============================================================================
// __init__ definite-assignment: bool-exhaustive match
// ============================================================================

TEST(Class, InitFieldAssignedInBoolMatchArmsOk) {
  // True + False literal arms over a bool subject cover every value, so a
  // field assigned in both arms is definitely assigned.
  auto r = semaCheck(R"(
    class A {
      x: int;
      fn __init__(b: bool) {
        match b {
          True  { self.x = 1; }
          False { self.x = 0; }
        }
      }
    }
    fn main() -> int {
      a: A = A(True);
      return a.x;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, InitFieldMissingInOneBoolMatchArmRejected) {
  auto r = semaCheck(R"(
    class A {
      x: int;
      fn __init__(b: bool) {
        match b {
          True  { self.x = 1; }
          False { println("no"); }
        }
      }
    }
    fn main() -> int {
      a: A = A(True);
      return a.x;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("not assigned on every path"),
            std::string::npos);
}

// ============================================================================
// Error recovery: a rejected class declaration (#79)
// ============================================================================
//
// One bad class must not make the others undeclared, and uses of the bad
// class itself are not reported again: only the declaration error is.

static size_t errorCount(const std::string &diags) {
  size_t n = 0;
  for (size_t at = diags.find("error:"); at != std::string::npos;
       at = diags.find("error:", at + 1))
    ++n;
  return n;
}

TEST(ClassRecovery, BuiltinNameClassDoesNotHideOtherClasses) {
  auto r = semaCheck(withClasses("class A { fn __init__() { } }\n"
                                 "class Int { }",
                                 "a = A();\nreturn 0;"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'Int' is a builtin class and cannot be "
                               "redeclared"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(errorCount(r.Diagnostics), 1u) << r.Diagnostics;
}

TEST(ClassRecovery, NameTakenByEnumReportsEachClassOnce) {
  auto r = semaCheck(withClasses("enum Color { Red }\n"
                                 "class Color { }\n"
                                 "class Color { }\n"
                                 "class Fine { fn m() -> int { return 1; } }",
                                 "c = Color();\nf = Fine();\nreturn f.m();"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("class 'Color' conflicts with an enum"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("redefinition of class 'Color'"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(errorCount(r.Diagnostics), 2u) << r.Diagnostics;
}

TEST(ClassRecovery, UndefinedSuperclassStillDeclaresTheClass) {
  // C is registered (on Obj), so constructing it and using its own members
  // is fine, and an inherited member that may be missing is not reported.
  auto r = semaCheck(withClasses(
      "class C : Missing { y: int; fn __init__() { self.y = 1; } }\n"
      "class D : C { fn __init__() { __super__(); }\n"
      "  fn get() -> int { return self.y + self.z; } }\n"
      "class Ok { fn __init__() { } }",
      "c = C();\nc.y = 4;\nd = D();\nn = d.inherited();\no = Ok();\n"
      "return d.get();"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("superclass 'Missing' of class 'C' is not "
                               "defined"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(errorCount(r.Diagnostics), 1u) << r.Diagnostics;
}

TEST(ClassRecovery, BadMemberTypeIsReportedOnce) {
  auto r = semaCheck(
      withClasses("class A { x: Nope; fn __init__(v: int) { self.x = v; }\n"
                  "          fn get() -> int { return self.x; } }\n"
                  "class B : A { fn __init__() { __super__(1); } }\n"
                  "class E { fn __init__(n: int) { } }",
                  "a = A(3);\nb = B();\nv: int = a.x + b.get();\ne = E(1, 2);\n"
                  "return v;"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("field 'x' in class 'A' has unknown class type "
                               "'Nope'"),
            std::string::npos)
      << r.Diagnostics;
  // A real error in a healthy class is still reported.
  EXPECT_NE(r.Diagnostics.find("function 'E' expects 1 argument(s), got 2"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(errorCount(r.Diagnostics), 2u) << r.Diagnostics;
}

TEST(ClassRecovery, InheritanceCycleIsReportedOnce) {
  auto r = semaCheck(withClasses("class P : Q { }\n"
                                 "class Q : P { fn f() -> int { return 1; } }\n"
                                 "class R : R { }",
                                 "q = Q();\np = P();\nr = R();\n"
                                 "return q.f() + p.f();"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("cyclic inheritance involving class 'P'"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("cyclic inheritance involving class 'R'"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(errorCount(r.Diagnostics), 2u) << r.Diagnostics;
}
