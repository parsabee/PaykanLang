// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Codegen tests: class layout, constructors, fields, methods, inheritance,
// and match statement dispatch.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "import ::io; fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}
static std::string withFns(const std::string &fns, const std::string &body) {
  return fns + "\n" + wrapMain(body);
}

// -- Class CodeGen — struct layout, constructor, fields, methods, inheritance

// --- Basic instantiation & constructor -------------------------------------

// A class with no fields and no __init__ can be instantiated without crashing.
TEST(Class, EmptyClassInstantiate) {
  auto r = compileAndRun(withFns(R"(
    class Empty {}
  )",
                                 R"(
    e: Empty = Empty();
    io::println("ok");
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "ok\n");
}

// Constructor with __init__ that stores an int field.
TEST(Class, InitStoresIntField) {
  auto r = compileAndRun(withFns(R"(
    class Counter {
      count: int;
    }

    impl Counter {
      fn __init__(n: int) {
        self.count = n;
      }
    }
  )",
                                 R"(
    c: Counter = Counter(42);
    io::println(Str<int>(c.count));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "42\n");
}

// A class without an __init__ is constructed from its fields, in order.
TEST(Class, DefaultInitTakesTheFields) {
  auto r = compileAndRun(withFns(R"(
    class Box {
      val: int;
      name: Str;
    }
  )",
                                 R"(
    b: Box = Box(5, "five");
    io::println(Str<int>(b.val) + " " + b.name);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "5 five\n");
}

// The default __init__ chains up through superclasses (to a written
// __init__ too) and stores every kind of field: an object, an optional, an
// array, a tuple and a generic one; nothing leaks.
TEST(Class, DefaultInitChainsAndStoresEveryField) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Vehicle { speed: int; }
    impl Vehicle { fn __init__(s: int) { self.speed = s * 10; } }
    class Car : Vehicle { name: Str; }
    class Node { label: Str; next: Node?; }
    class Bag : Car { xs: int[2]; pair: (int, Str); n: int?; }
    class Box<T> { v: T; }
    fn main() -> int {
      c = Car(5, "car");
      l = Node("a", Node("b", None));
      b = Bag(1, "bag", [2, 3], (4, "four"), None);
      x = Box(Node("c", None));
      io::println(Str(c.speed) + " " + c.name + " " + Str(b.speed) + " " +
              Str(b.xs.len()) + " " + b.pair.1 + " " + x.v.label);
      match l.next {
        m: Node { io::println(l.label + m.label); }
        None { io::println("none"); }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "50 car 10 2 four c\nab\n");
  g.expectNoLeaks("DefaultInitChainsAndStoresEveryField");
}

// --- Field read / write ----------------------------------------------------

// MemberAssignStmt: assign then read back independent int fields.
TEST(Class, FieldReadWrite) {
  auto r = compileAndRun(withFns(R"(
    class Point {
      x: int;
      y: int;
    }
  )",
                                 R"(
    p: Point = Point(0, 0);
    p.x = 3;
    p.y = 7;
    io::println(Str<int>(p.x));
    io::println(Str<int>(p.y));
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
    }

    impl Triple {
      fn __init__(x: int, y: int, z: int) {
        self.a = x;
        self.b = y;
        self.c = z;
      }
    }
  )",
                                 R"(
    t: Triple = Triple(1, 2, 3);
    io::println(Str<int>(t.a));
    io::println(Str<int>(t.b));
    io::println(Str<int>(t.c));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1\n2\n3\n");
}

// Str-typed field: store and retrieve a string.
TEST(Class, StrField) {
  auto r = compileAndRun(withFns(R"(
    class Greeter {
      msg: Str;
    }

    impl Greeter {
      fn __init__(s: Str) {
        self.msg = s;
      }
    }
  )",
                                 R"(
    g: Greeter = Greeter("hello");
    io::println(g.msg);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "hello\n");
}

// Reassigning a field (via a method) updates the stored value.
TEST(Class, FieldReassign) {
  auto r = compileAndRun(withFns(R"(
    class Mutable {
      val: int;
      fn set(v: int) { self.val = v; }
    }

    impl Mutable {
      fn __init__(v: int) { self.val = v; }
    }
  )",
                                 R"(
    m: Mutable = Mutable(10);
    m.set(99);
    io::println(Str<int>(m.val));
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
      fn get() -> int { return self.x; }
    }

    impl Foo {
      fn __init__(v: int) { self.x = v; }
    }
  )",
                                 R"(
    f: Foo = Foo(7);
    io::println(Str<int>(f.get()));
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
  )",
                                 R"(
    a: Acc = Acc(0);
    a.add(5);
    a.add(3);
    io::println(Str<int>(a.total));
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
      fn area() -> int { return self.w * self.h; }
    }

    impl Rect {
      fn __init__(w: int, h: int) { self.w = w; self.h = h; }
    }
  )",
                                 R"(
    r: Rect = Rect(4, 5);
    io::println(Str<int>(r.area()));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "20\n");
}

