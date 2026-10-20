// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: function types and lambdas (docs/language/15-function-types.md)
// -- function types in every type position, named functions and lambdas as
// values, calls through variables, fields and other expressions, captures
// and their rules, the modes of a function type's parameters, and what is
// not a function value.

#include "ParserDriver.h"
#include "Sema.h"
#include "TestUtils.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

void expectAll(const SemaResult &r, std::initializer_list<const char *> diags) {
  for (const char *d : diags)
    EXPECT_NE(r.Diagnostics.find(d), std::string::npos) << d << "\n"
                                                        << r.Diagnostics;
}

std::string writeProjectFile(const std::filesystem::path &dir,
                             const std::string &rel, const std::string &src) {
  auto full = dir / rel;
  std::filesystem::create_directories(full.parent_path());
  std::ofstream ofs(full);
  ofs << src;
  return full.string();
}

SemaResult semaCheckFile(const std::string &path) {
  paykan::parser::ParserDriver driver(paykan::test::testFrontend());
  if (driver.parseFile(path) != 0)
    return {false, "parse error", 1};
  std::ostringstream os;
  paykan::sema::DiagEngine diag(os);
  diag.setSourceInfo(driver.getCurrentFile(), &driver.getSourceLines());
  paykan::sema::Sema sema(driver.getASTContext(), diag,
                          std::filesystem::path(path).parent_path().string(),
                          driver.getFrontendName());
  auto ctx = sema.run(driver.getRoot());
  return {ctx.Ok, os.str(), ctx.ErrorCount};
}

} // namespace

// -- Types and values

