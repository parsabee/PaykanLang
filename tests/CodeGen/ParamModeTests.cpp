// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen tests: `view` and `inout` parameters, and the PIR address ops an
// `inout` parameter lowers to (local.addr, field.addr, ptr.load, ptr.store).

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// -- PIR address ops

// A callee reads and writes its caller's locals and an object's fields in
// place through the addresses it is given: every scalar type, a local and a
// field.
TEST(ParamMode, AddressOpsReadAndWriteTheCallersSlots) {
  LeakGuard guard;
  auto r = compileAndRunPIR(R"(module "t"
class C {
  field n: i64
  field f: f64
  vtable {
    destroy = @C.destroy : (obj) -> void
  }
}

fn @C.destroy(%self: obj) -> void {
  free %self
  ret
}

fn @bump(%n: ptr, %f: ptr, %b: ptr, %c: ptr) -> void {
  %v = ptr.load i64, %n
  %w = add %v, 1
  ptr.store %n, %w
  %x = ptr.load f64, %f
  %y = add %x, 1.5
  ptr.store %f, %y
  %bv = ptr.load bool, %b
  %nb = not %bv
  ptr.store %b, %nb
  %cv = ptr.load char, %c
  %ci = cast %cv to i64
  %cn = add %ci, 1
  %cc = cast %cn to char
  ptr.store %c, %cc
  ret
}

fn @main() -> i64 {
  local %k: i64
  local %g: f64
  local %t: bool
  local %ch: char
  store %k, 1
  store %g, 1.5
  store %t, false
  store %ch, 'a'
  %o = new C
  field.store %o, C.n, 40
  %pk = local.addr %k
  %pg = local.addr %g
  %pt = local.addr %t
  %pc = local.addr %ch
  call @bump(%pk, %pg, %pt, %pc)
  %pn = field.addr %o, C.n
  %pf = field.addr %o, C.f
  call @bump(%pn, %pf, %pt, %pc)
  %k2 = load %k
  %g2 = load %g
  %t2 = load %t
  %ch2 = load %ch
  %n2 = field.load %o, C.n
  %f2 = field.load %o, C.f
  free %o
  %gi = ftoi %g2
  %fi = ftoi %f2
  %ci2 = cast %ch2 to i64
  %c2 = sub %ci2, 97
  %tb = select %t2, 10, 0
  %s1 = add %k2, %n2
  %s2 = add %s1, %gi
  %s3 = add %s2, %fi
  %s4 = add %s3, %c2
  %s5 = add %s4, %tb
  ret %s5
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  // k 2 + n 41 + g 3 + f 1 + 'c' - 'a' 2 + t false 0.  The sum stays below
  // 128: the C backend's runner reads a higher exit status as a signal.
  EXPECT_EQ(r.ExitCode, 49) << r.StdErr;
  guard.expectNoLeaks("address ops");
}

// -- `view` parameters

// A `view` parameter is a read-only copy: of every value type, on a
// function, a constructor, a method, an override and a template's ordinary
// parameter, with any argument of its type.
TEST(ParamMode, ViewParametersArePassedByValue) {
  LeakGuard guard;
  auto r = compileAndRun(R"(
    enum Color { Red, Green }
    class Counter {
      n: int;
      fn __init__(start: view int) { self.n = start; }
      fn add(k: view int) -> int { self.n = self.n + k; return self.n; }
    }
    class Twice : Counter {
      fn __init__(s: view int) { __super__(s); }
      fn add(k: view int) -> int { self.n = self.n + 2 * k; return self.n; }
    }
    fn scale(x: view float, by: view float) -> float { y = x * by; return y; }
    fn name(c: view Color, loud: view bool, end: view char) -> Str {
      s = "red";
      if (c == Color::Green) { s = "green"; }
      if (loud) { s = s + Str(end); }
      return s;
    }
    fn at<T>(xs: T[], i: view int) -> T { return xs[i]; }
    fn main() -> int {
      let k = 3;
      c: Counter = Twice(k);
      println(Str(c.add(k)) + " " + Str(scale(1.5, 2)) + " " +
              name(Color::Green, True, '!') + " " + Str(at([4, 5], 1)) +
              " " + Str(k));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "9 3 green! 5 3\n");
  guard.expectNoLeaks("view parameters");
}

// -- `inout` parameters

// An `inout` parameter is the caller's variable: every value type, passing
// on, a swap, destructuring into it, a read back after a write, a loop, a
// template's ordinary parameter.
TEST(ParamMode, InoutParametersChangeTheCallersVariables) {
  LeakGuard guard;
  auto r = compileAndRun(R"(
    enum Color { Red, Green, Blue }
    fn bump(n: inout int) { n = n + 1; }
    fn twice(n: inout int) { bump(n); bump(n); }
    fn swap(a: inout int, b: inout int) { t = a; a = b; b = t; }
    fn scale(x: inout float, by: view float) { x = x * by; }
    fn flip(b: inout bool) { b = !b; }
    fn next(c: inout char) { c = char(int(c) + 1); }
    fn cycle(c: inout Color) {
      match c {
        Red { c = Color::Green; }
        Green { c = Color::Blue; }
        Blue { c = Color::Red; }
      }
    }
    fn split(lo: inout int, hi: inout int, v: int) { lo, hi = (v / 10, v % 10); }
    fn readBack(n: inout int) -> int { n = 41; return n + 1; }
    fn count<T>(xs: T[], total: inout int) { total = total + xs.len(); }
    fn main() -> int {
      k = 1; bump(k); twice(k);
      a = 1; b = 2; swap(a, b);
      x = 1.5; scale(x, 3);
      f = False; flip(f);
      ch = 'a'; next(ch);
      c = Color::Blue; cycle(c);
      lo = 0; hi = 0; split(lo, hi, 47);
      r = 0; s = readBack(r);
      i = 0; total = 0;
      while (i < 3) { count([1, 2], total); i = i + 1; }
      println(Str(k) + " " + Str(a) + Str(b) + " " + Str(x) + " " + Str(f) +
              " " + Str(ch) + " " + Str(c == Color::Red) + " " + Str(lo) +
              Str(hi) + " " + Str(r) + " " + Str(s) + " " + Str(total));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "4 21 4.5 True b True 47 41 42 6\n");
  guard.expectNoLeaks("inout parameters");
}

// Methods, constructors, `__super__` and overrides take `inout` parameters
// too; a virtual call reaches the override's address-taking slot.
TEST(ParamMode, InoutParametersOfMethodsAndConstructors) {
  LeakGuard guard;
  auto r = compileAndRun(R"(
    class Counter {
      n: int;
      fn __init__(start: inout int) { self.n = start; start = 0; }
      fn take(to: inout int) { to = to + self.n; }
    }
    class Twice : Counter {
      fn __init__(s: inout int) { __super__(s); s = 7; }
      fn take(to: inout int) { to = to + 2 * self.n; }
    }
    fn main() -> int {
      st = 5;
      c: Counter = Twice(st);
      acc = 1;
      c.take(acc);
      plain = Counter(acc);
      plain.take(st);
      println(Str(st) + " " + Str(acc) + " " + Str(c.n) + " " + Str(plain.n));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "18 0 5 11\n");
  guard.expectNoLeaks("inout methods");
}

// A field passed to an `inout` parameter is the object's own storage, by
// address: the callee's writes are seen at once through any reference to
// the object, `self.f` and chains work, and an object the call might lose
// (a chain, a call's result, an array element) is kept alive for the call.
TEST(ParamMode, InoutFieldsArePassedByAddress) {
  LeakGuard guard;
  auto r = compileAndRun(R"(
    class Inner { n: int; fn __init__() { self.n = 0; } }
    class Holder {
      inner: Inner; total: int; f: float;
      fn __init__() { self.inner = Inner(); self.total = 0; self.f = 0.5; }
      fn addTo(t: inout int) { t = t + 1; }
      fn selfBump() { bump(self.total); self.addTo(self.total); }
    }
    fn bump(n: inout int) { n = n + 1; }
    fn watch(t: inout int, h: Holder) {
      t = t + 1;
      println("seen " + Str(h.total));
      t = t + 1;
    }
    fn replace(n: inout int, h: Holder) { h.inner = Inner(); n = 99; }
    fn make() -> Holder { return Holder(); }
    fn twice(x: inout float) { x = x * 2; }
    fn main() -> int {
      h = Holder();
      bump(h.total);
      watch(h.total, h);
      h.selfBump();
      bump(h.inner.n);
      replace(h.inner.n, h);
      bump(make().total);
      hs = [Holder(), Holder()];
      bump(hs[1].total);
      twice(h.f);
      let p = Holder();
      bump(p.total);
      println(Str(h.total) + " " + Str(h.inner.n) + " " + Str(hs[1].total) +
              " " + Str(h.f) + " " + Str(p.total));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "seen 2\n5 0 1 1 1\n");
  guard.expectNoLeaks("inout fields");
}

// -- Local borrows

// A `view` local holds a copy of its initializer, of every value type, with
// its type inferred or written.
TEST(LocalBorrow, ViewLocalsOfValueTypes) {
  LeakGuard guard;
  auto r = compileAndRun(R"(
    enum Color { Red, Green }
    fn main() -> int {
      k = 3;
      v: view = k + 1;
      w: view float = k;
      c: view Color = Color::Green;
      b: view = v > 2;
      ch: view char = 'z';
      println(Str(v) + " " + Str(w) + " " + Str(c == Color::Green) + " " +
              Str(b) + " " + Str(ch));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "4 3 True True z\n");
  guard.expectNoLeaks("view locals");
}
