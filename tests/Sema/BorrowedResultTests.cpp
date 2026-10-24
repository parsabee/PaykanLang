// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: borrowed results (`-> view T`, `-> inout T`).  A function
// returns a borrow of what it was given as one; its caller sees a `view`
// result as a `view`, and an `inout` one points into what it keeps alive.

#include "TestUtils.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace paykan::test;

namespace {

/// Write @p content to @p dir / @p name; its path.
std::string writeModule(const std::filesystem::path &dir,
                        const std::string &name, const std::string &content) {
  std::filesystem::create_directories(dir);
  std::ofstream(dir / name) << content;
  return (dir / name).string();
}

/// Sema over the file @p path, importing from its directory.
SemaResult semaCheckPath(const std::string &path) {
  paykan::parser::ParserDriver drv(testFrontend());
  if (drv.parseFile(path) != 0)
    return {false, "parse error", 1};
  std::ostringstream os;
  paykan::sema::DiagEngine diag(os);
  diag.setSourceInfo(drv.getCurrentFile(), &drv.getSourceLines());
  paykan::sema::Sema sema(drv.getASTContext(), diag,
                          std::filesystem::path(path).parent_path().string(),
                          drv.getFrontendName());
  auto ctx = sema.run(drv.getRoot());
  return {ctx.Ok, os.str(), ctx.ErrorCount};
}

/// Expect @p r to have failed with each of @p diags (`line:col: error: ...`).
void expectErrors(const SemaResult &r,
                  std::initializer_list<const char *> diags) {
  EXPECT_FALSE(r.Ok);
  for (const char *diag : diags)
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
}

} // namespace

// A `view` result is part of `self` or of a `view` / `inout` parameter, or
// a borrow another call returns of those; a new value, a copy parameter or
// a local is an error.
TEST(BorrowedResult, WhatCanBeReturned) {
  auto r = semaCheck(R"(class Counter { n: int; }
impl Counter { fn __init__() { self.n = 0; } }
class Box {
  c: Counter; xs: int[1];
  view fn peek() -> view Counter { return self.c; }
  view fn whole() -> view Box { return self; }
  view fn first() -> view int { return self.xs[0]; }
  fn fresh() -> view Counter { return Counter(); }
}

impl Box {
  fn __init__() { self.c = Counter(); self.xs = [1]; }
}
fn longer(a: view Str, b: view Str) -> view Str {
  return if a.len() > b.len() then a else b;
}
fn pass(a: view Str, b: view Str) -> view Str { return longer(b, a); }
fn field(c: inout Counter) -> view int { return c.n; }
fn copy(a: view Str, d: Str) -> view Str { return longer(a, d); }
fn local(a: view Str) -> view Str {
  t = a + "!";
  return t;
}
fn main() -> int { return 0; }
)");
  expectErrors(
      r,
      {":8:39: error: a 'view' result must be part of 'self' or of a 'view' "
       "or 'inout' parameter, not a new value",
       ":19:51: error: 'd' is a copy: a 'view' result must be part of 'self' "
       "or of a 'view' or 'inout' parameter",
       ":22:10: error: 't' is a local: a 'view' result must be part of "
       "'self' or of a 'view' or 'inout' parameter"});
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
}

// A borrowed result is a `view`: it is read, passed on to `view`
// parameters or bound to a `view` local, never changed or stored.  A value
// type can still be copied out.
TEST(BorrowedResult, TheResultIsAView) {
  auto r = semaCheck(R"(import ::io; class Counter {
  n: int;
  fn tick() { self.n = self.n + 1; }
  view fn get() -> int { return self.n; }
}

impl Counter {
  fn __init__() { self.n = 0; }
}
class Box {
  c: Counter; k: int;
  view fn peek() -> view Counter { return self.c; }
  view fn num() -> view int { return self.k; }
}

impl Box {
  fn __init__() { self.c = Counter(); self.k = 3; }
}
fn keep(c: Counter) { }
fn show(c: view Counter) { io::println(Str(c.get())); }
fn twice(n: int) -> int { return 2 * n; }
fn main() -> int {
  b = Box();
  b.peek().tick();
  b.peek().n = 2;
  keep(b.peek());
  x = b.peek();
  show(b.peek());
  v: view = b.peek();
  k = b.num() + v.get();
  return twice(b.num()) + k;
}
)");
  expectErrors(
      r, {":24:3: error: the result of 'peek' is a 'view'; 'tick' is not a "
          "'view fn'",
          ":25:3: error: the result of 'peek' is a 'view'; cannot assign to "
          "its field 'n'",
          ":26:8: error: the result of 'peek' is a 'view'; it can only be "
          "passed to a 'view' parameter, and parameter 1 of 'keep' is not one",
          ":27:7: error: the result of 'peek' is a 'view'; it cannot be "
          "stored, only read or passed to a 'view' parameter"});
  // `twice(b.num())`: a `view int` result is passed on as a copy.
  EXPECT_EQ(r.Diagnostics.find(":31:"), std::string::npos) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 4u) << r.Diagnostics;
}

