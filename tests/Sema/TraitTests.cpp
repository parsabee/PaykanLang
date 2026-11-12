// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: traits (#216).  `impl Trait for A` must give A every method the
// trait requires, with its signature (a method A already has counts); a
// class implements what its superclasses do, and every class implements the
// builtin `Showable` through `Obj`.  A trait is not a type.  A type parameter's
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

// `Sendable` (#378) is what the send check finds nothing against: a value,
// a unique object or an aggregate of Sendable parts, a frozen object and a
// cell; not a `shared` reference, a function value or an `Obj`, nor what
// holds one.  It is structural: no block implements it, and only the
// standard library opts a type out.
TEST(Traits, SendableIsWhatCrosses) {
  auto r = semaCheck(R"(import ::collections as cl;
class Node { v: int; }
class Cache { hits: int; }
class Job { name: Str; cache: shared Cache; }
struct Pair { a: int; n: Node; }
struct Mine { id: int; }
fn need<T: Sendable>(x: T) -> int { return 1; }
impl Sendable for Node {}
impl !Sendable for Mine {}
impl !Hashable for Mine {}
fn main() -> int {
  let f: atomic shared = Node(3);
  let s: shared = Node(5);
  let o: Obj = Node(6);
  let opt: Node? = Node(7);
  let ok = need(Node(1)) + need(1) + need("s") + need(f) +
           need(Pair(1, Node(1))) + need((1, "a")) +
           need(cl::List<Node>()) + need(move opt);
  let a = need(s);
  let b = need(Job("j", Cache(0)));
  let c = need(fn() => 3);
  let d = need(move o);
  let e = need(cl::List<shared Node>());
  let g = need((1, s));
  return ok;
}
)");
  EXPECT_EQ(r.ErrorCount, 9) << r.Diagnostics;
  expectAll(
      r,
      {":8:1: error: 'Sendable' is built in: a type is Sendable by what it "
       "holds, and cannot be implemented (a class whose 'shared' links stay "
       "inside it waits for #377)",
       ":9:1: error: only the standard library opts a type out of "
       "'Sendable': struct 'Mine' is Sendable by what it holds",
       ":10:1: error: 'impl !Hashable for': only 'Sendable' is opted out of "
       "('impl !Sendable for T {}')",
       ":19:11: error: 'shared Node' does not implement trait 'Sendable', "
       "which 'need' requires of 'T': a 'shared' reference is aliased on its "
       "own thread",
       ":20:11: error: 'Job' does not implement trait 'Sendable', which "
       "'need' requires of 'T': field 'cache' is 'shared Cache': a 'shared' "
       "reference is aliased on its own thread",
       ":21:11: error: '() -> int' does not implement trait 'Sendable', "
       "which 'need' requires of 'T': is a function value, whose captures "
       "are not known",
       ":22:11: error: 'Obj' does not implement trait 'Sendable', which "
       "'need' requires of 'T': is an 'Obj', which may be any class",
       ":23:11: error: 'List<shared Node>' does not implement trait "
       "'Sendable', which 'need' requires of 'T': field '__items' is 'shared "
       "Node'",
       ":24:11: error: '(int, shared Node)' does not implement trait "
       "'Sendable', which 'need' requires of 'T': field '.1' is 'shared "
       "Node'"});
}

