// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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
  auto r = semaCheck(R"(import ::io; class Counter {
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
  io::println(c.toString());
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

// A field of a conditional's object is a field of what one of its branches
// holds: it cannot change where either branch's could not, through an
// assignment, a compound one, an `inout` argument or a call of a method that
// is not a `view fn` (a `view` parameter, a `view` local, `self` in a `view
// fn`, nested conditionals too), and a reference it holds is not stored,
// passed to a parameter that is not `view` or returned where a `view`'s
// could not be.  It used to change the object the `view` holds.  Where both
// branches can change, it can, through a captured copy's object as well; and
// a method that changes `self` so is not told to be a `view fn`.
TEST(ParamMode, AConditionalsObjectIsEachBranchs) {
  auto r = semaCheck(R"(class Node {
  n: int;
  next: Node?;
  fn inc() { self.n = self.n + 1; }
  view fn peek(o: Node, k: int) -> int { (if k == 0 then self else o).n = 9; return self.n; }
  fn setBoth(o: Node, k: int) { (if k == 0 then self else o).n = 1; }
}
struct S { o: Node; }
fn bump(x: inout int) { x = x + 1; }
fn keep(x: Node?) -> int { return 0; }
fn out(a: view Node, b: Node, k: int) -> Node? {
  b.next = (if k == 0 then a else b).next;
  keep((if k == 0 then b else a).next);
  return (if k == 0 then a else b).next;
}
fn f(a: view Node, b: Node, k: int) {
  (if k == 0 then a else b).n = 7;
  (if k == 0 then b else a).n += 1;
  bump((if k == 0 then a else a).n);
  (if k == 0 then a else b).inc();
  (if k == 0 then b else (if k == 1 then b else a)).n = 1;
  v: view = b;
  (if k == 0 then v else b).n = 2;
}
fn g(a: Node, b: Node, s: S, t: S, k: int) {
  (if k == 0 then a else b).n = 7;
  (if k == 0 then a else b).n += 1;
  bump((if k == 0 then a else b).n);
  (if k == 0 then a else b).inc();
  h = fn() { (if k == 0 then s else t).o.n = 5; (if k == 0 then s else t).o = Node(1, None); };
  h();
}
fn main() -> int { return 0; })");
  EXPECT_EQ(r.ErrorCount, 11u) << r.Diagnostics;
  expectErrors(
      r, {":5:42: error: 'self' is read-only in 'view fn peek'; cannot assign "
          "to its field 'n'",
          ":12:12: error: 'a' is a 'view' parameter; it cannot be stored",
          ":13:8: error: 'a' is a 'view' parameter; it can only be passed to "
          "a 'view' parameter, and parameter 1 of 'keep' is not one",
          ":14:10: error: 'a' is a 'view' parameter; it cannot be returned",
          ":17:3: error: 'a' is a 'view' parameter; cannot assign to its "
          "field 'n'",
          ":18:3: error: 'a' is a 'view' parameter; cannot assign to its "
          "field 'n'",
          ":19:8: error: 'a' is a 'view' parameter; it cannot be passed to "
          "'inout' parameter 'x'",
          ":20:3: error: 'a' is a 'view' parameter; 'inc' is not a 'view fn'",
          ":21:3: error: 'a' is a 'view' parameter; cannot assign to its "
          "field 'n'",
          ":23:3: error: 'v' is a 'view' local; cannot assign to its field "
          "'n'",
          ":30:49: error: 's' is captured by copy by the function"});
  EXPECT_EQ(r.Diagnostics.find("'setBoth' never changes 'self'"),
            std::string::npos)
      << r.Diagnostics;
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

// A `view` argument (a slice's too) and the receiver of a struct's `view
// fn` are borrowed until the call runs: a call in a later argument cannot
// pass what shares their storage to an `inout` parameter, nor call a method
// on it that may change it, a class's included.
TEST(ParamMode, ABorrowLastsUntilItsCallRuns) {
  auto r = semaCheck(R"(struct P { a: int; }
impl P {
  view fn show(x: int) -> int => self.a;
  fn reset() -> int { self.a = 0; return 0; }
}
struct H { p: P; }
class C { p: P; fn change() -> int { self.p = P(0); return 0; } }
class K { n: int; fn tick() -> int { self.n = self.n + 1; return 0; } }
fn sum(xs: view int[], k: int) -> int => xs[0] + k;
fn bump(a: inout int[3]) -> int { a = [9, 9, 9]; return 0; }
fn clear(xs: inout int[]) -> int { xs[0] = 0; return 0; }
fn e4(xs: inout int[]) -> int => sum(xs, clear(xs));
fn bumpP(p: inout P) -> int { p = P(9); return 0; }
fn bumpH(h: inout H) -> int { h = H(P(9)); return 0; }
fn bumpN(n: inout int) -> int { n = 9; return 0; }
fn bumpK(k: inout K) -> int { k = K(0); return 0; }
fn look(p: view P, x: int) -> int => p.a;
fn showK(k: view K, x: int) -> int => k.n;
fn main() -> int {
  arr = [1, 2, 3];
  p = P(1);
  h = H(P(1));
  c = C(P(1));
  k = K(1);
  n = sum(arr, bump(arr));
  n = look(p, bumpP(p));
  n = p.show(bumpP(p));
  n = look(p, p.reset());
  n = look(h.p, bumpH(h));
  n = look(p, bumpN(p.a));
  n = look(c.p, c.change());
  n = showK(k, k.tick());
  n = showK(k, bumpK(k));
  return n;
})");
  EXPECT_EQ(r.ErrorCount, 10u) << r.Diagnostics;
  expectErrors(
      r,
      {":12:48: error: 'xs' is passed to 'view' parameter 'xs' of 'sum'; a "
       "later argument of 'sum' cannot pass it to 'inout' parameter 'xs' of "
       "'clear'",
       ":25:21: error: 'arr' is passed to 'view' parameter 'xs' of 'sum'; a "
       "later argument of 'sum' cannot pass it to 'inout' parameter 'a' of "
       "'bump'",
       ":26:21: error: 'p' is passed to 'view' parameter 'p' of 'look'; a "
       "later argument of 'look' cannot pass it to 'inout' parameter 'p' of "
       "'bumpP'",
       ":27:20: error: 'p' is the receiver of 'view fn show'; an argument of "
       "'show' cannot pass it to 'inout' parameter 'p' of 'bumpP'",
       ":28:15: error: 'p' is passed to 'view' parameter 'p' of 'look'; a "
       "later argument of 'look' cannot call 'reset' on it (not a 'view fn')",
       ":29:23: error: 'h' contains 'h.p', passed to 'view' parameter 'p' of "
       "'look'; a later argument of 'look' cannot pass it to 'inout' "
       "parameter 'h' of 'bumpH'",
       ":30:21: error: 'p.a' is part of 'p', passed to 'view' parameter 'p' "
       "of 'look'; a later argument of 'look' cannot pass it to 'inout' "
       "parameter 'n' of 'bumpN'",
       ":31:17: error: 'c' contains 'c.p', passed to 'view' parameter 'p' of "
       "'look'; a later argument of 'look' cannot call 'change' on it (not a "
       "'view fn')",
       ":32:16: error: 'k' is passed to 'view' parameter 'k' of 'showK'; a "
       "later argument of 'showK' cannot call 'tick' on it (not a 'view fn')",
       ":33:22: error: 'k' is passed to 'view' parameter 'k' of 'showK'; a "
       "later argument of 'showK' cannot pass it to 'inout' parameter 'k' of "
       "'bumpK'"});
}

// The same in every kind of call, as the borrowing call and as the one
// that writes: a method, a struct's method on `self`, a constructor,
// `__super__`, and a function value (a variable's, a field's, an
// expression's), whose parameters are named by number.
TEST(ParamMode, ABorrowLastsUntilItsCallRunsInEveryKindOfCall) {
  auto r = semaCheck(R"(struct P { a: int; }
impl P {
  view fn show(x: int) -> int => self.a;
  fn again() -> int => self.show(bumpP(self));
}
class Reader { view fn look(p: view P, x: int) -> int => p.a; }
class Base { a: int; }
impl Base { fn __init__(p: view P, x: int) { self.a = p.a; } }
class D : Base { y: int; }
impl D { fn __init__(p: P) { __super__(p, bumpP(p)); self.y = 0; } }
class Shown { a: int; }
impl Shown { fn __init__(p: view P, x: int) { self.a = p.a; } }
class Maker { a: int; }
impl Maker { fn __init__(p: inout P) { self.a = p.a; } }
class Holder { f: (view P, int) -> int; }
fn bumpP(p: inout P) -> int { p = P(9); return 0; }
fn look(p: view P, x: int) -> int => p.a;
fn pick() -> (view P, int) -> int => look;
fn main() -> int {
  p = P(1);
  g = look;
  bumpV = bumpP;
  holder = Holder(look);
  n = Reader().look(p, bumpP(p));
  n = Shown(p, bumpP(p)).a;
  n = g(p, bumpP(p));
  n = holder.f(p, bumpP(p));
  n = pick()(p, bumpP(p));
  n = look(p, Maker(p).a);
  n = look(p, bumpV(p));
  return n;
})");
  EXPECT_EQ(r.ErrorCount, 9u) << r.Diagnostics;
  expectErrors(
      r,
      {":4:40: error: 'self' is the receiver of 'view fn show'; an argument "
       "of 'show' cannot pass it to 'inout' parameter 'p' of 'bumpP'",
       ":10:49: error: 'p' is passed to 'view' parameter 'p' of '__super__'; "
       "a later argument of '__super__' cannot pass it to 'inout' parameter "
       "'p' of 'bumpP'",
       ":24:30: error: 'p' is passed to 'view' parameter 'p' of 'look'; a "
       "later argument of 'look' cannot pass it to 'inout' parameter 'p' of "
       "'bumpP'",
       ":25:22: error: 'p' is passed to 'view' parameter 'p' of 'Shown'; a "
       "later argument of 'Shown' cannot pass it to 'inout' parameter 'p' of "
       "'bumpP'",
       ":26:18: error: 'p' is passed to 'view' parameter '1' of 'g'; a later "
       "argument of 'g' cannot pass it to 'inout' parameter 'p' of 'bumpP'",
       ":27:25: error: 'p' is passed to 'view' parameter '1' of 'f'; a later "
       "argument of 'f' cannot pass it to 'inout' parameter 'p' of 'bumpP'",
       ":28:23: error: 'p' is passed to 'view' parameter '1' of '(view P, "
       "int) -> int'; a later argument of '(view P, int) -> int' cannot pass "
       "it to 'inout' parameter 'p' of 'bumpP'",
       ":29:21: error: 'p' is passed to 'view' parameter 'p' of 'look'; a "
       "later argument of 'look' cannot pass it to 'inout' parameter 'p' of "
       "'Maker'",
       ":30:21: error: 'p' is passed to 'view' parameter 'p' of 'look'; a "
       "later argument of 'look' cannot pass it to 'inout' parameter '1' of "
       "'bumpV'"});
}

// The writing call may be anywhere in a later argument: under other calls,
// a conditional's branch, an operator, a literal, a subscript or a tuple
// index.  A borrowed conditional borrows both branches.  A write is
// reported once, by the innermost call borrowing what it writes.
TEST(ParamMode, ACallAnywhereInALaterArgumentCounts) {
  auto r = semaCheck(R"(struct P { a: int; }
fn bumpP(p: inout P) -> int { p = P(9); return 0; }
fn look(p: view P, x: int) -> int => p.a;
fn pair(p: view P, q: view P, x: int) -> int => p.a + q.a;
fn id(x: int) -> int => x;
fn main() -> int {
  p = P(1);
  q = P(2);
  n = look(p, id(id(bumpP(p))));
  n = look(p, if n == 0 then bumpP(p) else 0);
  n = look(if n == 0 then p else q, bumpP(q));
  n = pair(p, q, bumpP(p) + bumpP(q));
  n = look(p, look(p, bumpP(p)));
  n = look(p, -bumpP(p));
  n = look(p, [1, 2][bumpP(p)]);
  n = look(p, [bumpP(p)][0]);
  n = look(p, (bumpP(p), 1).1);
  return n;
})");
  EXPECT_EQ(r.ErrorCount, 10u) << r.Diagnostics;
  const std::string later =
      "is passed to 'view' parameter 'p' of 'look'; a later argument of "
      "'look' cannot pass it to 'inout' parameter 'p' of 'bumpP'";
  for (const char *at :
       {":9:27", ":10:36", ":13:29", ":14:22", ":15:28", ":16:22", ":17:22"})
    EXPECT_NE(r.Diagnostics.find(std::string(at) + ": error: 'p' " + later),
              std::string::npos)
        << at << "\n"
        << r.Diagnostics;
  expectErrors(
      r, {(":11:43: error: 'q' " + later).c_str(),
          ":12:24: error: 'p' is passed to 'view' parameter 'p' of 'pair'; a "
          "later argument of 'pair' cannot pass it to 'inout' parameter 'p' of "
          "'bumpP'",
          ":12:35: error: 'q' is passed to 'view' parameter 'q' of 'pair'; a "
          "later argument of 'pair' cannot pass it to 'inout' parameter 'p' of "
          "'bumpP'"});
}

// One write, one error.  A call in a later argument that reports a conflict
// of its own arguments (`two(p, p.a)`) is not reported again for it, only
// for its other writes.  And an `inout` result passed on to an `inout`
// parameter is written by the call that returns it, which is the error, not
// the call it is passed to: a method that is not a `view fn` (a struct's or
// a class's), or a function through its `inout` parameter.  (Each was a
// second error, at the same place or at the argument around it.)
TEST(ParamMode, ACallInALaterArgumentIsReportedOncePerWrite) {
  auto r = semaCheck(R"(struct P { a: int; }
struct H { p: P; q: P; }
impl H { fn slotP() -> inout P { return self.p; } }
class K { p: P; fn slotK() -> inout P { return self.p; } }
fn slot(h: inout H) -> inout P { return h.p; }
fn look(p: view P, x: int) -> int => p.a;
fn bump(p: inout P) -> int { p = P(9); return 0; }
fn bumpN(n: inout int) -> int { n = 9; return 0; }
fn two(p: inout P, n: inout int) -> int => 0;
fn main() -> int {
  p = P(1);
  h = H(P(1), P(2));
  k = K(P(1));
  n = look(p, two(p, p.a));
  n = look(h.p, bump(h.slotP()));
  n = look(h.p, bumpN(h.slotP().a));
  n = look(k.p, bump(k.slotK()));
  n = look(h.p, bump(slot(h)));
  n = look(h.q, look(h.p, bump(h.slotP())));
  return n;
})");
  EXPECT_EQ(r.ErrorCount, 7u) << r.Diagnostics;
  const std::string slotP =
      "'h' contains 'h.p', passed to 'view' parameter 'p' of 'look'; a later "
      "argument of 'look' cannot call 'slotP' on it (not a 'view fn')";
  for (const char *at : {":15:22", ":16:23", ":19:32"})
    EXPECT_NE(r.Diagnostics.find(std::string(at) + ": error: " + slotP),
              std::string::npos)
        << at << "\n"
        << r.Diagnostics;
  expectErrors(
      r,
      {":14:22: error: 'p.a' is part of 'p', passed to 'inout' parameter 'p'; "
       "it cannot also be passed to 'inout' parameter 'n'",
       ":14:19: error: 'p' is passed to 'view' parameter 'p' of 'look'; a "
       "later argument of 'look' cannot pass it to 'inout' parameter 'p' of "
       "'two'",
       ":17:22: error: 'k' contains 'k.p', passed to 'view' parameter 'p' of "
       "'look'; a later argument of 'look' cannot call 'slotK' on it (not a "
       "'view fn')",
       ":18:27: error: 'h' contains 'h.p', passed to 'view' parameter 'p' of "
       "'look'; a later argument of 'look' cannot pass it to 'inout' "
       "parameter 'h' of 'slot'"});
}

// What the rule does not see stays accepted, the caller copying what a later
// argument may write (docs/language/02-basics.md, "Evaluation order"): a
// plain parameter, a write through an object (by a function, a function
// value, a virtual method) or by a lambda's body, a borrow after the write
// or in a call that has returned, another variable or another field, and
// `==`'s operands.
TEST(ParamMode, ALaterArgumentMayWriteWhatNoBorrowNames) {
  auto r = semaCheck(R"(struct P { a: int; }
impl P { view fn show(x: int) -> int => self.a; }
struct Two { x: P; y: P; }
class C { p: P; xs: int[3]; }
class V { n: int; view fn write(c: C) -> int => 0; }
class W : V {
  n2: int;
  view fn write(c: C) -> int { c.xs = [9, 9, 9]; return 0; }
}
fn sum(xs: view int[], k: int) -> int => xs[0] + k;
fn bumpP(p: inout P) -> int { p = P(9); return 0; }
fn look(p: view P, x: int) -> int => p.a;
fn look2(x: int, p: view P) -> int => p.a;
fn first(p: P, x: int) -> int => p.a;
fn add(a: int, b: int) -> int => a + b;
fn resetC(c: C) -> int { c.p = P(9); c.xs = [9, 9, 9]; return 0; }
fn applyP(f: (inout P) -> int) -> int { q = P(0); return f(q); }
fn gs(xs: view int[], f: () -> int) -> int => sum(xs, f());
fn gv(p: view P, c: C) -> int => look(p, resetC(c));
fn gw(xs: view int[], v: V, c: C) -> int => sum(xs, v.write(c));
fn main() -> int {
  p = P(1);
  q = P(2);
  two = Two(P(1), P(2));
  c = C(P(1), [1, 2, 3]);
  f = fn() -> int => resetC(c);
  n = first(p, bumpP(p));
  n = first(c.p, resetC(c));
  n = look(c.p, resetC(c));
  n = sum(c.xs, resetC(c));
  n = c.p.show(resetC(c));
  n = look(c.p, f());
  n = gs(c.xs, f) + gv(c.p, c) + gw(c.xs, W(0, 0), c);
  n = look2(bumpP(p), p);
  n = add(look(p, 0), bumpP(p));
  n = look(p, applyP(fn(p: inout P) -> int => bumpP(p)));
  n = look(two.x, bumpP(two.y));
  n = look(p, bumpP(q));
  b = p == P(bumpP(p) + 9);
  return n + (if b then 1 else 0);
})");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
}

// The samples/sema programs of the shapes samples/codegen/78 no longer runs,
// each rejected with exactly the error its ERROR line names.
TEST(ParamMode, TheSamplesOfABorrowALaterArgumentWrites) {
  struct Sample {
    const char *Source;
    const char *Error;
  };
  const Sample samples[] = {
      {R"(// ERROR: 'p' is passed to 'view' parameter 'p' of 'look'; a later argument of 'look' cannot pass it to 'inout' parameter 'p' of 'bump'
// A `view` parameter is borrowed until the call runs, so a call in a later
// argument cannot write it (docs/language/03-functions.md, "Exclusivity").
struct P {
  a: int;
}

fn look(p: view P, x: int) -> int => p.a;

fn bump(p: inout P) -> int {
  p = P(9);
  return 0;
}

fn main() -> int {
  p = P(1);
  return look(p, bump(p));
}
)",
       ":17:23: error: 'p' is passed to 'view' parameter 'p' of 'look'; a "
       "later argument of 'look' cannot pass it to 'inout' parameter 'p' of "
       "'bump'"},
      {R"(// ERROR: 'arr' is passed to 'view' parameter 'xs' of 'total'; a later argument of 'total' cannot pass it to 'inout' parameter 'a' of 'bumpA'
// A slice parameter is borrowed until the call runs, so a call in a later
// argument cannot write its array (docs/language/03-functions.md,
// "Exclusivity").
fn total(xs: view int[], x: int) -> int => xs[0];

fn bumpA(a: inout int[3]) -> int {
  a = [9, 9, 9];
  return 0;
}

fn main() -> int {
  arr = [1, 2, 3];
  return total(arr, bumpA(arr));
}
)",
       ":14:27: error: 'arr' is passed to 'view' parameter 'xs' of 'total'; a "
       "later argument of 'total' cannot pass it to 'inout' parameter 'a' of "
       "'bumpA'"},
      {R"(// ERROR: 'counter' is the receiver of 'view fn get'; an argument of 'get' cannot pass it to 'inout' parameter 'c' of 'bumpCounter'
// The receiver of a struct's `view fn` is borrowed until the method runs,
// so a call in its arguments cannot write it
// (docs/language/03-functions.md, "Exclusivity").
struct Counter {
  k: int;
}

impl Counter {
  view fn get(x: int) -> int => self.k;
}

fn bumpCounter(c: inout Counter) -> int {
  c = Counter(9);
  return 0;
}

fn main() -> int {
  counter = Counter(1);
  return counter.get(bumpCounter(counter));
}
)",
       ":20:34: error: 'counter' is the receiver of 'view fn get'; an argument "
       "of 'get' cannot pass it to 'inout' parameter 'c' of 'bumpCounter'"},
  };
  for (const Sample &s : samples) {
    auto r = semaCheck(s.Source);
    EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
    expectErrors(r, {s.Error});
  }
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
  auto r = semaCheck(R"(import ::io; class Counter {
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
  io::println(v.toString() + s + Str(s.len() + a.len() + a[0] + v.n));
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
  auto r = semaCheck(R"(import ::io; class Counter { n: int; }
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
  io::println(v);
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
        ":29:11: error: 's' is a 'view' local; " + kPass + "Str' is not one"})
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 11u) << r.Diagnostics;
  // An `int` read through a `view` (`k`, `v.n`) is passed to a by-value
  // parameter as a copy, and a literal holds one of it.
  for (const char *line :
       {":26:", ":27:", ":30:", ":31:", ":32:", ":33:", ":34:"})
    EXPECT_EQ(r.Diagnostics.find(line), std::string::npos) << line << "\n"
                                                           << r.Diagnostics;
}

