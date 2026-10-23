// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: traits (#216).  `impl Trait for A` must give A every method the
// trait requires, with its signature (a method A already has counts); a
// class implements what its superclasses do, and every class implements the
// builtin `Show` through `Obj`.  A trait is not a type.  A type parameter's
// bounds are checked where the template is instantiated.

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

// Requirements met by the impl block and by the class body, a generic
// trait, and `Self` (in a trait, an impl and a class body).
TEST(Traits, ConformingProgramChecks) {
  auto r = semaCheck(R"(trait Named { view fn name() -> Str; }
trait Into<T> { view fn into() -> T; }
trait Same { view fn same(other: view Self) -> bool; }
class A {
  n: int;
  view fn name() -> Str { return "A"; }
  view fn twin() -> Self { return A(0); }
}

impl A {
  fn __init__(n: int) { self.n = n; }
}
impl Named for A {}
impl Into<int> for A { view fn into() -> int { return self.n; } }
impl Same for A {
  view fn same(other: view Self) -> bool { return self.n == other.n; }
}
class B : A {}
impl B { fn __init__() { __super__(1); } }
fn main() -> int {
  b = B();
  return b.into() + b.name().len() + b.twin().into() + Str(b.same(b)).len();
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0) << r.Diagnostics;
}

TEST(Traits, DeclarationsAreChecked) {
  auto r = semaCheck(R"(trait Named { view fn name() -> Str; }
trait Named { fn x(); }
trait Shape { fn __init__(); fn area() -> int; fn area() -> int; }
trait Bad { fn f() -> Nope; }
class Show {}
class Named {}
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"redefinition of trait 'Named'",
                "a trait cannot require '__init__'",
                "duplicate method 'area' in trait 'Shape'",
                "method 'f' of trait 'Bad' has unknown class type 'Nope'",
                "'Show' is a builtin trait and cannot be redeclared",
                "'Named' is already declared as a trait"});
}

// An impl must give the class each required method with the trait's
// signature (`view fn` and modes included), and nothing else.
TEST(Traits, ImplsAreChecked) {
  auto r = semaCheck(R"(trait Named { view fn name() -> Str; }
trait Into<T> { view fn into() -> T; }
class A { n: int; view fn name() -> int { return 1; } }
impl A { fn __init__() { self.n = 0; } }
impl Named for A {}
impl Into<int> for A {
  fn into() -> int { return 1; }
  fn extra() {}
  static fn make() -> A { return A(); }
}
class C { n: int; }
impl C { fn __init__() { self.n = 0; } }
impl Named for C {}
impl Into for C {}
impl Show for C {}
impl Nope for C {}
impl Str for C {}
impl Named for int {}
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'view fn name() -> int' of class 'A' does not match 'view fn "
                "name() -> Str' of trait 'Named'",
                "'fn into() -> int' of class 'A' does not match 'view fn "
                "into() -> int' of trait 'Into<int>'",
                "'extra' is not a method of trait 'Into<int>'",
                "'make' is not a method of trait 'Into<int>'",
                "class 'C' does not implement 'view fn name() -> Str' of trait "
                "'Named'",
                "trait 'Into' expects 1 type argument(s), got 0",
                "class 'C' already implements 'Show' (through 'Obj')",
                "unknown trait 'Nope'", "'Str' is not a trait",
                "'int' is a built-in type"});
}

TEST(Traits, TraitsAreNotTypes) {
  auto r = semaCheck(R"(trait Named { view fn name() -> Str; }
trait Into<T> { view fn into() -> T; }
fn f(n: Named) {}
fn g(n: Into<int>) {}
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"parameter 'n' names trait 'Named', which is not a type",
                "'Into' is a trait, not a type"});
}

