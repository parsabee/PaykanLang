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
      fn __init__(start: view int) { self.n = start; }
      fn add(k: view int, c: view Color) -> int { self.n = self.n + k; return k; }
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
  fn __init__(s: view Str) { self.n = s.len(); }
  fn put(c: view Counter) -> int { return c.n; }
}
fn f(o: view int?, xs: inout int[], t: view (int, int), b: view Obj) { }
fn reset(c: inout Counter, s: inout Str) { c = Counter(s); s = "new"; }
fn first<T>(x: view T) -> T { return x; }
fn swap<T>(a: inout T, b: inout T) { t = a; a = b; b = t; }
class Box<T> { n: int; fn __init__(v: view T) { self.n = 1; } }
fn main() -> int {
  c = Counter("ab");
  s = "x";
  xs = [1];
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
// no field or element write, no call of a method that is not a `view fn`
// (`push` included), no `inout` argument.  `view fn`s and reads are fine.
TEST(ParamMode, ViewReferenceParameters) {
  auto r = semaCheck(R"(class Counter {
  n: int;
  fn __init__() { self.n = 0; }
  fn tick() { self.n = self.n + 1; }
  view fn get() -> int { return self.n; }
}
fn reset(c: inout Counter) { c = Counter(); }
fn bump(n: inout int) { n = n + 1; }
fn look(c: view Counter, s: view Str, xs: view int[]) -> int {
  c.tick();
  c.n = 2;
  xs.push(1);
  xs[0] = 1;
  s = "x";
  reset(c);
  bump(c.n);
  return c.get() + c.n + s.len() + xs.len() + xs[0];
}
fn main() -> int { return 0; })");
  EXPECT_EQ(r.ErrorCount, 7u) << r.Diagnostics;
  expectErrors(
      r, {":10:3: error: 'c' is a 'view' parameter; 'tick' is not a 'view fn'",
          ":11:3: error: 'c' is a 'view' parameter; cannot assign to its "
          "field 'n'",
          ":12:3: error: 'xs' is a 'view' parameter; 'push' is not a 'view fn'",
          ":13:3: error: 'xs' is a 'view' parameter; cannot assign to its "
          "elements",
          ":14:3: error: cannot assign to 'view' parameter 's'",
          ":15:9: error: 'c' is a 'view' parameter; it cannot be passed to "
          "'inout' parameter 'c'",
          ":16:8: error: 'c' is a 'view' parameter; it cannot be passed to "
          "'inout' parameter 'n'"});
}

// A `view` stays one: it can only be passed on to a `view` parameter,
// whatever its type, and one that shares what it holds (an object, string,
// array, tuple or optional) is never stored -- assigned, put in an array or
// tuple, pushed -- nor returned.  A `match` arm's name for it is a `view`.
// `self` in a `view fn` is one too.  A value type is copied when it is
// stored, and builtins such as `println` and `equals` only read.
TEST(ParamMode, ViewOnlyPassesToView) {
  auto r = semaCheck(R"(class Counter {
  n: int;
  fn __init__() { self.n = 0; }
  fn tick() { self.n = self.n + 1; }
  view fn count() -> int { return self.n; }
  view fn show() { report(self); }
}
class Holder { c: Counter; fn __init__(c: view Counter) { self.c = c; } }
fn report(c: Counter) { c.tick(); }
fn look(c: view Counter) -> int { return c.count(); }
fn bump(n: int) -> int { return n + 1; }
fn pick(c: view Counter) -> Counter { return c; }
fn uses(c: view Counter, n: view int, flag: view bool) -> int {
  report(c);
  d = c;
  e: Counter = c;
  xs = [c];
  t = (c, 1);
  ys: Counter[] = [];
  ys.push(c);
  h = Holder(c);
  h.c = c;
  f = if flag then c else Counter();
  match c {
    k: Counter { k.tick(); k = Counter(); }
  }
  bump(c.n);
  bump(n);
  println(c);
  m = n;
  ints: int[] = [];
  ints.push(n);
  c.equals(c);
  return look(c) + bump(m) + bump(n + 1);
}
fn main() -> int { return 0; })");
  const std::string kPass =
      "it can only be passed to a 'view' parameter, and parameter 1 of ";
  const std::string kStore =
      "'c' is a 'view' parameter; it cannot be stored, only read or passed "
      "to a 'view' parameter";
  for (const std::string &diag :
       {":6:27: error: 'self' is read-only in 'view fn show'; " + kPass +
            "'report' is not one",
        ":8:68: " + std::string("error: ") + kStore,
        std::string(":12:46: error: 'c' is a 'view' parameter; it cannot be "
                    "returned"),
        ":14:10: error: 'c' is a 'view' parameter; " + kPass +
            "'report' is not one",
        ":15:7: error: " + kStore, ":16:16: error: " + kStore,
        ":17:9: error: " + kStore, ":18:8: error: " + kStore,
        ":20:11: error: " + kStore, ":22:9: error: " + kStore,
        ":23:7: error: " + kStore,
        std::string(":25:18: error: 'k' is bound to a 'view'; 'tick' is not a "
                    "'view fn'"),
        std::string(":25:28: error: cannot assign to 'k': it is bound to a "
                    "'view'"),
        ":27:8: error: 'c' is a 'view' parameter; " + kPass +
            "'bump' is not one",
        ":28:8: error: 'n' is a 'view' parameter; " + kPass +
            "'bump' is not one"})
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 15u) << r.Diagnostics;
  // Passing on to a `view` parameter, copying a value type, and builtins.
  for (const char *line :
       {":21:", ":29:", ":30:", ":31:", ":32:", ":33:", ":34:"})
    EXPECT_EQ(r.Diagnostics.find(line), std::string::npos) << line << "\n"
                                                           << r.Diagnostics;
}

