// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: `view fn` methods.  A method may change `self` unless it is a
// `view fn`, where `self` is read-only; only a `view fn` may be called on a
// `view`, and a method that never changes `self` but is not one gets a
// warning.

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

void expectAll(const SemaResult &r, std::initializer_list<const char *> diags) {
  for (const char *d : diags)
    EXPECT_NE(r.Diagnostics.find(d), std::string::npos) << d << "\n"
                                                        << r.Diagnostics;
}

} // namespace

// In a `view fn`, `self` and everything reached through it is read-only: no
// field or element write, no call of a method that is not a `view fn`, no
// `inout` argument, and a `view` of it stays one, also when returned
// (`-> view T`).  Other objects can change.
TEST(ViewFn, SelfIsReadOnly) {
  auto r = semaCheck(R"(class Child {
  k: int;
  fn tick() { self.k = self.k + 1; }
}

impl Child {
  fn __init__() { self.k = 0; }
}
class Counter {
  n: int; c: Child; xs: int[1];
  fn tick() { self.n = self.n + 1; }
  view fn a() { self.n = 1; }
  view fn b() { self.tick(); }
  view fn c2() { bump(self.n); }
  view fn d() { self.c.tick(); }
  view fn e() { fill(self.xs); }
  view fn f() { self.xs[0] = 2; }
  view fn g() { keep(self.c); }
  view fn h() { o = self.c; }
  view fn i() { t: inout = self.n; }
  view fn child() -> Child { return self.c; }
  view fn ok() -> int {
    o = Child();
    o.tick();
    return self.n + self.xs.len() + self.c.k;
  }
}

impl Counter {
  fn __init__() { self.n = 0; self.c = Child(); self.xs = [1]; }
}
fn bump(n: inout int) { n = n + 1; }
fn fill(xs: inout int[]) { }
fn keep(c: Child) { }
fn main() -> int { return 0; }
)");
  EXPECT_EQ(r.ErrorCount, 10u) << r.Diagnostics;
  expectAll(r, {
                   ":12:17: error: 'self' is read-only in 'view fn a'; cannot "
                   "assign to its field 'n'",
                   ":13:17: error: 'self' is read-only in 'view fn b'; 'tick' "
                   "is not a 'view fn'",
                   ":14:23: error: 'self' is read-only in 'view fn c2'; it "
                   "cannot be passed to 'inout' parameter 'n'",
                   ":15:17: error: 'self' is read-only in 'view fn d'; 'tick' "
                   "is not a 'view fn'",
                   ":16:22: error: 'self' is read-only in 'view fn e'; it "
                   "cannot be passed to 'inout' parameter 'xs'",
                   ":17:17: error: 'self' is read-only in 'view fn f'; cannot "
                   "assign to its elements",
                   ":18:22: error: 'self' is read-only in 'view fn g'; it can "
                   "only be passed to a 'view' parameter, and parameter 1 of "
                   "'keep' is not one",
                   ":19:21: error: 'self' is read-only in 'view fn h'; it "
                   "cannot be stored, only read or passed to a 'view' "
                   "parameter",
                   ":20:28: error: 'self' is read-only in 'view fn i'; it "
                   "cannot be named by 'inout' local 't'",
                   ":21:37: error: 'self' is read-only in 'view fn child', so "
                   "part of it can only be returned as a 'view': write '-> "
                   "view Child'",
               });
}

// Only a method is a `view fn`, never `__init__`, and an override keeps the
// marker of the method it overrides.
TEST(ViewFn, Declarations) {
  auto r = semaCheck(R"(class Base {
  fn get() -> int { return 1; }
  view fn show() -> Str { return "b"; }
}
class Sub : Base {
  view fn get() -> int { return 2; }
  fn show() -> Str { return "s"; }
}
class Pt { x: int; }
impl Pt { view fn __init__() { self.x = 0; } }
view fn free() { }
class Shape { view fn toString() -> Str { return "s"; } }
class Bad { fn toString() -> Str { return "b"; } }
fn main() -> int { return 0; }
)");
  EXPECT_EQ(r.ErrorCount, 5u) << r.Diagnostics;
  expectAll(r,
            {
                ":6:3: error: override of 'get' cannot be a 'view fn': the "
                "method it overrides may change 'self'",
                ":7:3: error: override of 'show' must be a 'view fn', like "
                "the method it overrides",
                ":10:11: error: '__init__' cannot be a 'view fn': it sets up "
                "'self'",
                ":11:1: error: only a method can be a 'view fn': 'free' is a "
                "free function, with no 'self'",
                ":13:13: error: override of 'toString' must be a 'view fn', "
                "like the method it overrides",
            });
}

