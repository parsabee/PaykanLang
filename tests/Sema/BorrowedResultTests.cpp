// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: borrowed results (`-> view T`).  A function returns a borrow
// of what it was given as one, and its caller sees the result as a `view`.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

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

// `main` returns a copy, an `inout` result is not supported yet, and an
// override returns its result the way the method it overrides does.
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
fn bump(n: inout int) -> inout int { return n; }
fn main() -> view int { return 0; }
)");
  expectErrors(
      r, {":9:3: error: override of 'get' must return 'view', like the method "
          "it overrides",
          ":10:3: error: override of 'copy' cannot return a borrow: the "
          "method it overrides returns a copy",
          ":12:1: error: an 'inout' result is not supported yet: 'bump' can "
          "return a 'view'",
          ":13:1: error: 'main' cannot return a borrow"});
  EXPECT_EQ(r.ErrorCount, 4u) << r.Diagnostics;
}