// `main` returns a copy, and an override returns its result the way the
// method it overrides does.
TEST(BorrowedResult, Declarations) {
  auto r = semaCheck(R"(class Base {
  n: int;
  view fn get() -> view int { return self.n; }
  view fn copy() -> int { return self.n; }
}

impl Base {
  fn __init__() { self.n = 0; }
}
class Sub : Base {
  view fn get() -> int { return 1; }
  view fn copy() -> view int { return self.n; }
}

impl Sub {
  fn __init__() { __super__(); }
}
fn main() -> view int { return 0; }
)");
  expectErrors(
      r, {":11:3: error: override of 'get' must return 'view', like the method "
          "it overrides",
          ":12:3: error: override of 'copy' cannot return a borrow: the "
          "method it overrides returns a copy",
          ":18:1: error: 'main' cannot return a borrow"});
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
}

// In a `view fn`, part of `self` that shares what it holds is a `view`, so
// it is returned as one, `-> view T`; a value type is copied.  Another
// method returns part of `self` for its caller to change, so it is not one
// that never changes `self`.
TEST(BorrowedResult, ViewFnReturnsAView) {
  auto r = semaCheck(R"(class Counter { n: int; }
impl Counter { fn __init__() { self.n = 0; } }
class Box<T> {
  v: T;
  view fn get() -> T { return self.v; }
}

impl Box<T> {
  fn __init__(v: T) { self.v = v; }
}
class Named {
  name: Str; c: Counter; n: int;
  view fn getName() -> Str { return self.name; }
  view fn pick(f: bool) -> Counter { return if f then self.c else Counter(); }
  view fn me() -> Named { return self; }
  view fn toString() -> Str { return self.name; }
  view fn peek() -> view Counter { return self.c; }
  view fn num() -> int { return self.n; }
  view fn label() -> Str { return self.name + "!"; }
  fn grab() -> Counter { return self.c; }
}

impl Named {
  fn __init__() { self.name = "a"; self.c = Counter(); self.n = 1; }
}
fn main() -> int {
  b = Box<Str>("s");
  i = Box<int>(1);
  return i.get();
}
)");
  expectErrors(
      r, {":5:31: error: 'self' is read-only in 'view fn get', so part of it "
          "can only be returned as a 'view': write 'view' before its result "
          "type",
          ":13:37: error: 'self' is read-only in 'view fn getName', so part "
          "of it can only be returned as a 'view': write '-> view Str'",
          ":14:55: error: 'self' is read-only in 'view fn pick', so part of "
          "it can only be returned as a 'view': write '-> view Counter'",
          ":15:34: error: 'self' is read-only in 'view fn me', so part of it "
          "can only be returned as a 'view': write '-> view Named'",
          ":16:38: error: 'self' is read-only in 'view fn toString', so part "
          "of it can only be returned as a 'view'; the method it overrides "
          "returns a copy, so return a new value"});
  EXPECT_EQ(r.ErrorCount, 5u) << r.Diagnostics;

  r = semaCheck(R"(class Counter { n: int; }
impl Counter { fn __init__() { self.n = 0; } }
class Box {
  c: Counter; n: int;
  fn grab() -> Counter { return self.c; }
  fn num() -> int { return self.n; }
}

impl Box {
  fn __init__() { self.c = Counter(); self.n = 1; }
}
fn main() -> int { return 0; }
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("'grab' never changes"), std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(":6:3: warning: 'num' never changes 'self'"),
            std::string::npos)
      << r.Diagnostics;
}