// A template is checked once against its bounds (#373): what it sends, makes
// `atomic shared` or `locked`, returns from an `async fn` or gives to a
// template that needs it is a `T: Sendable`, the template's or its `impl`
// block's.
TEST(Traits, ATemplateSendsWhatItsBoundsSay) {
  auto r = semaCheck(R"(import ::collections as cl;
fn need<T: Sendable>(x: view T) -> int { return 1; }
async fn total<T: Sendable>(xs: cl::List<T>) -> int { return xs.len(); }
fn publish<T: Sendable>(x: T) -> atomic shared T { return move x; }
async fn eat<T>(x: T) -> int { return 1; }
async fn back<T>(x: T) -> T { return move x; }
fn relay<T>(x: T) -> int { return await eat(move x); }
fn freeze<T>(x: T) -> int { let f: atomic shared T = move x; return 1; }
fn cell<T>(x: T) -> int { let c: locked mut T = move x; return 1; }
fn ask<T>(x: view T) -> int { return need(x); }
fn list<T>(xs: cl::List<T>) -> int { return await total(move xs); }
fn fine<T: Sendable>(x: T) -> int { return await eat(move x) + need(1); }
struct Box<T> { v: T; }
impl Box<T: Sendable> {
  fn send(x: T) -> int { return await eat(move x); }
}
impl Box<U> {
  fn leak(x: U) -> int { return await eat(move x); }
}
fn main() -> int { return fine(1); }
)");
  EXPECT_EQ(r.ErrorCount, 7) << r.Diagnostics;
  expectAll(
      r, {":6:1: error: 'T' is not 'Sendable': declare 'T: Sendable' to "
          "return it from an 'async fn'",
          ":7:45: error: 'T' is not 'Sendable': declare 'T: Sendable' to send "
          "it to a task",
          ":8:29: error: 'T' is not 'Sendable': declare 'T: Sendable' to make "
          "it 'atomic shared'",
          ":9:27: error: 'T' is not 'Sendable': declare 'T: Sendable' to make "
          "it 'locked mut'",
          ":10:38: error: 'T' is not 'Sendable': declare 'T: Sendable' for "
          "'need', which needs it",
          ":11:51: error: 'T' is not 'Sendable': declare 'T: Sendable' for "
          "'total', which needs it",
          ":18:43: error: 'U' is not 'Sendable': declare 'U: Sendable' to send "
          "it to a task"});
}

// A generic type's async method returns its `T` in every instance, so an
// instance whose argument does not cross is refused with the block that
// lets the others have the method: a conditional `impl` bounded `Sendable`.
TEST(Traits, AnAsyncMethodOfTheSendableInstances) {
  auto bad = semaCheck(R"(class Node { id: int; }
class Cell<T: Copyable> {
  v: T;
  async view fn later() -> T { return self.v; }
}
impl Cell<T> { fn __init__(v: T) { self.v = v; } }
fn main() -> int {
  let n: shared Node = Node(4);
  let b = Cell<shared Node>(n);
  return b.v.id;
}
)");
  EXPECT_EQ(bad.ErrorCount, 1) << bad.Diagnostics;
  expectAll(bad,
            {":4:3: error: the result of 'async fn' 'later' (a 'shared Node') "
             "cannot cross to the task's awaiter: is 'shared Node': a 'shared' "
             "reference is aliased on its own thread; put the method in an "
             "'impl Cell<...>' block that bounds the parameter 'Sendable', "
             "which only the instances that meet the bound have"});
  auto good = semaCheck(R"(class Node { id: int; }
class Cell<T: Copyable> {
  v: T;
  view fn now() -> T { return self.v; }
}
impl Cell<T> { fn __init__(v: T) { self.v = v; } }
impl Cell<T: Sendable> {
  async view fn later() -> T { return self.v; }
}
fn main() -> int {
  let n: shared Node = Node(4);
  let b = Cell<shared Node>(n);
  let i = Cell<int>(3);
  return b.now().id + await i.later() - 7;
}
)");
  EXPECT_TRUE(good.Ok) << good.Diagnostics;
}

