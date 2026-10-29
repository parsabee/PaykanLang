// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Codegen tests: a value held in memory (a struct, a tuple, a fixed-size
// array, a tagged union) is the value it had where it was evaluated
// (docs/language/02-basics.md, "Evaluation order").  Passed by address, its
// callee copies it (or reads it where it is) only once the later arguments
// have run, and `==` compares only once its right operand has; an operand
// after it that writes its storage, through an `inout` argument, a struct's
// method that may change it, or a call given the object it is in or a
// closure that reaches it, must not change the value used.  Each program
// runs as the driver runs one (the moves at last use, the lending and the
// reference-count elimination) and without the passes, under the tracking
// allocator: the copy the caller takes is released once it is used.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

#include <string>

using namespace paykan;
using namespace paykan::test;

namespace {

/// The declarations every program here shares.
const std::string kPrelude = R"(import ::io;
struct P { a: int; }
struct Named { name: Str; n: int; }
class C { p: P; named: Named; n: int; t: (int, int); }
fn first(p: P, x: int) -> int => p.a;
fn reset(c: C) -> int {
  c.p = P(9);
  c.named = Named("z" + Str(9), 9);
  c.t = (9, 9);
  return 0;
}
fn bump(p: inout P) -> int { p = P(9); return 0; }
fn bumpT(t: inout (int, int)) -> int { t = (9, 9); return 0; }
fn fresh() -> C => C(P(1), Named("a" + Str(1), 1), 1, (1, 2));
)";

/// Run @p body (the rest of the program, after kPrelude) with and without
/// the driver's passes: each run prints @p expected and frees everything.
void expectOutput(const char *label, const std::string &body,
                  const std::string &expected) {
  const std::string source = kPrelude + body;
  {
    LeakGuard g;
    pir::Statistics stats;
    auto r = compilePassesAndRun(source, stats);
    ASSERT_TRUE(r.CompileOk) << label << ": " << r.StdErr;
    EXPECT_EQ(r.ExitCode, 0) << label << ": " << r.StdErr;
    EXPECT_EQ(r.StdOut, expected) << label;
    g.expectNoLeaks(label);
  }
  LeakGuard g;
  auto r = compileAndRun(source);
  ASSERT_TRUE(r.CompileOk) << label << ": " << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << label << ": " << r.StdErr;
  EXPECT_EQ(r.StdOut, expected) << label << " (no passes)";
  g.expectNoLeaks(label);
}

} // namespace

// The three shapes of the finding: a struct read through an object that a
// later argument's call replaces, a local a later `inout` argument
// replaces, and the left operand of `==`.
TEST(EvalOrder, MemArgumentIsTheValueRead) {
  expectOutput("field", R"(fn main() -> int {
  c = fresh();
  io::println(Str(first(c.p, reset(c))));
  p = P(1);
  io::println(Str(first(p, bump(p))));
  q = P(1);
  io::println(Str(q == P(bump(q) + 9)));
  q = P(1);
  io::println(Str(q != P(bump(q) + 9)));
  c = fresh();
  io::println(Str(c.p == P(reset(c) + 9)));
  return 0;
})",
               "1\n1\nFalse\nTrue\nFalse\n");
}

// A tuple, a fixed-size array and a tagged union, through `inout`.
TEST(EvalOrder, EveryValueHeldInMemory) {
  expectOutput("kinds", R"(enum E { A(n: int), B }
fn firstT(t: (int, int), x: int) -> int => t.0;
fn firstA(a: int[3], x: int) -> int => a[0];
fn bumpA(a: inout int[3]) -> int { a = [9, 9, 9]; return 0; }
fn tag(e: E, x: int) -> Str => e.toString();
fn bumpE(e: inout E) -> int { e = E::B; return 0; }
fn main() -> int {
  t = (1, 2);
  io::println(Str(firstT(t, bumpT(t))));
  a = [1, 2, 3];
  io::println(Str(firstA(a, bumpA(a))));
  e = E::A(1);
  io::println(tag(e, bumpE(e)));
  e = E::A(1);
  io::println(Str(e == (if bumpE(e) == 0 then E::B else E::B)));
  c = fresh();
  io::println(Str(firstT(c.t, reset(c))));
  t = (1, 2);
  io::println(Str(t == (bumpT(t) + 1, 2)));
  return 0;
})",
               "1\n1\nA(n: 1)\nFalse\n1\nTrue\n");
}