// A `view` local of a borrowed result views what the call borrowed (its
// receiver and the arguments to its `view` / `inout` parameters) until the
// local's last use, and so does a `match` arm's `n: view T` of one.
TEST(BorrowedResult, CallSiteBorrows) {
  auto r = semaCheck(R"(import ::io; class Counter {
  n: int;
  fn tick() { self.n = self.n + 1; }
  view fn get() -> int { return self.n; }
}

impl Counter {
  fn __init__() { self.n = 0; }
}
class Box {
  c: Counter;
  view fn peek() -> view Counter { return self.c; }
}

impl Box {
  fn __init__() { self.c = Counter(); }
}
fn longer(a: view Str, b: view Str) -> view Str {
  return if a.len() > b.len() then a else b;
}
fn main() -> int {
  b = Box();
  v: view = b.peek();
  b.c = Counter();
  io::println(Str(v.get()));
  b.c = Counter();
  s = "hello";
  t = "hi";
  w: view = longer(s, t);
  t = "bye";
  io::println(w);
  match b.peek() {
    d: view Counter { b.c.tick(); io::println(Str(d.get())); }
    _ { }
  }
  return 0;
}
)");
  expectErrors(
      r, {":24:3: error: 'b' is viewed by 'view' local 'v' until 'v' is last "
          "used; cannot assign to its field 'c'",
          ":30:3: error: 't' is viewed by 'view' local 'w' until 'w' is last "
          "used; it cannot be assigned",
          ":33:23: error: 'b' is viewed by 'view' local 'd' until 'd' is last "
          "used; 'tick' is not a 'view fn'"});
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
  // After v's last use, b can change.
  EXPECT_EQ(r.Diagnostics.find(":20:"), std::string::npos) << r.Diagnostics;
}