// `let` only fixes the variable: any method can be called on what it holds,
// its own and the builtins', `view fn` or not.
TEST(ViewFn, LetCanCallAnyMethod) {
  auto r = semaCheck(R"(import ::io; class Counter {
  n: int;
  fn tick() { self.n = self.n + 1; }
  view fn get() -> int { return self.n; }
}

impl Counter {
  fn __init__() { self.n = 0; }
}
fn main() -> int {
  let c = Counter();
  let s = "abc";
  let xs = [1, 2];
  c.tick();
  s.concat("d");
  io::println(c.toString() + s.toString());
  return c.get() + s.len() + xs.len();
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// A `view fn` of the class, an inherited one and an override of one can be
// called on a `view`; a plain `fn` cannot.
TEST(ViewFn, CallableOnAView) {
  auto r = semaCheck(R"(import ::io; class Counter {
  n: int;
  fn tick() { self.n = self.n + 1; }
  view fn get() -> int { return self.n; }
  view fn toString() -> Str { return "c" + Str(self.n); }
}

impl Counter {
  fn __init__() { self.n = 0; }
}
class Fast : Counter {
  view fn get() -> int { return 2 * self.n; }
}

impl Fast {
  fn __init__() { __super__(); }
}
fn show(c: view Counter, f: view Fast) -> int {
  io::println(c.toString() + f.toString());
  f.tick();
  return c.get() + f.get();
}
fn main() -> int { return show(Counter(), Fast()); }
)");
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  expectAll(r, {":20:3: error: 'f' is a 'view' parameter; 'tick' is not a "
                "'view fn'"});
}

// The warning: a method that never changes `self` and is not a `view fn`.  A
// method calling only such methods on `self` gets it too, in the same
// compile; a method with an override that changes `self` does not, nor one
// passing part of `self` to a parameter that is not `view`, and a generic
// class's method gets one warning, not one per instance.
TEST(ViewFn, WarnsWhenSelfNeverChanges) {
  auto r = semaCheck(R"(class Shape {
  w: int;
  fn area() -> int { return self.w * self.w; }
  fn twice() -> int { return 2 * self.area(); }
  fn grow() { self.w = self.w + 1; }
  fn grown() -> int { self.grow(); return self.w; }
  fn hook() -> int { return 0; }
  view fn done() -> int { return self.w; } fn sized() -> int { return twiceOf(self.w); }
}

impl Shape {
  fn __init__() { self.w = 2; }
}
class Square : Shape {
  fn hook() -> int { self.w = 0; return 1; }
}

impl Square {
  fn __init__() { __super__(); }
}
class Box<T> { v: T; fn get() -> T { return self.v; } }
impl Box<T> { fn __init__(v: T) { self.v = v; } }
fn main() -> int {
  b = Box<int>(1);
  c = Box<Str>("a");
  return Shape().twice() + b.get() + c.get().len();
}
fn twiceOf(n: int) -> int { return 2 * n; }
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  const char *kSuffix = "never changes 'self': make it a 'view fn'";
  EXPECT_NE(r.Diagnostics.find(":3:3: warning: 'area' never changes 'self': "
                               "make it a 'view fn' so 'view' parameters can "
                               "call it"),
            std::string::npos)
      << r.Diagnostics;
  for (const char *loc : {":3:3: warning: 'area' ", ":4:3: warning: 'twice' ",
                          ":21:22: warning: 'get' "})
    EXPECT_NE(r.Diagnostics.find(std::string(loc) + kSuffix), std::string::npos)
        << loc << "\n"
        << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("'get' never"),
            r.Diagnostics.rfind("'get' never"))
      << r.Diagnostics;
  for (const char *quiet :
       {"'grow'", "'grown'", "'hook'", "'done'", "'sized'", "'__init__'"})
    EXPECT_EQ(r.Diagnostics.find(std::string("warning: ") + quiet),
              std::string::npos)
        << quiet << "\n"
        << r.Diagnostics;
}

// A module's `view fn` markers are part of its interface: overrides in the
// importer are checked against them.
TEST(ViewFn, AcrossModules) {
  auto dir = tempDir() / "view_fn_modules";
  std::filesystem::remove_all(dir);
  writeModule(dir, "base.pkn",
              "class P { n: int;\n"
              "  view fn get() -> int { return self.n; }\n"
              "  fn set(v: int) { self.n = v; } }\n"
              "impl P { fn __init__() { self.n = 0; } }\n");
  auto r = semaCheckPath(writeModule(dir, "main.pkn", R"(import base;
class Q : base::P { fn get() -> int { return 1; } }
impl Q { fn __init__() { __super__(); } }
fn main() -> int {
  let p = base::P();
  p.set(2);
  return p.get();
})"));
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  expectAll(r, {":2:21: error: override of 'get' must be a 'view fn', like "
                "the method it overrides"});
  std::filesystem::remove_all(dir);
}