// An async method's result is send-checked against what its type holds,
// which is known once the module's classes are: the order of the
// declarations does not matter.  The hint is for an instance a bound would
// keep the method from.
TEST(Traits, AnAsyncMethodsResultIsCheckedOnceTypesAreWhole) {
  auto r = semaCheck(R"(class Node { v: int; }
class A { n: int; async view fn mk() -> Job { return Job(Node(self.n)); } }
class Job { s: shared Node; }
struct Cell<T> { n: int; }
impl Cell<T: Sendable> { async view fn bad() -> Obj? { return None; } }
struct Wrap<T> { v: T; }
impl Wrap<T> { async view fn get() -> T { return self.v; } }
class Late { c: Cell<int>?; w: Wrap<shared Node>?; }
fn main() -> int { return 0; }
)");
  EXPECT_EQ(r.ErrorCount, 3) << r.Diagnostics;
  expectAll(r,
            {":2:19: error: the result of 'async fn' 'mk' (a 'Job') cannot "
             "cross to the task's awaiter: field 's' is 'shared Node'",
             ":5:26: error: the result of 'async fn' 'bad' (a 'Obj?') cannot "
             "cross to the task's awaiter: is an 'Obj', which may be any "
             "class\n",
             ":7:16: error: the result of 'async fn' 'get' (a 'shared Node') "
             "cannot cross to the task's awaiter: is 'shared Node': a "
             "'shared' reference is aliased on its own thread; put the "
             "method in an 'impl Wrap<...>' block"});
}

