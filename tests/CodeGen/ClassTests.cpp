// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen tests: class layout, constructors, fields, methods, inheritance,
// and match statement dispatch.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}
static std::string withFns(const std::string &fns, const std::string &body) {
  return fns + "\n" + wrapMain(body);
}

// ============================================================================
// Class CodeGen — struct layout, constructor, fields, methods, inheritance
// ============================================================================

// --- Basic instantiation & constructor -------------------------------------

// A class with no fields and no __init__ can be instantiated without crashing.
TEST(Class, EmptyClassInstantiate) {
  auto r = compileAndRun(withFns(R"(
    class Empty {}
  )", R"(
    e: Empty = Empty();
    out("ok");
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "ok\n");
}

// Constructor with __init__ that stores an int field.
TEST(Class, InitStoresIntField) {
  auto r = compileAndRun(withFns(R"(
    class Counter {
      count: int;
      fn __init__(n: int) {
        self.count = n;
      }
    }
  )", R"(
    c: Counter = Counter(42);
    out(StringInt(c.count));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "42\n");
}

// Default (zero) initialisation: int field is 0 before any __init__ writes it.
TEST(Class, FieldDefaultZero) {
  auto r = compileAndRun(withFns(R"(
    class Box {
      val: int;
    }
  )", R"(
    b: Box = Box();
    out(StringInt(b.val));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n");
}

// --- Field read / write ----------------------------------------------------

// MemberAssignStmt: assign then read back independent int fields.
TEST(Class, FieldReadWrite) {
  auto r = compileAndRun(withFns(R"(
    class Point {
      x: int;
      y: int;
    }
  )", R"(
    p: Point = Point();
    p.x = 3;
    p.y = 7;
    out(StringInt(p.x));
    out(StringInt(p.y));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3\n7\n");
}

// Three int fields written via __init__ and read back.
TEST(Class, MultipleIntFields) {
  auto r = compileAndRun(withFns(R"(
    class Triple {
      a: int;
      b: int;
      c: int;
      fn __init__(x: int, y: int, z: int) {
        self.a = x;
        self.b = y;
        self.c = z;
      }
    }
  )", R"(
    t: Triple = Triple(1, 2, 3);
    out(StringInt(t.a));
    out(StringInt(t.b));
    out(StringInt(t.c));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1\n2\n3\n");
}

// Str-typed field: store and retrieve a string.
TEST(Class, StrField) {
  auto r = compileAndRun(withFns(R"(
    class Greeter {
      msg: Str;
      fn __init__(s: Str) {
        self.msg = s;
      }
    }
  )", R"(
    g: Greeter = Greeter("hello");
    out(g.msg);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "hello\n");
}

// Reassigning a field (via a method) updates the stored value.
TEST(Class, FieldReassign) {
  auto r = compileAndRun(withFns(R"(
    class Mutable {
      val: int;
      fn __init__(v: int) { self.val = v; }
      fn set(v: int) { self.val = v; }
    }
  )", R"(
    m: Mutable = Mutable(10);
    m.set(99);
    out(StringInt(m.val));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "99\n");
}

// --- Methods ---------------------------------------------------------------

// Method with no parameters returns a field value.
TEST(Class, MethodReturnsField) {
  auto r = compileAndRun(withFns(R"(
    class Foo {
      x: int;
      fn __init__(v: int) { self.x = v; }
      fn get() -> int { return self.x; }
    }
  )", R"(
    f: Foo = Foo(7);
    out(StringInt(f.get()));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "7\n");
}

// Method takes a parameter and mutates a field.
TEST(Class, MethodWithParam) {
  auto r = compileAndRun(withFns(R"(
    class Acc {
      total: int;
      fn add(n: int) { self.total = self.total + n; }
    }
  )", R"(
    a: Acc = Acc();
    a.add(5);
    a.add(3);
    out(StringInt(a.total));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "8\n");
}

// Method returns a computed value (width * height).
TEST(Class, MethodComputed) {
  auto r = compileAndRun(withFns(R"(
    class Rect {
      w: int;
      h: int;
      fn __init__(w: int, h: int) { self.w = w; self.h = h; }
      fn area() -> int { return self.w * self.h; }
    }
  )", R"(
    r: Rect = Rect(4, 5);
    out(StringInt(r.area()));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "20\n");
}

// Two distinct instances are fully independent.
TEST(Class, TwoInstancesIndependent) {
  auto r = compileAndRun(withFns(R"(
    class Val {
      n: int;
      fn __init__(v: int) { self.n = v; }
    }
  )", R"(
    a: Val = Val(1);
    b: Val = Val(2);
    out(StringInt(a.n));
    out(StringInt(b.n));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1\n2\n");
}

// --- Inheritance -----------------------------------------------------------

// Subclass inherits parent field via __super__.
TEST(Class, InheritedField) {
  auto r = compileAndRun(withFns(R"(
    class Animal {
      name: Str;
      fn __init__(n: Str) { self.name = n; }
    }
    class Dog : Animal {
      fn __init__(n: Str) { __super__(n); }
    }
  )", R"(
    d: Dog = Dog("Rex");
    out(d.name);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Rex\n");
}

// Subclass adds its own field on top of the parent's field.
TEST(Class, SubclassOwnField) {
  auto r = compileAndRun(withFns(R"(
    class Base {
      x: int;
      fn __init__(v: int) { self.x = v; }
    }
    class Child : Base {
      y: int;
      fn __init__(v: int, w: int) {
        __super__(v);
        self.y = w;
      }
    }
  )", R"(
    c: Child = Child(3, 4);
    out(StringInt(c.x));
    out(StringInt(c.y));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3\n4\n");
}

// Subclass overrides a method; its version is called.
TEST(Class, MethodOverride) {
  auto r = compileAndRun(withFns(R"(
    class Shape {
      fn describe() -> Str { return "shape"; }
    }
    class Circle : Shape {
      fn describe() -> Str { return "circle"; }
    }
  )", R"(
    c: Circle = Circle();
    out(c.describe());
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "circle\n");
}

// __super__ correctly chains through parent __init__.
TEST(Class, SuperInit) {
  auto r = compileAndRun(withFns(R"(
    class Vehicle {
      speed: int;
      fn __init__(s: int) { self.speed = s; }
    }
    class Car : Vehicle {
      brand: Str;
      fn __init__(s: int, b: Str) {
        __super__(s);
        self.brand = b;
      }
    }
  )", R"(
    car: Car = Car(120, "Toyota");
    out(StringInt(car.speed));
    out(car.brand);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "120\nToyota\n");
}

// Three-level inheritance: grandchild accesses grandparent field.
TEST(Class, ThreeLevelInheritance) {
  auto r = compileAndRun(withFns(R"(
    class A {
      val: int;
      fn __init__(v: int) { self.val = v; }
    }
    class B : A {
      fn __init__(v: int) { __super__(v); }
    }
    class C : B {
      fn __init__(v: int) { __super__(v); }
    }
  )", R"(
    c: C = C(99);
    out(StringInt(c.val));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "99\n");
}

// Inherited method (not overridden) is callable on subclass instance.
TEST(Class, InheritedMethod) {
  auto r = compileAndRun(withFns(R"(
    class Base {
      n: int;
      fn __init__(v: int) { self.n = v; }
      fn doubled() -> int { return self.n * 2; }
    }
    class Sub : Base {
      fn __init__(v: int) { __super__(v); }
    }
  )", R"(
    s: Sub = Sub(6);
    out(StringInt(s.doubled()));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "12\n");
}

// --- Class used in functions -----------------------------------------------

// Class instance passed to a function; function reads a field.
TEST(Class, PassToFunction) {
  auto r = compileAndRun(withFns(R"(
    class Num {
      v: int;
      fn __init__(x: int) { self.v = x; }
    }
    fn show(n: Num) {
      out(StringInt(n.v));
    }
  )", R"(
    x: Num = Num(55);
    show(x);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "55\n");
}

// Function returns a class instance; caller reads its fields.
TEST(Class, ReturnClassFromFunction) {
  auto r = compileAndRun(withFns(R"(
    class Pair {
      a: int;
      b: int;
      fn __init__(x: int, y: int) { self.a = x; self.b = y; }
    }
    fn makePair(x: int, y: int) -> Pair {
      return Pair(x, y);
    }
  )", R"(
    p: Pair = makePair(3, 7);
    out(StringInt(p.a));
    out(StringInt(p.b));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3\n7\n");
}

// --- Control flow inside methods -------------------------------------------

// Method with if/else returns different strings.
TEST(Class, MethodWithIfElse) {
  auto r = compileAndRun(withFns(R"(
    class Sign {
      fn of(n: int) -> Str {
        if (n > 0) {
          return "pos";
        } else {
          return "neg";
        }
      }
    }
  )", R"(
    s: Sign = Sign();
    out(s.of(5));
    out(s.of(-3));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "pos\nneg\n");
}

// Method uses a while loop to accumulate into a field.
TEST(Class, MethodWithLoop) {
  auto r = compileAndRun(withFns(R"(
    class Summer {
      total: int;
      fn sumTo(n: int) {
        i: int = 1;
        while (i <= n) {
          self.total = self.total + i;
          i = i + 1;
        }
      }
    }
  )", R"(
    s: Summer = Summer();
    s.sumTo(5);
    out(StringInt(s.total));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "15\n");
}

// ============================================================================
// Match statement
// ============================================================================

// Shared class hierarchy used across match tests.
static const std::string kMatchHierarchy = R"(
  class Animal {
    fn __init__() {}
    fn speak() -> Str { return "..."; }
  }
  class Dog : Animal {
    fn __init__() { __super__(); }
    fn speak() -> Str { return "Woof"; }
  }
  class Cat : Animal {
    fn __init__() { __super__(); }
    fn speak() -> Str { return "Meow"; }
  }
  class Bird : Animal {
    fn __init__() { __super__(); }
    fn speak() -> Str { return "Tweet"; }
  }
  class Labrador : Dog {
    name: Str;
    fn __init__() { __super__(); self.name = "Rex"; }
  }
)";

// Basic dispatch: each subclass hits the right arm.
TEST(MatchCodeGen, BasicDispatch) {
  auto r = compileAndRun(withFns(kMatchHierarchy, R"(
    a: Animal = Dog();
    match a {
      Dog  { out("dog"); }
      Cat  { out("cat"); }
      Bird { out("bird"); }
      _    { out("other"); }
    }
    b: Animal = Cat();
    match b {
      Dog  { out("dog"); }
      Cat  { out("cat"); }
      Bird { out("bird"); }
      _    { out("other"); }
    }
    c: Animal = Bird();
    match c {
      Dog  { out("dog"); }
      Cat  { out("cat"); }
      Bird { out("bird"); }
      _    { out("other"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "dog\ncat\nbird\n");
}

// Wildcard arm fires when no type arm matches.
TEST(MatchCodeGen, WildcardFallthrough) {
  auto r = compileAndRun(withFns(kMatchHierarchy, R"(
    a: Animal = Bird();
    match a {
      Dog { out("dog"); }
      Cat { out("cat"); }
      _   { out("other"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "other\n");
}

// Wildcard-only match always fires.
TEST(MatchCodeGen, WildcardOnly) {
  auto r = compileAndRun(withFns(kMatchHierarchy, R"(
    a: Animal = Cat();
    match a {
      _ { out("wildcard"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "wildcard\n");
}

// Binding introduces a narrowed alias; int field is accessible.
TEST(MatchCodeGen, BindingIntField) {
  auto r = compileAndRun(withFns(R"(
    class Shape { fn __init__() {} }
    class Rect : Shape {
      w: int;
      h: int;
      fn __init__(a: int, b: int) { __super__(); self.w = a; self.h = b; }
    }
  )", R"(
    s: Shape = Rect(4, 5);
    match s {
      r: Rect { out(StringInt(r.w)); out(StringInt(r.h)); }
      _       { out("other"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "4\n5\n");
}

// Binding with a Str field.
TEST(MatchCodeGen, BindingStrField) {
  auto r = compileAndRun(withFns(kMatchHierarchy, R"(
    a: Animal = Labrador();
    match a {
      lab: Labrador { out(lab.name); }
      _             { out("other"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Rex\n");
}

// Binding + method call on the narrowed type.
TEST(MatchCodeGen, BindingMethodCall) {
  auto r = compileAndRun(withFns(kMatchHierarchy, R"(
    a: Animal = Dog();
    match a {
      dog: Dog { out(dog.speak()); }
      _        { out("other"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Woof\n");
}

// Exact-type dispatch: Labrador does NOT match Dog arm.
TEST(MatchCodeGen, ExactTypeLabradorNotDog) {
  auto r = compileAndRun(withFns(kMatchHierarchy, R"(
    a: Animal = Labrador();
    match a {
      Dog      { out("dog"); }
      Labrador { out("labrador"); }
      _        { out("other"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "labrador\n");
}

// Exact-type dispatch: Labrador falls to wildcard when only Dog arm present.
TEST(MatchCodeGen, LabradorFallsToWildcard) {
  auto r = compileAndRun(withFns(kMatchHierarchy, R"(
    a: Animal = Labrador();
    match a {
      Dog { out("dog"); }
      Cat { out("cat"); }
      _   { out("other"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "other\n");
}

// Match inside a function: match-as-sole-return path.
TEST(MatchCodeGen, MatchInFunction) {
  auto r = compileAndRun(withFns(kMatchHierarchy + R"(
    fn describe(x: Animal) -> Str {
      match x {
        Dog  { return "dog"; }
        Cat  { return "cat"; }
        Bird { return "bird"; }
        _    { return "unknown"; }
      }
      return "unreachable";
    }
  )", R"(
    out(describe(Dog()));
    out(describe(Cat()));
    out(describe(Bird()));
    out(describe(Labrador()));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "dog\ncat\nbird\nunknown\n");
}

// Multiple match statements in sequence on the same subject.
TEST(MatchCodeGen, MultipleMatchStmts) {
  auto r = compileAndRun(withFns(kMatchHierarchy, R"(
    a: Animal = Cat();
    match a {
      Dog { out("1:dog"); }
      Cat { out("1:cat"); }
      _   { out("1:other"); }
    }
    match a {
      Bird { out("2:bird"); }
      Cat  { out("2:cat"); }
      _    { out("2:other"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1:cat\n2:cat\n");
}

// Arm body with control flow (if/else inside a match arm).
TEST(MatchCodeGen, ControlFlowInArm) {
  auto r = compileAndRun(withFns(R"(
    class Node { fn __init__() {} }
    class Leaf : Node {
      val: int;
      fn __init__(v: int) { __super__(); self.val = v; }
    }
  )", R"(
    n: Node = Leaf(7);
    match n {
      leaf: Leaf {
        if (leaf.val > 5) {
          out("big");
        } else {
          out("small");
        }
      }
      _ { out("other"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "big\n");
}

// Match on a subject that is itself a function call result.
TEST(MatchCodeGen, SubjectIsCallExpr) {
  auto r = compileAndRun(withFns(kMatchHierarchy + R"(
    fn makeAnimal(kind: int) -> Animal {
      if (kind == 1) { return Dog(); }
      return Cat();
    }
  )", R"(
    match makeAnimal(1) {
      Dog { out("dog"); }
      Cat { out("cat"); }
      _   { out("other"); }
    }
    match makeAnimal(2) {
      Dog { out("dog"); }
      Cat { out("cat"); }
      _   { out("other"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "dog\ncat\n");
}

// Three-level hierarchy: exact-type dispatch at every level.
TEST(MatchCodeGen, DeepInheritanceExactType) {
  auto r = compileAndRun(withFns(R"(
    class A { fn __init__() {} }
    class B : A { fn __init__() { __super__(); } }
    class C : B { fn __init__() { __super__(); } }
  )", R"(
    x: A = A();
    match x {
      C { out("C"); }
      B { out("B"); }
      A { out("A"); }
      _ { out("?"); }
    }
    y: A = B();
    match y {
      C { out("C"); }
      B { out("B"); }
      A { out("A"); }
      _ { out("?"); }
    }
    z: A = C();
    match z {
      C { out("C"); }
      B { out("B"); }
      A { out("A"); }
      _ { out("?"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "A\nB\nC\n");
}

// Arm with a local Str variable (tests that arm scope cleanup is correct).
TEST(MatchCodeGen, ArmScopeCleanup) {
  auto r = compileAndRun(withFns(kMatchHierarchy, R"(
    a: Animal = Dog();
    match a {
      Dog {
        tmp: Str = "cleanup-me";
        out(tmp);
      }
      _ { out("other"); }
    }
    out("after");
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "cleanup-me\nafter\n");
}
