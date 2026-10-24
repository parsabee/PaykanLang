// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: class declarations, inheritance, methods, fields, match.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string withClasses(const std::string &classDefs,
                               const std::string &body = "return 0;") {
  return classDefs + "import ::io; \nfn main() -> int {\n" + body + "\n}\n";
}

// -- Basic class structure

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

TEST(Class, SelfCannotBeAssigned) {
  // `self` names the object the method was called on; rebinding it would
  // change nothing the caller sees.
  auto r = semaCheck(withClasses(R"(
    class Counter {
      count: int;
      fn reset() { self = Counter(0); }
    }
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("cannot assign to 'self'"), std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
}

TEST(Class, ClassInitSetField) {
  auto r = semaCheck(withClasses(R"(
    class Point {
      x: int; y: int;
    }

    impl Point {
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

// -- destroy is the compiler-generated destructor: final and not callable

TEST(Class, DestroyDirectCallRejected) {
  auto r =
      semaCheck(withClasses("class Box { v: int; }\n"
                            "impl Box { fn __init__(x: int) { self.v = x; } }",
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
      withClasses("class Box { v: int;\n"
                  "  fn destroy() { } }\n"
                  "impl Box { fn __init__(x: int) { self.v = x; } }"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("destroy"), std::string::npos) << r.Diagnostics;
}

// -- Duplicate definitions

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

// Top-level names are shared with the builtins: a class name is also its
// constructor, so it may not reuse a builtin function, builtin class, or enum.

// Regression: `class print` used to silently replace the builtin `print`.
// The standard library's names (io::print, io::File, err::Error) are not
// reserved: a program may name its own classes so.  (Type names are still
// global across an import graph, so one that imports ::io cannot also
// declare a `File`: docs/design/pkm.md, module-local type names.)
TEST(Class, TheStandardLibrarysNamesAreNotReserved) {
  auto r = semaCheck("class print {}\n"
                     "impl print { fn __init__() {} }\n"
                     "class File {}\nclass Error {}\n"
                     "fn main() -> int { p = print(); return 0; }\n");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  r = semaCheck("import ::io;\nclass File {}\n"
                "fn main() -> int { return 0; }\n");
  EXPECT_NE(
      r.Diagnostics.find(
          "class 'File' conflicts with an imported type of the same name"),
      std::string::npos)
      << r.Diagnostics;
}

// The conversion builtins removed by #64 no longer reserve their names.
TEST(Class, RemovedConversionNamesAreFree) {
  auto r = semaCheck(withClasses("class StrInt {}\n"
                                 "impl StrInt { fn __init__() {} }"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// The boxed primitive classes removed by #96 no longer reserve their names.
TEST(Class, RemovedBoxedClassNamesAreFree) {
  auto r =
      semaCheck(withClasses("class Int { v: int; }\n"
                            "impl Int { fn __init__(x: int) { self.v = x; } }\n"
                            "class Char {}\n"
                            "impl Char { fn __init__() {} }",
                            "i: Int = Int(3); n: int = i.v; return 0;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// A builtin type's name (`int`, a sized type, `void`) is nobody's to
// declare, whatever the kind of declaration.
TEST(Class, BuiltinTypeNamesAreReserved) {
  for (const char *decl :
       {"class int { v: int; }", "class u8 { v: int; }",
        "struct f32 { v: int; }", "enum unsigned { A }",
        "fn float(x: int) -> int { return x; }", "fn i64() { }",
        "trait void { fn f(); }", "class bool { v: int; }"}) {
    auto r =
        semaCheck(std::string(decl) + "\nfn main() -> int { return 0; }\n");
    EXPECT_FALSE(r.Ok) << decl;
    EXPECT_NE(r.Diagnostics.find("is a builtin type and cannot be redeclared"),
              std::string::npos)
        << decl << "\n"
        << r.Diagnostics;
  }
}

TEST(Class, NameShadowsBuiltinClassRejected) {
  for (const char *name : {"Obj", "Str"}) {
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
  auto r = semaCheck(withClasses(R"(import ::io;
    class Logger {
      open: int;
      fn print(msg: Str) { io::println(msg); }
      fn StrInt(n: int) -> Str { return "n"; }
    }

    impl Logger {
      fn __init__() { self.open = 0; }
    }
  )",
                                 R"(
    l: Logger = Logger();
    l.print("a");
    io::print(l.StrInt(l.open) + Str<int>(2));
    return 0;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Unknown types

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

// -- Field access & type errors

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

// -- Method return type checks

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

// -- Inheritance

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

// -- __super__

TEST(Class, SuperCallValid) {
  auto r = semaCheck(withClasses(R"(
    class Animal {
      name: Str;
    }

    impl Animal {
      fn __init__(n: Str) { self.name = n; }
    }
    class Dog : Animal {
      breed: Str;
    }

    impl Dog {
      fn __init__(n: Str, b: Str) { __super__(n); self.breed = b; }
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, SuperCallMissing) {
  auto r = semaCheck(withClasses(R"(
    class Animal { name: Str; }
    impl Animal { fn __init__(n: Str) { self.name = n; } }
    class Dog : Animal { breed: Str; }
    impl Dog { fn __init__(n: Str, b: Str) { self.breed = b; } }
  )"));
  EXPECT_FALSE(r.Ok);
}

TEST(Class, SuperCallWrongArgType) {
  auto r = semaCheck(withClasses(R"(
    class Animal {}
    impl Animal { fn __init__(x: int) {} }
    class Dog : Animal {}
    impl Dog { fn __init__() { __super__(True); } }
  )"));
  EXPECT_FALSE(r.Ok);
}

// `__super__`'s arguments are checked against the superclass `__init__`'s
// parameters as a call's are: a literal takes its parameter's type.
TEST(Class, SuperCallLiteralsTakeTheParameterTypes) {
  auto r = semaCheck(withClasses(R"(
    class Node { v: int; }
    class Base {}
    impl Base {
      fn __init__(xs: float[2], link: Node?, pair: (Node?, int), q: int?,
                  names: Str?[2]) {}
    }
    class Derived : Base {}
    impl Derived {
      fn __init__() { __super__([1, 2], None, (None, 3), 4, [None, "b"]); }
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  auto bad = semaCheck(withClasses(R"(
    class Base {}
    impl Base { fn __init__(xs: float[2]) {} }
    class Derived : Base {}
    impl Derived { fn __init__() { __super__([1, 2, 3]); } }
  )"));
  EXPECT_FALSE(bad.Ok);
  EXPECT_NE(bad.Diagnostics.find("array literal has 3 element(s), but "
                                 "'float[2]' has 2"),
            std::string::npos)
      << bad.Diagnostics;
}

TEST(Class, SuperCallOutsideInit) {
  auto r = semaCheck(withClasses(R"(
    class A { fn foo() { __super__(); } }
  )"));
  EXPECT_FALSE(r.Ok);
}

TEST(Class, SuperCallNoSuperclass) {
  auto r = semaCheck(withClasses(R"(
    class A {}
    impl A { fn __init__() { __super__(); } }
  )"));
  EXPECT_FALSE(r.Ok);
}

// -- match statement

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
      x: Obj = Dog("");
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

// -- Check 3 — override signature mismatch (strict equality, no covariance)

TEST(Class, OverrideReturnTypeMismatchRejected) {
  auto r = semaCheck(R"(
    class Animal {
      fn describe() -> int { return 1; }
    }

    impl Animal {
      fn __init__() {}
    }
    class Dog : Animal {
      fn describe() -> Str { return "dog"; }
    }

    impl Dog {
      fn __init__() { __super__(); }
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
      fn take(x: int) -> int { return x; }
    }

    impl Base {
      fn __init__() {}
    }
    class Derived : Base {
      fn take(x: Str) -> int { return 0; }
    }

    impl Derived {
      fn __init__() { __super__(); }
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
      fn f(x: int) -> int { return x; }
    }

    impl Base {
      fn __init__() {}
    }
    class Derived : Base {
      fn f() -> int { return 0; }
    }

    impl Derived {
      fn __init__() { __super__(); }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Class, OverrideMatchingSignatureAccepted) {
  auto r = semaCheck(R"(
    class Animal {
      fn describe() -> int { return 1; }
    }

    impl Animal {
      fn __init__() {}
    }
    class Dog : Animal {
      fn describe() -> int { return 2; }
    }

    impl Dog {
      fn __init__() { __super__(); }
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
      view fn toString() -> Str { return "Person " + self.name; }
      view fn equals(other: Obj) -> bool { return True; }
    }

    impl Person {
      fn __init__(n: Str) { self.name = n; }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, OverrideThreeLevelMismatchRejected) {
  auto r = semaCheck(R"(
    class A { fn kind() -> int { return 0; } }
    impl A { fn __init__() {} }
    class B : A { fn kind() -> int { return 1; } }
    impl B { fn __init__() { __super__(); } }
    class C : B { fn kind() -> Str { return "c"; } }
    impl C { fn __init__() { __super__(); } }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
}

// -- Check 2 — derived __init__ must call __super__() as its first statement

TEST(Class, SuperRequiredForParameterizedBase) {
  auto r = semaCheck(R"(
    class Base {
      val: int;
    }

    impl Base {
      fn __init__(v: int) { self.val = v; }
    }
    class Derived : Base {}

    impl Derived {
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
    class Base {}
    impl Base { fn __init__() {} }
    class Child : Base {
      x: int;
    }

    impl Child {
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
    }

    impl Base {
      fn __init__(v: int) { self.val = v; }
    }
    class Derived : Base {
      y: int;
    }

    impl Derived {
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

TEST(Class, DerivedWithoutInitPassesTheBaseInitsArguments) {
  auto r = semaCheck(R"(
    class Base {
      val: int;
    }

    impl Base {
      fn __init__(v: int) { self.val = v; }
    }
    class Derived : Base {
      y: Str;
    }
    fn main() -> int {
      d = Derived(1, "a");
      return d.val;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// A class without an `__init__` gets the default one: the superclass
// constructor's arguments, then one per field, in order.  A generic one's
// infers its type arguments from them.
TEST(Class, DefaultInitTakesTheFields) {
  auto r = semaCheck(R"(
    class Point { x: int; y: int; }
    class Named : Point { name: Str; }
    class Empty {}
    class Box<T> { v: T; }
    class Tagged<T> : Named { tag: T; }
    fn main() -> int {
      p = Point(1, 2);
      n = Named(3, 4, "n");
      e = Empty();
      b = Box(5);
      t = Tagged(6, 7, "t", True);
      ok: bool = t.tag;
      return p.x + n.y + b.v + t.x;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Class, DefaultInitNeedsEveryField) {
  auto r = semaCheck(R"(
    class P { x: int; s: Str; }
    class E {}
    fn main() -> int {
      a = P();
      b = P(1, 2);
      c = E(1);
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  for (const char *d :
       {"function 'P' expects 2 argument(s), got 0 (it has no '__init__', so "
        "it takes x: int, s: Str)",
        "argument 2 of 'P'",
        "function 'E' expects 0 argument(s), got 1 (it has no '__init__', so "
        "it takes nothing)"})
    EXPECT_NE(r.Diagnostics.find(d), std::string::npos) << d << "\n"
                                                        << r.Diagnostics;
}

// An `__init__` of a subclass calls the superclass's default one when it
// has fields to set; one of a class with none, up the chain, does nothing.
TEST(Class, SuperCallOfADefaultInit) {
  auto r = semaCheck(R"(
    class Shape {}
    class Rect : Shape { w: int; }
    impl Rect { fn __init__(w: int) { self.w = w; } }
    class Square : Rect {}
    class Cube : Square { d: int; }
    impl Cube { fn __init__(w: int) { __super__(w); self.d = w; } }
    fn main() -> int { return Rect(1).w + Cube(2).d; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  r = semaCheck(R"(
    class Base { v: int; }
    class Derived : Base { y: int; }
    impl Derived { fn __init__(y: int) { self.y = y; } }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find(
                "'__init__' in class 'Derived' must call '__super__' as its "
                "first statement because its superclass 'Base' has fields to "
                "initialize (its default '__init__' takes v: int)"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Class, SuperFirstStatementAccepted) {
  auto r = semaCheck(R"(
    class Base {}
    impl Base { fn __init__() {} }
    class Child : Base {
      x: int;
    }

    impl Child {
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
    }

    impl Base {
      fn __init__(v: int) { self.val = v; }
    }
    class Derived : Base {
      y: int;
    }

    impl Derived {
      fn __init__(v: int) { __super__(v); self.y = v; }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Check 1 — __init__ must definitely assign every field on every path

TEST(Class, InitMissingFieldRejected) {
  auto r = semaCheck(R"(
    class Bad {
      x: int;
      y: int;
      view fn toString() -> Str { return Str<int>(self.x) + Str<int>(self.y); }
    }

    impl Bad {
      fn __init__(a: int) { self.x = a; }
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
    }

    impl Point {
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
    }

    impl C {
      fn __init__(a: int) {
        if a > 0 { self.x = 1; } else { self.x = 2; }
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
    }

    impl C {
      fn __init__(a: int) {
        if a > 0 { self.x = 1; }
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
    }

    impl C {
      fn __init__(a: int) {
        self.x = a;
        if a > 0 { return; }
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
    }

    impl C {
      fn __init__(a: int) {
        self.x = a;
        if a > 0 { return; }
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
    }

    impl C {
      fn __init__(a: int) {
        while a > 0 { self.x = a; }
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
    }

    impl Base {
      fn __init__(a: int) { self.a = a; }
    }
    class Derived : Base {
      b: int;
    }

    impl Derived {
      fn __init__(a: int, b: int) { __super__(a); self.b = b; }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Every field before `self` (docs/language/05-classes.md)

// Until every field is assigned, `self` is only a place to assign fields and
// to read the ones assigned on every path so far: reading another field,
// calling a method on it, passing, storing or capturing it are errors, in a
// class's and a struct's `__init__`, and so is any use in `__super__(...)`'s
// arguments.
TEST(Class, InitUsesSelfOnlyOnceEveryFieldIsAssigned) {
  auto r = semaCheck(R"(import ::io;
class C {
  name: Str;
  n: int;
  view fn show() -> int { return self.n; }
}
fn look(c: C) -> int { return c.n; }
impl C {
  fn __init__(flag: bool) {
    io::println(self.name);
    self.n = self.n + 1;
    if flag { self.name = "a"; }
    x = self.show();
    y = look(self);
    f = fn() -> int { return self.n; };
    self.name = self.name + "b";
  }
}
class B { a: int; }
impl B { fn __init__(a: int) { self.a = a; } }
class D : B { d: int; }
impl D {
  fn __init__() {
    __super__(self.a);
    self.d = self.a;
  }
}
struct S { x: int; y: int; }
impl S { fn __init__() { self.x = self.y; self.y = 1; } }
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  for (const char *e :
       {"10:17: error: field 'name' is read before it is assigned",
        "11:14: error: field 'n' is read before it is assigned",
        "13:9: error: 'self' is used before every field is assigned: 'name' "
        "is not assigned yet",
        "14:14: error: 'self' is used before every field is assigned: 'name' "
        "is not assigned yet",
        "15:9: error: 'self' is used before every field is assigned: 'name' "
        "is not assigned yet",
        "16:17: error: field 'name' is read before it is assigned",
        "24:15: error: 'self' is used before every field is assigned: "
        "'__super__' has not run yet",
        "29:35: error: field 'y' is read before it is assigned"})
    EXPECT_NE(r.Diagnostics.find(e), std::string::npos) << e << "\n"
                                                        << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 8u) << r.Diagnostics;
}

// What is assigned on every path so far can be read, an optional field (None
// until assigned) and the superclass's fields after `__super__(...)` too;
// once every field is, `self` is an ordinary object.  A `while True` left
// only by `return` assigns what each return has.
TEST(Class, InitReadsWhatIsAssignedAndThenUsesSelf) {
  auto r = semaCheck(R"(import ::io;
class B { a: int; }
impl B { fn __init__(a: int) { self.a = a; } }
class C : B {
  name: Str;
  next: C?;
  n: int;
  view fn show() -> Str { return self.name + ""; }
}
fn look(c: C) -> int { return c.n; }
impl C {
  fn __init__(flag: bool) {
    __super__(1);
    if flag { self.name = "a"; } else { self.name = "b"; }
    match self.next { c: C { self.n = c.n; } None { self.n = self.a; } }
    self.n = self.n + self.name.len();
    io::println(self.show() + Str(look(self)));
    f = fn() -> int { return self.n; };
    keep = self;
  }
}
struct S { x: int; y: int; }
impl S { fn __init__() { self.x = 1; self.y = self.x; self.x = self.sum(); } }
impl S { view fn sum() -> int { return self.x + self.y; } }
class W { a: int; }
impl W {
  fn __init__(k: int) { while True { if k > 0 { self.a = k; return; } k += 1; } }
}
fn main() -> int { c = C(True); return S().y + c.n + W(0).a; }
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// Calling the function a field holds reads the field and passes `self`
// nowhere: once the field is assigned (an own one, or a superclass's after
// `__super__(...)`), `self.f(1)` is fine before every field is; before it, it
// reads a field with no value.
TEST(Class, InitCallsAFunctionFieldOnceItIsAssigned) {
  auto ok = semaCheck(R"(
class C { f: (int) -> int; n: int; }
impl C { fn __init__() { self.f = fn(x: int) => x + 1; self.n = self.f(1); } }
struct S { f: (int) -> int; n: int; }
impl S { fn __init__() { self.f = fn(x: int) => x * 3; self.n = self.f(2); } }
class B { f: (int) -> int; }
class D : B { n: int; }
impl D { fn __init__() { __super__(fn(x: int) => x + 1); self.n = self.f(1); } }
fn main() -> int { return C().n + S().n + D().n; }
)");
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;

  auto bad = semaCheck(R"(
class C { f: (int) -> int; n: int; }
impl C { fn __init__() { self.n = self.f(1); self.f = fn(x: int) => x; } }
class B { f: (int) -> int; k: int; }
class D : B { n: int; }
impl D { fn __init__() { __super__(fn(x: int) => x, self.f(1)); self.n = 1; } }
fn main() -> int { return C().n + D().n; }
)");
  EXPECT_FALSE(bad.Ok);
  for (const char *e :
       {"3:35: error: field 'f' is read before it is assigned",
        "6:53: error: 'self' is used before every field is assigned: "
        "'__super__' has not run yet"})
    EXPECT_NE(bad.Diagnostics.find(e), std::string::npos) << e << "\n"
                                                          << bad.Diagnostics;
  EXPECT_EQ(bad.ErrorCount, 2u) << bad.Diagnostics;
}

// A write in a field's own storage, to an element of a fixed-size array
// field or to a field of a struct field, reads nothing and assigns nothing:
// the field is still to be assigned, and only what reads it is reported.
TEST(Class, InitWritesInAFieldReadNothing) {
  auto r = semaCheck(R"(
struct P { x: int; y: int; }
class D { p: P; grid: int[2][2]; arr: int[3]; n: int; }
impl D {
  fn __init__() {
    self.p.x = 1;
    self.arr[0] = 5;
    self.grid[0][1] = self.p.y;
    self.n = self.grid[0][0] + self.arr[0];
  }
}
fn main() -> int { return D().n; }
)");
  EXPECT_FALSE(r.Ok);
  for (const char *e :
       {"8:23: error: field 'p' is read before it is assigned",
        "9:14: error: field 'grid' is read before it is assigned",
        "9:32: error: field 'arr' is read before it is assigned",
        "5:3: error: field 'p' of class 'D' is not assigned on every path",
        "5:3: error: field 'grid' of class 'D' is not assigned on every path",
        "5:3: error: field 'arr' of class 'D' is not assigned on every path"})
    EXPECT_NE(r.Diagnostics.find(e), std::string::npos) << e << "\n"
                                                        << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 6u) << r.Diagnostics;
}

// An initializer has no result: constructing gives the object (or the
// struct) it set up.
TEST(Class, InitWithAResultTypeRejected) {
  for (const char *decl :
       {"class C { x: int; }\nimpl C { fn __init__() -> Str { self.x = 1; "
        "return \"a\"; } }\n",
        "class C { x: int; }\nimpl C { fn __init__() -> C { self.x = 1; "
        "return self; } }\n",
        "class C { x: int; }\nimpl C { fn __init__() -> void { self.x = 1; } "
        "}\n",
        "struct C { x: int; }\nimpl C { fn __init__() -> int { self.x = 1; "
        "return 1; } }\n"}) {
    auto r = semaCheck(std::string(decl) +
                       "fn main() -> int { c = C(); return c.x; }\n");
    EXPECT_FALSE(r.Ok) << decl;
    EXPECT_NE(r.Diagnostics.find("2:10: error: '__init__' cannot have a "
                                 "result type: it sets up 'self' and returns "
                                 "nothing"),
              std::string::npos)
        << decl << r.Diagnostics;
    // Its constructions and their uses are follow-ons.
    EXPECT_EQ(r.ErrorCount, 1u) << decl << r.Diagnostics;
  }
  // Named as written.
  auto r = semaCheck("class C { x: int; }\nimpl C { fn ___init__() -> int { "
                     "self.x = 1; return 1; } }\n"
                     "fn main() -> int { return 0; }\n");
  EXPECT_NE(r.Diagnostics.find("2:10: error: '___init__' cannot have a result "
                               "type"),
            std::string::npos)
      << r.Diagnostics;
}

// -- `self` as a parameter name is rejected

TEST(Class, SelfParameterNameRejected) {
  auto r = semaCheck(R"(
    class A {
      fn m(self: int) -> int { return 1; }
    }

    impl A {
      fn __init__() {}
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
    class A {}

    impl A {
      fn __init__(self: int) {}
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'self' cannot be used as a parameter name"),
            std::string::npos);
}

// -- `self` cannot be bound in a method

// In a method's body `self` names the object the method was called on: no
// declaration (one in a nested block too), `inout` local, `for` loop
// variable, `match` binding or destructuring target can give the name to
// another value, so `self.f = v` always assigns the method's own object.
TEST(Class, SelfCannotBeBoundInAMethod) {
  const char *const bodies[] = {
      "match other { self: A { self.n = 1; } None { } }",
      "match other { self: inout A { self.n = 1; } None { } }",
      "if self.n > 0 { self: A = A(); }",
      "if self.n > 0 { x = A(); self: inout = x; }",
      "for self in 0..3 { }",
      "self, k = (A(), 2);",
  };
  for (const char *body : bodies) {
    auto r = semaCheck(std::string(R"(
    class A {
      n: int;
      fn m(other: A?) { )") +
                       body + R"( }
    }
    impl A {
      fn __init__() { self.n = 0; }
    }
    fn main() -> int { return 0; }
  )");
    EXPECT_FALSE(r.Ok) << body;
    EXPECT_NE(r.Diagnostics.find("error: 'self' cannot be used as a variable "
                                 "name in method 'm' of class 'A'; it is the "
                                 "implicit receiver"),
              std::string::npos)
        << body << "\n"
        << r.Diagnostics;
    EXPECT_EQ(r.ErrorCount, 1u) << body << "\n" << r.Diagnostics;
  }
}

// An `__init__`'s arm binding `self` would make `self.f = 1` assign the
// matched object's field, though the field-assignment check counted it as
// the new object's: the object would be used with `f` unset.
TEST(Class, InitCannotBindSelfInAMatchArm) {
  auto r = semaCheck(R"(
    class A { f: int; }
    impl A {
      fn __init__(other: A?) {
        match other {
          self: A { self.f = 1; }
          None { self.f = 2; }
        }
      }
    }
    fn main() -> int { return A(A(None)).f; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("6:11: error: 'self' cannot be used as a "
                               "variable name in method '__init__' of class "
                               "'A'; it is the implicit receiver"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
}

// A lambda written in a method is a function of its own, whose parameter
// may be named `self`, and a free function has no receiver: its variables
// may take the name.
TEST(Class, SelfIsAVariableNameOutsideAMethodsBody) {
  auto r = semaCheck(R"(
    class A {
      n: int;
      view fn m() -> int {
        g = fn(self: int) -> int { return self + 1; };
        return g(self.n);
      }
    }
    impl A {
      fn __init__() { self.n = 0; }
    }
    fn f(x: int?) -> int {
      match x {
        self: int { return self; }
        None { return 0; }
      }
    }
    fn main() -> int {
      for self in 0..2 { }
      return f(A().m());
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- __init__ definite-assignment: bool-exhaustive match

TEST(Class, InitFieldAssignedInBoolMatchArmsOk) {
  // True + False literal arms over a bool subject cover every value, so a
  // field assigned in both arms is definitely assigned.
  auto r = semaCheck(R"(
    class A {
      x: int;
    }

    impl A {
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
  auto r = semaCheck(R"(import ::io;
    class A {
      x: int;
    }

    impl A {
      fn __init__(b: bool) {
        match b {
          True  { self.x = 1; }
          False { io::println("no"); }
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

// -- Error recovery: a rejected class declaration (#79)
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
  auto r = semaCheck(withClasses("class A {}\n"
                                 "impl A { fn __init__() { } }\n"
                                 "class Obj { }",
                                 "a = A();\nreturn 0;"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'Obj' is a builtin class and cannot be "
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
      "class C : Missing { y: int; }\n"
      "impl C { fn __init__() { self.y = 1; } }\n"
      "class D : C {\n"
      "  fn get() -> int { return self.y + self.z; } }\n"
      "impl D { fn __init__() { __super__(); } }\n"
      "class Ok {}\n"
      "impl Ok { fn __init__() { } }",
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
      withClasses("class A { x: Nope;\n"
                  "          fn get() -> int { return self.x; } }\n"
                  "impl A { fn __init__(v: int) { self.x = v; } }\n"
                  "class B : A {}\n"
                  "impl B { fn __init__() { __super__(1); } }\n"
                  "class E {}\n"
                  "impl E { fn __init__(n: int) { } }",
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
