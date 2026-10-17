// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: `view` and `inout` parameters.

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

// A `view` parameter is passed by value and read-only; any argument of its
// type does, and reading it gives a copy.
TEST(ParamMode, ViewParameters) {
  auto ok = semaCheck(R"(
    enum Color { Red, Green }
    class Counter {
      n: int;
      fn add(k: view int, c: view Color) -> int { self.n = self.n + k; return k; }
    }

    impl Counter {
      fn __init__(start: view int) { self.n = start; }
    }
    fn twice(x: view float) -> float { y = x; y = y * 2; return y; }
    fn pass(x: view int) -> int { return x + x; }
    fn main() -> int {
      let k = 2;
      t = twice(k);
      c = Counter(k);
      c.add(c.n + 1, Color::Red);
      return pass(k) + pass(3);
    }
  )");
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;

  auto r = semaCheck(R"(fn f(x: view int) {
  x = 1;
  x, y = (2, 3);
  { x = 4; }
  { x: int = 5; x = 6; }
}
fn main() -> int { return 0; })");
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
  expectErrors(r, {":2:3: error: cannot assign to 'view' parameter 'x'",
                   ":3:3: error: cannot assign to 'view' parameter 'x'",
                   ":4:5: error: cannot assign to 'view' parameter 'x'"});
}

// Every type takes a mode: classes, strings, optionals, arrays, tuples, `Obj`
// and type parameters as well as int, float, bool, char and enum.
TEST(ParamMode, ModesApplyToEveryType) {
  auto r = semaCheck(R"(class Counter {
  n: int;
  view fn put(c: view Counter) -> int { return c.n; }
}

impl Counter {
  fn __init__(s: view Str) { self.n = s.len(); }
}
fn f(o: view int?, xs: inout int[], t: view (int, int), b: view Obj) { }
fn reset(c: inout Counter, s: inout Str) { c = Counter(s); s = "new"; }
fn first<T>(x: view T) -> T { return x; }
fn swap<T>(a: inout T, b: inout T) { t = a; a = b; b = t; }
class Box<T> { n: int; }
impl Box<T> { fn __init__(v: view T) { self.n = 1; } }
fn main() -> int {
  c = Counter("ab");
  s = "x";
  xs: int[1] = [1];
  reset(c, s);
  f(None, xs, (1, 2), c);
  a = "a";
  b = "b";
  swap(a, b);
  i = 1;
  j = 2;
  swap(i, j);
  return first(1) + first<int>(2) + Box<Str>("v").n + c.put(c);
})");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// What a `view` parameter holds cannot change through it, whatever its type:
// no field or element write, no call of a method that is not a `view fn`, no
// `inout` argument.  Reads and `view fn` calls are fine.
TEST(ParamMode, ViewReferenceParameters) {
  auto r = semaCheck(R"(class Counter {
  n: int;
  fn tick() { self.n = self.n + 1; }
}

impl Counter {
  fn __init__() { self.n = 0; }
}
fn reset(c: inout Counter) { c = Counter(); }
fn bump(n: inout int) { n = n + 1; }
fn look(c: view Counter, s: view Str, xs: view int[]) -> int {
  c.tick();
  c.n = 2;
  xs[0] = 1;
  s = "x";
  reset(c);
  bump(c.n);
  println(c.toString());
  return c.n + s.len() + xs.len() + xs[0];
}
fn main() -> int { return 0; })");
  EXPECT_EQ(r.ErrorCount, 6u) << r.Diagnostics;
  expectErrors(
      r, {":12:3: error: 'c' is a 'view' parameter; 'tick' is not a 'view fn'",
          ":13:3: error: 'c' is a 'view' parameter; cannot assign to its "
          "field 'n'",
          ":14:3: error: 'xs' is a 'view' parameter; cannot assign to its "
          "elements",
          ":15:3: error: cannot assign to 'view' parameter 's'",
          ":16:9: error: 'c' is a 'view' parameter; it cannot be passed to "
          "'inout' parameter 'c'",
          ":17:8: error: 'c' is a 'view' parameter; it cannot be passed to "
          "'inout' parameter 'n'"});
}