// An array or tuple literal holds its elements: a `view` of a value type is
// copied into it, one that shares what it holds cannot be an element, at any
// depth (each literal reports its own), as it cannot be stored.
TEST(LocalBorrow, LiteralsTakeNoSharingViewElements) {
  auto ok = semaCheck(R"(class Named { name: Str; n: int; }
impl Named {
  view fn pair() -> (int, int) { return (self.n, self.name.len()); }
}
fn arr(n: view int) -> int[2] { return [n, 1]; }
fn tup(s: view Str) -> (int, Str) { return (s.len(), Str(s.len())); }
fn main() -> int {
  k: view = 3;
  return arr(k)[0] + tup("s").0 + [k; 2][1] + (k, (k, 1)).1.0;
})");
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;
  auto r = semaCheck(R"(class Node { n: int; }
class Named { name: Str; n: int; }
impl Named {
  view fn pair() -> (Str, int) { return (self.name, self.n); }
  view fn names() -> Str[2] { return [self.name, "x"]; }
  view fn me() -> Named[1] { return [self]; }
}
fn arr(s: view Str) -> Str[2] { return [s, "x"]; }
fn rep(s: view Str) -> Str[3] { return [s; 3]; }
fn nest(s: view Str) -> ((Str, int), (Str, Str)) { return ((s, 1), (s, "y")); }
fn main() -> int {
  v: view = Node(0);
  ys: Node[1] = [v];
  w = [(1, v)];
  return 0;
})");
  const std::string kStore =
      "; it cannot be stored, only read or passed to a 'view' parameter";
  for (const std::string &diag :
       {":4:42: error: 'self' is read-only in 'view fn pair'" + kStore,
        ":5:39: error: 'self' is read-only in 'view fn names'" + kStore,
        ":6:38: error: 'self' is read-only in 'view fn me'" + kStore,
        ":8:41: error: 's' is a 'view' parameter" + kStore,
        ":9:41: error: 's' is a 'view' parameter" + kStore,
        ":10:61: error: 's' is a 'view' parameter" + kStore,
        ":10:69: error: 's' is a 'view' parameter" + kStore,
        ":13:18: error: 'v' is a 'view' local" + kStore,
        ":14:12: error: 'v' is a 'view' local" + kStore})
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 9u) << r.Diagnostics;
}