// Asking whether a type is Sendable sends nothing: a class a bound was
// asked about is not one the program sends, and its subclasses are not
// checked as if it were.  A type that holds itself is judged once.
TEST(Traits, AskingWhetherATypeIsSendableSendsNothing) {
  auto r = semaCheck(R"(class Node { v: int; }
class Base { s: shared Node; }
class Der : Base { k: int; }
struct Box<T> { v: T; }
impl Box<T: Sendable> { view fn f() -> int { return 1; } }
class Many<T> { one: T?; }
struct Rec { kids: Many<Rec>; n: int; }
enum Tree { Leaf, Fork(kids: Many<Tree>) }
fn need<T: Sendable>(x: view T) -> int { return 1; }
fn main() -> int {
  let b: Box<Base>? = None;
  let r = Rec(Many<Rec>(None), 1);
  return need(r) + need(Tree::Leaf) - 2;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// What an `impl TRAIT for` block of a template may not say is an error at
// the block, whether or not the template is ever instantiated.
TEST(Traits, ATemplatesImplBlockIsCheckedWhereItIsWritten) {
  auto r = semaCheck(R"(struct Box<T> { v: T; }
impl !Sendable for Box<T> {}
class Cell<T> { n: int; }
impl !Hashable for Cell<T> {}
struct B<T> { v: T; }
impl Sendable for B<T> {}
impl Copyable for B<T> {}
fn main() -> int { return 0; }
)");
  EXPECT_EQ(r.ErrorCount, 4) << r.Diagnostics;
  expectAll(r, {":2:1: error: only the standard library opts a type out of "
                "'Sendable': struct 'Box<T>' is Sendable by what it holds",
                ":4:1: error: 'impl !Hashable for': only 'Sendable' is opted "
                "out of ('impl !Sendable for T {}')",
                ":6:1: error: 'Sendable' is built in",
                ":7:1: error: 'Copyable' is built in"});
}

// A template that copies its `T` into a task needs `T: Copyable`, as one
// that copies it anywhere else does.
TEST(Traits, ATemplateThatCopiesIntoATask) {
  auto r = semaCheck(R"(async fn eat<U>(y: U) -> int { return 1; }
fn pass<T>(x: T) -> int { return await eat(x); }
fn main() -> int { return pass(1) - 1; }
)");
  EXPECT_EQ(r.ErrorCount, 1) << r.Diagnostics;
  expectAll(r, {":2:44: error: 'T' is not 'Copyable': declare 'T: Copyable' "
                "to copy it into a task"});
}

// `Showable` was `Show` through 0.3: the old name is an error that names
// the new one, in a bound and in an `impl ... for` block, unless the program
// declares a trait of that name itself.
TEST(Traits, TheOldNameOfShowable) {
  auto r = semaCheck(R"(class A { n: int; }
struct P { x: int; }
fn show<T: Show>(v: view T) -> Str { return v.toString(); }
fn both<T: Hashable + Show>(v: view T) -> Str { return v.toString(); }
fn ok<T: Showable>(v: view T) -> Str { return v.toString(); }
impl Show for P { view fn text() -> Str { return "p"; } }
fn main() -> int { return ok(A(1)).len(); }
)");
  EXPECT_EQ(r.ErrorCount, 3) << r.Diagnostics;
  for (const char *at : {":3:1:", ":4:1:", ":6:1:"})
    EXPECT_NE(r.Diagnostics.find(std::string(at) +
                                 " error: 'Show' is now 'Showable'"),
              std::string::npos)
        << at << "\n"
        << r.Diagnostics;
  auto own = semaCheck(R"(trait Show { view fn text() -> Str; }
class A { n: int; }
impl Show for A { view fn text() -> Str { return "a"; } }
fn t<T: Show>(v: view T) -> Str { return v.text(); }
fn main() -> int { return t(A(1)).len(); }
)");
  EXPECT_TRUE(own.Ok) << own.Diagnostics;
  // A type of the program's own named `Show`, a template too, is no trait.
  for (const char *decl :
       {"class Show { n: int; }", "class Show<T> { v: T; }",
        "struct Show<T> { v: T; }", "enum Show<T> { One(value: T), Zero }"}) {
    auto ty = semaCheck(std::string(decl) + R"(
fn t<T: Show>(v: view T) -> int { return 0; }
fn main() -> int { return 0; }
)");
    EXPECT_NE(ty.Diagnostics.find(":2:1: error: 'Show' is not a trait"),
              std::string::npos)
        << decl << "\n"
        << ty.Diagnostics;
    EXPECT_EQ(ty.Diagnostics.find("is now"), std::string::npos)
        << decl << "\n"
        << ty.Diagnostics;
  }
}

TEST(Traits, DeclarationsAreChecked) {
  auto r = semaCheck(R"(trait Named { view fn name() -> Str; }
trait Named { fn x(); }
trait Shape { fn __init__(); fn area() -> int; fn area() -> int; }
trait Bad { fn f() -> Nope; }
class Showable {}
class Named {}
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"redefinition of trait 'Named'",
                "a trait cannot require '__init__'",
                "duplicate method 'area' in trait 'Shape'",
                "method 'f' of trait 'Bad' has unknown class type 'Nope'",
                "'Showable' is a builtin trait and cannot be redeclared",
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
impl Showable for C {}
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
                "class 'C' already implements 'Showable' (through 'Obj')",
                "unknown trait 'Nope'", "'Str' is not a trait",
                "'int' is a built-in type"});
}

// A requirement's parameters follow a function's rules (findings 63 and
// new-F1b-1): distinct names, none naming a type.  It has no body, so a
// function of the module may name one, as for a native function.  Each
// error is reported once, at the requirement.
TEST(Traits, RequirementParametersAreChecked) {
  auto r = semaCheck(R"(trait T { fn m(x: int, x: int); }
trait U { fn n(u8: int); }
class A {}
trait V { view fn k(a: int, Str: int) -> int; fn j(A: int); }
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 4) << r.Diagnostics;
  expectAll(r, {"1:24: error: duplicate parameter 'x' in method 'm' of trait "
                "'T'",
                "2:16: error: 'u8' is a type name and cannot be used as a "
                "parameter name",
                "4:29: error: 'Str' is a type name and cannot be used as a "
                "parameter name",
                "4:52: error: 'A' is a type name and cannot be used as a "
                "parameter name"});

  auto ok = semaCheck(R"(trait T { fn m(x: int, y: int) -> int; fn n(f: int); }
class A { view fn z() -> int { return 0; } }
impl A {
  fn m(x: int, y: int) -> int { return x + y; }
  fn n(v: int) {}
}
impl T for A {}
fn f() {}
fn main() -> int { return A().m(1, 2); }
)");
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;
  EXPECT_EQ(ok.ErrorCount, 0) << ok.Diagnostics;
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
// by every class (`Showable`), and checked for a class field once the impl
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
fn greet<T: Named + Showable>(x: view T) -> Str { return x.name() + x.toString(); }
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

// A bounded generic enum named before the `impl` blocks are known (a class
// field's type) is checked once they are, as a generic class is: at the
// field.
TEST(Traits, BoundedGenericEnumDeferred) {
  auto ok = semaCheck(R"(// A key, or none.
enum Maybe<T: Hashable> { Nothing, Just(v: T) }
class C { m: Maybe<int>; }
impl C { fn __init__() { self.m = Maybe<int>::Nothing; } }
fn main() -> int { c = C(); return 0; }
)");
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;
  EXPECT_EQ(ok.ErrorCount, 0) << ok.Diagnostics;

  auto r = semaCheck(R"(// A key, or none.
enum Maybe<T: Hashable> { Nothing, Just(v: T) }
class C { m: Maybe<float>; }
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1) << r.Diagnostics;
  expectAll(r, {":3:11: error: 'float' does not implement trait 'Hashable', "
                "which 'Maybe' requires of 'T'"});
}

// A bound error found once the `impl` blocks are known, in a template this
// module instantiates for a class field's type, has the instantiation note
// at the field, as one found before has at its request: for a class, a
// struct and an enum, and a trait of this module.
TEST(Traits, DeferredBoundErrorHasTheInstantiationNote) {
  struct Case {
    const char *Source, *Error, *Note;
  };
  const Case cases[] = {
      {"class Gen<T: Hashable> { v: T; }\nclass Holder<T> { g: Gen<float>; "
       "}\nclass M { h: Holder<int>; }\nfn main() -> int { return 0; }\n",
       ":2:19: error: 'float' does not implement trait 'Hashable', which "
       "'Gen' requires of 'T'",
       ":3:11: note: in instantiation of 'Holder<int>' requested here"},
      {"trait Named { view fn name() -> Str; }\nclass Gen<T: Named> { v: T; "
       "}\nclass Holder<T> { g: Gen<T>; }\nclass M { h: Holder<int>; }\nfn "
       "main() -> int { return 0; }\n",
       ":3:19: error: 'int' does not implement trait 'Named', which 'Gen' "
       "requires of 'T'",
       ":4:11: note: in instantiation of 'Holder<int>' requested here"},
      {"struct S<T: Hashable> { v: T; }\nstruct W<T> { s: S<T>; }\nclass M { "
       "w: W<float>; }\nfn main() -> int { return 0; }\n",
       ":2:15: error: 'float' does not implement trait 'Hashable', which 'S' "
       "requires of 'T'",
       ":3:11: note: in instantiation of 'W<float>' requested here"},
      {"enum Maybe<T: Hashable> { Just(v: T), Nothing }\nclass Holder<T> { m: "
       "Maybe<T>; }\nclass M { h: Holder<float>; }\nfn main() -> int { "
       "return 0; }\n",
       ":2:19: error: 'float' does not implement trait 'Hashable', which "
       "'Maybe' requires of 'T'",
       ":3:11: note: in instantiation of 'Holder<float>' requested here"},
  };
  for (const Case &c : cases) {
    auto r = semaCheck(c.Source);
    EXPECT_FALSE(r.Ok) << c.Source;
    EXPECT_EQ(r.ErrorCount, 1) << r.Diagnostics;
    expectAll(r, {c.Error, c.Note});
  }
}

// A bound found to fail once the `impl` blocks are known is the one error
// (KF-6), as one checked at the request is: the instantiation was made
// before, for a class's or a struct's field, but its bodies are not
// checked, and the bound of an instantiation made in it is a follow-on.
// Base also reported the bodies' errors ("method call on non-class type
// 'int'"), and for a `collections::HashMap<Foo, int>` field nine more in
// collections.pkn: "no method 'hash' on type 'Foo'" seven times, and
// `_Table`'s bound twice.  Another request of a template, not made in such
// an instantiation, is still checked.  So are the bodies of a template
// with no bound that such an instantiation uses (`Inner<Foo>` for its
// field `i: Inner<T>?`): not, when it alone uses them, and still, when the
// program uses them too.
TEST(Traits, AFailedDeferredBoundIsTheOneError) {
  struct Case {
    const char *Source, *Error;
  };
  const Case cases[] = {
      {"trait Named { view fn name() -> Str; }\n"
       "struct Box<T: Named> { v: T; }\n"
       "impl Box<T> { view fn label() -> Str => self.v.name(); }\n"
       "class Holder { b: Box<int>; }\n"
       "fn main() -> int { return 0; }\n",
       ":4:16: error: 'int' does not implement trait 'Named', which 'Box' "
       "requires of 'T'"},
      {"trait Named { view fn name() -> Str; }\n"
       "class Box<T: Named> {\n"
       "  v: T;\n"
       "  view fn label() -> Str { return self.v.name() + \"!\"; }\n"
       "}\n"
       "struct S { b: Box<int>; }\n"
       "fn main() -> int { return 0; }\n",
       ":6:12: error: 'int' does not implement trait 'Named', which 'Box' "
       "requires of 'T'"},
      {"import ::collections;\n"
       "class Foo { n: int; }\n"
       "class Holder { m: collections::HashMap<Foo, int>; }\n"
       "fn main() -> int { return 0; }\n",
       ":3:16: error: 'Foo' does not implement trait 'Hashable', which "
       "'collections::HashMap' requires of 'K'"},
  };
  for (const Case &c : cases) {
    auto r = semaCheck(c.Source);
    EXPECT_FALSE(r.Ok) << c.Source;
    EXPECT_EQ(r.ErrorCount, 1) << r.Diagnostics;
    expectAll(r, {c.Error});
  }
  auto r = semaCheck(
      "trait Named { view fn name() -> Str; }\n"
      "struct Inner<T: Named> { v: T; }\n"
      "impl Inner<T> { view fn label() -> Str => self.v.name(); }\n"
      "struct Outer<T: Named> { i: Inner<T>; }\n"
      "impl Outer<T> {\n"
      "  view fn label() -> Str => self.i.label() + self.i.v.name();\n"
      "}\n"
      "class Holder { o: Outer<int>; j: Inner<Str>; }\n"
      "fn main() -> int { return 0; }\n");
  EXPECT_EQ(r.ErrorCount, 2) << r.Diagnostics;
  expectAll(r, {":8:16: error: 'int' does not implement trait 'Named', which "
                "'Outer' requires of 'T'",
                ":8:31: error: 'Str' does not implement trait 'Named', which "
                "'Inner' requires of 'T'"});

  const std::string unbounded =
      "trait H { view fn h() -> int; }\n"
      "class Inner<T> { v: T; }\n"
      "impl Inner<T> { view fn get() -> int { return self.v.h(); } }\n"
      "class Outer<T: H> { i: Inner<T>?; }\n"
      "impl Outer<T> { view fn g() -> int { return 1; } }\n"
      "class Foo { x: int; }\n";
  const char *bound =
      "error: 'Foo' does not implement trait 'H', which 'Outer' requires of "
      "'T'";
  const char *body = ":3:47: error: no method 'h' on type 'Foo'";
  r = semaCheck(unbounded + "class Holder { o: Outer<Foo>?; }\n"
                            "fn main() -> int { return 0; }\n");
  EXPECT_EQ(r.ErrorCount, 1) << r.Diagnostics;
  expectAll(r, {":7:16: error: 'Foo' does not implement trait 'H', which "
                "'Outer' requires of 'T'"});
  // Its own request, by the program: a field before the failed one, one
  // after, or a body's, also one checked after it, an instantiation's (a
  // generic function's, a generic class's method).
  const std::string holder = "class Holder { o: Outer<Foo>?; }\n";
  const std::string uses[] = {
      "class Other { i: Inner<Foo>?; }\n" + holder +
          "fn main() -> int { return 0; }\n",
      holder + "class Other { i: Inner<Foo>?; }\n" +
          "fn main() -> int { return 0; }\n",
      holder +
          "fn main() -> int { let i = Inner<Foo>(Foo(1)); return i.get(); }\n",
      holder + "fn use<U>(u: U) -> int { let i = Inner<Foo>(Foo(1)); "
               "return i.get(); }\n"
               "fn main() -> int { return use<int>(1); }\n",
      holder + "class W<U> { u: U; }\n" +
          "impl W<U> {\n"
          "  view fn run() -> int { let i = Inner<Foo>(Foo(1)); "
          "return i.get(); }\n"
          "}\n"
          "fn main() -> int { let w = W<int>(1); return w.run(); }\n"};
  for (const std::string &use : uses) {
    r = semaCheck(unbounded + use);
    EXPECT_EQ(r.ErrorCount, 2) << r.Diagnostics;
    expectAll(r, {bound, body});
  }
}

// int, float, bool and char implement `Showable`: `n.toString()` is a `Str`. So
// does an enum: its variant's name.
TEST(Traits, ScalarsImplementShow) {
  auto r = semaCheck(R"(enum Color { Red }
fn show<T: Showable>(x: view T) -> Str { return x.toString(); }
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

// A tuple, struct, enum or optional shows itself when every part does.  A
// function value does not, so one holding a function value (directly, as an
// optional, or inside another aggregate) does not implement `Showable`: no
// print function, no `T: Showable` bound and no `toString()` takes it, and the
// message names the part.  They implemented it and printed the closure's
// code address and environment.
TEST(Traits, AnAggregateHoldingAFunctionValueIsNotShow) {
  auto r = semaCheck(R"(import ::io;
struct S { f: (int) -> int; }
struct Outer { n: int; s: S; }
struct O { g: ((int) -> int)?; }
enum E { F(h: (int) -> int), N }
enum Maybe<X> { Just(v: X), Nothing }
fn show<X: Showable>(x: view X) -> Str { return x.toString(); }
fn main() -> int {
  k = 5;
  s = S(fn(x: int) => x + k);
  io::println(s);
  io::println(s.toString());
  io::println((1, fn(x: int) => x));
  io::println((1, S(fn(x: int) => x)).toString());
  io::println(Outer(1, S(fn(x: int) => x)));
  io::println(O(None).toString());
  io::println(E::N);
  io::println(E::N.toString());
  o: S? = S(fn(x: int) => x);
  io::println(o);
  io::println(o.toString());
  io::println(show(s));
  io::println(Maybe::Just(fn(x: int) => x).toString());
  of: ((int) -> int)? = None;
  io::println(of);
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  auto expect = [&](const std::string &d) {
    EXPECT_NE(r.Diagnostics.find(d), std::string::npos) << d << "\n"
                                                        << r.Diagnostics;
  };
  const std::string fn = "has type '(int) -> int', which does not implement it";
  const std::string println = "which 'io::println' requires of 'T': its ";
  const std::string noToString = "no method 'toString' on type ";
  const std::string notShow = "which does not implement trait 'Showable': its ";
  expect("'S' does not implement trait 'Showable', " + println + "field 'f' " +
         fn);
  expect(noToString + "'S', " + notShow + "field 'f' " + fn);
  expect("'(int, (int) -> int)' does not implement trait 'Showable', " +
         println + "element 1 " + fn);
  expect(noToString + "'(int, S)', " + notShow +
         "element 1 has type 'S', whose field 'f' " + fn);
  expect("'Outer' does not implement trait 'Showable', " + println +
         "field 's' has type 'S', whose field 'f' " + fn);
  expect(noToString + "'O', " + notShow +
         "field 'g' has type '((int) -> int)?', which does not implement it");
  expect("'E' does not implement trait 'Showable', " + println +
         "field 'h' of variant 'F' " + fn);
  expect(noToString + "'E', " + notShow + "field 'h' of variant 'F' " + fn);
  expect("'S?' does not implement trait 'Showable', " + println + "field 'f' " +
         fn);
  expect(noToString + "'S?', " + notShow + "field 'f' " + fn);
  expect(
      "'S' does not implement trait 'Showable', which 'show' requires of 'X': "
      "its field 'f' " +
      fn);
  expect(noToString + "'Maybe<(int) -> int>', " + notShow +
         "field 'v' of variant 'Just' " + fn);
  expect("'((int) -> int)?' does not implement trait 'Showable', which "
         "'io::println' requires of 'T'\n");
  EXPECT_EQ(r.ErrorCount, 13) << r.Diagnostics;
}

// An aggregate whose parts share their types (a DAG) is decided part by
// part, each type once per question (Sema::TextQuery), and a part with no
// text is named through every level down to it.
TEST(Traits, ShowThroughSharedPartsNamesTheWholePath) {
  auto r = semaCheck(R"(import ::io;
struct F { f: (int) -> int; }
enum E0 { Z, Y(g: F) }
enum E1 { A(x: E0), B(y: E0), C(z: E0) }
enum E2 { A(x: E1), B(y: E1), C(z: E1) }
struct Ok1 { a: (int, char); b: (int, char); }
struct Ok2 { a: Ok1; b: Ok1; c: Ok1?; }
fn main() -> int {
  io::println(Ok2(Ok1((1, 'a'), (2, 'b')), Ok1((3, 'c'), (4, 'd')), None));
  io::println(E2::C(E1::A(E0::Z)));
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(
                "'E2' does not implement trait 'Showable', which 'io::println' "
                "requires of 'T': its field 'x' of variant 'A' has type 'E1', "
                "whose field 'x' of variant 'A' has type 'E0', whose field 'g' "
                "of variant 'Y' has type 'F', whose field 'f' has type '(int) "
                "-> int', which does not implement it"),
            std::string::npos)
      << r.Diagnostics;
}

// The aggregates whose parts all show themselves do, through every form:
// printed, `toString()`, an optional of one, a `T: Showable` bound, nested.  A
// function value inside a class does not matter: an object shows itself
// through `Obj`.
TEST(Traits, AnAggregateOfShowPartsIsShow) {
  auto r = semaCheck(R"(import ::io;
class Handler { f: (int) -> int; }
impl Handler { fn __init__() { self.f = fn(x: int) => x; } }
struct P { x: int; name: Str; h: Handler; }
struct Q { p: P; at: (float, char?); }
enum E { A(q: Q), B(n: int?), N }
enum Maybe<X> { Just(v: X), Nothing }
fn show<X: Showable>(x: view X) -> Str { return x.toString(); }
fn mkQ(n: int) -> Q { return Q(P(n, "p", Handler()), (2.5, None)); }
fn main() -> int {
  p = P(1, "p", Handler());
  q = mkQ(1);
  io::println(p);
  io::println(q.toString());
  io::println((1, mkQ(2)));
  io::println(E::A(mkQ(3)));
  io::println(E::B(None).toString());
  o: Q? = None;
  io::println(o);
  io::println(o.toString());
  m: Maybe<Q> = Maybe::Just(move q);
  return show(m).len() + show((move p, E::N)).len();
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0) << r.Diagnostics;
}

// A struct's `toString` is built in, so one with a part that has no text
// cannot implement `Showable` with an impl block; one whose parts all have one
// already does.
TEST(Traits, AStructWithoutATextCannotImplementShow) {
  auto r = semaCheck(R"(struct S { f: (int) -> int; }
struct P { x: int; }
impl Showable for S {}
impl Showable for P {}
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r,
            {"struct 'S' cannot implement 'Showable' (its 'toString' is built "
             "in): its field 'f' has type '(int) -> int', which does not "
             "implement it",
             "struct 'P' already implements 'Showable'"});
  EXPECT_EQ(r.ErrorCount, 2) << r.Diagnostics;
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