// The arguments of an `inout` parameter of a class type: a variable or field
// of exactly that class, so a subclass variable cannot be given another
// subclass; not `self`, and not a `let` local, though a field of what one
// holds can be.
TEST(ParamMode, InoutReferenceArguments) {
  auto r = semaCheck(R"(class Counter {
  n: int;
  fn me() { reset(self); }
}

impl Counter {
  fn __init__() { self.n = 0; }
}
class Fast : Counter {}
impl Fast { fn __init__() { __super__(); } }
class Holder { c: Counter; }
impl Holder { fn __init__() { self.c = Counter(); } }
fn reset(c: inout Counter) { c = Counter(); }
fn main() -> int {
  let k = Counter();
  k.n = 3;
  reset(k);
  let h = Holder();
  reset(h.c);
  f = Fast();
  reset(f);
  g: Counter = Fast();
  reset(g);
  return 0;
})");
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
  expectErrors(
      r, {":3:19: error: 'self' cannot be passed to 'inout' parameter 'c': the "
          "method would no longer know its object",
          ":17:9: error: 'k' is declared with 'let' and cannot be passed to "
          "'inout' parameter 'c'",
          ":21:9: error: argument 1 of 'reset' has type 'Fast', but 'inout' "
          "parameter 'c' has type 'Counter'"});
}

// An override keeps every parameter's mode.
TEST(ParamMode, OverridesKeepModes) {
  auto r = semaCheck(R"(class Base {
  fn a(k: view int) { }
  fn b(k: int) { }
  fn c(k: view int, j: int) { }
}
class Sub : Base {
  fn a(k: int) { }
  fn b(k: view int) { }
  fn c(k: view int, j: int) { }
}
fn main() -> int { return 0; })");
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
  expectErrors(r, {":7:3: error: override of 'a' must keep 'view' on "
                   "parameter 'k'",
                   ":8:3: error: override of 'b' must not add 'view' to "
                   "parameter 'k'"});
}

// The arguments of an `inout` parameter: a variable or a field of exactly
// its type, not a `view` parameter or a `let` local.
TEST(ParamMode, InoutArguments) {
  auto r = semaCheck(R"(class C {
  n: int;
  fn add(to: inout float) { }
}

impl C {
  fn __init__(n: inout int) { self.n = n; }
}
fn bump(n: inout int) { n = n + 1; }
fn put(c: inout char) { }
fn take(v: view int, p: int, q: inout int) {
  let k = 1;
  x = 2;
  c = C(x);
  bump(v);
  bump(k);
  bump(3);
  bump(x + 1);
  c.add(x);
  xs = [1];
  bump(xs[0]);
  s = "ab";
  put(s[0]);
  bump(p);
  bump(q);
  bump(x);
  bump(c.n);
  c2 = C(k);
  bump(Str(1));
}
fn main() -> int { return 0; })");
  expectErrors(
      r,
      {":15:8: error: 'v' is a 'view' parameter; it cannot be passed to "
       "'inout' parameter 'n'",
       ":16:8: error: 'k' is declared with 'let' and cannot be passed to "
       "'inout' parameter 'n'",
       ":17:8: error: argument 1 of 'bump' must be a variable or a field: "
       "parameter 'n' is 'inout'",
       ":18:8: error: argument 1 of 'bump' must be a variable or a field: "
       "parameter 'n' is 'inout'",
       ":19:9: error: argument 1 of 'add' has type 'int', but 'inout' "
       "parameter 'to' has type 'float'",
       ":21:8: error: an array element cannot be passed to 'inout' parameter "
       "'n' yet",
       ":23:7: error: a character of a string cannot be passed to 'inout' "
       "parameter 'c'",
       ":28:10: error: 'k' is declared with 'let' and cannot be passed to "
       "'inout' parameter 'n'",
       ":29:8: error: argument 1 of 'bump' has type 'Str', expected 'int'"});
  EXPECT_EQ(r.ErrorCount, 9u) << r.Diagnostics;
  // Plain and `inout` parameters, variables and fields are accepted.
  for (const char *line : {":24:", ":25:", ":26:", ":27:"})
    EXPECT_EQ(r.Diagnostics.find(line), std::string::npos) << line << "\n"
                                                           << r.Diagnostics;
}