// A `view` parameter stays one too: it can only be passed on to a `view`
// parameter.
TEST(ParamMode, ViewParametersOnlyPassToView) {
  // A `view` of a reference (an object, a `Str`, a value holding one) goes
  // only to a `view` parameter; a value type leaves a `view` as a copy.
  auto r = semaCheck(R"(import ::io; class Counter { n: int; }
struct P { x: int; name: Str; }
fn twice(n: int) -> int { return 2 * n; }
fn look(n: view int) -> int { return n; }
fn keep(c: Counter) -> int { return c.n; }
fn peek(c: view Counter) -> int { return c.n; }
fn tag(s: Str) -> int { return s.len(); }
fn hold(p: P) -> int { return p.x; }
fn f(n: view int, c: view Counter, s: view Str, p: view P) -> int {
  io::println(Str(n));
  m = n;
  return twice(n) + look(n) + twice(m) + keep(c) + peek(c) + tag(s) + hold(p) + twice(c.n) + twice(p.x);
}
fn main() -> int { return f(1, Counter(1), "s", P(1, "p")); })");
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
  expectErrors(r, {":12:47: error: 'c' is a 'view' parameter; it can only be "
                   "passed to a 'view' parameter, and parameter 1 of 'keep' "
                   "is not one",
                   ":12:66: error: 's' is a 'view' parameter; it can only be "
                   "passed to a 'view' parameter, and parameter 1 of 'tag' "
                   "is not one",
                   ":12:76: error: 'p' is a 'view' parameter; it can only be "
                   "passed to a 'view' parameter, and parameter 1 of 'hold' "
                   "is not one"});
}