// Through a function value: `g(c.p, k())`, a closure that reaches the object,
// and `h(t, bump(t))`; and a function value held in a field.
TEST(EvalOrder, FunctionValueCalls) {
  expectOutput("function values",
               R"(fn firstT(t: (int, int), x: int) -> int => t.0;
class Holder { f: (P, int) -> int; }
fn main() -> int {
  c = fresh();
  g = first;
  k = fn() -> int => reset(c);
  io::println(Str(g(c.p, k())));
  h = firstT;
  t = (1, 2);
  io::println(Str(h(t, bumpT(t))));
  c = fresh();
  io::println(Str(first(c.p, k())));
  o = Holder(first);
  c = fresh();
  io::println(Str(o.f(c.p, reset(c))));
  return 0;
})",
               "1\n1\n1\n1\n");
}

// A method's arguments, a constructor's (of a class and of a struct, which
// take a value held in memory by address), `__super__`'s, and the receiver
// of a struct's `view fn`; a method that may change `self` runs on the
// receiver's storage, as an `inout` argument does.
TEST(EvalOrder, MethodsConstructorsAndReceivers) {
  expectOutput("methods", R"(class Reader {
  base: int;
  view fn first(p: P, x: int) -> int => p.a + self.base;
}
struct Counter { k: int; }
impl Counter {
  view fn get(x: int) -> int => self.k;
  fn take(x: int) -> int => self.k;
}
fn bumpCounter(c: inout Counter) -> int { c = Counter(9); return 0; }
class Shown { p: P; x: int; }
struct Made { p: P; x: int; }
class Base { p: P; }
impl Base { fn __init__(p: P, x: int) { self.p = p; } }
class Derived : Base { y: int; }
impl Derived {
  fn __init__(c: C) {
    __super__(c.p, reset(c));
    self.y = 0;
  }
}
struct Pair { p: P; q: P; }
impl Pair { fn bumpQ() -> int { self.q = P(7); return 0; } }
fn main() -> int {
  r = Reader(0);
  c = fresh();
  io::println(Str(r.first(c.p, reset(c))));
  k = Counter(1);
  io::println(Str(k.get(bumpCounter(k))));
  k = Counter(1);
  io::println(Str(k.take(bumpCounter(k))));
  c = fresh();
  io::println(Str(Shown(c.p, reset(c)).p.a));
  c = fresh();
  io::println(Str(Made(c.p, reset(c)).p.a));
  c = fresh();
  io::println(Str(Derived(c).p.a));
  pair = Pair(P(1), P(2));
  io::println(Str(first(pair.q, pair.bumpQ())));
  return 0;
})",
               "1\n1\n9\n1\n1\n1\n2\n");
}

// A `view` parameter and a slice are given the value read, and so is an
// argument nested in another call's arguments, or given through an
// `inout` result.
TEST(EvalOrder, ViewsSlicesAndNesting) {
  expectOutput("views", R"(fn look(p: view P, x: int) -> int => p.a;
fn total(xs: view int[], x: int) -> int => xs[0] + xs[2];
fn bumpA(a: inout int[3]) -> int { a = [9, 9, 9]; return 0; }
fn apply(f: () -> int) -> int => f();
struct Slot { p: P; q: P; }
impl Slot { fn slot() -> inout P { return self.p; } }
fn main() -> int {
  p = P(1);
  io::println(Str(look(p, bump(p))));
  a = [1, 2, 3];
  io::println(Str(total(a, bumpA(a))));
  c = fresh();
  io::println(Str(first(c.p, first(c.p, reset(c)))));
  c = fresh();
  io::println(Str(first(c.p, apply(fn() -> int => reset(c)))));
  s = Slot(P(1), P(2));
  io::println(Str(first(s.slot(), bump(s.p))));
  return 0;
})",
               "1\n4\n1\n1\n1\n");
}