// Exclusivity: one call cannot pass the same variable, or the same chain of
// fields on one, to two `inout` parameters.  Other places, which might be the
// same storage at run time, are accepted.
TEST(ParamMode, InoutArgumentsAreExclusive) {
  auto r = semaCheck(R"(class P {
  x: int; y: int; next: P?;
  fn same() { swap(self.x, self.x); }
  fn other(q: P) { swap(self.x, q.x); }
}

impl P {
  fn __init__() { self.x = 0; self.y = 0; self.next = None; }
}
class Box { p: P; }
impl Box { fn __init__() { self.p = P(); } }
fn swap(a: inout int, b: inout int) { t = a; a = b; b = t; }
fn three(a: inout int, b: inout int, c: int) { }
fn main() -> int {
  k = 1;
  j = 2;
  swap(k, k);
  three(k, j, k);
  p = P();
  q = p;
  b = Box();
  swap(p.x, p.x);
  swap(b.p.x, b.p.x);
  swap(p.x, p.y);
  swap(p.x, q.x);
  swap(b.p.x, p.x);
  swap(k, p.x);
  return 0;
})");
  EXPECT_EQ(r.ErrorCount, 4u) << r.Diagnostics;
  expectErrors(r, {":3:28: error: 'self.x' is passed to two 'inout' "
                   "parameters ('a' and 'b')",
                   ":17:11: error: 'k' is passed to two 'inout' parameters "
                   "('a' and 'b')",
                   ":22:13: error: 'p.x' is passed to two 'inout' parameters "
                   "('a' and 'b')",
                   ":23:15: error: 'b.p.x' is passed to two 'inout' "
                   "parameters ('a' and 'b')"});
}

// What an `inout` argument names shares no storage with anything else the
// call borrows: a `view` argument, another `inout` one, or the receiver of
// a method that may change it or of a `view fn`.  A variable and a field
// chain on it share storage; two fields of one variable don't.
TEST(ParamMode, InoutOverlapsNothingElse) {
  auto r = semaCheck(R"(class P {
  x: int; y: int;
  fn take(m: inout int) { self.y = m; }
  view fn look(m: inout int) -> int { return self.x; }
  fn merge(o: view P) { self.x = o.x; }
  fn own() { self.take(self.x); }
}

impl P {
  fn __init__() { self.x = 0; self.y = 0; }
}
class Q { p: P; n: int; }
impl Q { fn __init__() { self.p = P(); self.n = 0; } }
fn both(a: view int, b: inout int) { }
fn whole(a: view P, b: inout int) { }
fn two(a: inout P, b: inout int) { }
fn rev(a: inout int, b: view P) { }
fn elem(a: view int, b: inout int[]) { }
fn main() -> int {
  k = 1;
  p = P();
  q = Q();
  xs: int[1] = [1];
  both(k, k);
  whole(p, p.x);
  two(p, p.x);
  rev(p.x, p);
  p.take(p.x);
  p.look(p.x);
  p.merge(p);
  elem(xs[0], xs);
  q.p.take(q.p.y);
  both(p.x, p.y);
  q.p.take(q.n);
  return 0;
}
)");
  EXPECT_EQ(r.ErrorCount, 10u) << r.Diagnostics;
  expectErrors(
      r, {":6:24: error: 'self.x' is part of 'self', the receiver of 'take' "
          "(not a 'view fn'); it cannot also be passed to 'inout' parameter "
          "'m'",
          ":24:11: error: 'k' is passed to 'view' parameter 'a'; it cannot "
          "also be passed to 'inout' parameter 'b'",
          ":25:12: error: 'p.x' is part of 'p', passed to 'view' parameter "
          "'a'; it cannot also be passed to 'inout' parameter 'b'",
          ":26:10: error: 'p.x' is part of 'p', passed to 'inout' parameter "
          "'a'; it cannot also be passed to 'inout' parameter 'b'",
          ":27:12: error: 'p' contains 'p.x', passed to 'inout' parameter "
          "'a'; it cannot also be passed to 'view' parameter 'b'",
          ":28:10: error: 'p.x' is part of 'p', the receiver of 'take' (not a "
          "'view fn'); it cannot also be passed to 'inout' parameter 'm'",
          ":29:10: error: 'p.x' is part of 'p', the receiver of 'view fn "
          "look'; it cannot also be passed to 'inout' parameter 'm'",
          ":30:11: error: 'p' is the receiver of 'merge' (not a 'view fn'); "
          "it cannot also be passed to 'view' parameter 'o'",
          ":31:15: error: 'xs' is passed to 'view' parameter 'a'; it cannot "
          "also be passed to 'inout' parameter 'b'",
          ":32:12: error: 'q.p.y' is part of 'q.p', the receiver of 'take' "
          "(not a 'view fn'); it cannot also be passed to 'inout' parameter "
          "'m'"});
}