TEST(ParamMode, ValueTypesLeaveAViewAsCopies) {
  // An `int` (or a struct, tuple or fixed-size array of value types) read
  // through any `view` -- a parameter, a local, a `view fn`'s `self`, a
  // `view` result, an element of a `view` array -- can be passed to a
  // by-value parameter: the parameter is a copy, as an assignment of it
  // is.  What holds a reference still cannot.
  auto r = semaCheck(R"(
struct P { x: int; y: int; }
struct Q { x: int; name: Str; }
class C {
  n: int; p: P; q: Q; name: Str;
  view fn twiceN(f: (int) -> int) -> int { return f(self.n); }
  view fn twiceP(g: (P) -> int) -> int { return g(self.p); }
  view fn num() -> view int { return self.n; }
  view fn bad(h: (Str) -> int, k: (Q) -> int) -> int { return h(self.name) + k(self.q); }
}
fn twice(n: int) -> int { return 2 * n; }
fn sumP(p: P) -> int { return p.x + p.y; }
fn sumQ(q: Q) -> int { return q.x; }
fn total(xs: view int[], f: (int) -> int) -> int {
  t = 0; i = 0;
  while i < xs.len() { t = t + f(xs[i]); i = i + 1; }
  return t;
}
fn main() -> int {
  c = C(1, P(1, 2), Q(1, "q"), "c");
  v: view = c;
  k: view = 3;
  dbl = fn(x: int) -> int { return x * 2; };
  n = twice(k) + twice(v.n) + sumP(v.p) + twice(v.p.x) + twice(c.num()) +
      c.twiceN(dbl) + c.twiceP(sumP) + total([1, 2, 3], dbl) + twice(v.q.x);
  n = n + sumQ(v.q);
  return n;
}
)");
  EXPECT_FALSE(r.Ok);
  for (const char *diag :
       {"'self' is read-only in 'view fn bad'; it can only be passed to a "
        "'view' parameter, and parameter 1 of 'h' is not one",
        "'self' is read-only in 'view fn bad'; it can only be passed to a "
        "'view' parameter, and parameter 1 of 'k' is not one",
        "'v' is a 'view' local; it can only be passed to a 'view' parameter, "
        "and parameter 1 of 'sumQ' is not one"})
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
}