// A slice parameter passed on to another sees the elements read too, when a
// later argument writes them: copied by its length, which only the running
// program knows, its references kept and let go of after the call, through
// a function, a method, a constructor and a function value alike.
TEST(EvalOrder, ASlicePassedOnIsTheValueRead) {
  expectOutput("slices", R"(class A { xs: int[3]; ss: Str[3]; }
class K {
  n: int;
  view fn pick(ss: view Str[], x: int) -> Str => ss[2] + "";
}
class Kept { s: Str; }
impl Kept { fn __init__(ss: view Str[], x: int) { self.s = ss[1] + ""; } }
fn total(xs: view int[], x: int) -> int => xs[0] + xs[1] + xs[2];
fn names(ss: view Str[], x: int) -> Str => ss[0] + ss[2];
fn resetA(a: A) -> int {
  a.xs = [9, 9, 9];
  a.ss = ["z" + Str(1), "z" + Str(2), "z" + Str(3)];
  return 0;
}
fn ints(xs: view int[], a: A) -> int => total(xs, resetA(a));
fn strs(ss: view Str[], a: A) -> Str => names(ss, resetA(a));
fn method(ss: view Str[], a: A) -> Str => K(0).pick(ss, resetA(a));
fn made(ss: view Str[], a: A) -> Str => Kept(ss, resetA(a)).s;
fn value(ss: view Str[], a: A) -> Str {
  f = names;
  return f(ss, resetA(a));
}
fn freshA() -> A => A([1, 2, 3], ["a" + Str(1), "b", "c" + Str(3)]);
fn main() -> int {
  a = freshA();
  io::println(Str(ints(a.xs, a)));
  a = freshA();
  io::println(strs(a.ss, a));
  a = freshA();
  io::println(method(a.ss, a));
  a = freshA();
  io::println(made(a.ss, a));
  a = freshA();
  io::println(value(a.ss, a) + " " + a.ss[0]);
  return 0;
})",
               "6\na1c3\nc3\nb\na1c3 z1\n");
}

// The copy holds its own references: what a later call lets go of from the
// storage stays alive until the callee has used the copy, and the copy's
// references are released after the call.
TEST(EvalOrder, CopyKeepsItsReferences) {
  expectOutput("references",
               R"(fn label(v: Named, x: int) -> Str => v.name + Str(v.n);
fn bumpNamed(v: inout Named) -> int { v = Named("y" + Str(8), 8); return 0; }
fn main() -> int {
  c = fresh();
  io::println(label(c.named, reset(c)));
  v = Named("b" + Str(2), 2);
  io::println(label(v, bumpNamed(v)));
  c = fresh();
  io::println(Str(c.named == Named("a" + Str(1), reset(c) + 1)));
  io::println(c.named.name);
  return 0;
})",
               "a11\nb22\nTrue\nz9\n");
}

// What worked before still does: a number or a string is read where it is
// evaluated, a deeper chain whose object a call replaces keeps the object
// it read from, and a later call that writes another part, or nothing the
// argument is in, leaves the value alone.
TEST(EvalOrder, ControlsThatAlreadyWorked) {
  expectOutput("controls", R"(class D { c: C; }
fn firstN(n: int, x: int) -> int => n;
fn shout(s: inout Str) -> Str { s = s + "!"; return s; }
fn swapIn(d: D) -> int { d.c = fresh(); d.c.p = P(5); return 0; }
fn bumpN(n: inout int) -> int { n = 9; return 0; }
fn reads(c: C) -> int => c.p.a;
struct Pair { p: P; q: P; }
fn main() -> int {
  c = fresh();
  io::println(Str(firstN(c.n, bumpN(c.n))));
  s = "hi";
  io::println(s + shout(s));
  d = D(fresh());
  io::println(Str(first(d.c.p, swapIn(d))));
  pair = Pair(P(1), P(2));
  io::println(Str(first(pair.q, bump(pair.p))));
  c = fresh();
  io::println(Str(first(c.p, bumpN(c.n))));
  c = fresh();
  io::println(Str(first(c.p, reads(c))));
  p = P(1);
  io::println(Str(bump(p) + first(p, 0)));
  return 0;
})",
               "1\nhihi!\n1\n2\n1\n1\n9\n");
}
