// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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
  // 128: a shell reads a higher exit status as a signal.
  EXPECT_EQ(r.ExitCode, 49) << r.StdErr;
  guard.expectNoLeaks("address ops");
}

// -- `view` parameters

// A `view` parameter is a read-only copy: of every value type, on a
// function, a constructor, a method, an override and a template's ordinary
// parameter, with any argument of its type.
TEST(ParamMode, ViewParametersArePassedByValue) {
  LeakGuard guard;
  auto r = compileAndRun(R"(import ::io;
    enum Color { Red, Green }
    class Counter {
      n: int;
      fn add(k: view int) -> int { self.n = self.n + k; return self.n; }
    }

    impl Counter {
      fn __init__(start: view int) { self.n = start; }
    }
    class Twice : Counter {
      fn add(k: view int) -> int { self.n = self.n + 2 * k; return self.n; }
    }

    impl Twice {
      fn __init__(s: view int) { __super__(s); }
    }
    fn scale(x: view float, by: view float) -> float { y = x * by; return y; }
    fn name(c: view Color, loud: view bool, end: view char) -> Str {
      s = "red";
      if c == Color::Green { s = "green"; }
      if loud { s = s + Str(end); }
      return s;
    }
    fn at<T>(xs: view T[], i: view int) -> T { return xs[i]; }
    fn main() -> int {
      let k = 3;
      c: Counter = Twice(k);
      ns: int[2] = [4, 5];
      io::println(Str(c.add(k)) + " " + Str(scale(1.5, 2)) + " " +
              name(Color::Green, True, '!') + " " + Str(at(ns, 1)) + " " +
              Str(k));
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
  auto r = compileAndRun(R"(import ::io;
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
    fn count<T>(xs: view T[], total: inout int) { total = total + xs.len(); }
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
      pair: int[2] = [1, 2];
      while i < 3 { count(pair, total); i = i + 1; }
      io::println(Str(k) + " " + Str(a) + Str(b) + " " + Str(x) + " " + Str(f) +
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
  auto r = compileAndRun(R"(import ::io;
    class Counter {
      n: int;
      fn take(to: inout int) { to = to + self.n; }
    }

    impl Counter {
      fn __init__(start: inout int) { self.n = start; start = 0; }
    }
    class Twice : Counter {
      fn take(to: inout int) { to = to + 2 * self.n; }
    }

    impl Twice {
      fn __init__(s: inout int) { __super__(s); s = 7; }
    }
    fn main() -> int {
      st = 5;
      c: Counter = Twice(st);
      acc = 1;
      c.take(acc);
      plain = Counter(acc);
      plain.take(st);
      io::println(Str(st) + " " + Str(acc) + " " + Str(c.n) + " " + Str(plain.n));
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
  auto r = compileAndRun(R"(import ::io;
    class Inner {
      n: int;
      fn addTo(t: inout int) { t = t + 1; }
    }

    impl Inner {
      fn __init__() { self.n = 0; }
    }
    class Holder {
      inner: Inner; total: int; f: float;
      fn selfBump() { bump(self.total); self.inner.addTo(self.total); }
    }

    impl Holder {
      fn __init__() { self.inner = Inner(); self.total = 0; self.f = 0.5; }
    }
    fn bump(n: inout int) { n = n + 1; }
    fn watch(t: inout int, h: Holder) {
      t = t + 1;
      io::println("seen " + Str(h.total));
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
      io::println(Str(h.total) + " " + Str(h.inner.n) + " " + Str(hs[1].total) +
              " " + Str(h.f) + " " + Str(p.total));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "seen 2\n5 0 1 1 1\n");
  guard.expectNoLeaks("inout fields");
}

// A `view` parameter of an object, string, array or type parameter shares
// what it was given, like a parameter without a mode; it reads it and passes
// it on to other `view` parameters.
TEST(ParamMode, ViewParametersOfReferenceTypes) {
  LeakGuard guard;
  auto r = compileAndRun(R"(import ::io;
    class Counter {
      n: int;
      fn tick() { self.n = self.n + 1; }
    }

    impl Counter {
      fn __init__(n: int) { self.n = n; }
    }
    class Fast : Counter {}
    impl Fast { fn __init__(n: int) { __super__(n); } }
    fn sum(c: view Counter, xs: view int[], s: view Str) -> int {
      return c.n + xs.len() + s.len() + xs[0];
    }
    fn passOn(c: view Counter, xs: view int[]) -> int {
      return sum(c, xs, "ab");
    }
    fn size<T>(xs: view T[]) -> int { return xs.len(); }
    fn main() -> int {
      c = Counter(30);
      c.tick();
      names: Str[2] = ["a", "b"];
      cs: Counter[3] = [c, c, c];
      io::println(Str(sum(c, [1, 2], "abc")) + " " + Str(sum(Fast(4), [5], "")) +
              " " + Str(passOn(c, [7])) + " " + Str(size(names)) +
              Str(size(cs)));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "37 10 41 23\n");
  guard.expectNoLeaks("view reference parameters");
}

// An `inout` parameter of a reference type is the caller's variable itself:
// assigning to it replaces what the caller holds (releasing the old value,
// with nothing retained for the call), through a field, passing on, a
// method's virtual slot, a template, destructuring and self-assignment; its
// value can be returned and kept, and another reference to the old object
// keeps that object alive.
TEST(ParamMode, InoutParametersOfReferenceTypes) {
  LeakGuard guard;
  auto r = compileAndRun(R"(import ::io;
    class Counter {
      n: int;
      fn tick() { self.n = self.n + 1; }
      fn adopt(c: inout Counter) { c = Counter(self.n); }
    }

    impl Counter {
      fn __init__(n: int) { self.n = n; }
    }
    class Fast : Counter {
      fn adopt(c: inout Counter) { c = Fast(10 * self.n); }
    }

    impl Fast {
      fn __init__(n: int) { __super__(n); }
    }
    class Holder { c: Counter; }
    impl Holder { fn __init__() { self.c = Counter(100); } }
    fn same(c: inout Counter) { c = c; c.tick(); }
    fn keep(c: inout Counter) -> Counter { return c; }
    fn pass(c: inout Counter) { same(c); reset(c); }
    fn reset(c: inout Counter) { c = Counter(50); }
    fn swap<T>(a: inout T, b: inout T) { t = a; a = b; b = t; }
    fn flip(a: inout Str, b: inout Str) { a, b = (b, a); }
    fn clear(o: inout Counter?) { o = None; }
    fn fill(o: inout Counter?) { o = Counter(5); }
    fn pair(t: inout (int, Str)) { t = (t.0 + 1, t.1 + "?"); }
    fn refill(xs: inout int[]) { xs[1] = xs[0] + 8; xs[0] = xs.len(); }
    fn seen(c: inout Counter, other: Counter) -> int {
      c = Counter(0);
      return other.n;
    }
    fn main() -> int {
      x = Counter(1);
      same(x);
      k = keep(x);
      pass(x);
      f: Counter = Fast(3);
      f.adopt(x);
      a = "a"; b = "b";
      swap(a, b);
      flip(a, b);
      i = 1; j = 2;
      swap(i, j);
      p = Counter(7); q = Counter(8);
      swap(p, q);
      o: Counter? = Counter(3);
      clear(o);
      fill(o);
      t = (1, "t");
      pair(t);
      xs: int[2] = [1, 7];
      refill(xs);
      y = Counter(4);
      z = y;
      w = seen(y, z);
      h = Holder();
      reset(h.c);
      swap(h.c, p);
      io::println(Str(k.n) + " " + Str(x.n) + " " + a + b + " " + Str(i) +
              Str(j) + " " + Str(p.n) + Str(q.n) + " " + Str(o != None) +
              " " + Str(t.0) + t.1 + " " + Str(xs[0]) + Str(xs[1]) + " " +
              Str(w) + Str(y.n) + " " + Str(h.c.n));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3 30 ab 21 507 True 2t? 29 40 8\n");
  guard.expectNoLeaks("inout reference parameters");
}

// -- Local borrows

// A `view` local holds a copy of its initializer, of every value type, with
// its type inferred or written.
TEST(LocalBorrow, ViewLocalsOfValueTypes) {
  LeakGuard guard;
  auto r = compileAndRun(R"(import ::io;
    enum Color { Red, Green }
    fn main() -> int {
      k = 3;
      v: view = k + 1;
      w: view float = k;
      c: view Color = Color::Green;
      b: view = v > 2;
      ch: view char = 'z';
      io::println(Str(v) + " " + Str(w) + " " + Str(c == Color::Green) + " " +
              Str(b) + " " + Str(ch));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "4 3 True True z\n");
  guard.expectNoLeaks("view locals");
}

// A `view` local of an object, string or array shares what it was given and
// reads it: its fields and elements, `toString` (an override included), `len`,
// a `match` arm's name for it, and printing it.
TEST(LocalBorrow, ViewLocalsOfReferenceTypes) {
  LeakGuard guard;
  auto r = compileAndRun(R"(import ::io;
    class Counter {
      n: int;
      view fn toString() -> Str { return "counter " + Str(self.n); }
    }

    impl Counter {
      fn __init__(n: int) { self.n = n; }
    }
    class Fast : Counter {}
    impl Fast { fn __init__(n: int) { __super__(n); } }
    fn main() -> int {
      c: Counter = Fast(3);
      v: view = c;
      s: view = "abc";
      xs: view int[2] = [4, 5];
      o: view Counter? = c;
      kind = "plain";
      match v { f: Fast { kind = "fast " + Str(f.n); } _ { } }
      io::println(v);
      io::println(v.toString() + " " + s + Str(s.len()) + " " +
              Str(xs.len() + xs[1]) + " " + Str(o != None) + " " + kind);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "counter 3\ncounter 3 abc3 7 True fast 3\n");
  guard.expectNoLeaks("view reference locals");
}

// A literal holds its own copy of each element read through a `view` (a value
// type, and what is made from a shared one), and its own reference to each
// object it is given: the value it makes outlives them and is released once.
TEST(LocalBorrow, LiteralsHoldTheirElements) {
  LeakGuard guard;
  auto r = compileAndRun(R"(import ::io;
    class Node { n: int; }
    class Named { name: Str; n: int; }
    impl Named {
      view fn pair() -> (Str, int) { return (self.name + "!", self.n); }
      view fn sizes() -> int[2] { return [self.name.len(), self.n]; }
    }
    fn tup(s: view Str) -> (Str, int) { return (s + "", s.len()); }
    fn arr(k: view int) -> int[3] { return [k; 3]; }
    fn wrap(n: Node) -> (Node, (Node, int)) { return (n, (n, 1)); }
    fn main() -> int {
      t = tup(Str(12));
      a = arr(4);
      w = wrap(Node(5));
      w.0.n = 6;
      h = Named("h", 9);
      p = h.pair();
      sizes = h.sizes();
      v: view = Node(1);
      xs = [v.n, 2];
      io::println(t.0 + " " + Str(t.1) + " " + Str(a[2]) + " " + Str(w.1.0.n) +
              " " + p.0 + Str(p.1) + " " + Str(sizes[0] + sizes[1]) + " " +
              Str(xs[0] + xs[1]));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "12 2 4 6 h!9 10 3\n");
  guard.expectNoLeaks("literals of views");
}

// An `inout` local is another name for a variable or a field, of every
// type: assigning to it writes there (an object, string or optional is
// replaced, the old one released; an array's elements are written in place),
// and it passes its address on to an `inout` parameter.  A field's object
// stays alive while the local can be used.
TEST(LocalBorrow, InoutLocalsWriteThrough) {
  LeakGuard guard;
  auto r = compileAndRun(R"(import ::io;
    class Counter {
      n: int;
      fn get() -> int { return self.n; }
    }

    impl Counter {
      fn __init__(n: int) { self.n = n; }
    }
    class Holder {
      c: Counter; total: int;
      fn addTo(k: int) { t: inout = self.total; t = t + k; }
    }

    impl Holder {
      fn __init__() { self.c = Counter(100); self.total = 0; }
    }
    fn bump(n: inout int) { n = n + 1; }
    fn make() -> Holder { return Holder(); }
    fn main() -> int {
      k = 1;
      x: inout = k;
      x = x + 10;
      bump(x);
      f = 1.5;
      g: inout float = f;
      g = g * 2;
      s = "hi";
      t: inout = s;
      t = t + "!";
      c = Counter(1);
      cc: inout = c;
      cc = Counter(7);
      h = Holder();
      hc: inout = h.c;
      hc = Counter(9);
      ht: inout = h.total;
      ht = 5;
      h.addTo(3);
      xs: int[2] = [1, 2];
      ys: inout = xs;
      ys[1] = 3;
      ys = [ys[1], 8];
      o: Counter? = None;
      oo: inout = o;
      oo = Counter(4);
      m: inout = make().total;
      m = m + 1;
      io::println(Str(k) + " " + Str(f) + " " + s + " " + Str(c.get()) + " " +
              Str(h.c.get()) + " " + Str(h.total) + " " + Str(xs[0]) +
              Str(xs[1]) + " " + Str(o != None) + " " + Str(m));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "12 3 hi! 7 9 8 38 True 1\n");
  guard.expectNoLeaks("inout locals");
}

// A `match` arm's `n: view T` reads the subject as a plain binding would:
// an object (passed on to a `view` parameter), a string and a primitive
// unwrapped from optionals.
TEST(LocalBorrow, ViewArms) {
  LeakGuard guard;
  auto r = compileAndRun(R"(import ::io;
    class Animal {
      n: int;
      view fn get() -> int { return self.n; }
    }

    impl Animal {
      fn __init__() { self.n = 4; }
    }
    class Dog : Animal {}
    impl Dog { fn __init__() { __super__(); } }
    fn show(a: view Animal) -> int { return a.get(); }
    fn main() -> int {
      a: Animal = Dog();
      match a {
        d: view Dog { io::println(Str(d.get() + show(d))); }
        _ { }
      }
      o: Str? = "hi";
      match o {
        s: view Str { io::println(s + Str(s.len())); }
        None { }
      }
      n: int? = 7;
      match n {
        k: view int { io::println(Str(k * 2)); }
        None { }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "8\nhi2\n14\n");
  guard.expectNoLeaks("view arms");
}

// A `match` arm's `n: inout T` writes through to the subject: a variable
// (another reference to the old object keeps it), a field, an optional, and
// an `inout` parameter passed on.  The match keeps the object it matched
// until it ends.
TEST(LocalBorrow, InoutArmsWriteThrough) {
  LeakGuard guard;
  auto r = compileAndRun(R"(import ::io;
    class Animal {
      n: int;
      fn tick() { self.n = self.n + 1; }
      view fn get() -> int { return self.n; }
    }

    impl Animal {
      fn __init__(n: int) { self.n = n; }
    }
    class Dog : Animal {}
    impl Dog { fn __init__(n: int) { __super__(n); } }
    class Home { pet: Animal; }
    impl Home { fn __init__() { self.pet = Dog(1); } }
    fn adopt(p: inout Animal) {
      match p {
        d: inout Dog { d = Dog(d.get() + 30); }
        _ { }
      }
    }
    fn main() -> int {
      a: Animal = Dog(5);
      keep = a;
      match a {
        d: inout Dog {
          d.tick();
          d = Dog(10);
          d.tick();
          io::print(Str(d.get()) + " ");
        }
        _ { }
      }
      io::print(Str(a.get()) + " " + Str(keep.get()) + " ");
      h = Home();
      match h.pet {
        d: inout Dog { d = Dog(42); }
        _ { }
      }
      o: Animal? = Animal(3);
      match o {
        x: inout Animal { x = Dog(7); }
        None { }
      }
      match o {
        y: Dog { io::print("dog" + Str(y.get()) + " "); }
        _ { }
      }
      adopt(a);
      io::println(Str(h.pet.get()) + " " + Str(a.get()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "11 11 6 dog7 42 41\n");
  guard.expectNoLeaks("inout arms");
}

// An `inout` arm narrows the subject (a variant, an optional's value, a
// subclass), and another reference to its object can change what the subject
// holds while the arm runs: an alias (`g = h`), a second argument, a
// parameter that is `self`.  The binding is a value of its own, written back
// on every way out of the arm (its end, `return`, `break`), so it never reads
// the changed storage as the narrowed type.
TEST(LocalBorrow, InoutArmsSurviveAChangeThroughAnotherReference) {
  LeakGuard guard;
  auto r = compileAndRun(R"(import ::io;
    enum Sh { E, L(s: Str) }
    struct P { y: Str; }
    class A { n: int; }
    impl A { fn __init__(n: int) { self.n = n; } }
    class B : A { x: Str; }
    impl B { fn __init__(n: int, x: Str) { __super__(n); self.x = x; } }
    class H {
      s: Sh;
      p: P?;
      a: A;
      fn poke(other: H) {
        match self.s { l: inout L { other.s = Sh::E; io::print(l.s + " "); } _ { } }
      }
    }
    impl H {
      fn __init__() { self.s = Sh::L("x" + "y"); self.p = P("a" + "b"); self.a = B(1, "b" + "x"); }
    }
    fn f(x: inout Sh, o: H) {
      match x { l: inout L { o.s = Sh::E; io::print(l.s + " "); } _ { } }
    }
    fn firstL(x: inout Sh, o: H) -> Str {
      while True {
        match x { l: inout L { o.s = Sh::E; l.s = l.s + "!"; return l.s; } _ { break; } }
      }
      return "none";
    }
    fn main() -> int {
      h = H();
      g = h;
      match h.s { l: inout L { g.s = Sh::E; io::print(l.s + " "); } _ { } }
      match h.p { q: inout P { g.p = None; q.y = q.y + "c"; io::print(q.y + " "); } None { } }
      match h.a { b: inout B { g.a = A(2); io::print(b.x + " "); } _ { } }
      io::println(h.s.toString() + " " + h.p.toString() + " " + Str(h.a.n));
      f(h.s, h);
      h.poke(h);
      io::print(firstL(h.s, h) + " ");
      while True {
        match h.s { l: inout L { l.s = "q"; break; } _ { break; } }
      }
      io::println(h.s.toString());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "xy abc bx L(s: xy) P(y: abc) 1\n"
                      "xy xy xy! L(s: q)\n");
  EXPECT_EQ(r.ExitCode, 0);
  guard.expectNoLeaks("inout arms changed through another reference");
}

// A struct held in an object is in the object's storage: an `inout` arm or
// local naming a field of it keeps the object (`o.h`), so another reference
// that replaces it (`g.h = ...`) frees nothing the arm's write-back or the
// local's write still reaches.  What they write goes to the object no longer
// reached.
TEST(LocalBorrow, InoutPlacesInAStructKeepItsObject) {
  LeakGuard guard;
  auto r = compileAndRun(R"(import ::io;
    enum Sh { E, L(s: Str) }
    class Box { v: Str; }
    struct Q { n: Str; }
    struct P { s: Sh; q: Q?; b: Box; }
    class H { p: P; }
    class O { h: H; }
    fn fresh(t: Str) -> H { return H(P(Sh::L(t + ""), Q(t + "q"), Box(t + "b"))); }
    fn main() -> int {
      o = O(fresh("x"));
      g = o;
      match o.h.p.s { l: inout L { g.h = fresh("y"); l.s = l.s + "!"; } _ { } }
      match o.h.p.q { q: inout Q { g.h = fresh("z"); q.n = q.n + "!"; } None { } }
      match o.h.p.b { b: inout Box { g.h = fresh("w"); y = fresh("v"); b = Box("five"); } _ { } }
      x: inout = o.h.p.s;
      g.h = fresh("u");
      x = Sh::L("q" + "r");
      io::println(o.h.p.s.toString() + " " + o.h.p.q.toString() + " " + o.h.p.b.v);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "L(s: u) Q(n: uq) ub\n");
  EXPECT_EQ(r.ExitCode, 0);
  guard.expectNoLeaks("inout places in a struct held in an object");
}

// An `inout` arm's binding goes back to the subject after its last use in
// the arm: what the arm does to the subject after that, through its
// variable or another reference, stays; a binding the arm never uses puts
// nothing back.
TEST(LocalBorrow, InoutArmsGoBackAtTheirLastUse) {
  LeakGuard guard;
  auto r = compileAndRun(R"(import ::io;
    enum Sh { E, L(s: Str) }
    class H { s: Sh; }
    fn main() -> int {
      h = H(Sh::L("x" + "y"));
      g = h;
      match h.s { l: inout L { l.s = l.s + "1"; h.s = Sh::E; } _ { } }
      io::print(h.s.toString() + " ");
      h.s = Sh::L("a" + "b");
      match h.s { l: inout L { io::print(l.s + " "); g.s = Sh::L("c" + "d"); } _ { } }
      io::print(h.s.toString() + " ");
      match h.s { l: inout L { h.s = Sh::E; } _ { } }
      io::print(h.s.toString() + " ");
      k: Sh = Sh::L("y");
      match k { l: inout L { l.s = "b" + "c"; } _ { } }
      io::print(k.toString() + " ");
      match k { l: inout L { io::print(l.s + " "); k = Sh::E; } _ { } }
      io::println(k.toString());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "E ab L(s: cd) E L(s: bc) bc E\n");
  guard.expectNoLeaks("inout arms written back at their last use");
}

// A borrow's last use is of the name that means it: a binder of the same
// name further on (a lambda's parameter, a `for` loop's variable, a
// declaration in a block) is another variable.  So an `inout` arm's binding
// goes back to the subject before it, and the arm reads what it wrote; a
// `view` local's string, and a `view` arm's object, are replaced after
// their last use.  (Sema kept each borrow live: `'b' is borrowed by 'inout'
// local 'r' until 'r' is last used`.)
TEST(LocalBorrow, ABorrowEndsBeforeABinderOfItsName) {
  LeakGuard guard;
  auto r = compileAndRun(R"(import ::io;
    enum E { A, R(w: int, s: Str) }
    class B { e: E; }
    class N { s: Str; }
    fn main() -> int {
      let b = B(E::R(1, "a" + "b"));
      match b.e {
        r: inout R {
          r.w += 100;
          r.s = r.s + "!";
          io::print(Str(b.e) + " ");
          let f = fn(r: int) -> int => r * 2;
          for r in 0..2 { io::print(Str(r + f(3)) + " "); }
          { let r = "x" + "y"; io::print(r + " "); }
          b.e = E::A;
        }
        A { }
      }
      io::println(Str(b.e));
      s = "o" + "rig";
      v: view = s;
      io::print(v + " ");
      s = "changed";
      let g = fn(v: Str) -> Str => v + "?";
      on: N? = N("n" + "1");
      match on {
        n: view N {
          io::print(n.s + " ");
          on = N("n" + "2");
          let h = fn(n: N?) -> Str => if n == None then "-" else "+";
          io::println(g(s) + " " + h(on));
        }
        None { }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "R(w: 101, s: ab!) 6 7 xy A\norig n1 changed? +\n");
  guard.expectNoLeaks("borrows ending before a binder of their name");
}

// The subject of a `match` with an `inout` arm is found once, before any arm
// runs: a call on the way to it (a method with an `inout` result, a function
// returning an object) runs once, and the binding goes back where its value
// was read, even when another call would name other storage.
TEST(LocalBorrow, InoutArmsFindTheirSubjectOnce) {
  LeakGuard guard;
  auto r = compileAndRun(R"(import ::io;
    enum Sh { E, L(s: Str) }
    class Animal { n: int; }
    impl Animal { fn __init__(n: int) { self.n = n; } }
    class Dog : Animal {}
    impl Dog { fn __init__(n: int) { __super__(n); } }
    struct Q { y: Str; }
    struct W { s: Sh; a: Animal; q: Q?; }
    class B {
      w0: W;
      w1: W;
      k: int;
      fn next() -> inout W {
        self.k += 1;
        if self.k % 2 == 1 { return self.w0; }
        return self.w1;
      }
    }
    class H { s: Sh; }
    fn mk(t: Str) -> H { io::print("mk "); return H(Sh::L(t + "")); }
    fn first(b: B) -> Str {
      match b.next().s { l: inout L { l.s = l.s + "?"; return l.s; } _ { return "-"; } }
    }
    fn main() -> int {
      b = B(W(Sh::L("a" + ""), Animal(1), Q("p" + "")), W(Sh::E, Dog(2), None), 0);
      match b.next().s { l: inout L { l.s = l.s + "!"; } _ { } }
      match b.next().a { d: inout Dog { d = Dog(d.n + 5); } _ { io::print("not a dog "); } }
      match b.next().q { q: inout Q { q.y = q.y + "!"; } None { } }
      io::print(first(b) + " " + first(b) + " ");
      io::println(b.w0.s.toString() + " " + b.w0.q.toString() + " " + Str(b.w0.a.n) + " " +
                  b.w1.s.toString() + " " + Str(b.w1.a.n) + " " + Str(b.k));
      match mk("m").s { l: inout L { l.s = l.s + "!"; io::println(l.s); } _ { } }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "- a!? L(s: a!?) Q(y: p!) 1 E 7 5\nmk m!\n");
  EXPECT_EQ(r.ExitCode, 0);
  guard.expectNoLeaks("inout arms whose subject is reached through a call");
}

// A call's `inout` result is an `inout` arm's subject as a variable or a
// field is: the call runs once, and the binding goes back to the storage it
// returned, a variant, a subclass and an optional's value, on the arm's end,
// `return`, `break` and `continue`; through a method of a class, of a
// generic class and of a struct, on an `inout` parameter, and a function's.
// The match keeps the object the storage is in, which another reference may
// replace meanwhile.  (Sema rejected each: `'inout' arm 'r' needs a
// variable or a field to match on`.)
TEST(LocalBorrow, InoutArmsMatchAnInoutResult) {
  LeakGuard guard;
  auto r = compileAndRun(R"(import ::io;
    enum E { A, R(w: int, s: Str) }
    class Animal { n: int; }
    impl Animal { fn __init__(n: int) { self.n = n; } }
    class Dog : Animal {}
    impl Dog { fn __init__(n: int) { __super__(n); } }
    struct Q { y: Str; }
    class B {
      e: E; a: Animal; q: Q?; calls: int;
      fn slot() -> inout E { self.calls += 1; return self.e; }
      fn pet() -> inout Animal { self.calls += 1; return self.a; }
      fn opt() -> inout Q? { self.calls += 1; return self.q; }
    }
    class Cell<T> { v: T; fn slot() -> inout T { return self.v; } }
    struct S { e: E; }
    impl S { fn slot() -> inout E { return self.e; } }
    class H { b: B; s: S; }
    fn pick(x: inout E, y: inout E, first: bool) -> inout E {
      return if first then x else y;
    }
    fn early(b: B) -> Str {
      match b.slot() { r: inout R { r.w += 1; return Str(r.w); } A { return "-"; } }
    }
    fn viaParam(b: inout B) { match b.slot() { r: inout R { r.w += 10; } A { } } }
    fn reset(h: H) { h.b = B(E::A, Animal(0), None, 0); h.s = S(E::A); }
    fn main() -> int {
      let b = B(E::R(1, "a" + "b"), Dog(2), Q("p" + ""), 0);
      match b.slot() { r: inout R { r.w += 100; r.s = r.s + "!"; } A { } }
      io::println(Str(b.e) + " " + Str(b.calls));
      match b.pet() { d: inout Dog { d = Dog(d.n + 5); } _ { } }
      match b.opt() { q: inout Q { q.y = q.y + "?"; } None { } }
      io::println(Str(b.a.n) + " " + Str(b.q) + " " + Str(b.calls));
      io::println(early(b) + " " + Str(b.e) + " " + Str(b.calls));
      b2 = B(E::R(2, "c" + ""), Animal(0), None, 0);
      viaParam(b2);
      i = 0;
      while i < 3 {
        i += 1;
        match b2.slot() {
          r: inout R { r.w += 1; if i == 2 { continue; } if i == 3 { break; } }
          A { }
        }
      }
      x = E::R(1, "x" + "");
      y = E::A;
      match pick(x, y, True) { r: inout R { r.w = 7; } A { } }
      match pick(x, y, False) { r: inout R { r.w = 8; } A { io::print("A "); } }
      io::println(Str(b2.e) + " " + Str(x) + " " + Str(y));
      let c = Cell<E>(E::R(4, "c" + ""));
      match c.slot() { r: inout R { r.w = 40; } A { } }
      s = S(E::R(3, "s" + ""));
      match s.slot() { r: inout R { r.w *= 2; } A { } }
      let h = H(B(E::R(5, "h" + ""), Animal(0), None, 0), S(E::R(6, "t" + "")));
      let g = h;
      match h.b.slot() { r: inout R { g.b = B(E::A, Animal(1), None, 0); r.w = 9; } A { } }
      match h.s.slot() { r: inout R { reset(g); r.w = 60; } A { } }
      io::println(Str(c.v) + " " + Str(s.e) + " " + Str(h.b.e) + " " + Str(h.s.e));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "R(w: 101, s: ab!) 1\n7 Q(y: p?) 3\n102 R(w: 102, s: "
                      "ab!) 4\nA R(w: 15, s: c) R(w: 7, s: x) A\nR(w: 40, s: "
                      "c) R(w: 6, s: s) A R(w: 60, s: t)\n");
  EXPECT_EQ(r.ExitCode, 0);
  guard.expectNoLeaks("inout arms matching an inout result");
}

// A borrowed result is returned like any other: part of `self` (a field, an
// element) or of a `view` parameter, read by its caller, bound to a `view`
// local and passed on to a `view` parameter.
TEST(BorrowedResult, ViewResultsAreRead) {
  LeakGuard guard;
  auto r = compileAndRun(R"(import ::io;
    class Counter {
      n: int;
      view fn get() -> int { return self.n; }
    }

    impl Counter {
      fn __init__(n: int) { self.n = n; }
    }
    class Box {
      c: Counter; xs: int[2];
      view fn peek() -> view Counter { return self.c; }
      view fn first() -> view int { return self.xs[0]; }
    }

    impl Box {
      fn __init__() { self.c = Counter(4); self.xs = [7, 8]; }
    }
    fn longer(a: view Str, b: view Str) -> view Str {
      return if a.len() > b.len() then a else b;
    }
    fn show(c: view Counter) -> int { return c.get(); }
    fn main() -> int {
      b = Box();
      v: view = b.peek();
      k = b.first();
      s = "hello";
      io::println(longer(s, "hi") + " " + Str(v.get() + show(b.peek()) + k));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "hello 15\n");
  guard.expectNoLeaks("view results");
}

// An `inout` result is the storage it names: bound to an `inout` local,
// passed on to an `inout` parameter, written through, a method called on it
// and read; a field of `self` (an object and an int) and a conditional of
// two `inout` parameters.
TEST(BorrowedResult, InoutResultsWriteThrough) {
  LeakGuard guard;
  auto r = compileAndRun(R"(import ::io;
    class Counter {
      n: int;
      fn tick() { self.n = self.n + 1; }
      view fn get() -> int { return self.n; }
    }

    impl Counter {
      fn __init__(n: int) { self.n = n; }
    }
    class Box {
      c: Counter; k: int;
      fn counter() -> inout Counter { return self.c; }
      fn num() -> inout int { return self.k; }
    }

    impl Box {
      fn __init__() { self.c = Counter(1); self.k = 3; }
    }
    fn pick(a: inout int, b: inout int, first: bool) -> inout int {
      return if first then a else b;
    }
    fn bump(n: inout int) { n = n + 1; }
    fn main() -> int {
      b = Box();
      x: inout = b.num();
      x = x + 10;
      b.counter().tick();
      c: inout = b.counter();
      c = Counter(40);
      c.tick();
      bump(b.num());
      i = 1;
      j = 2;
      p: inout = pick(i, j, False);
      p = 20;
      bump(pick(i, j, True));
      y = b.num() + b.counter().get();
      io::println(Str(b.k) + " " + Str(b.c.get()) + " " + Str(i) + " " + Str(j) +
              " " + Str(y));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "14 41 2 20 55\n");
  guard.expectNoLeaks("inout results");
}

// A field of a conditional's object is a borrowed result when it is one on
// each branch: an `inout` result is the field of the object the chosen
// branch holds, written through by an `inout` local and an `inout`
// argument (an `int` and a string, nested conditionals, an object in a
// tuple, `self` or a parameter), and the object stays alive while the
// result is used though another reference replaces it; a `view` result
// reads the field (of an object, of a struct).  (Sema rejected each: `...
// must be part of 'self' or of ... parameter, not a new value`.)
TEST(BorrowedResult, AFieldOfAConditionalIsWrittenThrough) {
  LeakGuard guard;
  auto r = compileAndRun(R"(import ::io;
    class K { n: int; s: Str; }
    class T {
      n: int; k: K;
      fn either(o: inout T, c: bool) -> inout int { return (if c then self else o).n; }
      view fn name(o: view T, c: bool) -> view Str { return (if c then self else o).k.s; }
    }
    class H { k: K; }
    struct P { x: int; s: Str; }
    fn pick(a: inout K, b: inout K, c: bool) -> inout int { return (if c then a else b).n; }
    fn pickS(a: inout K, b: inout K, c: bool) -> inout Str { return (if c then a else b).s; }
    fn nest(a: inout K, b: inout K, d: inout K, c: bool, e: bool) -> inout int {
      return (if c then (if e then a else b) else d).n;
    }
    fn tup(t: inout (K, int), u: inout (K, int), c: bool) -> inout int {
      return (if c then t else u).0.n;
    }
    fn pv(a: view K, b: view K, c: bool) -> view Str { return (if c then a else b).s; }
    fn pvs(a: view P, b: view P, c: bool) -> view Str { return (if c then a else b).s; }
    fn bump(n: inout int) { n += 1; }
    fn add(s: inout Str) { s = s + "+"; }
    fn main() -> int {
      a = K(1, "a" + "");
      b = K(2, "b" + "");
      p: inout = pick(a, b, True);
      p = p + 10;
      bump(pick(a, b, False));
      q: inout = pickS(a, b, False);
      q = q + "!";
      add(pickS(a, b, True));
      io::println(Str(a.n) + " " + Str(b.n) + " " + a.s + " " + b.s);
      d = K(3, "d" + "");
      bump(nest(a, b, d, True, False));
      bump(nest(a, b, d, False, True));
      t = (K(4, "t" + ""), 0);
      u = (K(5, "u" + ""), 0);
      bump(tup(t, u, False));
      io::println(Str(b.n) + " " + Str(d.n) + " " + Str(t.0.n) + " " + Str(u.0.n));
      let x = T(6, K(0, "x" + ""));
      y = T(7, K(0, "y" + ""));
      bump(x.either(y, True));
      bump(x.either(y, False));
      io::println(Str(x.n) + " " + Str(y.n) + " " + x.name(y, False) + x.name(y, True));
      let h = H(K(8, "h" + ""));
      let g = h;
      e = K(9, "e" + "");
      r: inout = pickS(h.k, e, True);
      g.k = K(0, "new" + "");
      r = r + "?";
      io::print(r + " ");
      io::println(h.k.s);
      v: view = pv(a, b, True);
      w: view = pvs(P(1, "p" + "1"), P(2, "p" + "2"), False);
      io::println(v + " " + w);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "11 3 a+ b!\n4 4 4 6\n7 8 yx\nh? new\na+ p2\n");
  guard.expectNoLeaks("a field of a conditional as a borrowed result");
}

// An `inout` result of a tuple, which is held in place, is the address of
// its storage: it is written through (by an `inout` local, an `inout`
// argument or another `inout` result), read in place, and copied out only
// when it is stored.  Nothing of it is a temporary to release, also for the
// references it holds.
TEST(BorrowedResult, InoutTupleResultsWriteThrough) {
  LeakGuard guard;
  auto r = compileAndRun(R"(import ::io;
    class H {
      t: (int, Str);
      n: ((int, Str), Str);
      fn get() -> inout (int, Str) { return self.t; }
      fn nested() -> inout ((int, Str), Str) { return self.n; }
    }
    fn pick(a: inout (int, Str), b: inout (int, Str), first: bool)
        -> inout (int, Str) {
      return if first then a else b;
    }
    fn set(x: inout (int, Str)) { x = (9, "nine"); }
    fn show(v: view (int, Str)) -> Str { return Str(v.0) + v.1; }
    fn main() -> int {
      h = H((1, "one"), ((2, "two"), "x"));
      copy = h.get();
      io::println(h.get().1 + " " + h.nested().0.1 + h.nested().1);
      set(h.get());
      io::println(show(copy) + " " + show(h.t));
      w: inout (int, Str) = h.get();
      w = (3, "three");
      inner: inout ((int, Str), Str) = h.nested();
      inner = ((4, "four"), "y");
      io::println(show(h.get()) + " " + show(h.n.0) + h.n.1);
      a = (5, "a");
      b = (6, "b");
      p: inout (int, Str) = pick(a, b, False);
      p = (7, "seven");
      set(pick(a, b, True));
      io::println(show(a) + " " + show(b));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "one twox\n1one 9nine\n3three 4foury\n9nine 7seven\n");
  guard.expectNoLeaks("inout tuple results");
}