// Two distinct instances are fully independent.
TEST(Class, TwoInstancesIndependent) {
  auto r = compileAndRun(withFns(R"(
    class Val {
      n: int;
    }

    impl Val {
      fn __init__(v: int) { self.n = v; }
    }
  )",
                                 R"(
    a: Val = Val(1);
    b: Val = Val(2);
    io::println(Str<int>(a.n));
    io::println(Str<int>(b.n));
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
    }

    impl Animal {
      fn __init__(n: Str) { self.name = n; }
    }
    class Dog : Animal {}

    impl Dog {
      fn __init__(n: Str) { __super__(n); }
    }
  )",
                                 R"(
    d: Dog = Dog("Rex");
    io::println(d.name);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Rex\n");
}

// Subclass adds its own field on top of the parent's field.
TEST(Class, SubclassOwnField) {
  auto r = compileAndRun(withFns(R"(
    class Base {
      x: int;
    }

    impl Base {
      fn __init__(v: int) { self.x = v; }
    }
    class Child : Base {
      y: int;
    }

    impl Child {
      fn __init__(v: int, w: int) {
        __super__(v);
        self.y = w;
      }
    }
  )",
                                 R"(
    c: Child = Child(3, 4);
    io::println(Str<int>(c.x));
    io::println(Str<int>(c.y));
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
  )",
                                 R"(
    c: Circle = Circle();
    io::println(c.describe());
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "circle\n");
}

// __super__ correctly chains through parent __init__.
TEST(Class, SuperInit) {
  auto r = compileAndRun(withFns(R"(
    class Vehicle {
      speed: int;
    }

    impl Vehicle {
      fn __init__(s: int) { self.speed = s; }
    }
    class Car : Vehicle {
      brand: Str;
    }

    impl Car {
      fn __init__(s: int, b: Str) {
        __super__(s);
        self.brand = b;
      }
    }
  )",
                                 R"(
    car: Car = Car(120, "Toyota");
    io::println(Str<int>(car.speed));
    io::println(car.brand);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "120\nToyota\n");
}

// Three-level inheritance: grandchild accesses grandparent field.
TEST(Class, ThreeLevelInheritance) {
  auto r = compileAndRun(withFns(R"(
    class A {
      val: int;
    }

    impl A {
      fn __init__(v: int) { self.val = v; }
    }
    class B : A {}

    impl B {
      fn __init__(v: int) { __super__(v); }
    }
    class C : B {}

    impl C {
      fn __init__(v: int) { __super__(v); }
    }
  )",
                                 R"(
    c: C = C(99);
    io::println(Str<int>(c.val));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "99\n");
}

// Inherited method (not overridden) is callable on subclass instance.
TEST(Class, InheritedMethod) {
  auto r = compileAndRun(withFns(R"(
    class Base {
      n: int;
      fn doubled() -> int { return self.n * 2; }
    }

    impl Base {
      fn __init__(v: int) { self.n = v; }
    }
    class Sub : Base {}

    impl Sub {
      fn __init__(v: int) { __super__(v); }
    }
  )",
                                 R"(
    s: Sub = Sub(6);
    io::println(Str<int>(s.doubled()));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "12\n");
}

// --- Class used in functions -----------------------------------------------

// Class instance passed to a function; function reads a field.
TEST(Class, PassToFunction) {
  auto r = compileAndRun(withFns(R"(import ::io;
    class Num {
      v: int;
    }

    impl Num {
      fn __init__(x: int) { self.v = x; }
    }
    fn show(n: Num) {
      io::println(Str<int>(n.v));
    }
  )",
                                 R"(
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
    }

    impl Pair {
      fn __init__(x: int, y: int) { self.a = x; self.b = y; }
    }
    fn makePair(x: int, y: int) -> Pair {
      return Pair(x, y);
    }
  )",
                                 R"(
    p: Pair = makePair(3, 7);
    io::println(Str<int>(p.a));
    io::println(Str<int>(p.b));
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
        if n > 0 {
          return "pos";
        } else {
          return "neg";
        }
      }
    }
  )",
                                 R"(
    s: Sign = Sign();
    io::println(s.of(5));
    io::println(s.of(-3));
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
        while i <= n {
          self.total = self.total + i;
          i = i + 1;
        }
      }
    }
  )",
                                 R"(
    s: Summer = Summer(0);
    s.sumTo(5);
    io::println(Str<int>(s.total));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "15\n");
}