// Modes travel with a module's exports: an override in another module keeps
// them, and calls into it are checked like local ones.
TEST(ParamMode, ModesAcrossModules) {
  auto dir = tempDir() / "param_modes_modules";
  std::filesystem::remove_all(dir);
  writeModule(dir, "base.pkn",
              "class Counter { n: int;\n"
              "  fn add(k: view int) -> int { return k; } }\n"
              "fn twice(n: view int) -> int { return n * 2; }\n");
  auto r = semaCheckPath(writeModule(dir, "main.pkn", R"(import base;
class Sub : base::Counter { fn add(k: int) -> int { return k; } }
fn main() -> int { return base::twice(3); })"));
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  expectErrors(r, {":2:29: error: override of 'add' must keep 'view' on "
                   "parameter 'k'"});

  // `inout` across modules, through a module cache entry whose exports carry
  // them.
  auto lib = writeModule(dir, "lib.pkn", "fn real() -> int { return 1; }\n");
  using paykan::ast::ParamMode;
  using Info = paykan::sema::Sema::ModuleInfo;
  const paykan::ast::ParamModes inoutN{{ParamMode::Inout}, {"n"}};
  Info info;
  info.ExportedFunctions = {{"bump", "void", {"int"}, inoutN},
                            {"Box", "Box", {"int"}, inoutN}};
  Info::ClassInfo box;
  box.Name = "Box";
  box.OriginModule = "lib";
  box.Fields = {{"v", "int"}};
  box.Methods = {{"put", "void", {"int"}, 0, inoutN},
                 {"__init__", "void", {"int"}, 0, inoutN}};
  info.ExportedClasses = {box};
  auto key = std::filesystem::canonical(lib).string();
  paykan::sema::Sema::ModuleCache[key] = info;
  r = semaCheckPath(writeModule(dir, "uses.pkn", R"(import lib;
class Mine : lib::Box {}
impl Mine { fn __init__(n: view int) { __super__(n); } }
class Other : lib::Box {
  fn put(n: int) { }
}
fn main() -> int {
  let k = 1;
  lib::bump(k);
  b = lib::Box(2);
  b.put(b.v + 1);
  x = 1;
  lib::bump(x);
  b.put(x);
  return 0;
})"));
  paykan::sema::Sema::ModuleCache.erase(key);
  EXPECT_EQ(r.ErrorCount, 5u) << r.Diagnostics;
  expectErrors(
      r, {":3:50: error: 'n' is a 'view' parameter; it cannot be passed to "
          "'inout' parameter 'n'",
          ":5:3: error: override of 'put' must keep 'inout' on parameter 'n'",
          ":9:13: error: 'k' is declared with 'let' and cannot be passed to "
          "'inout' parameter 'n'",
          ":10:16: error: argument 1 of 'lib::Box' must be a variable or a "
          "field: parameter 'n' is 'inout'",
          ":11:9: error: argument 1 of 'put' must be a variable or a field: "
          "parameter 'n' is 'inout'"});
  std::filesystem::remove_all(dir);
}

// -- Local borrows

// A `view` local reads any expression, with its type inferred or written:
// it cannot be assigned, destructured into or passed to an `inout`
// parameter.
TEST(LocalBorrow, ViewLocals) {
  auto ok = semaCheck(R"(
    enum Color { Red, Green }
    fn twice(n: view int) -> int { return 2 * n; }
    fn main() -> int {
      k = 3;
      v: view = k + 1;
      w: view float = k;
      c: view Color = Color::Green;
      return twice(v) + int(w);
    }
  )");
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;

  auto r = semaCheck(R"(fn bump(n: inout int) { n = n + 1; }
fn main() -> int {
  k = 3;
  v: view = k;
  v = 4;
  v, z = (1, 2);
  bump(v);
  s: view = "x";
  return 0;
})");
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
  expectErrors(
      r, {":5:3: error: cannot assign to 'view' local 'v'",
          ":6:3: error: cannot assign to 'view' local 'v'",
          ":7:8: error: 'v' is a 'view' local; it cannot be passed to 'inout' "
          "parameter 'n'"});
}

// An `inout` local names a variable or a field (of a `let` local's object
// too) of exactly its type: not an expression, an array element (not yet), a
// string's character, `self`, a `let` local or anything reached through a
// `view`.
TEST(LocalBorrow, InoutLocals) {
  auto r = semaCheck(R"(class Counter {
  n: int;
  fn me() { m: inout = self; t: inout = self.n; t = 1; }
}

impl Counter {
  fn __init__() { self.n = 0; }
}
class Fast : Counter {}
impl Fast { fn __init__() { __super__(); } }
fn bump(n: inout int) { n = n + 1; }
fn main() -> int {
  k = 1;
  a: inout = k + 1;
  xs = [1];
  b: inout = xs[0];
  s = "ab";
  c: inout = s[0];
  let l = 2;
  d: inout = l;
  v: view = Counter();
  e: inout = v.n;
  f: inout float = k;
  fast = Fast();
  g: inout Counter = fast;
  let lc = Counter();
  h: inout = lc.n;
  i: inout int = k;
  i = 3;
  bump(i);
  return 0;
})");
  EXPECT_EQ(r.ErrorCount, 8u) << r.Diagnostics;
  expectErrors(
      r, {":3:24: error: 'self' cannot be named by 'inout' local 'm': the "
          "method would no longer know its object",
          ":14:14: error: 'inout' local 'a' must name a variable or a field",
          ":16:14: error: an array element cannot be named by 'inout' local "
          "'b' yet",
          ":18:14: error: a character of a string cannot be named by 'inout' "
          "local 'c'",
          ":20:14: error: 'l' is declared with 'let' and cannot be named by "
          "'inout' local 'd'",
          ":22:14: error: 'v' is a 'view' local; it cannot be named by "
          "'inout' local 'e'",
          ":23:20: error: 'inout' local 'f' has type 'float', but what it "
          "names has type 'int'",
          ":25:22: error: 'inout' local 'g' has type 'Counter', but what it "
          "names has type 'Fast'"});
}

// Nothing reached through a `view` local of an object, string or array can
// change: no field or element write, no `inout` argument, and only a
// `view fn` (`toString`, an override of it, `len`) can be called on it.  A
// `match` arm's name for it is a `view` too.
TEST(LocalBorrow, ViewOfReferenceTypes) {
  auto r = semaCheck(R"(class Counter {
  n: int;
  fn tick() { self.n = self.n + 1; }
  view fn toString() -> Str { return "c" + Str(self.n); }
}

impl Counter {
  fn __init__() { self.n = 0; }
}
fn bump(n: inout int) { n = n + 1; }
fn fill(xs: inout int[]) { }
fn main() -> int {
  v: view = Counter();
  v.tick();
  v.n = 2;
  bump(v.n);
  s: view = "abc";
  s.concat("d");
  a: view = [1, 2];
  fill(a);
  a[0] = 5;
  match v { x: Counter { x.tick(); x = Counter(); } }
  println(v.toString() + s + Str(s.len() + a.len() + a[0] + v.n));
  return 0;
})");
  EXPECT_EQ(r.ErrorCount, 8u) << r.Diagnostics;
  expectErrors(
      r, {":14:3: error: 'v' is a 'view' local; 'tick' is not a 'view fn'",
          ":15:3: error: 'v' is a 'view' local; cannot assign to its field 'n'",
          ":16:8: error: 'v' is a 'view' local; it cannot be passed to "
          "'inout' parameter 'n'",
          ":18:3: error: 's' is a 'view' local; 'concat' is not a 'view fn'",
          ":20:8: error: 'a' is a 'view' local; it cannot be passed to "
          "'inout' parameter 'xs'",
          ":21:3: error: 'a' is a 'view' local; cannot assign to its "
          "elements",
          ":22:26: error: 'x' is bound to a 'view'; 'tick' is not a 'view fn'",
          ":22:36: error: cannot assign to 'x': it is bound to a "
          "'view'"});
}