// Bounds on functions and classes, met by a subclass through its superclass,
// by every class (`Show`), and checked for a class field once the impl
// blocks are known.
TEST(Traits, BoundsAreChecked) {
  auto r = semaCheck(R"(trait Named { view fn name() -> Str; }
trait Into<T> { view fn into() -> T; }
class Box<T: Named> { v: T; }
impl Box<T> { fn __init__(v: T) { self.v = v; } }
class Keeper { b: Box<A>; }
impl Keeper { fn __init__() { self.b = Box<A>(A()); } }
class A { n: int; view fn name() -> Str { return "A"; } }
impl A { fn __init__() { self.n = 0; } }
impl Named for A {}
class B : A {}
impl B { fn __init__() { __super__(); } }
class C { n: int; }
impl C { fn __init__() { self.n = 0; } }
fn greet<T: Named + Show>(x: view T) -> Str { return x.name() + x.toString(); }
fn conv<T, S: Into<T>>(s: view S) -> T { return s.into(); }
fn bad<T: Nope>(x: T) {}
fn main() -> int {
  ok = greet(B()) + Box<B>(B()).v.name();
  greet(C());
  b: Box<C> = Box<C>(C());
  return conv<int, A>(A());
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'C' does not implement trait 'Named', which 'greet' requires "
                "of 'T'",
                "'C' does not implement trait 'Named', which 'Box' requires of "
                "'T'",
                "'A' does not implement trait 'Into<int>', which 'conv' "
                "requires of 'S'",
                "unknown trait 'Nope'"});
  EXPECT_EQ(r.Diagnostics.find("'B' does not implement"), std::string::npos)
      << r.Diagnostics;
}

// int, float, bool and char implement `Show`: `n.toString()` is a `Str`.  So
// does an enum: its variant's name.
TEST(Traits, ScalarsImplementShow) {
  auto r = semaCheck(R"(enum Color { Red }
fn show<T: Show>(x: view T) -> Str { return x.toString(); }
fn main() -> int {
  n = 3;
  s: Str = n.toString() + 2.5.toString() + True.toString() + 'c'.toString();
  t = show(n) + show(1.5) + show(False) + show('x') + show("s");
  n.toString(1);
  show(Color::Red);
  return s.len() + t.len();
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"method 'toString' expects 0 argument(s), got 1"});
  EXPECT_EQ(r.ErrorCount, 1) << r.Diagnostics;
}

// int, bool, char and Str implement `Hashable`: `n.hash()` is an `int`.  A
// class implements it with an `impl Hashable for` block; float, enums and
// other classes do not.
TEST(Traits, BuiltinHashable) {
  auto r = semaCheck(R"(enum Color { Red }
class Key { id: int; }
impl Hashable for Key { view fn hash() -> int { return self.id; } }
class Plain { n: int; }
fn h<T: Hashable>(x: view T) -> int { return x.hash(); }
fn main() -> int {
  n = 3;
  s = "s";
  k: int = n.hash() + True.hash() + 'c'.hash() + s.hash() + "t".hash();
  k = k + h(n) + h(False) + h('x') + h("s") + h(Key(1));
  n.hash(1);
  h(1.5);
  h(Color::Red);
  h(Plain(1));
  return k;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"method 'hash' expects 0 argument(s), got 1",
                "'float' does not implement trait 'Hashable', which 'h' "
                "requires of 'T'",
                "'Color' does not implement trait 'Hashable', which 'h' "
                "requires of 'T'",
                "'Plain' does not implement trait 'Hashable', which 'h' "
                "requires of 'T'"});
  EXPECT_EQ(r.ErrorCount, 4) << r.Diagnostics;
}

TEST(Traits, HashableIsBuiltin) {
  auto r = semaCheck(R"(trait Hashable { view fn hash() -> int; }
class K { n: int; }
impl Hashable for K { fn hash() -> Str { return ""; } }
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'Hashable' is a builtin trait and cannot be redeclared"});
}

// int, float, bool, char, enums and tuples are ValueTypes; Str, classes and
// optionals are not, and a class cannot implement it.
TEST(Traits, BuiltinValueType) {
  auto r = semaCheck(R"(enum Color { Red }
class P { x: int; }
impl ValueType for P {}
fn v<T: ValueType>(x: view T) -> int { return 1; }
fn main() -> int {
  n: int? = 1;
  k: int = v(1) + v(2.5) + v(True) + v('c') + v(Color::Red) + v((1, "s"));
  k = k + v("s") + v(P(1)) + v(n);
  return k;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"class 'P' cannot implement 'ValueType': a class is shared "
                "by reference, not held in place",
                "'Str' does not implement trait 'ValueType', which 'v' "
                "requires of 'T'",
                "'P' does not implement trait 'ValueType', which 'v' "
                "requires of 'T'",
                "'int?' does not implement trait 'ValueType', which 'v' "
                "requires of 'T'"});
  EXPECT_EQ(r.ErrorCount, 4) << r.Diagnostics;
}