// A module's borrowed results are part of its interface: an importer sees
// them as `view`s.
TEST(BorrowedResult, AcrossModules) {
  auto dir = tempDir() / "borrowed_result_modules";
  std::filesystem::remove_all(dir);
  writeModule(dir, "base.pkn",
              "class Counter { n: int;\n"
              "  fn tick() { self.n = self.n + 1; } }\n"
              "impl Counter { fn __init__() { self.n = 0; } }\n"
              "class Box { c: Counter;\n"
              "  view fn peek() -> view Counter { return self.c; } }\n"
              "impl Box { fn __init__() { self.c = Counter(); } }\n"
              "fn inner(b: view Box) -> view Counter { return b.c; }\n");
  auto r = semaCheckPath(writeModule(dir, "main.pkn", R"(import base;
fn main() -> int {
  b = base::Box();
  b.peek().tick();
  base::inner(b).tick();
  return 0;
})"));
  expectErrors(r, {":4:3: error: the result of 'peek' is a 'view'; 'tick' is "
                   "not a 'view fn'",
                   ":5:3: error: the result of 'base::inner' is a 'view'; "
                   "'tick' is not a 'view fn'"});
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
  std::filesystem::remove_all(dir);
}

// An `inout` result names the storage of part of `self` or of an `inout`
// parameter, as an `inout` argument would (not `self` itself, an element
// yet, a `view` or a copy), of exactly the result's type.  A `view fn`
// returns none.  The caller's receiver must outlive the call, and while a
// borrow of the result is live, what it borrowed cannot be used.
TEST(BorrowedResult, InoutResults) {
  auto r = semaCheck(R"(class Animal { n: int; }
impl Animal { fn __init__(n: int) { self.n = n; } }
class Dog : Animal {}
impl Dog { fn __init__(n: int) { __super__(n); } }
class Box {
  a: Animal; d: Dog; xs: int[1]; k: int;
  fn me() -> inout Box { return self; }
  fn wrong() -> inout Animal { return self.d; }
  fn elem() -> inout int { return self.xs[0]; }
  fn fine() -> inout Animal { return self.a; }
  fn num() -> inout int { return self.k; }
}

impl Box {
  fn __init__() { self.a = Dog(1); self.d = Dog(2); self.xs = [1]; self.k = 0; }
}
class Look { a: Animal; view fn look() -> inout Animal { return self.a; } }
impl Look { fn __init__() { self.a = Dog(1); } }
fn fromView(v: view Animal) -> inout int { return v.n; }
fn fromCopy(c: Animal) -> inout int { return c.n; }
fn twice(a: inout int, b: inout int) { }
fn main() -> int {
  Box().fine();
  b = Box();
  x: inout = b.fine();
  b.d = Dog(5);
  x.n = 2;
  twice(b.num(), b.num());
  return 0;
}
)");
  expectErrors(
      r,
      {":7:33: error: 'self' cannot be returned 'inout': the method would no "
       "longer know its object",
       ":8:39: error: an 'inout' result has type 'Animal', but what it "
       "returns has type 'Dog'",
       ":9:35: error: an array element cannot be returned 'inout' yet",
       ":17:25: error: 'view fn look' cannot return 'inout': it does not "
       "change 'self'",
       ":19:51: error: 'v' is a 'view' parameter; it cannot be returned "
       "'inout'",
       ":20:46: error: 'c' is a copy: an 'inout' result must be part of 'self' "
       "or of an 'inout' parameter",
       ":23:3: error: the receiver of 'fine' must be a variable or a field: "
       "its 'inout' result is part of it",
       ":26:3: error: 'b' is borrowed by 'inout' local 'x' until 'x' is last "
       "used",
       ":28:18: error: 'b' is passed to two 'inout' parameters ('a' and "
       "'b')"});
  EXPECT_EQ(r.ErrorCount, 9u) << r.Diagnostics;
}

// An `inout` result points into storage its caller keeps alive: a class's
// `self` is the object the caller holds, so a reference reached through one
// of its fields leads to a separate object, which the caller does not keep
// (`self.c.k`, `self.c.d.x`, an element's object, a tuple's), in any
// branch, arm, loop or expression body.  A call's `inout` result can point
// into what it borrowed and one object past it: into `self.c`'s object for
// `self.c.slot()`, `pick(self.c)` and `self.c.dslot()`, into an object
// `self.s` holds for a struct's method.
TEST(BorrowedResult, InoutResultsStayInWhatTheCallerKeepsAlive) {
  auto r = semaCheck(
      R"(class D { x: int; fn xslot() -> inout int { return self.x; } }
class C {
  k: Str; d: D;
  fn slot() -> inout Str { return self.k; }
  fn dslot() -> inout D { return self.d; }
}
struct S { c: C; n: int; }
impl S { fn slot() -> inout Str { return self.c.k; } }
class H {
  c: C; k: Str; s: S; t: (C, int); arr: C[2];
  fn field() -> inout Str { return self.c.k; }
  fn deeper() -> inout int { return self.c.d.x; }
  fn element() -> inout Str { return self.arr[0].k; }
  fn inTuple() -> inout Str { return self.t.0.k; }
  fn either(f: bool) -> inout Str { return if f then self.k else self.c.k; }
  fn short() -> inout Str => self.c.k;
  fn branches(n: int) -> inout Str {
    if n == 0 { return self.k; } else { return self.c.k; }
  }
  fn arms(n: int) -> inout Str {
    match n {
      0 { return self.k; }
      _ { return self.c.k; }
    }
  }
  fn loop(n: int) -> inout Str {
    while n > 0 { return self.c.k; }
    return self.k;
  }
  fn chained() -> inout Str { return self.c.slot(); }
  fn viaStruct() -> inout Str { return self.s.slot(); }
  fn viaSlot() -> inout int { return self.c.dslot().x; }
  fn viaArg() -> inout Str { return pick(self.c); }
  fn slotOfC() -> inout C { return self.c; }
  fn viaOwnSlot() -> inout Str { return self.slotOfC().slot(); }
}
fn pick(c: inout C) -> inout Str { return c.k; }
fn main() -> int { return 0; }
)");
  const std::string rule =
      ": an 'inout' result can only point into 'self' or into an 'inout' "
      "parameter's storage; return a copy, or call an accessor on '";
  auto into = [&](const char *at, const char *fn, const char *place) {
    return std::string(at) + " error: '" + fn +
           "' returns an 'inout' reference into '" + place +
           "', a separate object" + rule + place + "' directly";
  };
  auto via = [&](const char *at, const char *fn, const char *callee,
                 const char *place) {
    return std::string(at) + " error: '" + fn + "' returns the 'inout' " +
           "result of '" + callee + "', which can point into '" + place +
           "', a separate object" + rule + place + "' directly";
  };
  for (const std::string &diag : {
           into(":11:36:", "field", "self.c"),
           into(":12:37:", "deeper", "self.c"),
           into(":13:38:", "element", "self.arr[0]"),
           into(":14:38:", "inTuple", "self.t.0"),
           into(":15:44:", "either", "self.c"),
           into(":16:30:", "short", "self.c"),
           into(":18:48:", "branches", "self.c"),
           into(":23:18:", "arms", "self.c"),
           into(":27:26:", "loop", "self.c"),
           via(":30:38:", "chained", "slot", "self.c"),
           std::string(":31:40: error: 'viaStruct' returns the 'inout' result "
                       "of 'slot', which can point into an object 'self.s' "
                       "holds") +
               rule + "self.s' directly",
           via(":32:38:", "viaSlot", "dslot", "self.c"),
           via(":33:37:", "viaArg", "pick", "self.c"),
           via(":35:41:", "viaOwnSlot", "slot", "self.slotOfC()"),
       })
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 14u) << r.Diagnostics;
}

