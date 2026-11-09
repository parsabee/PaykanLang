// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: the archetype check of templates (#368;
// docs/language/13-generics.md, "The archetype check";
// src/Sema/SemaArchetypes.cpp).  Each template of a module is checked once
// more with a move-only class for each type parameter without bounds: a
// copy of one is a missing `T: Copyable`, reported at the template whether
// or not an instance asks for it.

#include "TestUtils.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace paykan::test;

namespace {

void expectErrors(const SemaResult &r,
                  std::initializer_list<const char *> diags) {
  EXPECT_FALSE(r.Ok);
  for (const char *diag : diags)
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
}

size_t count(const std::string &text, const std::string &what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos;
       at = text.find(what, at + 1))
    ++n;
  return n;
}

} // namespace

// A copy of an unbounded `T` is reported at the template, unused or used
// only with `int`, which an instance would never show; `T: Copyable`
// allows it.
TEST(Archetype, ACopyOfAnUnboundedParameterIsReportedAtTheTemplate) {
  auto unused = semaCheck(R"(fn dup<T>(x: T) -> (T, T) { return (x, x); }
fn main() -> int { return 0; }
)");
  expectErrors(unused, {":1:37: error: 'T' is not 'Copyable': declare 'T: "
                        "Copyable' to copy it",
                        ":1:40: error: 'T' is not 'Copyable'"});
  EXPECT_EQ(unused.Diagnostics.find("in instantiation of"), std::string::npos)
      << unused.Diagnostics;
  auto withInt = semaCheck(R"(fn dup<T>(x: T) -> (T, T) { return (x, x); }
fn main() -> int { let p = dup(1); return p.0; }
)");
  expectErrors(withInt, {":1:37: error: 'T' is not 'Copyable'"});
  auto bounded =
      semaCheck(R"(fn dup<T: Copyable>(x: T) -> (T, T) { return (x, x); }
fn main() -> int { let p = dup(1); return p.0; }
)");
  EXPECT_TRUE(bounded.Ok) << bounded.Diagnostics;
}