// A `view` stays one: it can only be passed on to a `view` parameter,
// whatever its type, and one that shares what it holds (an object, string,
// array, tuple or optional) is never stored (assigned, put in an array or
// tuple) nor returned.  The builtin functions only read, but
// `Str(s)` returns `s` itself; a value type is copied when it is assigned.
TEST(LocalBorrow, AViewStaysAView) {
  auto r = semaCheck(R"(class Counter { n: int; }
impl Counter { fn __init__() { self.n = 0; } }
class Holder { c: Counter; }
impl Holder { fn __init__(c: Counter) { self.c = c; } }
fn report(c: Counter) { }
fn twice(n: int) -> int { return 2 * n; }
fn look(n: view int) -> int { return n; }
fn pick(c: Counter) -> Counter {
  v: view = c;
  return v;
}
fn main() -> int {
  v: view = Counter();
  k: view = 3;
  report(v);
  d = v;
  e: Counter = v;
  xs = [v];
  t = (v, 1);
  ys: Counter[1] = [Counter()];
  ys[0] = v;
  h = Holder(v);
  h.c = v;
  flag = True;
  f = if flag then v else Counter();
  twice(k);
  twice(v.n);
  s: view = "s";
  u = Str(s);
  println(v);
  m = k;
  ints: int[1] = [k];
  ints[0] = k;
  return look(k) + look(v.n) + twice(m) + twice(k + 1);
})");
  const std::string kStore =
      "'v' is a 'view' local; it cannot be stored, only read or passed to a "
      "'view' parameter";
  const std::string kPass =
      "it can only be passed to a 'view' parameter, and parameter 1 of '";
  for (const std::string &diag :
       {std::string(":10:10: error: 'v' is a 'view' local; it cannot be "
                    "returned"),
        ":15:10: error: 'v' is a 'view' local; " + kPass + "report' is not one",
        ":16:7: error: " + kStore, ":17:16: error: " + kStore,
        ":18:9: error: " + kStore, ":19:8: error: " + kStore,
        ":21:11: error: " + kStore,
        ":22:14: error: 'v' is a 'view' local; " + kPass + "Holder' is not one",
        ":23:9: error: " + kStore, ":25:7: error: " + kStore,
        ":26:9: error: 'k' is a 'view' local; " + kPass + "twice' is not one",
        ":27:9: error: 'v' is a 'view' local; " + kPass + "twice' is not one",
        ":29:11: error: 's' is a 'view' local; " + kPass + "Str' is not one"})
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 13u) << r.Diagnostics;
  for (const char *line : {":30:", ":31:", ":32:", ":33:", ":34:"})
    EXPECT_EQ(r.Diagnostics.find(line), std::string::npos) << line << "\n"
                                                           << r.Diagnostics;
}