// What an `inout` parameter names stays alive with the object a reference
// held there refers to, and so does a struct's `self`: one object further,
// not two (`n.c.k`, `n.arr[1].k`, `s.c.d.x`, `self.c.d.x`, a call's result
// in `n.c`).  A slice's caller keeps no object in the array alive.
TEST(BorrowedResult, InoutResultsOfParametersReachOneObject) {
  auto r = semaCheck(R"(class D { x: int; }
class C { k: Str; d: D; fn dslot() -> inout D { return self.d; } }
struct S { c: C; n: int; }
impl S {
  fn far() -> inout int { return self.c.d.x; }
  fn viaSlot() -> inout int { return self.c.dslot().x; }
}
class Node { k: Str; c: C; arr: C[2]; }
fn field(n: inout Node) -> inout Str { return n.c.k; }
fn element(n: inout Node) -> inout Str { return n.arr[1].k; }
fn inStruct(s: inout S) -> inout int { return s.c.d.x; }
fn viaSlot(n: inout Node) -> inout int { return n.c.dslot().x; }
fn viaArg(n: inout Node) -> inout Str { return pick(n.c); }
fn sliced(a: inout C[], i: int) -> inout Str { return a[i].k; }
fn pick(c: inout C) -> inout Str { return c.k; }
fn main() -> int { return 0; }
)");
  const std::string rule =
      ": an 'inout' result can only point into 'self' or into an 'inout' "
      "parameter's storage";
  auto fix = [](const char *place) {
    return std::string("; return a copy, or call an accessor on '") + place +
           "' directly";
  };
  auto into = [&](const char *at, const char *fn, const char *place) {
    return std::string(at) + " error: '" + fn +
           "' returns an 'inout' reference into '" + place +
           "', a separate object" + rule + fix(place);
  };
  auto via = [&](const char *at, const char *fn, const char *callee,
                 const char *place) {
    return std::string(at) + " error: '" + fn + "' returns the 'inout' " +
           "result of '" + callee + "', which can point into '" + place +
           "', a separate object" + rule + fix(place);
  };
  for (const std::string &diag : {
           into(":5:34:", "far", "self.c.d"),
           into(":6:38:", "viaSlot", "self.c.dslot()"),
           into(":9:47:", "field", "n.c"),
           into(":10:49:", "element", "n.arr[1]"),
           into(":11:47:", "inStruct", "s.c.d"),
           via(":12:49:", "viaSlot", "dslot", "n.c"),
           via(":13:48:", "viaArg", "pick", "n.c"),
           std::string(":14:55: error: 'sliced' returns an 'inout' reference "
                       "into an object of the slice 'a'") +
               rule + ", and its caller keeps no object in a slice alive" +
               fix("a[i]"),
       })
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 8u) << r.Diagnostics;
}

