// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: borrowed results (`-> view T`).  A function returns a borrow
// of what it was given as one, and its caller sees the result as a `view`.

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
  auto r = semaCheck(R"(class Counter { n: int; fn __init__() { self.n = 0; } }
class Box {
  c: Counter; xs: int[];
  fn __init__() { self.c = Counter(); self.xs = [1]; }
  view fn peek() -> view Counter { return self.c; }
  view fn whole() -> view Box { return self; }
  view fn first() -> view int { return self.xs[0]; }
  fn fresh() -> view Counter { return Counter(); }
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
       ":15:51: error: 'd' is a copy: a 'view' result must be part of 'self' "
       "or of a 'view' or 'inout' parameter",
       ":18:10: error: 't' is a local: a 'view' result must be part of "
       "'self' or of a 'view' or 'inout' parameter"});
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
}

// A borrowed result is a `view`: it is read, passed on to `view`
// parameters or bound to a `view` local, never changed or stored.  A value
// type can still be copied out.
TEST(BorrowedResult, TheResultIsAView) {
  auto r = semaCheck(R"(class Counter {
  n: int;
  fn __init__() { self.n = 0; }
  fn tick() { self.n = self.n + 1; }
  view fn get() -> int { return self.n; }
}
class Box {
  c: Counter; k: int;
  fn __init__() { self.c = Counter(); self.k = 3; }
  view fn peek() -> view Counter { return self.c; }
  view fn num() -> view int { return self.k; }
}
fn keep(c: Counter) { }
fn show(c: view Counter) { println(Str(c.get())); }
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
      r, {":18:3: error: the result of 'peek' is a 'view'; 'tick' is not a "
          "'view fn'",
          ":19:3: error: the result of 'peek' is a 'view'; cannot assign to "
          "its field 'n'",
          ":20:8: error: the result of 'peek' is a 'view'; it can only be "
          "passed to a 'view' parameter, and parameter 1 of 'keep' is not one",
          ":21:7: error: the result of 'peek' is a 'view'; it cannot be "
          "stored, only read or passed to a 'view' parameter",
          ":25:16: error: the result of 'num' is a 'view'; it can only be "
          "passed to a 'view' parameter, and parameter 1 of 'twice' is not "
          "one"});
  EXPECT_EQ(r.ErrorCount, 5u) << r.Diagnostics;
}

// `main` returns a copy, and an override returns its result the way the
// method it overrides does.
TEST(BorrowedResult, Declarations) {
  auto r = semaCheck(R"(class Base {
  n: int;
  fn __init__() { self.n = 0; }
  view fn get() -> view int { return self.n; }
  view fn copy() -> int { return self.n; }
}
class Sub : Base {
  fn __init__() { __super__(); }
  view fn get() -> int { return 1; }
  view fn copy() -> view int { return self.n; }
}
fn main() -> view int { return 0; }
)");
  expectErrors(
      r, {":9:3: error: override of 'get' must return 'view', like the method "
          "it overrides",
          ":10:3: error: override of 'copy' cannot return a borrow: the "
          "method it overrides returns a copy",
          ":12:1: error: 'main' cannot return a borrow"});
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
}

// A `view` local of a borrowed result views what the call borrowed (its
// receiver and the arguments to its `view` / `inout` parameters) until the
// local's last use, and so does a `match` arm's `n: view T` of one.
TEST(BorrowedResult, CallSiteBorrows) {
  auto r = semaCheck(R"(class Counter {
  n: int;
  fn __init__() { self.n = 0; }
  fn tick() { self.n = self.n + 1; }
  view fn get() -> int { return self.n; }
}
class Box {
  c: Counter;
  fn __init__() { self.c = Counter(); }
  view fn peek() -> view Counter { return self.c; }
}
fn longer(a: view Str, b: view Str) -> view Str {
  return if a.len() > b.len() then a else b;
}
fn main() -> int {
  b = Box();
  v: view = b.peek();
  b.c = Counter();
  println(Str(v.get()));
  b.c = Counter();
  s = "hello";
  t = "hi";
  w: view = longer(s, t);
  t = "bye";
  println(w);
  match b.peek() {
    d: view Counter { b.c.tick(); println(Str(d.get())); }
    _ { }
  }
  return 0;
}
)");
  expectErrors(
      r, {":18:3: error: 'b' is viewed by 'view' local 'v' until 'v' is last "
          "used; cannot assign to its field 'c'",
          ":24:3: error: 't' is viewed by 'view' local 'w' until 'w' is last "
          "used; it cannot be assigned",
          ":27:23: error: 'b' is viewed by 'view' local 'd' until 'd' is last "
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
              "  fn __init__() { self.n = 0; }\n"
              "  fn tick() { self.n = self.n + 1; } }\n"
              "class Box { c: Counter;\n"
              "  fn __init__() { self.c = Counter(); }\n"
              "  view fn peek() -> view Counter { return self.c; } }\n"
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
  auto r =
      semaCheck(R"(class Animal { n: int; fn __init__(n: int) { self.n = n; } }
class Dog : Animal { fn __init__(n: int) { __super__(n); } }
class Box {
  a: Animal; d: Dog; xs: int[];
  fn __init__() { self.a = Dog(1); self.d = Dog(2); self.xs = [1]; }
  fn me() -> inout Box { return self; }
  fn wrong() -> inout Animal { return self.d; }
  fn elem() -> inout int { return self.xs[0]; }
  fn fine() -> inout Animal { return self.a; }
  fn num() -> inout int { return self.a.n; }
}
class Look { a: Animal; fn __init__() { self.a = Dog(1); } view fn look() -> inout Animal { return self.a; } }
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
      {":6:33: error: 'self' cannot be returned 'inout': the method would no "
       "longer know its object",
       ":7:39: error: an 'inout' result has type 'Animal', but what it "
       "returns has type 'Dog'",
       ":8:35: error: an array element cannot be returned 'inout' yet",
       ":12:60: error: 'view fn look' cannot return 'inout': it does not "
       "change 'self'",
       ":13:51: error: 'v' is a 'view' parameter; it cannot be returned "
       "'inout'",
       ":14:46: error: 'c' is a copy: an 'inout' result must be part of 'self' "
       "or of an 'inout' parameter",
       ":17:3: error: the receiver of 'fine' must be a variable or a field: "
       "its 'inout' result is part of it",
       ":20:3: error: 'b' is borrowed by 'inout' local 'x' until 'x' is last "
       "used",
       ":22:18: error: 'b' is passed to two 'inout' parameters ('a' and "
       "'b')"});
  EXPECT_EQ(r.ErrorCount, 9u) << r.Diagnostics;
}
