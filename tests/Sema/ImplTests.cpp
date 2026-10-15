// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: `impl` blocks.  Their methods join the class but are called
// directly: none may take a name the class or a superclass already has, a
// subclass cannot override one, and an `impl` block adds only to a class of
// its own module.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

void expectAll(const SemaResult &r, std::initializer_list<const char *> diags) {
  for (const char *d : diags)
    EXPECT_NE(r.Diagnostics.find(d), std::string::npos) << d << "\n"
                                                        << r.Diagnostics;
}

} // namespace

// Methods of several blocks, a `static fn` called as `Type::name()`, a
// subclass using what it inherits, and a class-body method calling an
// `impl` method; the `view fn` rules hold as in a class body.
TEST(Impl, MethodsAndStaticFnsCheck) {
  auto r = semaCheck(R"(class A {
  n: int;
  view fn twice() -> int { return self.doubled(); }
}
impl A {
  fn __init__(n: int) { self.n = n; }
  static fn make() -> A { return A(1); }
  view fn doubled() -> int { return self.n * 2; }
}
impl A { fn bump() { self.n = self.n + 1; } }
class B : A {}
impl B { fn __init__() { __super__(2); } }
impl B { view fn plus(k: int) -> int { return self.doubled() + k; } }
fn main() -> int {
  a = A::make();
  a.bump();
  b = B();
  b.bump();
  return a.twice() + b.plus(1);
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0) << r.Diagnostics;
}

TEST(Impl, ViewFnRulesApply) {
  auto r = semaCheck(R"(class A { n: int; }
impl A { fn __init__() { self.n = 0; } }
impl A {
  fn bump() { self.n = self.n + 1; }
  view fn bad() { self.bump(); }
}
fn read(a: view A) { a.bump(); }
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'self' is read-only in 'view fn bad'; 'bump' is not a 'view "
                "fn'",
                "'bump' is not a 'view fn'"});
}

// No method may take a name already in the class or a superclass, in
// either direction, and a class has one `__init__`.
TEST(Impl, NamesAreNotReused) {
  auto r = semaCheck(R"(class A { n: int; fn f() {} }
impl A { fn __init__() { self.n = 0; } }
class B : A {}
impl B { fn __init__() { __super__(); } }
impl A {
  fn f() {}
  fn h() {}
  fn __init__() {}
  static fn h() {}
  static fn mk() -> A { return A(); }
  static fn mk() -> A { return A(); }
}
impl B { fn h() {} fn f() {} }
class C : A { fn h() {} }
impl C { fn __init__() { __super__(); } }
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"class 'A' already has a method 'f'",
                "class 'A' already has a method '__init__'",
                "class 'A' already has a method 'h'",
                "redefinition of function 'A::mk'",
                "class 'B' already has a method 'h' (from 'A')",
                "class 'B' already has a method 'f' (from 'A')",
                "method 'h' of 'A' is declared in an 'impl' block"});
}

// An `__init__` in an `impl` block is the class's constructor, with the
// same rules as one in the class body; a generic class's infers its type
// arguments.
TEST(Impl, InitCanGoInAnImplBlock) {
  auto ok = semaCheck(R"(class A { n: int; }
impl A { fn __init__(n: int) { self.n = n; } }
class B : A { m: int; }
impl B { fn __init__() { __super__(1); self.m = 2; } }
class Box<T> { v: T; }
impl Box<U> { fn __init__(v: U) { self.v = v; } }
fn main() -> int { b = B(); return b.n + b.m + A(3).n + Box(4).v; }
)");
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;
  auto bad = semaCheck(R"(class A { n: int; }
impl A { fn __init__(n: int) { self.n = n; } }
class C : A { }
class D { k: int; }
impl D { fn __init__() {} }
class E {}
impl E { fn __init__() {} }
impl E { fn __init__() {} }
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(bad.Ok);
  expectAll(bad, {"class 'C' must declare an '__init__' that calls",
                  "field 'k' of class 'D' is not assigned on every path",
                  "class 'E' already has a method '__init__'"});
}

TEST(Impl, OnlyClassesOfTheModule) {
  auto r = semaCheck(R"(enum Color { Red }
class Box<T> { v: T; }
impl Box<T> { fn __init__(v: T) { self.v = v; } }
impl Str { fn x() {} }
impl Color { fn x() {} }
impl Nope { fn x() {} }
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'Str' is a built-in type; an 'impl' block cannot add methods "
                "to it",
                "an 'impl' block for an enum is not supported yet",
                "'impl' for undefined class 'Nope'"});
}

// A generic class's `impl` blocks, by their own names for its type
// parameters, give each instantiation their methods and traits: also one
// made for a field type, before the module's `impl` blocks are checked.
TEST(Impl, GenericBlocksJoinEachInstantiation) {
  auto r = semaCheck(R"(class Box<T> { v: T; }
impl Box<T> { fn __init__(v: T) { self.v = v; } }
impl Box<U> {
  view fn get() -> view U { return self.v; }
  fn set(v: U) { self.v = v; }
}
trait Named { view fn name() -> Str; }
impl Named for Box<T> { view fn name() -> Str { return self.v.toString(); } }
fn label<T: Named>(x: view T) -> Str { return x.name(); }
class Holder { b: Box<Str>; }
impl Holder { fn __init__() { self.b = Box("s"); } }
fn main() -> int {
  b = Box(1);
  b.set(b.get() + 1);
  h = Holder();
  println(label(b) + label(h.b));
  return 0;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Impl, GenericBlocksAreChecked) {
  auto r = semaCheck(R"(class Box<T> { v: T; }
impl Box<T> { fn __init__(v: T) { self.v = v; } }
class A { n: int; }
impl A { fn __init__() { self.n = 0; } }
impl Box { fn f() {} }
impl Box<T, U> { fn f() {} }
impl Box<T> { static fn make() {} }
impl Box<A> { fn f() {} }
impl A<T> { fn f() {} }
trait Named { view fn name() -> Str; }
impl Named for Box<T> {}
class Cell<T> {
  v: T;
  view fn get() -> T { return self.v; }
}

impl Cell<T> {
  fn __init__(v: T) { self.v = v; }
}
impl Cell<T> { fn get() -> T { return self.v; } }
fn main() -> int { b = Box(1); c = Cell(2); return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'Box' is a generic class: write 'impl Box<T>'",
                "'impl Box' names 2 type parameter(s); generic class 'Box' "
                "has 1",
                "a 'static fn' in the 'impl' block of a generic class is not "
                "supported yet",
                "type parameter 'A' of 'impl' block for 'Box' shadows a type",
                "'A' is not a generic class: write 'impl A'",
                "class 'Cell<int>' already has a method 'get'",
                "class 'Box<int>' does not implement 'view fn name() -> Str' "
                "of trait 'Named'"});
}

TEST(Impl, UnknownStaticFnIsUndeclared) {
  auto r = semaCheck(R"(class A { n: int; }
impl A { fn __init__() { self.n = 0; } }
impl A { fn f() {} }
fn main() -> int { A::f(); return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"call to undeclared function 'A::f'"});
}