// Everything else stays: part of `self` (a field, a struct's, a tuple's, a
// class-typed field's slot), what an `inout` parameter names and the object
// a reference held there refers to (a class's, one in a struct, a tuple or
// a fixed-size array), and a call's result in those (`self.key()`,
// `self.p.xslot()` with `p` holding no object, `pick(n)`, `s.slot()`).
TEST(BorrowedResult, InoutResultsInSelfOrAnInoutParameter) {
  auto r = semaCheck(R"(class D { x: int; }
class C { k: Str; d: D; fn slot() -> inout Str { return self.k; } }
struct P { x: int; v: Str; }
impl P { fn xslot() -> inout int { return self.x; } }
struct S { c: C; n: int; p: P; }
impl S {
  fn own() -> inout int { return self.n; }
  fn inner() -> inout int { return self.p.x; }
  fn slot() -> inout Str { return self.c.k; }
  fn viaC() -> inout Str { return self.c.slot(); }
  fn ref() -> inout C { return self.c; }
  fn viaOwn() -> inout int { return self.own(); }
}
class H {
  c: C; k: Str; p: P; t: (Str, int);
  fn key() -> inout Str { return self.k; }
  fn slotOfC() -> inout C { return self.c; }
  fn inner() -> inout int { return self.p.x; }
  fn whole() -> inout P { return self.p; }
  fn tuple() -> inout (Str, int) { return self.t; }
  fn viaKey() -> inout Str { return self.key(); }
  fn viaP() -> inout int { return self.p.xslot(); }
  fn either(f: bool) -> inout Str { return if f then self.k else self.key(); }
  fn short() -> inout Str => self.k;
}
class Node { k: Str; c: C; p: P; }
fn whole(n: inout Node) -> inout Node { return n; }
fn field(n: inout Node) -> inout Str { return n.k; }
fn slot(n: inout Node) -> inout C { return n.c; }
fn inner(n: inout Node) -> inout int { return n.p.x; }
fn method(n: inout C) -> inout Str { return n.slot(); }
fn viaPick(n: inout C) -> inout Str { return pick(n); }
fn inStruct(s: inout S) -> inout Str { return s.c.k; }
fn viaStruct(s: inout S) -> inout Str { return s.slot(); }
fn viaStructC(s: inout S) -> inout Str { return s.viaC(); }
fn inTuple(t: inout (C, int)) -> inout Str { return t.0.k; }
fn inArray(a: inout C[2]) -> inout Str { return a[1].k; }
fn inStructArray(a: inout S[2]) -> inout Str { return a[0].c.k; }
fn either(a: inout Node, b: inout Node, f: bool) -> inout Str {
  return if f then a.k else b.k;
}
fn pick(c: inout C) -> inout Str { return c.k; }
fn main() -> int { return 0; }
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
}

// An override, an `impl ... for` method and every instantiation of a
// template are checked as any method is: `Wrap<D>` reaches `D`'s object
// through `self.v`, `Wrap<P>` reaches into the struct held in `self`.
TEST(BorrowedResult, InoutResultsOfOverridesTraitsAndTemplates) {
  auto r =
      semaCheck(R"(class C { k: Str; fn slot() -> inout Str { return self.k; } }
class Base { k: Str; fn slot() -> inout Str { return self.k; } }
class Sub : Base { c: C; fn slot() -> inout Str { return self.c.k; } }
trait Slotted { fn slot() -> inout Str; }
class Holder { c: C; }
impl Slotted for Holder { fn slot() -> inout Str { return self.c.k; } }
class Pair<T> { c: C; v: T; fn get() -> inout Str { return self.c.k; } }
class Wrap<T> { v: T; fn x() -> inout int { return self.v.x; } }
class D { x: int; }
struct P { x: int; }
fn deep<T>(n: inout T) -> inout Str { return n.c.k; }
class Node { c: C; }
class Two<T: Slotted> { v: T; fn e() -> inout Str { return self.v.slot(); } }
fn main() -> int {
  p = Pair<int>(C("a"), 1);
  d = Wrap<D>(D(1));
  s = Wrap<P>(P(2));
  n = Node(C("b"));
  x: inout = deep(n);
  t = Two<Holder>(Holder(C("c")));
  return 0;
}
)");
  const std::string rule =
      ": an 'inout' result can only point into 'self' or into an 'inout' "
      "parameter's storage; return a copy, or call an accessor on '";
  auto into = [&](const char *at, const char *fn, const char *place) {
    return std::string(at) + " error: '" + fn +
           "' returns an 'inout' reference into '" + place +
           "', a separate object" + rule + place + "' directly";
  };
  for (const std::string &diag : {
           into(":3:58:", "slot", "self.c"),
           into(":6:59:", "slot", "self.c"),
           into(":7:60:", "get", "self.c"),
           std::string(":15:7: note: in instantiation of 'Pair<int>' "
                       "requested here"),
           into(":8:52:", "x", "self.v"),
           std::string(":16:7: note: in instantiation of 'Wrap<D>' requested "
                       "here"),
           into(":11:46:", "deep<Node>", "n.c"),
           std::string(":19:14: note: in instantiation of 'deep<Node>' "
                       "requested here"),
           std::string(":13:60: error: 'e' returns the 'inout' result of "
                       "'slot', which can point into 'self.v', a separate "
                       "object") +
               rule + "self.v' directly",
           std::string(":20:7: note: in instantiation of 'Two<Holder>' "
                       "requested here"),
       })
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("in instantiation of 'Wrap<P>'"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 6u) << r.Diagnostics;
}

// A static method's `inout` result and a generic function's count as any
// call's, and a generic struct's methods are checked in each instantiation:
// a struct's `self` reaches one object, so `self.v.k` is allowed in
// `SW<Node>`, and `self.v.c.k` in `SX<Q>` for a struct `Q`, but not in
// `SX<Node>`, where `self.v.c` is a second object.
TEST(BorrowedResult, InoutResultsOfStaticGenericAndStructTemplateCalls) {
  auto r = semaCheck(R"(class C { k: Str; }
class Node { c: C; k: Str; }
struct Q { c: C; }
class H {
  c: C;
  fn viaStatic() -> inout Str { return H::pick(self.c); }
  fn viaGeneric() -> inout Str { return gpick<C>(self.c); }
}
impl H { static fn pick(c: inout C) -> inout Str { return c.k; } }
fn gpick<T>(c: inout T) -> inout Str { return c.k; }
struct SW<T> { v: T; }
impl SW<T> { fn one() -> inout Str { return self.v.k; } }
struct SX<T> { v: T; }
impl SX<T> { fn two() -> inout Str { return self.v.c.k; } }
fn main() -> int {
  a = SW<Node>(Node(C("a"), "b"));
  b = SX<Node>(Node(C("a"), "b"));
  c = SX<Q>(Q(C("a")));
  return 0;
}
)");
  const std::string rule =
      ": an 'inout' result can only point into 'self' or into an 'inout' "
      "parameter's storage; return a copy, or call an accessor on '";
  auto via = [&](const char *at, const char *fn, const char *callee) {
    return std::string(at) + " error: '" + fn + "' returns the 'inout' " +
           "result of '" + callee + "', which can point into 'self.c', a " +
           "separate object" + rule + "self.c' directly";
  };
  for (const std::string &diag : {
           via(":6:40:", "viaStatic", "H::pick"),
           via(":7:41:", "viaGeneric", "gpick<C>"),
           std::string(":14:45: error: 'two' returns an 'inout' reference "
                       "into 'self.v.c', a separate object") +
               rule + "self.v.c' directly",
           std::string(":17:7: note: in instantiation of 'SX<Node>' "
                       "requested here"),
       })
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
  for (const char *fine : {"'SW<Node>'", "'SX<Q>'"})
    EXPECT_EQ(r.Diagnostics.find(fine), std::string::npos) << fine << "\n"
                                                           << r.Diagnostics;
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
}

// The rule is checked where the function is defined: an importer of the
// module sees the error in the module's file, and still uses its other
// `inout` results.
TEST(BorrowedResult, InoutResultsAreCheckedInTheirModule) {
  auto dir = tempDir() / "borrowed_inout_result_modules";
  std::filesystem::remove_all(dir);
  writeModule(dir, "base.pkn",
              "class C { k: Str; fn slot() -> inout Str { return self.k; } }\n"
              "class H { c: C; fn e() -> inout Str { return self.c.k; } }\n"
              "fn pick(c: inout C) -> inout Str { return c.k; }\n");
  auto r = semaCheckPath(writeModule(dir, "main.pkn", R"(import base;
fn main() -> int {
  h = base::H(base::C("a"));
  x: inout = base::pick(h.c);
  return 0;
})"));
  expectErrors(r,
               {"base.pkn:2:46: error: 'e' returns an 'inout' reference into "
                "'self.c', a separate object: an 'inout' result can only point "
                "into 'self' or into an 'inout' parameter's storage; return a "
                "copy, or call an accessor on 'self.c' directly"});
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  std::filesystem::remove_all(dir);
}