// A `view` parameter stays one too: it can only be passed on to a `view`
// parameter.
TEST(ParamMode, ViewParametersOnlyPassToView) {
  auto r = semaCheck(R"(fn twice(n: int) -> int { return 2 * n; }
fn look(n: view int) -> int { return n; }
fn f(n: view int) -> int {
  println(Str(n));
  m = n;
  return twice(n) + look(n) + twice(m);
}
fn main() -> int { return f(1); })");
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  expectErrors(r, {":6:16: error: 'n' is a 'view' parameter; it can only be "
                   "passed to a 'view' parameter, and parameter 1 of 'twice' "
                   "is not one"});
}

// Exclusivity: while an `inout` local is live, the variable it names cannot
// be used; while a `view` local of a variable is live, the variable cannot
// change (a value type can still be passed on as a copy).  A borrow lives
// until its last use in its block, through a whole loop that uses it, and a
// borrow of a borrow keeps the first variable borrowed.
TEST(LocalBorrow, Exclusivity) {
  auto r = semaCheck(R"(class Counter {
  n: int;
  fn tick() { self.n = self.n + 1; }
}

impl Counter {
  fn __init__() { self.n = 0; }
}
fn twice(n: int) -> int { return 2 * n; }
fn report(c: Counter) { }
fn main() -> int {
  k = 1;
  x: inout = k;
  x = 2;
  y = k;
  x = 3;
  z = k;
  c = Counter();
  v: view = c;
  c.tick();
  c.n = 4;
  c = Counter();
  report(c);
  println(Str(v.n));
  c.tick();
  m = 5;
  w: view = m;
  t = twice(m);
  m = 6;
  print(Str(w));
  m = 7;
  i = 0;
  a = 1;
  ai: inout = a;
  while (i < 3) { a = a + 1; ai = ai + 1; i = i + 1; }
  p = 1;
  pi: inout = p;
  pj: inout = pi;
  p = 2;
  pj = 3;
  p = 4;
  e: view = k + 1;
  k = 9;
  return y + z + t + e;
})");
  const std::string kViewed =
      "error: 'c' is viewed by 'view' local 'v' until 'v' is last used; ";
  for (const std::string &diag :
       {std::string(":15:7: error: 'k' is borrowed by 'inout' local 'x' until "
                    "'x' is last used"),
        ":20:3: " + kViewed + "'tick' is not a 'view fn'",
        ":21:3: " + kViewed + "cannot assign to its field 'n'",
        ":22:3: " + kViewed + "it cannot be assigned",
        ":23:10: " + kViewed +
            "it can only be passed to a 'view' parameter, and parameter 1 of "
            "'report' is not one",
        std::string(":29:3: error: 'm' is viewed by 'view' local 'w' until "
                    "'w' is last used; it cannot be assigned"),
        std::string(":35:23: error: 'a' is borrowed by 'inout' local 'ai' "
                    "until 'ai' is last used"),
        std::string(":39:3: error: 'p' is borrowed by 'inout' local 'pj' "
                    "until 'pj' is last used")})
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 8u) << r.Diagnostics;
  // After a borrow's last use, and a copy passed on, are fine; a `view` of
  // an expression borrows nothing.
  for (const char *line : {":17:", ":25:", ":28:", ":31:", ":41:", ":43:"})
    EXPECT_EQ(r.Diagnostics.find(line), std::string::npos) << line << "\n"
                                                           << r.Diagnostics;
}