// -- Match statement

// Shared class hierarchy used across match tests.
static const std::string kMatchHierarchy = R"(
  class Animal {
    fn speak() -> Str { return "..."; }
  }

  impl Animal {
    fn __init__() {}
  }
  class Dog : Animal {
    fn speak() -> Str { return "Woof"; }
  }

  impl Dog {
    fn __init__() { __super__(); }
  }
  class Cat : Animal {
    fn speak() -> Str { return "Meow"; }
  }

  impl Cat {
    fn __init__() { __super__(); }
  }
  class Bird : Animal {
    fn speak() -> Str { return "Tweet"; }
  }

  impl Bird {
    fn __init__() { __super__(); }
  }
  class Labrador : Dog {
    name: Str;
  }

  impl Labrador {
    fn __init__() { __super__(); self.name = "Rex"; }
  }
)";

// Basic dispatch: each subclass hits the right arm.
TEST(MatchCodeGen, BasicDispatch) {
  auto r = compileAndRun(withFns(kMatchHierarchy, R"(
    a: Animal = Dog();
    match a {
      Dog  { io::println("dog"); }
      Cat  { io::println("cat"); }
      Bird { io::println("bird"); }
      _    { io::println("other"); }
    }
    b: Animal = Cat();
    match b {
      Dog  { io::println("dog"); }
      Cat  { io::println("cat"); }
      Bird { io::println("bird"); }
      _    { io::println("other"); }
    }
    c: Animal = Bird();
    match c {
      Dog  { io::println("dog"); }
      Cat  { io::println("cat"); }
      Bird { io::println("bird"); }
      _    { io::println("other"); }
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
      Dog { io::println("dog"); }
      Cat { io::println("cat"); }
      _   { io::println("other"); }
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
      _ { io::println("wildcard"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "wildcard\n");
}

// Binding introduces a narrowed alias; int field is accessible.
TEST(MatchCodeGen, BindingIntField) {
  auto r = compileAndRun(withFns(R"(
    class Shape {}
    impl Shape { fn __init__() {} }
    class Rect : Shape {
      w: int;
      h: int;
    }

    impl Rect {
      fn __init__(a: int, b: int) { __super__(); self.w = a; self.h = b; }
    }
  )",
                                 R"(
    s: Shape = Rect(4, 5);
    match s {
      r: Rect { io::println(Str<int>(r.w)); io::println(Str<int>(r.h)); }
      _       { io::println("other"); }
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
      lab: Labrador { io::println(lab.name); }
      _             { io::println("other"); }
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
      dog: Dog { io::println(dog.speak()); }
      _        { io::println("other"); }
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
      Dog      { io::println("dog"); }
      Labrador { io::println("labrador"); }
      _        { io::println("other"); }
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
      Dog { io::println("dog"); }
      Cat { io::println("cat"); }
      _   { io::println("other"); }
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
  )",
                                 R"(
    io::println(describe(Dog()));
    io::println(describe(Cat()));
    io::println(describe(Bird()));
    io::println(describe(Labrador()));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "dog\ncat\nbird\nunknown\n");
}

// Multiple match statements in sequence on the same subject.
TEST(MatchCodeGen, MultipleMatchStmts) {
  auto r = compileAndRun(withFns(kMatchHierarchy, R"(
    a: Animal = Cat();
    match a {
      Dog { io::println("1:dog"); }
      Cat { io::println("1:cat"); }
      _   { io::println("1:other"); }
    }
    match a {
      Bird { io::println("2:bird"); }
      Cat  { io::println("2:cat"); }
      _    { io::println("2:other"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1:cat\n2:cat\n");
}

// Arm body with control flow (if/else inside a match arm).
TEST(MatchCodeGen, ControlFlowInArm) {
  auto r = compileAndRun(withFns(R"(
    class Node {}
    impl Node { fn __init__() {} }
    class Leaf : Node {
      val: int;
    }

    impl Leaf {
      fn __init__(v: int) { __super__(); self.val = v; }
    }
  )",
                                 R"(
    n: Node = Leaf(7);
    match n {
      leaf: Leaf {
        if leaf.val > 5 {
          io::println("big");
        } else {
          io::println("small");
        }
      }
      _ { io::println("other"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "big\n");
}

// Match on a subject that is itself a function call result.
TEST(MatchCodeGen, SubjectIsCallExpr) {
  auto r = compileAndRun(withFns(kMatchHierarchy + R"(
    fn makeAnimal(kind: int) -> Animal {
      if kind == 1 { return Dog(); }
      return Cat();
    }
  )",
                                 R"(
    match makeAnimal(1) {
      Dog { io::println("dog"); }
      Cat { io::println("cat"); }
      _   { io::println("other"); }
    }
    match makeAnimal(2) {
      Dog { io::println("dog"); }
      Cat { io::println("cat"); }
      _   { io::println("other"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "dog\ncat\n");
}

// Three-level hierarchy: exact-type dispatch at every level.
TEST(MatchCodeGen, DeepInheritanceExactType) {
  auto r = compileAndRun(withFns(R"(
    class A {}
    impl A { fn __init__() {} }
    class B : A {}
    impl B { fn __init__() { __super__(); } }
    class C : B {}
    impl C { fn __init__() { __super__(); } }
  )",
                                 R"(
    x: A = A();
    match x {
      C { io::println("C"); }
      B { io::println("B"); }
      A { io::println("A"); }
      _ { io::println("?"); }
    }
    y: A = B();
    match y {
      C { io::println("C"); }
      B { io::println("B"); }
      A { io::println("A"); }
      _ { io::println("?"); }
    }
    z: A = C();
    match z {
      C { io::println("C"); }
      B { io::println("B"); }
      A { io::println("A"); }
      _ { io::println("?"); }
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
        io::println(tmp);
      }
      _ { io::println("other"); }
    }
    io::println("after");
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "cleanup-me\nafter\n");
}

// -- Value-mode match (primitive / Str subjects against literal patterns)

// String value match: the literal arm whose content equals the subject fires.
TEST(MatchCodeGen, StringValueMatch) {
  auto r = compileAndRun(wrapMain(R"(
    s: Str = "World";
    match s {
      "Hello" { io::println("got hello"); }
      "World" { io::println("got world"); }
      _ { io::println("got other"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "got world\n");
}

// String value match falls through to the wildcard when nothing matches.
TEST(MatchCodeGen, StringValueMatchWildcard) {
  auto r = compileAndRun(wrapMain(R"(
    s: Str = "Nope";
    match s {
      "Hello" { io::println("got hello"); }
      "World" { io::println("got world"); }
      _ { io::println("got other"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "got other\n");
}

// Integer value match.
TEST(MatchCodeGen, IntValueMatch) {
  auto r = compileAndRun(wrapMain(R"(
    n: int = 2;
    match n {
      1 { io::println("one"); }
      2 { io::println("two"); }
      3 { io::println("three"); }
      _ { io::println("many"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "two\n");
}

// Char value match.
TEST(MatchCodeGen, CharValueMatch) {
  auto r = compileAndRun(wrapMain(R"(
    c: char = 'b';
    match c {
      'a' { io::println("a"); }
      'b' { io::println("b"); }
      _ { io::println("other"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "b\n");
}

// Bool value match.
TEST(MatchCodeGen, BoolValueMatch) {
  auto r = compileAndRun(wrapMain(R"(
    b: bool = False;
    match b {
      True { io::println("yes"); }
      False { io::println("no"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "no\n");
}

// A value match on an int is a switch: on the whole 64-bit value (1 << 32
// plus 1 is not 1), a negative value takes the default, and of two arms
// naming a value the first is taken.
TEST(MatchCodeGen, IntValueMatchSwitchesOnTheWholeValue) {
  auto r = compileAndRun(R"(import ::io;
fn name(n: int) -> Str {
  match n {
    1 { return "one"; }
    2 { return "two"; }
    2 { return "again"; }
    7 { return "seven"; }
    _ { return "other"; }
  }
}
fn main() -> int {
  io::println(name(2));
  io::println(name(4294967297));
  io::println(name(-1));
  io::println(name(7));
  io::println(name(1));
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "two\nother\nother\nseven\none\n");
}

// The arms of a switch on an int break out of and continue the loop around
// it, as a chain's do.
TEST(MatchCodeGen, SwitchArmsBreakAndContinueTheLoop) {
  auto r = compileAndRun(wrapMain(R"(
    i = 0;
    total = 0;
    while i < 10 {
      match i {
        2 { i = i + 1; continue; }
        8 { break; }
        _ { total = total + i; }
      }
      i = i + 1;
    }
    io::println(Str<int>(total));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "26\n");
}

// A class method may call a module-level free function, regardless of whether
// the function is defined before or after the class (forward-declared).

TEST(Class, MethodCallsFreeFunctionDefinedBefore) {
  auto r =
      compileAndRun("fn helper() -> int { return 7; }\n"
                    "class C {\n"
                    "  fn get() -> int { return helper(); }\n"
                    "}\n"
                    "impl C {\n"
                    "  fn __init__() {}\n"
                    "}\n" +
                    wrapMain("c: C = C();\n  io::println(Str<int>(c.get()));"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "7\n");
}

TEST(Class, MethodCallsFreeFunctionDefinedAfter) {
  auto r =
      compileAndRun("class C {\n"
                    "  fn get() -> int { return helper(); }\n"
                    "}\n"
                    "impl C {\n"
                    "  fn __init__() {}\n"
                    "}\n"
                    "fn helper() -> int { return 42; }\n" +
                    wrapMain("c: C = C();\n  io::println(Str<int>(c.get()));"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "42\n");
}

// -- Default Obj.equals — identity semantics

// A user class without an equals override uses the inherited identity equals:
// an object equals itself, and not a distinct object. This must agree with the
// `==` operator.
TEST(Class, DefaultEqualsIsIdentity) {
  auto r = compileAndRun(withFns(R"(
    class Box { v: int; }
    impl Box { fn __init__(x: int) { self.v = x; } }
  )",
                                 R"(
    b: Box = Box(5);
    c: Box = Box(5);
    io::println(Str<bool>(b.equals(b)));
    io::println(Str<bool>(b.equals(c)));
    io::println(Str<bool>(b == b));
    io::println(Str<bool>(b == c));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\nFalse\nTrue\nFalse\n");
}

// `==` on reference types lowers to the virtual `equals` method.  A ref-typed
// call temporary compared against None must dispatch correctly (and not leak):
// `process() == None` returns None.equals(None) == True.  Regression: this used
// to compare a box pointer against the None singleton and was always false.
TEST(Class, CallTemporaryEqualsNone) {
  auto r = compileAndRun(R"(import ::io;
    fn process() -> Obj { return None; }
    fn main() -> int {
      if process() == None { io::println("none"); } else { io::println("something"); }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "none\n");
}

TEST(Class, CallTemporaryNotEqualsNone) {
  auto r = compileAndRun(R"(import ::io;
    class Thing { n: int; }
    impl Thing { fn __init__(x: int) { self.n = x; } }
    fn process() -> Obj { return Thing(5); }
    fn main() -> int {
      if process() != None { io::println("something"); } else { io::println("none"); }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "something\n");
}

// Two distinct call temporaries are distinct objects under identity `==`.
TEST(Class, TwoCallTemporariesDistinct) {
  auto r = compileAndRun(R"(import ::io;
    class Thing {}
    fn make() -> Obj { return Thing(); }
    fn main() -> int {
      if make() == make() { io::println("same"); } else { io::println("diff"); }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "diff\n");
}

// `==` on Str is value (content) equality, since it lowers to Str.equals.
// Two distinct string objects with equal content are `==`-equal.
TEST(Class, StringEqualityIsContent) {
  auto r = compileAndRun(R"(import ::io;
    fn main() -> int {
      a: Str = "abc";
      b: Str = "ab" + "c";
      io::println(Str<bool>(a == b));
      io::println(Str<bool>(a != b));
      io::println(Str<bool>(a == "xyz"));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\nFalse\nFalse\n");
}

// `==` dispatches a user `equals` override — including one that matches on its
// own class type (regression: the class's vtable global was created after its
// method bodies, so a self-type match referenced an unresolved placeholder).
TEST(Class, EqualsOperatorUsesOverrideWithSelfMatch) {
  auto r = compileAndRun(R"(import ::io;
    class Money {
      cents: int;
      view fn equals(o: Obj) -> bool {
        match o {
          m: Money { return self.cents == m.cents; }
          _        { return False; }
        }
      }
    }

    impl Money {
      fn __init__(c: int) { self.cents = c; }
    }
    fn main() -> int {
      a: Money = Money(5);
      b: Money = Money(5);
      c: Money = Money(9);
      io::println(Str<bool>(a == b));
      io::println(Str<bool>(a == c));
      io::println(Str<bool>(a != c));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\nFalse\nTrue\n");
}

// A method that matches on its own class type links correctly (vtable global
// must exist before the method body is emitted).
TEST(Class, MethodMatchesOwnType) {
  auto r = compileAndRun(R"(import ::io;
    class Node {
      v: int;
      fn isNode(o: Obj) -> bool {
        match o { n: Node { return True; } _ { return False; } }
      }
    }

    impl Node {
      fn __init__(x: int) { self.v = x; }
    }
    fn main() -> int {
      a: Node = Node(1);
      io::println(Str<bool>(a.isNode(a)));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\n");
}

// A user class may override equals; the override is dispatched through the
// vtable in place of the runtime default.
TEST(Class, EqualsOverrideDispatched) {
  auto r = compileAndRun(withFns(R"(
    class Always {
      v: int;
      view fn equals(o: Obj) -> bool { return True; }
    }

    impl Always {
      fn __init__(x: int) { self.v = x; }
    }
  )",
                                 R"(
    a: Always = Always(1);
    b: Always = Always(2);
    io::println(Str<bool>(a.equals(b)));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\n");
}

// equals must also work when the argument is a match binding (which stores the
// unboxed object): the call passes the binding's backing box, not a fresh wrap.
TEST(Class, EqualsWithMatchBoundArgument) {
  auto r = compileAndRun(wrapMain(R"(
    x: Obj = "7";
    y: Obj = "7";
    match x {
      a: Str {
        match y {
          b: Str { io::println(Str<bool>(a.equals(b))); }
          _ {}
        }
      }
      _ {}
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\n");
}

// A field of a conditional `(if c then a else b)` of class type, the object
// either branch gives: read (a `Str`, an `int`, a weak field, an element of a
// tuple field, a tuple and a struct field whole), joined with `+`, assigned
// (an `int`, a `Str`, a tuple, a struct, an optional), passed `inout` and
// matched, on both branches, with a fresh object in one of them.  Before, a
// read or an assignment dropped its statement (an `io::println` printed
// nothing, a variable it declared failed an assertion) or failed the PIR
// verifier, and an `inout` argument was an internal compiler error.
TEST(Class, FieldsOfAConditionalReceiver) {
  LeakGuard g;
  auto r = compileAndRun(withFns(R"(
    struct Point { x: int; y: int; }
    class Node {
      name: Str;
      n: int;
      t: (int, Str);
      p: Point;
      next: Node?;
      weak back: Node?;
    }
    fn bump(x: inout int) { x = x + 1; }
    fn shout(s: inout Str) { s = s + "!"; }
  )",
                                 R"(
    a = Node("a", 1, (10, "ta"), Point(1, 2), None, None);
    b = Node("b", 2, (20, "tb"), Point(3, 4), None, None);
    b.back = a;
    for k in 1..=2 {
      x = (if k == 1 then a else b).name;
      io::println(x + " " + (if k == 1 then a else b).name);
      c = Node("c", 3, (30, "tc"), Point(5, 6), None, None);
      io::println((if k == 1 then a else c).name + Str(k));
      io::println((if k == 1 then a else b).t.1 + Str((if k == 1 then a else b).t.0));
      t = (if k == 1 then a else b).t;
      p = (if k == 1 then a else b).p;
      io::println(t.1 + Str(p.y) + Str((if k == 1 then a else b).p.x));
      io::println((if k == 1 then a else b).back == None);
    }
    (if a.n == 1 then a else b).n = 5;
    (if a.n == 5 then b else a).name = "bb";
    (if a.n == 5 then b else a).t = (21, "tbb");
    (if a.n == 5 then b else a).p = Point(7, 8);
    (if a.n == 5 then a else b).next = b;
    io::println(Str(a.n) + " " + b.name + " " + b.t.1 + " " + Str(b.p.x));
    bump((if a.n == 5 then b else a).n);
    shout((if a.n == 5 then b else a).name);
    io::println(Str(b.n) + " " + b.name);
    match (if a.n == 5 then a else b).next {
      nx: Node { io::println("next " + nx.name); }
      _ { io::println("no next"); }
    }
    a.next = None;
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "a a\na1\nta10\nta21\nTrue\n"
                      "b b\nc2\ntb20\ntb43\nFalse\n"
                      "5 bb tbb 7\n3 bb!\nnext bb!\n");
  g.expectNoLeaks("FieldsOfAConditionalReceiver");
}