TEST(FunctionType, ValuesInEveryPosition) {
  auto r = semaCheck(R"(import ::io;
fn square(x: int) -> int { return x * x; }
fn apply(f: (int) -> int, x: int) -> int { return f(x); }
fn twice(f: (int) -> int) -> (int) -> int { return fn(x: int) -> int { return f(f(x)); }; }
class Button { label: Str; onClick: (Str) -> void; }
struct Op { name: Str; run: (int, int) -> int; }
class Box<T> { v: T; view fn get() -> view T { return self.v; } }
impl Box<T> { fn __init__(v: T) { self.v = v; } }
impl Op { static fn plus() -> (int, int) -> int { return fn(a: int, b: int) -> int { return a + b; }; } }
fn bump(n: inout int) { n = n + 1; }
fn main() -> int {
  n = 10;
  s = "s";
  add = fn(x: int) -> int { return x + n; };
  g = square;
  h = twice(add);
  k: (int) -> int = fn(x: int) -> int { return x; };
  fs: ((int) -> int)[2] = [square, add];
  b = Button("ok", fn(l: Str) { io::println(l + s); });
  b.onClick("clicked ");
  o = Op("add", fn(a: int, b: int) -> int { return a + b; });
  p = Op("plus", Op::plus());
  v = fn(x: view Str, c: inout int) -> bool { c = c + x.len(); return c > 3; };
  m = 0;
  ok = v("abc", m);
  t: ((int) -> int, Str) = (g, "t");
  bx = Box(add);
  w = bx.get()(1);                       // a view of a function value is called
  nested = fn() -> () -> int { return fn() -> int { return n; }; };
  return apply(g, 3) + h(1) + fs[1](2) + o.run(1, 2) + t.0(4) + p.run(1, 1) +
         (if ok then m else 0) + w + nested()() + add(1) + k(2);
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics, "");
}

TEST(FunctionType, StructuralAndCanonical) {
  auto r = semaCheck(R"(
fn id(x: int) -> int { return x; }
fn main() -> int {
  a: (int) -> int = id;
  b: (int) -> int = a;         // the same type, whoever spells it
  c: (view Str) -> int = fn(s: view Str) -> int { return s.len(); };
  d: (Str) -> int = c;         // a mode is part of the type
  e: (float) -> int = a;       // no promotion
  f: () -> void = fn() -> int { return 1; };
  g: (int) -> int = fn(x: int, y: int) -> int { return x; };
  return a(1) + b(2);
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"initializer of type '(view Str) -> int' does not match "
                "declared type '(Str) -> int'",
                "initializer of type '(int) -> int' does not match declared "
                "type '(float) -> int'",
                "initializer of type '() -> int' does not match declared "
                "type '() -> void'",
                "initializer of type '(int, int) -> int' does not match "
                "declared type '(int) -> int'"});
  EXPECT_EQ(r.ErrorCount, 4u) << r.Diagnostics;
}

TEST(FunctionType, WhatIsNotAValue) {
  auto r = semaCheck(R"(
fn first<T>(xs: view T[]) -> view T { return xs[0]; }
fn peek(s: view Str) -> view Str { return s; }
class C { x: int; fn m() -> int { return self.x; } }
fn main() -> int {
  c = C;
  f = first;
  q = peek;
  o = C(1);
  m = o.m;
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'C' is a constructor, not a function value (wrap it: "
                "'fn(...) -> C { return C(...); }')",
                "'first' is a generic function; a generic function cannot be "
                "a function value yet (wrap a call to it in a lambda)",
                "'peek' returns a borrow ('-> view'); it cannot be a function "
                "value yet",
                "no field 'm' in class 'C'"});
}

TEST(FunctionType, CallsAreChecked) {
  auto r = semaCheck(R"(
fn square(x: int) -> int { return x * x; }
fn bump(n: inout int) { n = n + 1; }
class C { cb: (int) -> int; n: int; }
fn main() -> int {
  n = 1;
  n(2);
  s = square;
  s(1, 2);
  s("a");
  v = fn(x: view Str, c: inout int) {};
  k = 0;
  v("a", 1);
  let m = 0;
  v("a", m);
  c = C(square, 1);
  c.n(1);
  c.cb("x");
  o: ((int) -> int)? = None;
  o(1);
  fs: ((int) -> int)[1] = [square];
  fs[0]("a");
  r = fs[0](1).len();
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'n' has type 'int', which is not a function type; it cannot "
                "be called",
                "'s' has type '(int) -> int': it takes 1 argument(s), got 2",
                "argument 1 of 's' has type 'Str', expected 'int'",
                "argument 2 of 'v' must be a variable or a field: parameter "
                "'2' is 'inout'",
                "'m' is declared with 'let' and cannot be passed to 'inout' "
                "parameter '2'",
                "no method 'n' on type 'C' (its field 'n' has type 'int', "
                "which is not a function type)",
                "argument 1 of 'cb' has type 'Str', expected 'int'",
                "argument 1 of '(int) -> int' has type 'Str', expected 'int'",
                "method call on non-class type 'int'"});
  EXPECT_NE(r.Diagnostics.find("cannot use optional '((int) -> int)?' as "
                               "'(int) -> int' without unwrapping (use match)"),
            std::string::npos)
      << r.Diagnostics;
}

// -- Captures

TEST(FunctionType, CapturesAreReadOnlyCopies) {
  auto r = semaCheck(R"(
struct P { x: int; }
class Node { n: int; }
struct W { node: Node; x: int; }
fn bump(n: inout int) { n = n + 1; }
fn main() -> int {
  n = 1;
  p = P(1);
  node = Node(1);
  xs: int[2] = [1, 2];
  w = fn() { n = 2; };
  u = fn() { bump(n); };
  q = fn() { p.x = 2; };
  a = fn() { xs[0] = 2; };
  w = W(Node(1), 0);
  ok = fn() -> int {
    node.n = 2;
    w.node.n = w.node.n + 7;           // the object the copy holds: shared
    return n + p.x + xs[1] + node.n + w.node.n;
  };
  return ok();
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"cannot assign to 'n': the function captures it by copy, and "
                "the copy is read-only (assign it before the function is "
                "made, or keep it in an object)",
                "'n' is captured by copy by the function, and the copy is "
                "read-only; it cannot be passed to 'inout' parameter 'n'",
                "'p' is captured by copy by the function; cannot assign to "
                "its field 'x'",
                "'xs' is captured by copy by the function; cannot assign to "
                "its elements"});
  EXPECT_EQ(r.ErrorCount, 4u) << r.Diagnostics;
}

TEST(FunctionType, BorrowsAndSelfCannotBeCaptured) {
  auto r = semaCheck(R"(
struct P { x: int; }
class C {
  x: int;
  view fn bad() -> () -> int { return fn() -> int { return self.x; }; }
  fn good() -> () -> int { return fn() -> int { return self.x; }; }
  fn change() -> () -> void { return fn() { self.x = self.x + 1; }; }
}
impl P { fn m() -> () -> int { return fn() -> int { return self.x; }; } }
fn f(v: view Str, k: inout int, xs: view int[]) -> () -> int {
  loc: view = v;
  g = fn() -> int { return v.len(); };
  h = fn() -> int { return k; };
  i = fn() -> int { return xs.len(); };
  j = fn() -> int { return loc.len(); };
  return g;
}
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'self' is read-only in 'view fn bad'; a function cannot "
                "capture it (its copy could change the object)",
                "'self' is the receiver's storage in a method of struct 'P'; "
                "a function cannot capture it (copy it into a variable first)",
                "'v' is a 'view' parameter; a function cannot capture a "
                "borrow (it may outlive the call that holds it)",
                "'k' is an 'inout' parameter or local; a function cannot "
                "capture a borrow",
                "'xs' is a 'view' parameter; a function cannot capture a "
                "borrow",
                "'loc' is a 'view' local; a function cannot capture a borrow"});
  EXPECT_EQ(r.ErrorCount, 6u) << r.Diagnostics;
  // A method whose `self` a closure captures may change it: no `view fn`
  // suggestion for `good`.
  EXPECT_EQ(r.Diagnostics.find("never changes 'self'"), std::string::npos)
      << r.Diagnostics;
}

TEST(FunctionType, ViewsOfFunctionValuesAreCalledNotStored) {
  // A closure is a reference, like an object: a `view` of one (a `view`
  // parameter, a `view fn`'s result) is called or passed on, not stored.
  auto r = semaCheck(R"(
class Box<T> { v: T; view fn get() -> view T { return self.v; } }
fn apply(f: view (int) -> int, x: int) -> int { return f(x); }
fn keep(f: view (int) -> int) -> (int) -> int {
  g = f;
  return f;
}
fn main() -> int {
  b = Box(fn(x: int) -> int { return x; });
  ok = b.get()(1) + apply(b.get(), 2);
  h: (int) -> int = b.get();
  return ok;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'f' is a 'view' parameter; it cannot be stored, only read "
                "or passed to a 'view' parameter",
                "'f' is a 'view' parameter; it cannot be returned",
                "the result of 'get' is a 'view'; it cannot be stored, only "
                "read or passed to a 'view' parameter"});
  EXPECT_EQ(r.ErrorCount, 3u);
}

TEST(FunctionType, LambdaBodiesAreFunctionBodies) {
  auto r = semaCheck(R"(
fn main() -> int {
  i = 0;
  while i < 3 {
    f = fn() -> int { break; };
    g = fn() -> int { if i > 1 { return 1; } };
    h = fn(x: int, x: int) -> int { return x; };
    k = fn(v: void) {};
    l = fn() { return 1; };
    m = fn() -> int { return; };
    i = i + 1;
  }
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'break' outside of a loop",
                "the lambda does not always return a value (its result type",
                "duplicate parameter 'x' in the lambda",
                "parameter 'v' cannot have type 'void'",
                "non-void function must return a value"});
}

TEST(FunctionType, NestedLambdasCaptureThroughTheOuterOne) {
  auto r = semaCheck(R"(
fn main() -> int {
  n = 1;
  s = "s";
  outer = fn(k: int) -> () -> int {
    inner = fn() -> int { return n + k + s.len(); };
    return inner;
  };
  g = outer(2);
  return g();
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(FunctionType, NotAnObjectNorShowNorHashable) {
  auto r = semaCheck(R"(import ::io;
fn show<T: Show>(x: view T) -> Str { return x.toString(); }
fn hashOf<T: Hashable>(x: view T) -> int { return x.hash(); }
fn main() -> int {
  f = fn() -> int { return 1; };
  g = f;
  io::println(f);
  o: Obj = f;
  s = f.toString();
  a = show(f);
  b = hashOf(f);
  return if f == g && f != f then 1 else 0;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'() -> int' does not implement trait 'Show'",
                "which 'io::println' requires of 'T'",
                "initializer of type '() -> int' does not match declared type",
                "method call on non-class type '() -> int'",
                "does not implement trait 'Show'",
                "does not implement trait 'Hashable'"});
}

TEST(FunctionType, OptionalFunctionsUnwrapWithMatch) {
  auto r = semaCheck(R"(import ::io;
class Button { onClick: ((Str) -> void)?; }
fn main() -> int {
  b = Button(None);
  b.onClick = fn(s: Str) { io::println(s); };
  match b.onClick {
    f: (Str) -> void { f("hi"); }
    None { }
  }
  match b.onClick {
    f: (int) -> void { }
    None { }
  }
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"match arm type '(int) -> void' does not match the optional "
                "subject type '((Str) -> void)?'"});
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
}

TEST(FunctionType, GenericsTakeFunctionTypes) {
  auto r = semaCheck(R"(
fn mapAll<T, U>(xs: view T[], f: (view T) -> U) -> U {
  return f(xs[0]);
}
class Handler<T> { on: (T) -> T; }
impl Handler<T> { fn __init__(on: (T) -> T) { self.on = on; } view fn run(x: T) -> T { return self.on(x); } }
class Box<T> { v: T; view fn same(o: view T) -> bool { return self.v == o; } }
impl Box<T> { fn __init__(v: T) { self.v = v; } }
fn main() -> int {
  n = mapAll([1, 2], fn(x: view int) -> int { return x + 1; });
  h = Handler(fn(s: Str) -> Str { return s + "!"; });
  k = fn(x: int) -> int { return x; };
  b = Box(k);
  return n + h.run("a").len() + k(1) + (if b.same(k) then 1 else 0);
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Modules

TEST(FunctionType, AcrossModules) {
  auto dir = paykan::test::tempDir() / "pkn_sema_function_types_imp";
  std::filesystem::remove_all(dir);
  writeProjectFile(dir, "lib/ops.pkn", R"(
fn square(x: int) -> int { return x * x; }
fn twice(f: (int) -> int) -> (int) -> int { return fn(x: int) -> int { return f(f(x)); }; }
class Button { label: Str; onClick: (Str) -> Str; fn click() -> Str { return self.onClick(self.label); } }
struct Op { name: Str; run: (int, int) -> int; }
fn makeCounter(start: int) -> () -> int { return fn() -> int { return start + 1; }; }
)");
  auto r = semaCheckFile(writeProjectFile(dir, "main.pkn", R"(
import lib::ops;
fn plus(a: int, b: int) -> int { return a + b; }
fn main() -> int {
  sq = ops::square;
  t = ops::twice(sq);
  b = ops::Button("b", fn(l: Str) -> Str { return l + "!"; });
  c = ops::makeCounter(41);
  o = ops::Op("plus", plus);
  k: (int) -> int = ops::twice(fn(x: int) -> int { return x + 1; });
  return t(3) + b.click().len() + c() + o.run(2, 3) + k(0);
}
)"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics, "");
  std::filesystem::remove_all(dir);
}