// A `match` arm's `n: view T` is a `view` local of the subject, whatever the
// subject is: nothing changes through it, it passes on only to `view`
// parameters, and the subject's variable cannot change until its last use.
TEST(LocalBorrow, ViewArms) {
  auto r = semaCheck(R"(class Animal {
  n: int;
  fn tick() { self.n = self.n + 1; }
  view fn get() -> int { return self.n; }
}

impl Animal {
  fn __init__() { self.n = 0; }
}
class Dog : Animal {}
impl Dog { fn __init__() { __super__(); } }
fn keep(a: Animal) { }
fn show(a: view Animal) { }
fn main() -> int {
  a: Animal = Dog();
  match a {
    d: view Dog {
      d.tick();
      d.n = 2;
      keep(d);
      show(d);
      a.tick();
      println(Str(d.get()));
      a.tick();
    }
    _ { }
  }
  o: Animal? = a;
  match o {
    x: view Animal { x = Animal(); }
    None { }
  }
  return 0;
})");
  expectErrors(
      r, {":18:7: error: 'd' is a 'view' local; 'tick' is not a 'view fn'",
          ":19:7: error: 'd' is a 'view' local; cannot assign to its field 'n'",
          ":20:12: error: 'd' is a 'view' local; it can only be passed to a "
          "'view' parameter, and parameter 1 of 'keep' is not one",
          ":22:7: error: 'a' is viewed by 'view' local 'd' until 'd' is last "
          "used; 'tick' is not a 'view fn'",
          ":30:22: error: cannot assign to 'view' local 'x'"});
  EXPECT_EQ(r.ErrorCount, 5u) << r.Diagnostics;
  // After the binding's last use the subject can change again.
  EXPECT_EQ(r.Diagnostics.find(":24:"), std::string::npos) << r.Diagnostics;
}

