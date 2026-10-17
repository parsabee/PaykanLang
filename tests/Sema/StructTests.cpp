// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: structs (docs/language/14-structs.md) — declarations and their
// fields, nominal typing, construction, field access and assignment, `impl`
// blocks (methods, `__init__`, `static fn`, traits), `self` in a method, the
// `view` / `inout` / `let` rules, and what is not supported yet.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

void expectAll(const SemaResult &r, std::initializer_list<const char *> diags) {
  for (const char *d : diags)
    EXPECT_NE(r.Diagnostics.find(d), std::string::npos) << d << "\n"
                                                        << r.Diagnostics;
}

std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

const char *kPoint = R"(struct Point { x: int; y: int; }
)";

} // namespace

// -- Declarations and fields

TEST(Struct, FieldsConstructionAccessAndAssignment) {
  auto r = semaCheck(std::string(kPoint) + wrapMain(R"(
    p = Point(1, 2);
    q: Point = Point(3, 4);
    x: int = p.x + q.y;
    p.x = 5;
    p.y = p.x * 2;
    s = p;
    s.x = 0;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Struct, FieldTypes) {
  auto r = semaCheck(R"(
enum Color { Red, Green }
class Node { n: int; }
class Box<T> { v: T; }
struct Inner { a: int; }
struct Mixed {
  i: int; f: float; b: bool; c: char; s: Str; o: Obj;
  maybe: Str?; oi: int?; t: (int, Str); in: Inner; node: Node;
  color: Color; box: Box<int>; nested: (Inner, (int, Inner));
}
fn main() -> int {
  m = Mixed(1, 2.5, True, 'c', "s", Node(1), None, None, (1, "t"), Inner(2),
            Node(3), Color::Red, Box<int>(4), (Inner(5), (6, Inner(7))));
  m.in.a = 8;
  m.nested.1.1.a = 1;
  bump(m.nested.1.1.a);
  return m.i + m.in.a + m.t.0 + m.nested.0.a + m.box.v;
}
fn bump(n: inout int) { n = n + 1; }
)");
  EXPECT_FALSE(r.Ok); // a struct in a tuple is part of the tuple's value
  expectAll(r, {"cannot assign to field 'a' of a struct in element '.1' of "
                "type '(int, Inner)': tuples are immutable; build a new tuple "
                "instead",
                "a field of a struct held in a tuple cannot be passed to "
                "'inout' parameter 'n': tuples are immutable"});
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
}

TEST(Struct, StructInEveryTypePosition) {
  auto r = semaCheck(R"(
struct P { x: int; }
class Holder { p: P; ps: P[2]; view fn get() -> P { return self.p; } }
class Box<T> { v: T; }
impl Box<T> { fn __init__(v: T) { self.v = v; } }
fn f(a: P, b: view P, c: inout P) -> P { c.x = b.x; return a; }
fn g() -> (P, int) { return (P(1), 2); }
fn h(xs: view P[]) -> int { return xs[0].x + xs.len(); }
fn main() -> int {
  xs: P[3] = [P(1), P(2), P(3)];
  t: (P, P) = (P(1), P(2));
  b = Box<P>(P(1));
  c = Box(P(2));
  hld = Holder(P(3), [P(4), P(5)]);
  hld.p.x = 1;
  hld.ps[1].x = 2;
  r = P(9);
  q = f(xs[0], t.1, r);
  return q.x + g().0.x + h(xs) + b.v.x + c.v.x + hld.get().x + r.x;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Struct, DuplicateAndTakenNames) {
  auto r = semaCheck(R"(
struct A { x: int; } class A { y: int; }
struct B { x: int; } fn B() -> int { return 1; }
enum C { One } struct C { x: int; }
struct D { x: int; } struct D { y: int; }
struct E { x: int; } trait E { view fn f() -> int; }
struct Str { x: int; }
struct F { x: int; x: Str; }
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r,
            {"class 'A' conflicts with a struct of the same name",
             "'B' is already declared as a struct",
             "'C' is already declared as an enum", "redefinition of struct 'D'",
             "'E' is already declared as a trait",
             "'Str' is a builtin class and cannot be redeclared",
             "duplicate field 'x' in struct 'F'"});
}

TEST(Struct, FieldCannotBeArrayVoidOrOptionalStruct) {
  auto r = semaCheck(R"(
struct A { xs: int[3]; }
struct B { x: int; }
fn main() -> int {
  b: B? = None;
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"field 'xs' in struct 'A' has type 'int[3]': a struct field "
                "cannot be a fixed-size array yet",
                "variable 'b' has type 'B?': optional struct types are not "
                "supported yet"});
}

TEST(Struct, CannotContainItself) {
  auto r = semaCheck(R"(
struct A { b: B; } struct B { a: A; }
struct C { c: (int, C); }
struct D { d: D?; }
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"struct 'A' contains itself (through 'B'); a struct is held "
                "in place, so it cannot hold one of its own (hold it in a "
                "class instead)",
                "struct 'C' contains itself: a field holds a 'C'",
                "optional struct types are not supported yet"});
}

TEST(Struct, AClassMayHoldAStructThatHoldsTheClass) {
  // Through a reference there is no containment.
  auto r = semaCheck(R"(
class Node { p: P; }
struct P { node: Node?; n: int; }
fn main() -> int { return 0; }
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Nominal typing

TEST(Struct, NominalNotStructural) {
  auto r = semaCheck(R"(
struct A { x: int; y: int; }
struct B { x: int; y: int; }
fn takeA(a: A) -> int { return a.x; }
fn main() -> int {
  a = A(1, 2);
  b: B = a;
  t: (int, int) = a;
  a2: A = (1, 2);
  takeA(B(1, 2));
  return if a == B(1, 2) then 1 else 0;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"initializer of type 'A' does not match declared type 'B'",
                "initializer of type 'A' does not match declared type '(int, "
                "int)'",
                "initializer of type '(int, int)' does not match declared "
                "type 'A'",
                "argument 1 of 'takeA' has type 'B', expected 'A'",
                "operands of '==' have mismatched types 'A' and 'B'"});
}

TEST(Struct, NotAnObjectNotATupleNotMatched) {
  auto r = semaCheck(R"(
struct A { x: int; y: int; }
fn main() -> int {
  a = A(1, 2);
  o: Obj = a;
  v = a.0;
  p, q = a;
  match a { _ { } }
  return a.equals(a);
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"initializer of type 'A' does not match declared type 'Obj'",
                "tuple index '.0' applied to struct type 'A': a struct's "
                "fields are named (x, y)",
                "cannot destructure a value of type 'A'; only tuples can be "
                "destructured",
                "match on a struct ('A') is not supported yet: a struct is a "
                "value, not an object",
                "no method 'equals' on type 'A'"});
}

TEST(Struct, ConstructorArgumentsAndUnknownField) {
  auto r = semaCheck(R"(
struct A { x: int; y: Str; }
fn main() -> int {
  a = A(1);
  b = A("s", 1);
  c = A(1, "s");
  c.z = 2;
  return c.z;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"function 'A' expects 2 argument(s), got 1 (it has no "
                "'__init__', so it takes x: int, y: Str)",
                "argument 1 of 'A' has type 'Str', expected 'int'",
                "argument 2 of 'A' has type 'int', expected 'Str'",
                "no field 'z' in struct 'A'"});
}

// -- impl blocks

TEST(Struct, ImplMethodsInitStaticAndTraits) {
  auto r = semaCheck(R"(
struct Point { x: int; y: int; }
impl Point {
  fn __init__(x: int, y: int) { self.x = x; self.y = y; }
  static fn origin() -> Point { return Point(0, 0); }
  view fn sum() -> int { return self.x + self.y; }
  fn shift(d: int) { self.x = self.x + d; self.y = self.y + d; }
  view fn moved(d: int) -> Point { q = self; q.shift(d); return q; }
  fn slot() -> inout int { return self.x; }
  view fn same(other: view Self) -> bool { return self.x == other.x; }
}
impl Point { fn reset() { self.shift(-self.x); } }
impl Hashable for Point {
  view fn hash() -> int { return (self.x * 31 + self.y).hash(); }
}
trait Named { view fn name() -> Str; }
impl Named for Point { view fn name() -> Str { return "Point"; } }
fn show<T: Show>(x: view T) -> Str { return x.toString(); }
fn twice<T: ValueType>(x: T) -> (T, T) { return (x, x); }
fn hashOf<T: Hashable>(x: view T) -> int { return x.hash(); }
fn greet<T: Named>(x: view T) -> Str { return x.name(); }
fn bump(n: inout int) { n = n + 1; }
fn main() -> int {
  p = Point::origin();
  p.shift(2);
  bump(p.slot());
  k: inout int = p.slot();
  k = 7;
  m = p.moved(1);
  pair = twice(p);
  println(show(p) + greet(p) + p.toString());
  return p.sum() + m.x + pair.1.y + hashOf(p) + (if p.same(m) then 1 else 0);
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
}

TEST(Struct, MethodBodyRules) {
  auto r = semaCheck(R"(
struct A { x: int; y: int; }
impl A { fn __init__(x: int) { self.x = x; } }
struct B { x: int; }
impl B {
  fn __init__(x: int) { self.x = x; __super__(); }
  fn r() { self = B(1); }
}
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"field 'y' of struct 'A' is not assigned on every path "
                "through '__init__'",
                "'__super__' called in struct 'B', which has no superclass "
                "(a struct does not inherit)",
                "cannot assign to 'self': assign its fields instead"});
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
}

TEST(Struct, ImplRules) {
  auto r = semaCheck(R"(
struct A { x: int; y: int; }
impl A {
  fn m() {}
  fn m() {}
  view fn toString() -> Str { return "a"; }
  static fn x() -> int { return 1; }
  static fn m() -> int { return 2; }
}
impl Show for A {}
impl ValueType for A {}
impl Hashable for A {}
impl int { fn f() {} }
impl Missing { fn f() {} }
impl A<T> { fn g() {} }
fn main() -> int { return A::x(); }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"struct 'A' already has a method 'm'",
                "struct 'A' already has a method 'toString'",
                "struct 'A' already implements 'Show'",
                "struct 'A' already implements 'ValueType'",
                "struct 'A' does not implement 'view fn hash() -> int' of "
                "trait 'Hashable'",
                "'int' is a built-in type; an 'impl' block cannot add methods "
                "to it",
                "'impl' for undefined class 'Missing'",
                "'A' is not a generic struct: write 'impl A'"});
}

TEST(Struct, ViewFnRules) {
  auto r = semaCheck(R"(
struct A { x: int; s: Str; }
impl A {
  view fn bad() { self.x = 1; }
  fn ok() { self.x = 1; }
  view fn name() -> Str { return self.s; }
  view fn nameView() -> view Str { return self.s; }
  view fn copy() -> A { return self; }
  view fn callsOk() { self.ok(); }
  view fn vgo() { bump(self); }
}
fn bump(a: inout A) { a.x = a.x + 1; }
fn g(a: view A) { a.ok(); }
fn h(a: view A) -> A { b = a; return b; }
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'self' is read-only in 'view fn bad'; cannot assign to its "
                "field 'x'",
                "'self' is read-only in 'view fn name', so part of it can only "
                "be returned as a 'view': write '-> view Str'",
                "'self' is read-only in 'view fn copy', so part of it can only "
                "be returned as a 'view'",
                "'self' is read-only in 'view fn callsOk'; 'ok' is not a 'view "
                "fn'",
                "'self' is read-only in 'view fn vgo'; it cannot be passed to "
                "'inout' parameter 'a'",
                "'a' is a 'view' parameter; 'ok' is not a 'view fn'",
                "'a' is a 'view' parameter; it cannot be stored, only read or "
                "passed to a 'view' parameter"});
}

TEST(Struct, SelfOfValuesIsCopiedOutOfAViewFn) {
  // A struct of value types shares nothing: a `view fn` may copy `self`.
  auto r = semaCheck(R"(
struct P { x: int; y: int; }
impl P {
  view fn moved(d: int) -> P { q = self; q.x = q.x + d; return q; }
  view fn itself() -> P { return self; }
}
fn main() -> int { p = P(1, 2); return p.moved(1).x + p.itself().y; }
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Struct, SelfInAMethodIsTheReceiversStorage) {
  auto r = semaCheck(R"(
struct A { x: int; }
fn bump(a: inout A) { a.x = a.x + 1; }
fn bumpi(n: inout int) { n = n + 1; }
impl A {
  fn go() { bump(self); bumpi(self.x); }
  fn take(m: inout int) { self.x = m; }
}
fn main() -> int {
  a = A(1);
  a.go();
  a.take(a.x);
  return a.x;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'a.x' is part of 'a', the receiver of 'take' (not a 'view "
                "fn'); it cannot also be passed to 'inout' parameter 'm'"});
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
}

TEST(Struct, WarnsAboutAMethodThatNeverChangesSelf) {
  auto r = semaCheck(R"(
struct A { x: int; }
impl A { fn get() -> int { return self.x; } }
fn main() -> int { return A(1).get(); }
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  expectAll(r, {"'get' never changes 'self': make it a 'view fn'"});
}

// -- let, view and inout places

TEST(Struct, LetAndViewFieldsCannotBeAssigned) {
  auto r = semaCheck(R"(
struct A { x: int; in: (int, int); }
struct B { a: A; }
fn f(a: view A) { a.x = 1; }
fn main() -> int {
  let a = A(1, (1, 2));
  a.x = 3;
  let b = B(a);
  b.a.x = 4;
  c = A(1, (1, 2));
  v: view = c;
  c.x = 2;
  return v.x;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'a' is a 'view' parameter; cannot assign to its field 'x'",
                "'a' is declared with 'let'; its fields are part of its value "
                "and cannot be assigned",
                "'b' is declared with 'let'; its fields are part of its value "
                "and cannot be assigned",
                "'c' is viewed by 'view' local 'v' until 'v' is last used; "
                "cannot assign to its field 'x'"});
}

TEST(Struct, InoutPlaces) {
  auto r = semaCheck(R"(
struct A { x: int; s: Str; }
class H { a: A; }
fn mk() -> A { return A(1, "a"); }
fn bump(n: inout int) { n = n + 1; }
fn main() -> int {
  a = mk();
  bump(a.x);
  h = H(a);
  bump(h.a.x);
  m: inout = h.a.s;
  m = "b";
  bump(mk().x);
  k: inout int = mk().x;
  xs: A[2] = [mk(), mk()];
  bump(xs[0].x);
  return a.x;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"a field of a struct that is not a variable or a field (a "
                "temporary, which dies with the statement) cannot be passed "
                "to 'inout' parameter 'n'",
                "a field of a struct that is not a variable or a field (a "
                "temporary, which dies with the statement) cannot be named by "
                "'inout' local 'k'",
                "an array element cannot be passed to 'inout' parameter 'n' "
                "yet"});
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
}

TEST(Struct, NativeFunctions) {
  auto r = semaCheck(R"(
struct A { x: int; }
native fn f(a: view A) -> int = "c_f";
native fn g<T>(a: view T) -> int = "c_g";
fn main() -> int { return g(A(1)); }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"native function 'f': parameter 'a' of type 'A' cannot cross "
                "into C"});
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
}

TEST(Struct, EqualityPrintingAndShow) {
  auto r = semaCheck(R"(
struct A { x: int; s: Str; }
fn main() -> int {
  a = A(1, "a");
  b = A(1, "a");
  println(a);
  print(a);
  s: Str = a.toString();
  t = (a, 1);
  println(t);
  return if a == b && !(a != b) && t == (b, 1) then 0 else 1;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Struct, TypeParameterShadowingAndNames) {
  auto r = semaCheck(R"(
struct P { x: int; }
class Box<P> { v: P; }
fn f<P>(x: P) -> P { return x; }
fn main() -> int { P = 1; return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"type parameter 'P' of generic class 'Box' shadows a type of "
                "the same name",
                "type parameter 'P' of generic function 'f' shadows a type of "
                "the same name",
                "'P' is a type name and cannot be used as a variable"});
}