// Exclusivity: while an `inout` local is live, the variable it names cannot
// be used; while a `view` local of a variable is live, the variable cannot
// change (a value type can still be passed on as a copy).  A borrow lives
// until its last use in its block, through a whole loop that uses it, and a
// borrow of a borrow keeps the first variable borrowed.
TEST(LocalBorrow, Exclusivity) {
  auto r = semaCheck(R"(import ::io; class Counter {
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
  io::println(Str(v.n));
  c.tick();
  m = 5;
  w: view = m;
  t = twice(m);
  m = 6;
  io::print(Str(w));
  m = 7;
  i = 0;
  a = 1;
  ai: inout = a;
  while i < 3 { a = a + 1; ai = ai + 1; i = i + 1; }
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
        std::string(":35:21: error: 'a' is borrowed by 'inout' local 'ai' "
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

// A statement uses the names it holds, no others: a lambda that does not
// capture a borrow and a call through a function value keep no unrelated
// borrow live, and a call through a function-valued borrow uses it.
TEST(LocalBorrow, LambdasAndFunctionValueCallsUseWhatTheyName) {
  auto ok = semaCheck(R"(import ::io;
class Counter { n: int; }
impl Counter {
  fn __init__() { self.n = 0; }
  fn tick() { self.n = self.n + 1; }
}
fn twice(n: int) -> int { return n * 2; }
fn main() -> int {
  k = 1;
  x: inout = k;
  x = 5;
  io::println(Str(k));
  double = fn(n: int) -> int { return n * 2; };
  fs = [twice];
  y: inout = k;
  y = 6;
  k = 7;
  io::println(Str(double(k)) + Str(fs[0](k)));
  c = Counter();
  v: view = c;
  io::println(Str(v.n));
  c.tick();
  g = fn() -> int { return 1; };
  return g();
})");
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;
  auto r = semaCheck(R"(import ::io;
fn sq(x: int) -> int => x * x;
fn neg(x: int) -> int => -x;
fn main() -> int {
  f = sq;
  v: view = f;
  f = neg;
  io::println(Str(v(3)));
  g = sq;
  s: inout = g;
  g = neg;
  s(3);
  return 0;
})");
  expectErrors(r, {":7:3: error: 'f' is viewed by 'view' local 'v' until 'v' "
                   "is last used; it cannot be assigned",
                   ":11:3: error: 'g' is borrowed by 'inout' local 's' until "
                   "'s' is last used"});
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
}

// A lambda that captures a borrowed variable uses it, and copies the
// reference it holds: an error while the borrow is live, as `y = k` and
// `g = h` are.  A value type's copy is only read; after the borrow's last
// use the capture is fine.
TEST(LocalBorrow, CapturingABorrowedVariable) {
  auto r = semaCheck(R"(import ::io;
class Node { name: Str; }
class Holder { n: Node; }
fn main() -> int {
  k = 1;
  x: inout = k;
  f = fn() -> int { return k; };
  x = 5;
  h = Holder(Node("a"));
  v: view = h.n;
  g = fn() { h.n = Node("z"); };
  io::println(v.name);
  m = 2;
  w: view = m;
  r = fn() -> int { return m; };
  io::println(Str(w + r()));
  late = fn() -> int { return k; };
  return f() + late();
})");
  expectErrors(r, {":7:28: error: 'k' is borrowed by 'inout' local 'x' until "
                   "'x' is last used; a function cannot capture it",
                   ":11:14: error: 'h' is viewed by 'view' local 'v' until 'v' "
                   "is last used; a function cannot capture it (it could "
                   "change it through its copy)"});
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
}

// A borrow is live to the last statement that uses it, by the name that
// means it there: a binder of the same name further on is another variable
// and keeps it live no longer.  So the borrowed variable can be used after
// the borrow's last use, ahead of a lambda's parameter, a `for` loop's
// variable, a `match` arm's binding or a declaration in an inner block of
// that name.  (Each kept the borrow live: `'k' is borrowed by 'inout' local
// 'r' until 'r' is last used`.)
TEST(LocalBorrow, ABinderOfTheSameNameIsAnotherVariable) {
  auto program = [](const std::string &later) {
    return R"(import ::io;
fn main() -> int {
  k = 1;
  o: int? = 5;
  r: inout = k;
  r = 5;
  io::println(Str(k));
  )" + later +
           R"(
  return 0;
})";
  };
  for (const char *later :
       {"f = fn(r: int) => r * 2; io::println(Str(f(3)));",
        "{ r: int = 7; io::println(Str(r)); }",
        "{ let r = 7; io::println(Str(r)); }",
        "{ r: int, q: int = (7, 8); io::println(Str(r + q)); }",
        "for r in 0..2 { io::println(Str(r)); }",
        "match o { r: int { io::println(Str(r)); } None { } }",
        "if k > 0 { while k > 4 { r: int = 3; io::println(Str(r)); k = 0; } }",
        "f = fn(n: int) -> int { r: int = n; return r; };",
        "g = fn(q: int) -> int { for r in 0..q { } return q; };"}) {
    auto ok = semaCheck(program(later));
    EXPECT_TRUE(ok.Ok) << later << "\n" << ok.Diagnostics;
  }
  // A `view` local, and a `match` arm's binding that borrows the subject.
  auto ok = semaCheck(R"(import ::io;
enum E { A, R(w: int) }
class B { e: E; }
class N { s: Str; }
fn main() -> int {
  s = "orig";
  v: view = s;
  io::println(v);
  s = "changed";
  g = fn(v: int) => v + 1;
  b = B(E::R(1));
  match b.e {
    r: inout R {
      r.w += 100;
      io::println(b.e);
      h = fn(r: int) -> int => r * 2;
      for r in 0..2 { }
      { let r = 7; }
    }
    A { }
  }
  on: N? = N("a");
  match on {
    n: view N {
      io::println(n.s);
      on = N("b");
      m = fn(n: int) -> int => n + 1;
    }
    None { }
  }
  return g(1);
})");
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;
  // A use of the borrow before a binder hides it, or where none does, is
  // one: in a lambda, a loop's range or body, another arm, a declaration's
  // initializer, an inner block before its declaration, and a destructuring
  // target without a type, which assigns the borrow.
  const char *const kBorrowed = ":7:19: error: 'k' is borrowed by 'inout' "
                                "local 'r' until 'r' is last used";
  for (const char *later :
       {"{ io::println(Str(r)); r: int = 7; }",
        "{ r: int = r + 1; io::println(Str(r)); }",
        "{ r: int, q: int = (r, 8); io::println(Str(q)); }",
        "{ r, q = (7, 8); io::println(Str(q)); }", "for i in 0..r { }",
        "for q in 0..2 { io::println(Str(r)); }",
        "match o { r: int { } None { io::println(Str(r)); } }",
        "while o == None { q: int = r; }"}) {
    auto r = semaCheck(program(later));
    expectErrors(r, {kBorrowed});
    EXPECT_EQ(r.ErrorCount, 1u) << later << "\n" << r.Diagnostics;
  }
  // A lambda that uses it captures the borrow, an error of its own.
  for (const char *later :
       {"f = fn(q: int) => r * q;",
        "f = fn(q: int) -> int { { r: int = 2; } return r * q; };",
        "{ g = fn() -> int { return r; }; r: int = 1; }"}) {
    auto r = semaCheck(program(later));
    expectErrors(r, {kBorrowed, "error: 'r' is an 'inout' parameter or local; "
                                "a function cannot capture a borrow"});
    EXPECT_EQ(r.ErrorCount, 2u) << later << "\n" << r.Diagnostics;
  }
}

// A `match` arm's `n: view T` is a `view` local of the subject, whatever the
// subject is: nothing changes through it, it passes on only to `view`
// parameters, and the subject's variable cannot change until its last use.
TEST(LocalBorrow, ViewArms) {
  auto r = semaCheck(R"(import ::io; class Animal {
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
      io::println(Str(d.get()));
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

// An `inout` arm cannot change a value inside an optional yet: an enum's,
// held like a primitive's, as much as an `int`'s.
TEST(LocalBorrow, InoutArmOnAnOptionalEnum) {
  auto r = semaCheck(R"(enum Suit { Hearts, Spades }
fn main() -> int {
  s: Suit? = Suit::Hearts;
  match s { k: inout Suit { k = Suit::Spades; } None { } }
  return 0;
})");
  expectErrors(r, {":4:9: error: 'inout' arm 'k' cannot change the 'Suit' "
                   "inside an optional yet"});
}