// A transfer is no copy: `move`, a by-value parameter returned, a result
// handed on.
TEST(Archetype, ATransferIsNoCopy) {
  auto r = semaCheck(R"(enum Res<T, E> { Ok(v: T), Err(e: E) }
class Box<T> { v: T; }
impl Box<T> {
  fn __init__(v: T) { self.v = move v; }
  fn swap(w: T) -> T { let old = move self.v; self.v = move w; return old; }
}
fn ok<T, E>(v: T) -> Res<T, E> => Res::Ok(move v);
fn id<T>(x: T) -> T { return x; }
fn wrap<T>(x: T) -> Box<T> { return Box<T>(id(move x)); }
fn main() -> int { return 0; }
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// A copy out of a borrow: a `view fn`'s field returned, a `view`
// parameter's element returned, stored or passed by value.  A method may
// instead go in a conditional `impl` block, which an `impl` block names in
// its own words.
TEST(Archetype, ACopyOutOfABorrowIsReported) {
  auto r =
      semaCheck(R"(class Box<T> { v: T; view fn get() -> T { return self.v; } }
impl Box<T> { fn __init__(v: T) { self.v = move v; } }
fn first<T>(xs: view T[]) -> T { return xs[0]; }
fn keep<T>(x: view T) -> T { let k = x; return k; }
fn sink<T>(x: T) -> int { return 0; }
fn pass<T>(x: view T) -> int { return sink(x); }
class R<T> { v: T; }
impl R<U> { view fn peek() -> U { return self.v; } }
fn main() -> int { return 0; }
)");
  expectErrors(
      r, {":1:50: error: 'T' is not 'Copyable': declare 'T: Copyable' to "
          "return a copy, or put the method in 'impl Box<T: Copyable>'",
          ":3:41: error: 'T' is not 'Copyable': declare 'T: Copyable' to "
          "return a copy",
          ":4:38: error: 'T' is not 'Copyable': declare 'T: Copyable' to copy "
          "it",
          ":6:44: error: 'T' is not 'Copyable': declare 'T: Copyable' to pass "
          "a copy",
          ":8:42: error: 'U' is not 'Copyable': declare 'U: Copyable' to "
          "return a copy, or put the method in 'impl R<U: Copyable>'"});
  auto fine = semaCheck(R"(class Box<T> { v: T; }
impl Box<T> { fn __init__(v: T) { self.v = move v; } }
impl Box<T: Copyable> { view fn get() -> T { return self.v; } }
fn main() -> int { let b = Box<int>(1); return b.get(); }
)");
  EXPECT_TRUE(fine.Ok) << fine.Diagnostics;
}

// A template handing an unbounded `T` to one that needs `T: Copyable`
// misses the bound too.
TEST(Archetype, AnUnboundedParameterForACopyableOneIsReported) {
  auto r = semaCheck(R"(fn twice<T: Copyable>(x: T) -> (T, T) { return (x, x); }
fn both<T>(x: T) -> (T, T) { return twice(move x); }
fn main() -> int { let p = both(1); return p.0; }
)");
  expectErrors(r, {":2:37: error: 'T' is not 'Copyable': declare 'T: "
                   "Copyable' for 'twice', which needs it"});
}

// Only copies are reported: an archetype has no `+` and no `toString`, which
// an instance with `int` has; an unused body is otherwise unchecked, and a
// module with another error is not checked at all.
TEST(Archetype, OnlyCopiesAreReported) {
  auto r = semaCheck(R"(fn add<T>(a: T, b: T) -> T { return a + b; }
fn show<T>(x: view T) -> Str { return Str(x); }
class Broken<T> {
  v: T;
  fn bad() -> int { return undefined_name; }
}
fn main() -> int { return add(1, 2); }
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  auto other = semaCheck(R"(fn dup<T>(x: T) -> (T, T) { return (x, x); }
fn main() -> int { return nothing; }
)");
  EXPECT_FALSE(other.Ok);
  EXPECT_EQ(other.Diagnostics.find("is not 'Copyable'"), std::string::npos)
      << other.Diagnostics;
}

// A parameter bounded `Copyable` takes a type meeting its bounds, whose
// copies are fine; the others are archetypes.
TEST(Archetype, ACopyableParameterTakesAStandIn) {
  auto r = semaCheck(R"(fn pair<K: Copyable, V>(k: K, v: V) -> (K, K, V, V) {
  return (k, k, v, v);
}
fn main() -> int { return 0; }
)");
  expectErrors(r, {":2:17: error: 'V' is not 'Copyable'",
                   ":2:20: error: 'V' is not 'Copyable'"});
  EXPECT_EQ(count(r.Diagnostics, "is not 'Copyable'"), 2u) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("'K' is not"), std::string::npos)
      << r.Diagnostics;
}

// A template it instantiates with its own archetype reports its own copies
// once, in its own words.
TEST(Archetype, ATemplatesCopiesAreItsOwn) {
  auto r = semaCheck(R"(fn dup<U>(x: U) -> (U, U) { return (x, x); }
fn use<T>(x: T) -> int { let p = dup(move x); return 0; }
fn main() -> int { return 0; }
)");
  expectErrors(r, {":1:37: error: 'U' is not 'Copyable'",
                   ":1:40: error: 'U' is not 'Copyable'"});
  EXPECT_EQ(count(r.Diagnostics, "is not 'Copyable'"), 2u) << r.Diagnostics;
}

// In a template's code a `view` of a type argument's Copyable form is
// copied as the argument is (#368): T = `shared mut Node` meets
// `T: Copyable`, and its copy is the same object.
TEST(Archetype, ACopyableArgumentsFormIsCopiedOutOfABorrow) {
  auto r = semaCheck(R"(class Node { v: int; }
class Box<T: Copyable> { v: T; view fn get() -> T { return self.v; } }
impl Box<T: Copyable> { fn __init__(v: T) { self.v = move v; } }
fn first<T: Copyable>(xs: view T[]) -> T { return xs[0]; }
fn keep<T: Copyable>(x: view T) -> T { let k = x; return k; }
fn main() -> int {
  s: shared mut Node = Node(1);
  let b = Box<shared mut Node>(s);
  let g = b.get();
  let a: shared mut Node[2] = [s, s];
  let t = first<shared mut Node>(a);
  let k = keep<shared mut Node>(s);
  return g.v + t.v + k.v;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  auto unique = semaCheck(R"(class Node { v: int; }
fn keep<T: Copyable>(x: view T) -> T { let k = x; return k; }
fn main() -> int { let k = keep<Node>(Node(1)); return 0; }
)");
  expectErrors(unique, {"'Node' does not implement trait 'Copyable'"});
}

// A module the archetype check fails is reported once, through the import
// that loads it; a module importing it after that adds no error of its
// own, and what it takes from it is poisoned under that report (the
// invariant check counted it as an unreported failure).
TEST(Archetype, AFailedModuleImportedTwiceIsReportedOnce) {
  const auto dir = paykan::test::tempDir() / "archetype_failed_module";
  std::filesystem::remove_all(dir);
  auto write = [&](const std::string &name, const std::string &text) {
    std::filesystem::create_directories(dir);
    std::ofstream(dir / name) << text;
  };
  write("lib.pkn", R"(fn dup<T>(x: T) -> (T, T) { return (x, x); }
class Box<T> { v: T; }
impl Box<T> { fn __init__(v: T) { self.v = move v; } }
)");
  write("a.pkn", R"(import lib;
fn a() -> int { b = lib::Box<int>(1); return b.v; }
)");
  write("main.pkn", R"(import lib;
import a;
fn main() -> int { return a::a(); }
)");
  paykan::parser::ParserDriver drv(paykan::test::testFrontend());
  ASSERT_EQ(drv.parseFile((dir / "main.pkn").string()), 0);
  std::ostringstream os;
  paykan::sema::DiagEngine diags(os);
  diags.setSourceInfo(drv.getCurrentFile(), &drv.getSourceLines());
  paykan::sema::Sema sema(drv.getASTContext(), diags, dir.string(),
                          drv.getFrontendName());
  const bool ok = sema.run(drv.getRoot()).Ok;
  std::filesystem::remove_all(dir);
  EXPECT_FALSE(ok);
  EXPECT_EQ(count(os.str(), "'T' is not 'Copyable'"), 2u) << os.str();
  EXPECT_EQ(os.str().find("internal compiler error"), std::string::npos)
      << os.str();
}

// Only a copy the archetype is what keeps from copying: a class instance
// holding it (`Box<T>`) is move-only for every T, and a function value
// shares its closure for every T.
TEST(Archetype, OnlyTheArchetypesCopiesAreItsOwn) {
  auto r = semaCheck(R"(class Box<T> { v: T; }
fn dupBox<T>(b: Box<T>) -> (Box<T>, Box<T>) { return (b, b); }
fn main() -> int { return 0; }
)");
  EXPECT_EQ(r.Diagnostics.find("is not 'Copyable'"), std::string::npos)
      << r.Diagnostics;
}

// What a copy needs of the code's own parameters, as the code names them:
// a lambda's in an `impl R<U>` method too, a field's and a signature's
// bound, `[v; n]`, a capture, an untyped parameter, every parameter of a
// class's `impl` header, and none for a trait's method.
TEST(Archetype, EveryCopyIsTheCodesOwn) {
  auto r = semaCheck(R"(class R<T> { v: T; }
impl R<U> {
  fn __init__(v: U) { self.v = move v; }
  view fn mk() -> (U) -> (U, U) { return fn(x: U) -> (U, U) => (x, x); }
}
class Needs<X: Copyable> { x: X; }
class Holder<B> { n: Needs<B>?; }
fn sig<A, B>(x: Needs<B>?) -> int { return 0; }
fn three<T>(v: T) -> T[3] { return [v; 3]; }
fn cap<T>(key: T) -> () -> int { return fn() -> int => count(key); }
fn count<T>(x: view T) -> int { return 1; }
fn twice(x) => (x, x);
class Pair<A, B> { a: A; b: B; view fn dupB() -> (B, B) { return (self.b, self.b); } }
trait Peek { view fn peek() -> int; }
class Cell<T> { v: T; }
impl Peek for Cell<T> { view fn peek() -> int { let c = self.v; return 0; } }
fn main() -> int { return 0; }
)");
  expectErrors(
      r, {":4:65: error: 'U' is not 'Copyable': declare 'U: Copyable' to copy "
          "it, or put the method in 'impl R<U: Copyable>'",
          ":7:19: error: 'B' is not 'Copyable': declare 'B: Copyable' for "
          "'Needs', which needs it",
          ":8:14: error: 'B' is not 'Copyable': declare 'B: Copyable' for "
          "'Needs', which needs it",
          ":9:37: error: 'T' is not 'Copyable': declare 'T: Copyable' to "
          "repeat it",
          ":10:62: error: 'T' is not 'Copyable': declare 'T: Copyable' to "
          "capture it",
          ":12:17: error: parameter 'x' takes any type, which may not be "
          "'Copyable': write its type, 'x: T' with 'T: Copyable', to copy it",
          "error: 'B' is not 'Copyable': declare 'B: Copyable' to copy it, or "
          "put the method in 'impl Pair<A, B: Copyable>'",
          ":16:57: error: 'T' is not 'Copyable': declare 'T: Copyable' to copy "
          "it\n"});
}

// An unconditional derived `clone` is every T's that has one: an archetype
// stands for any, so the template is still checked.  A bounded parameter
// a value cannot stand in for (`shared K`) takes a reference instead.
TEST(Archetype, ATemplateIsCheckedWhateverItsStandIns) {
  auto r =
      semaCheck(R"(class Box<T> { v: T; view fn get() -> T { return self.v; } }
impl Box<T> { fn __init__(v: T) { self.v = move v; } }
impl Cloneable for Box<T> {}
fn keys<K: Hashable, V>(k: shared K, v: V) -> (V, V) { return (v, v); }
fn main() -> int { return 0; }
)");
  expectErrors(r, {":1:50: error: 'T' is not 'Copyable'",
                   ":4:64: error: 'V' is not 'Copyable'"});
}

// The check leaves nothing behind: an unused template's methods get no
// `view fn` suggestion, a method returning part of `self` to change still
// changes it, and a conditional's copy is each branch's (a `view` of a
// unique object is no shared copy beside a type argument's form).
TEST(Archetype, TheCheckLeavesNothingBehind) {
  auto unused = semaCheck(R"(class Box<T> {
  v: T;
  n: int;
  fn count() -> int { return self.n; }
}
impl Box<T> { fn __init__(v: T) { self.v = move v; self.n = 0; } }
fn main() -> int { return 0; }
)");
  EXPECT_TRUE(unused.Ok) << unused.Diagnostics;
  EXPECT_EQ(unused.Diagnostics.find("never changes"), std::string::npos)
      << unused.Diagnostics;
  auto shared = semaCheck(R"(class Node { k: int; }
class Box<T: Copyable> { v: T; fn get() -> T { return self.v; } }
impl Box<T> { fn __init__(v: T) { self.v = move v; } }
fn main() -> int {
  s: shared mut Node = Node(1);
  b: mut Box<shared mut Node> = Box<shared mut Node>(s);
  let g = b.get();
  return g.k - 1;
}
)");
  EXPECT_TRUE(shared.Ok) << shared.Diagnostics;
  EXPECT_EQ(shared.Diagnostics.find("never changes"), std::string::npos)
      << shared.Diagnostics;
  auto joined = semaCheck(R"(class Node { v: int; }
fn keep<T: Copyable>(c: bool, x: view T, u: view Node) -> T {
  let k = if c then u else x;
  return k;
}
fn main() -> int {
  s: shared Node = Node(0);
  n: mut Node = Node(1);
  let a = keep<shared Node>(True, s, n);
  return a.v;
}
)");
  expectErrors(joined, {"'u' is a 'view' parameter; it cannot be stored"});
}

// A copy through both branches of one place is one error.
TEST(Archetype, ACopyOfOnePlaceThroughBothBranchesIsOneError) {
  auto r = semaCheck(R"(class Node { v: int; }
fn m(x: Node, c: bool) -> int { let y = if c then x else x; return 0; }
fn main() -> int { return m(Node(1), True); }
)");
  EXPECT_EQ(count(r.Diagnostics, "cannot copy 'x'"), 1u) << r.Diagnostics;
}

// Every instance's own code reports, whatever the order the templates come
// in: a static fn's signature and body, a field another template's
// signature made first, an `impl` method's signature in its block's names,
// a lambda in a static fn, a conditional block's method (with a stand-in
// for its `Copyable` bound), a struct with a `clone` of its own, an enum's
// payload.
TEST(Archetype, EveryInstancesCodeReports) {
  auto r = semaCheck(R"(class Box<T> { v: T; }
impl Box<T> {
  fn __init__(v: T) { self.v = move v; }
  static fn twice(x: T) -> (T, T) { return (x, x); }
}
fn f<B>(h: Holder<B>) -> int { return 0; }
class Needs<X: Copyable> { x: X; }
class Holder<B> { n: Needs<B>?; }
impl Holder<C> { fn ret() -> Needs<C>? { return None; } }
class R<T> { v: T; }
impl R<U> { static fn mk() -> (U) -> (U, U) { return fn(x: U) -> (U, U) => (x, x); } }
class Pair<A, B> { a: A; b: B; }
impl Pair<A: Copyable, B> { view fn dupB() -> (B, B) { return (self.b, self.b); } }
struct P<T> { v: T?; n: int; }
impl Cloneable for P<T> { view fn clone() -> P<T> { return P<T>(None, self.n); } }
impl P<T> { fn dup() -> (T?, T?) { return (self.v, self.v); } }
enum E<T> { A(n: Needs<T>?), Z }
fn main() -> int { return 0; }
)");
  expectErrors(
      r, {":4:45: error: 'T' is not 'Copyable': declare 'T: Copyable' to copy "
          "it, or put the method in 'impl Box<T: Copyable>'",
          ":8:19: error: 'B' is not 'Copyable': declare 'B: Copyable' for "
          "'Needs', which needs it",
          ":9:18: error: 'C' is not 'Copyable': declare 'C: Copyable' for "
          "'Needs', which needs it, or put the method in 'impl Holder<C: "
          "Copyable>'",
          ":11:77: error: 'U' is not 'Copyable': declare 'U: Copyable' to copy "
          "it, or put the method in 'impl R<U: Copyable>'",
          ":13:64: error: 'B' is not 'Copyable': declare 'B: Copyable' to copy "
          "it, or put the method in 'impl Pair<A: Copyable, B: Copyable>'",
          ":16:44: error: 'T' is not 'Copyable': declare 'T: Copyable' to copy "
          "it, or put the method in 'impl P<T: Copyable>'",
          ":17:15: error: 'T' is not 'Copyable': declare 'T: Copyable' for "
          "'Needs', which needs it"});
}

// A parameter bounded by another trait is an archetype too, which meets
// it: its copies are checked as an unbounded one's, and a type that needs
// the bound of it (`cl::HashMap<K, V>`) takes it.  Untyped parameters get
// names the template leaves free, one each.
TEST(Archetype, ABoundOtherThanCopyableIsAnArchetypes) {
  auto r = semaCheck(R"(import ::collections as cl;
fn pair<K: Hashable, V>(k: K, v: V) -> (K, K) { return (k, k); }
fn lookup<K, V>(m: view cl::HashMap<K, V>, k: view K) -> V { return m.at(k); }
fn f<T>(x: T, y) => (y, y, move x);
fn main() -> int { return 0; }
)");
  expectErrors(
      r, {":2:57: error: 'K' is not 'Copyable': declare 'K: Copyable' to copy "
          "it",
          ":3:69: error: 'V' is not 'Copyable': declare 'V: Copyable' to "
          "return a copy",
          ":4:22: error: parameter 'y' takes any type, which may not be "
          "'Copyable': write its type, 'y: U' with 'U: Copyable', to copy it"});
}

// The conditionals the copy rule meets more than once: one error for one
// place through any branches, a nested conditional's or an element's; a
// `view` result is storage, never a new value a binding may hold `shared`;
// a conditional's clone is a copy only when each branch is a type
// argument's form.
TEST(Archetype, AConditionalIsEachOfItsBranches) {
  auto once = semaCheck(R"(class Node { v: int; }
fn m(x: Node, c: bool, d: bool) -> int { let y = if c then x else if d then x else x; return 0; }
fn n(xs: Node[2], c: bool) -> int { let z = if c then xs[0] else xs[0]; return z.v; }
fn main() -> int { return m(Node(1), True, False) + n([Node(1), Node(2)], True); }
)");
  EXPECT_EQ(count(once.Diagnostics, "cannot copy 'x'"), 1u) << once.Diagnostics;
  EXPECT_EQ(count(once.Diagnostics, "cannot copy 'xs[0]'"), 1u)
      << once.Diagnostics;
  auto view = semaCheck(R"(class Node { v: int; }
class Holder { n: Node; view fn peek() -> view Node { return self.n; } }
impl Holder { fn __init__() { self.n = Node(1); } }
fn keep<T: Copyable>(c: bool, x: view T, h: view Holder) -> T? {
  match if c then h.peek() else x {
    k: Node { return k; }
    _ { return None; }
  }
}
fn main() -> int {
  s: shared mut Node = Node(0);
  let h = Holder();
  let a = keep<shared mut Node>(True, s, h);
  return 0;
}
)");
  expectErrors(view, {"'k' binds part of a conditional whose branches have "
                      "two forms, 'shared mut Node' and 'Node'"});
  auto clone = semaCheck(R"(class Node { v: int; }
fn keep<T: Copyable>(c: bool, x: view T, u: view Node) -> T {
  return (if c then u else x).clone();
}
fn main() -> int {
  s: shared mut Node = Node(0);
  let n = Node(1);
  let a = keep<shared mut Node>(True, s, n);
  return 0;
}
)");
  expectErrors(clone, {"class 'Node' is not Cloneable"});
}

// What an archetype stands for, it is in every part: a derived `clone`
// holding it in a part, a bound of `shared T`, the `clone()` its
// `Cloneable` bound gives (another archetype, whose copies are its own),
// and a `shared K` stand-in for a `Copyable` parameter.
TEST(Archetype, AnArchetypeIsWhatItStandsForInEveryPart) {
  auto r = semaCheck(R"(class Box<T: Cloneable> { v: T?; }
impl Box<T> { fn __init__(v: T) { self.v = move v; } }
impl Cloneable for Box<T> {}
impl Box<T> { fn dup(x: T) -> (T, T) { return (x, x); } }
class Need<U: Hashable> { u: U; }
fn f<T: Hashable>(n: view Need<shared T>, x: T) -> (T, T) { return (x, x); }
fn g<T: Cloneable>(xs: view T[]) -> (T, T) { let c = xs[0].clone(); return (c, c); }
fn keys<K: Hashable + Copyable, V>(k: shared K, v: V) -> (V, V) { return (v, v); }
fn main() -> int { return 0; }
)");
  expectErrors(r, {":4:48: error: 'T' is not 'Copyable'",
                   ":6:69: error: 'T' is not "
                   "'Copyable'",
                   ":7:77: error: 'T' is not 'Copyable'",
                   ":8:75: error: 'V' is not 'Copyable'"});
}

// An `impl` block's names are its own methods': another template's instance
// made from one's signature, and an inferred body checked from one's body,
// are in their own template's names, a field with no method hint.
TEST(Archetype, AnImplBlocksNamesStayInIt) {
  auto r = semaCheck(R"(class Needs<X: Copyable> { x: X; }
class R<B, C> { v: B; c: C; }
impl R<Q, P> { fn mk() -> Holder<Q, P>? { return None; } }
class Holder<B, C> { n: Needs<B>?; c: C; }
class Outer<T> { v: T; }
impl Outer<V> {
  fn __init__(v: V) { self.v = move v; }
  fn go(w: V) -> int { let p = Box<V>::two(move w); return 0; }
}
class Box<T> { v: T; }
impl Box<T> {
  fn __init__(v: T) { self.v = move v; }
  static fn two(x: T) => (x, x);
}
fn main() -> int { return 0; }
)");
  expectErrors(r, {":4:22: error: 'B' is not 'Copyable': declare 'B: Copyable' "
                   "for 'Needs', which needs it\n",
                   ":13:27: error: 'T' is not 'Copyable': declare 'T: "
                   "Copyable' to copy it, or put the method in 'impl Box<T: "
                   "Copyable>'"});
  EXPECT_EQ(count(r.Diagnostics, "'B' is not"), 1u) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("'Q' is not"), std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("'V' is not"), std::string::npos)
      << r.Diagnostics;
}

// Each branch of a conditional is checked as itself: two temporaries that
// print alike are two branches, a clone of a conditional copies only one
// form, and a later branch's part of `self` still changes `self`.
TEST(Archetype, EachBranchIsItself) {
  auto temps = semaCheck(R"(class Node { v: int; }
fn main() -> int {
  s: shared mut Node = Node(1);
  let c = False;
  n: Node = if c then (Node(2), 0).0 else (s, 0).0;
  return n.v;
}
)");
  expectErrors(temps, {"cannot store a 'shared mut Node' in variable 'n'"});
  auto forms = semaCheck(R"(class Node { v: int; }
fn f<T: Copyable, U>(c: bool, t: view T, u: view U) -> T {
  return (if c then t else u).clone();
}
fn main() -> int {
  a: shared mut Node = Node(1);
  b: mut Node = Node(2);
  let z = f<shared mut Node, mut Node>(False, a, b);
  return z.v;
}
)");
  expectErrors(forms, {"class 'Node' is not Cloneable"});
  auto self = semaCheck(R"(class Node { k: int; }
class Box<T: Copyable> {
  v: T;
  fn get(c: bool, x: view T) -> T { let k = if c then x else self.v; return k; }
}
impl Box<T> { fn __init__(v: T) { self.v = move v; } }
fn main() -> int {
  s: shared mut Node = Node(1);
  b: mut Box<shared mut Node> = Box<shared mut Node>(s);
  let g = b.get(False, s);
  return g.k - 1;
}
)");
  EXPECT_TRUE(self.Ok) << self.Diagnostics;
  EXPECT_EQ(self.Diagnostics.find("never changes"), std::string::npos)
      << self.Diagnostics;
}

// A `view` result of a type argument's form lends exactly that form, as
// the template's code may keep a copy of it: another object of the same
// class, or a unique part of `self`, is not it.
TEST(Archetype, AViewResultOfAnArgumentsFormLendsThatForm) {
  auto r = semaCheck(R"(class Node { id: int; }
fn pick<T: Copyable>(x: view T, y: view Node) -> view T { return y; }
class Box<T: Copyable> { v: T; other: Node; view fn peek() -> view T { return self.other; } }
impl Box<T> { fn __init__(v: T) { self.v = move v; self.other = Node(2); } }
fn main() -> int {
  s: shared mut Node = Node(1);
  n: mut Node = Node(2);
  let a = pick<shared mut Node>(s, n);
  let b = Box<shared mut Node>(Node(3));
  return 0;
}
)");
  expectErrors(r, {":2:66: error: a 'view' result of a type argument's form "
                   "is held as 'shared mut Node', but what it returns is held "
                   "as 'Node'",
                   ":3:79: error: a 'view' result of a type argument's form"});
  auto fine = semaCheck(R"(class Node { id: int; }
fn pick<T: Copyable>(x: view T, y: view T) -> view T { return y; }
class Box<T: Copyable> { v: T; view fn peek() -> view T { return self.v; } }
impl Box<T> { fn __init__(v: T) { self.v = move v; } }
fn main() -> int {
  s: shared mut Node = Node(1);
  let b = Box<shared mut Node>(s);
  return pick<shared mut Node>(s, s).id + b.peek().id;
}
)");
  EXPECT_TRUE(fine.Ok) << fine.Diagnostics;
  // A unique form copies for no T, so its `view` may read any form.
  auto unique = semaCheck(R"(import ::collections as cl;
class Node { id: int; }
class Box<T> { v: T; view fn peek() -> view T { return self.v; } }
impl Box<T> { fn __init__(v: T) { self.v = move v; } }
fn main() -> int {
  let b = Box<mut Node>(Node(1));
  let m = cl::HashMap<Str, mut Node>();
  return b.peek().id;
}
)");
  EXPECT_TRUE(unique.Ok) << unique.Diagnostics;
}

// One slot, one error per distinct message: two branches of one form, or
// two calls of one `inout` result, say the same thing.
TEST(Archetype, BranchesWithOneErrorReportItOnce) {
  auto r = semaCheck(R"(class Node { v: int; }
class B { n: mut Node; fn get() -> inout Node { return self.n; } }
fn main() -> int {
  let s: shared Node = Node(1);
  let t: shared Node = Node(2);
  c = True;
  a: mut Node = if c then s else t;
  b: mut B = B(Node(3));
  let k = if c then b.get() else b.get();
  return 0;
}
)");
  EXPECT_EQ(
      count(r.Diagnostics, "cannot store a 'shared Node' in variable 'a'"), 1u)
      << r.Diagnostics;
  EXPECT_EQ(count(r.Diagnostics, "cannot copy 'b.get()'"), 1u) << r.Diagnostics;
}