// The arguments of an `inout` parameter of a class type: a variable or field
// of exactly that class, so a subclass variable cannot be given another
// subclass; not `self`, and not a `let` local, though a field of what one
// holds can be.
TEST(ParamMode, InoutReferenceArguments) {
  auto r = semaCheck(R"(class Counter {
  n: int;
  fn __init__() { self.n = 0; }
  fn me() { reset(self); }
}
class Fast : Counter { fn __init__() { __super__(); } }
class Holder { c: Counter; fn __init__() { self.c = Counter(); } }
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
      r, {":4:19: error: 'self' cannot be passed to 'inout' parameter 'c': the "
          "method would no longer know its object",
          ":12:9: error: 'k' is 'let'; it cannot be passed to 'inout' "
          "parameter 'c'",
          ":16:9: error: argument 1 of 'reset' has type 'Fast', but 'inout' "
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
  fn __init__(n: inout int) { self.n = n; }
  fn add(to: inout float) { }
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
      {":12:8: error: 'v' is a 'view' parameter; it cannot be passed to "
       "'inout' "
       "parameter 'n'",
       ":13:8: error: 'k' is 'let'; it cannot be passed to "
       "'inout' parameter 'n'",
       ":14:8: error: argument 1 of 'bump' must be a variable or a field: "
       "parameter 'n' is 'inout'",
       ":15:8: error: argument 1 of 'bump' must be a variable or a field: "
       "parameter 'n' is 'inout'",
       ":16:9: error: argument 1 of 'add' has type 'int', but 'inout' "
       "parameter 'to' has type 'float'",
       ":18:8: error: an array element cannot be passed to 'inout' parameter "
       "'n' yet",
       ":20:7: error: a character of a string cannot be passed to 'inout' "
       "parameter 'c'",
       ":25:10: error: 'k' is 'let'; it cannot be passed to "
       "'inout' parameter 'n'",
       ":26:8: error: argument 1 of 'bump' has type 'Str', expected 'int'"});
  EXPECT_EQ(r.ErrorCount, 9u) << r.Diagnostics;
  // Plain and `inout` parameters, variables and fields are accepted.
  for (const char *line : {":21:", ":22:", ":23:", ":24:"})
    EXPECT_EQ(r.Diagnostics.find(line), std::string::npos) << line << "\n"
                                                           << r.Diagnostics;
}

// Exclusivity: one call cannot pass the same variable, or the same chain of
// fields on one, to two `inout` parameters.  Other places, which might be the
// same storage at run time, are accepted.
TEST(ParamMode, InoutArgumentsAreExclusive) {
  auto r = semaCheck(R"(class P {
  x: int; y: int; next: P?;
  fn __init__() { self.x = 0; self.y = 0; self.next = None; }
  fn same() { swap(self.x, self.x); }
  fn other(q: P) { swap(self.x, q.x); }
}
class Box { p: P; fn __init__() { self.p = P(); } }
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
  expectErrors(r, {":4:28: error: 'self.x' is passed to two 'inout' "
                   "parameters ('a' and 'b')",
                   ":13:11: error: 'k' is passed to two 'inout' parameters "
                   "('a' and 'b')",
                   ":18:13: error: 'p.x' is passed to two 'inout' parameters "
                   "('a' and 'b')",
                   ":19:15: error: 'b.p.x' is passed to two 'inout' "
                   "parameters ('a' and 'b')"});
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
class Mine : lib::Box { fn __init__(n: view int) { __super__(n); } }
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
      r, {":2:62: error: 'n' is a 'view' parameter; it cannot be passed to "
          "'inout' "
          "parameter 'n'",
          ":4:3: error: override of 'put' must keep 'inout' on parameter 'n'",
          ":8:13: error: 'k' is 'let'; it cannot be passed to "
          "'inout' parameter 'n'",
          ":9:16: error: argument 1 of 'lib::Box' must be a variable or a "
          "field: parameter 'n' is 'inout'",
          ":10:9: error: argument 1 of 'put' must be a variable or a field: "
          "parameter 'n' is 'inout'"});
  std::filesystem::remove_all(dir);
}