// A `match` arm's `n: inout T` is the subject's storage: the subject must be
// a variable or a field that an `inout` argument could be (not an
// expression, `self`, a `let` local or a `view`), not a primitive inside an
// optional yet, and while n is live the subject's variable cannot be used.
// What is assigned to n has n's type, which fits the subject.
TEST(LocalBorrow, InoutArms) {
  auto r = semaCheck(R"(class Animal {
  n: int;
  fn tick() { self.n = self.n + 1; }
}

impl Animal {
  fn __init__(n: int) { self.n = n; }
}
class Dog : Animal {}
impl Dog { fn __init__(n: int) { __super__(n); } }
fn make() -> Animal { return Dog(1); }
fn look(v: view Animal) {
  match v { d: inout Dog { } _ { } }
}
class Box {
  a: Animal;
  fn swap() { match self { b: inout Box { } _ { } } }
  view fn peek() { match self.a { d: inout Dog { } _ { } } }
}

impl Box {
  fn __init__() { self.a = Dog(2); }
}
fn main() -> int {
  match make() { d: inout Dog { } _ { } }
  let fixed: Animal = Dog(3);
  match fixed { d: inout Dog { } _ { } }
  n: int? = 4;
  match n { k: inout int { k = 5; } None { } }
  a: Animal = Dog(5);
  match a {
    d: inout Dog {
      a.tick();
      d = Animal(1);
      d.tick();
      a.tick();
    }
    _ { }
  }
  return 0;
})");
  expectErrors(
      r,
      {":13:9: error: 'v' is a 'view' parameter; it cannot be bound by 'inout' "
       "arm 'd'",
       ":17:21: error: 'self' cannot be bound by 'inout' arm 'b': the method "
       "would no longer know its object",
       ":18:26: error: 'self' is read-only in 'view fn peek'; it cannot be "
       "bound by 'inout' arm 'd'",
       ":25:9: error: 'inout' arm 'd' needs a variable or a field to match on",
       ":27:9: error: 'fixed' is declared with 'let' and cannot be bound by "
       "'inout' arm 'd'",
       ":29:9: error: 'inout' arm 'k' cannot change the 'int' inside an "
       "optional yet",
       ":33:7: error: 'a' is borrowed by 'inout' local 'd' until 'd' is last "
       "used",
       ":34:7: error: cannot assign value of type 'Animal' to variable 'd' of "
       "type 'Dog'"});
  EXPECT_EQ(r.ErrorCount, 8u) << r.Diagnostics;
  // After d's last use the subject can be used again.
  EXPECT_EQ(r.Diagnostics.find(":36:"), std::string::npos) << r.Diagnostics;
}
