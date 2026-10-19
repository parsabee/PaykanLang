// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// CodeGen / E2E tests for optional primitives `int?`, `float?`, `bool?` and
// `char?` (#66).  An optional primitive is a value, the primitive and a
// presence flag (#96), held inline in variables, fields, array elements and
// tuple elements, and never an object.  Every test runs under the tracking
// allocator and asserts zero live heap blocks.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

using namespace paykan::test;

// -- Declarations, None, printing, match

TEST(OptionalPrimitive, EveryPrimitiveSomeAndNone) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn si(o: int?) -> Str {
      match o { n: int { return "int " + Str<int>(n); } None { return "no int"; } }
    }
    fn sf(o: float?) -> Str {
      match o { f: float { return "float " + Str<float>(f); } None { return "no float"; } }
    }
    fn sb(o: bool?) -> Str {
      match o { b: bool { return "bool " + Str<bool>(b); } None { return "no bool"; } }
    }
    fn sc(o: char?) -> Str {
      match o { c: char { return "char " + Str<char>(c); } None { return "no char"; } }
    }
    fn main() -> int {
      println(si(42));   println(si(None));
      println(sf(2.5));  println(sf(None));
      println(sb(True)); println(sb(False)); println(sb(None));
      println(sc('z'));  println(sc(None));
      a: int? = 7;  f: float? = 1.25;  b: bool? = False;  c: char? = 'q';
      println(a); println(f); println(b); println(c);
      a = None; f = None; b = None; c = None;
      println(a); println(f); println(b); println(c);
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "int 42\nno int\nfloat 2.5\nno float\nbool True\n"
                      "bool False\nno bool\nchar z\nno char\n"
                      "7\n1.25\nFalse\nq\nNone\nNone\nNone\nNone\n");
  g.expectNoLeaks("EveryPrimitiveSomeAndNone");
}

TEST(OptionalPrimitive, IntPromotesIntoFloatOptional) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn show(o: float?) -> Str {
      match o { f: float { return Str<float>(f / 2.0); } None { return "-"; } }
    }
    fn main() -> int {
      f: float? = 3;
      println(show(f));
      println(show(5));
      f = 9;
      println(show(f));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1.5\n2.5\n4.5\n");
  g.expectNoLeaks("IntPromotesIntoFloatOptional");
}

TEST(OptionalPrimitive, ExtremeValuesRoundTrip) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn back(o: int?) -> int { match o { n: int { return n; } None { return 0; } } }
    fn backf(o: float?) -> float { match o { f: float { return f; } None { return 0.0; } } }
    fn backc(o: char?) -> char { match o { c: char { return c; } None { return 'a'; } } }
    fn main() -> int {
      big: int = 9223372036854775807;
      small: int = -9223372036854775807 - 1;
      println(Str<int>(back(big)));
      println(Str<int>(back(small)));
      println(Str<bool>(back(small) == small));
      nan: float = 0.0 / 0.0;
      inf: float = 1.0 / 0.0;
      println(Str<float>(backf(nan)));
      println(Str<float>(backf(inf)));
      println(Str<float>(backf(-inf)));
      println(Str<float>(backf(-0.0)));
      println(Str<int>(Str<char>(backc('~')).len()));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "9223372036854775807\n-9223372036854775808\nTrue\n"
                      "nan\ninf\n-inf\n-0\n1\n");
  g.expectNoLeaks("ExtremeValuesRoundTrip");
}

TEST(OptionalPrimitive, MatchBindingIsAPlainValue) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn main() -> int {
      o: int? = 10;
      match o {
        n: int {
          n = n + 5;                 // the binding is an ordinary int
          o = None;                  // the subject box stays alive for the arm
          println(Str<int>(n));
        }
        None { println("none"); }
      }
      println(o);
      total = 0;
      xs: int?[4] = [1, 2, 3, None];
      i = 0;
      while (i < xs.len()) {
        match xs[i] {
          v: int { if (v == 2) { i = i + 1; continue; } total = total + v; }
          None { break; }
        }
        i = i + 1;
      }
      println(Str<int>(total));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "15\nNone\n4\n");
  g.expectNoLeaks("MatchBindingIsAPlainValue");
}

TEST(OptionalPrimitive, WildcardAndNoneArmsAndEarlyReturn) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn sign(o: int?) -> int {
      match o {
        n: int { if (n < 0) { return -1; } return 1; }
        _ { }
      }
      return 0;
    }
    fn main() -> int {
      println(Str<int>(sign(-4)));
      println(Str<int>(sign(4)));
      println(Str<int>(sign(None)));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "-1\n1\n0\n");
  g.expectNoLeaks("WildcardAndNoneArmsAndEarlyReturn");
}

// -- Comparisons

TEST(OptionalPrimitive, CompareAgainstNoneAndEachOther) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn main() -> int {
      a: int? = 5;  b: int? = 5;  c: int? = 6;  n: int? = None;  m: int? = None;
      println(Str<bool>(a == None));
      println(Str<bool>(None != a));
      println(Str<bool>(n == None));
      println(Str<bool>(a == b));
      println(Str<bool>(a != c));
      println(Str<bool>(a == n));
      println(Str<bool>(n == m));
      t: bool? = True;  u: bool? = True;
      println(Str<bool>(t == u));
      x: char? = 'a';  y: char? = 'b';
      println(Str<bool>(x == y));
      z: float? = -0.0;  w: float? = 0.0;
      println(Str<bool>(z == w));
      nan: float = 0.0 / 0.0;
      p: float? = nan;  q: float? = nan;
      println(Str<bool>(p == q));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "False\nTrue\nTrue\nTrue\nTrue\nFalse\nTrue\nTrue\n"
                      "False\nTrue\nFalse\n");
  g.expectNoLeaks("CompareAgainstNoneAndEachOther");
}

// -- Sinks: params/returns, fields, arrays, tuples, ternary, copies, Obj

TEST(OptionalPrimitive, ParamsAndReturns) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn half(n: int) -> int? {
      if (n % 2 != 0) { return None; }
      return n / 2;
    }
    fn orZero(o: int?) -> int { match o { n: int { return n; } None { return 0; } } }
    fn pass(o: int?) -> int? { return o; }
    fn main() -> int {
      println(half(10));
      println(half(7));
      println(Str<int>(orZero(pass(half(8)))));
      println(Str<int>(orZero(pass(None))));
      i = 0;
      sum = 0;
      while (i < 20) { sum = sum + orZero(half(i)); i = i + 1; }
      println(Str<int>(sum));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "5\nNone\n4\n0\n45\n");
  g.expectNoLeaks("ParamsAndReturns");
}

TEST(OptionalPrimitive, FieldsStartAsNoneAndAreReleased) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    class Reading {
      value: float?;
      ok: bool?;
      tag: char?;
      count: int?;
      fn get() -> int? { return self.count; }
      fn bump() {
        match self.count {
          n: int { self.count = n + 1; }
          None { self.count = 1; }
        }
      }
    }

    impl Reading {
      fn __init__() { }
    }
    fn main() -> int {
      r = Reading();
      println(r.value); println(r.ok); println(r.tag); println(r.count);
      r.value = 2.5; r.ok = True; r.tag = 'k';
      r.bump(); r.bump(); r.bump();
      println(r.value); println(r.ok); println(r.tag); println(r.get());
      r.count = None;
      println(r.get());
      r.count = 4;
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "None\nNone\nNone\nNone\n2.5\nTrue\nk\n3\nNone\n");
  g.expectNoLeaks("FieldsStartAsNoneAndAreReleased");
}

// A `bool?` or `char?` is two bytes, an `int?` or `float?` sixteen, each at
// its payload's alignment: small optionals sit between other fields and in
// their own arrays without disturbing their neighbours.
TEST(OptionalPrimitive, SmallOptionalsKeepTheirNeighbours) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    class Mixed {
      a: bool?;
      n: int;
      b: char?;
      c: bool?;
      f: float?;
      d: char?;
      s: Str;
      e: int?;
    }
    fn main() -> int {
      m = Mixed(True, -1, 'x', None, 2.5, 'y', "str", 7);
      println(m.a); println(Str(m.n)); println(m.b); println(m.c);
      println(m.f); println(m.d); println(m.s); println(m.e);
      m.c = False; m.b = None; m.n = 9;
      println(m.a); println(m.b); println(m.c); println(m.d); println(Str(m.n));
      bs: bool?[8] = [True; 8];
      cs: char?[8] = ['a'; 8];
      i: int = 0;
      while (i < 8) {
        if (i % 4 == 0) { bs[i] = None; cs[i] = 'z'; }
        else { bs[i] = i % 2 == 0; cs[i] = None; }
        i = i + 1;
      }
      println(Str(bs.len()) + " " + Str(cs.len()));
      println(bs[0]); println(bs[1]); println(bs[2]); println(cs[4]);
      println(cs[5]); println(bs[7]); println(cs[7]);
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\n-1\nx\nNone\n2.5\ny\nstr\n7\n"
                      "True\nNone\nFalse\ny\n9\n"
                      "8 8\nNone\nFalse\nTrue\nz\nNone\nFalse\nNone\n");
  g.expectNoLeaks("SmallOptionalsKeepTheirNeighbours");
}

TEST(OptionalPrimitive, ArraysOfOptionalPrimitives) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn main() -> int {
      xs: int?[4] = [1, 2, None, 4];
      xs[1] = None;
      xs[2] = 30;
      i = 0;
      while (i < xs.len()) { println(xs[i]); i = i + 1; }
      last = xs[xs.len() - 1];
      println(last);
      fs: float?[2] = [1, None];
      println(fs[0]);
      println(fs[1]);
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1\nNone\n30\n4\n4\n1\nNone\n");
  g.expectNoLeaks("ArraysOfOptionalPrimitives");
}

TEST(OptionalPrimitive, TupleElements) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn find(xs: view int[], key: int) -> (int?, Str) {
      i = 0;
      while (i < xs.len()) {
        if (xs[i] == key) { return (i, "found"); }
        i = i + 1;
      }
      none: int? = None;
      return (none, "missing");
    }
    fn main() -> int {
      xs: int[3] = [4, 5, 6];
      a = find(xs, 6);
      b = find(xs, 9);
      println(a.0); println(a.1);
      println(b.0); println(b.1);
      idx, msg = find(xs, 4);
      match idx { n: int { println(Str<int>(n) + " " + msg); } None { } }
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "2\nfound\nNone\nmissing\n0 found\n");
  g.expectNoLeaks("TupleElements");
}

TEST(OptionalPrimitive, TernaryCopyAndReassignment) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn main() -> int {
      i = 0;
      while (i < 4) {
        e = if i % 2 == 0 then i else None;     // int?
        println(e);
        i = i + 1;
      }
      a: int? = 1;
      b = if a == None then 5 else a;           // int?: mixes int and int?
      println(b);
      m = a;
      println(m);
      x: int? = 1;
      y: int? = x;                              // a copy
      x = 2;
      println(y);
      println(x);
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\nNone\n2\nNone\n1\n1\n1\n2\n");
  g.expectNoLeaks("TernaryCopyAndReassignment");
}

// `inout` optional values: a parameter, a local borrow and a field, written
// through (None included); a `view` parameter and an override's.
TEST(OptionalPrimitive, InoutAndViewOptionals) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn bump(o: inout int?) {
      match o { n: int { o = n + 1; } None { o = 0; } }
    }
    fn clear(o: inout float?) { o = None; }
    fn show(o: view int?) -> Str {
      match o { n: view int { return Str<int>(n); } None { return "None"; } }
    }
    class Base {
      fn pick(c: bool) -> char? { return None; }
    }

    impl Base {
      fn __init__() { }
    }
    class Derived : Base {
      fn pick(c: bool) -> char? { if (c) { return 'd'; } return None; }
    }

    impl Derived {
      fn __init__() { __super__(); }
    }
    class Box2 { n: int?; f: float?; }
    impl Box2 { fn __init__() { } }
    fn main() -> int {
      x: int? = None;
      bump(x); bump(x);
      println(show(x));
      b = Box2();
      bump(b.n);
      println(show(b.n));
      y: inout = b.n;
      bump(y);
      println(b.n);
      b.f = 1.5;
      clear(b.f);
      println(b.f);
      o: Base = Derived();
      println(o.pick(True));
      println(o.pick(False));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1\n0\n1\nNone\nd\nNone\n");
  g.expectNoLeaks("InoutAndViewOptionals");
}

// -- Generics

TEST(OptionalPrimitive, GenericArgumentsAndOptionalOfTypeParameter) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    class Cell<T> {
      v: T;
      fn get() -> T { return self.v; }
    }

    impl Cell<T> {
      fn __init__(x: T) { self.v = x; }
    }
    class Slot<T> {
      v: T?;
      fn put(x: T) { self.v = x; }
      fn take() -> T? { r = self.v; self.v = None; return r; }
    }

    impl Slot<T> {
      fn __init__() { }
    }
    fn wrap<T>(x: T) -> T? { return x; }
    fn orElse<T>(o: T?, d: T) -> T { match o { v: T { return v; } None { return d; } } }
    fn main() -> int {
      c = Cell<int?>(None);
      println(c.get());
      c.v = 8;
      println(c.get());
      s = Slot<float>();
      println(s.take());
      s.put(2);
      println(s.take());
      println(s.take());
      println(wrap(5));
      println(Str<int>(orElse(wrap(6), 0)));
      none: char? = None;
      println(Str<char>(orElse(none, '?')));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "None\n8\nNone\n2\nNone\n5\n6\n?\n");
  g.expectNoLeaks("GenericArgumentsAndOptionalOfTypeParameter");
}

// -- Inline storage: arrays, tuples, generics (#96)

// An `int?[n]` & co. holds its elements inline: every value, None included,
// is kept in place, and nothing is allocated per element.
TEST(OptionalPrimitive, ArraysHoldOptionalsInline) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn main() -> int {
      xs: int?[9] = [0; 9];
      i = 0;
      while (i < xs.len()) {
        if (i % 3 == 0) { xs[i] = None; } else { xs[i] = i; }
        i = i + 1;
      }
      sum = 0;
      nones = 0;
      i = 0;
      while (i < xs.len()) {
        match xs[i] { n: int { sum = sum + n; } None { nones = nones + 1; } }
        i = i + 1;
      }
      println(Str<int>(sum) + " " + Str<int>(nones));
      println(xs[0]); println(xs[1]);
      cs: char?[3] = ['a', None, 'c'];
      bs: bool?[3] = [True, None, False];
      fs: float?[3] = [1, None, -0.5];
      j = 0;
      while (j < 3) { print(cs[j]); print(" "); print(bs[j]); print(" "); println(fs[j]); j = j + 1; }
      cs[1] = 'b';
      println(cs[1]);
      println(Str<bool>(cs[2] == cs[0]));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "27 3\nNone\n1\na True 1\nNone None None\nc False "
                      "-0.5\nb\nFalse\n");
  g.expectNoLeaks("ArraysHoldOptionalsInline");
}

// A tuple holds an optional value element inline, its presence flag and then
// its payload: printing, equality and the elements after it are unaffected.
TEST(OptionalPrimitive, TuplesHoldOptionalsInline) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn pair(n: int) -> (int?, Str, float?) {
      if (n < 0) { return (None, "neg", None); }
      return (n, "pos", n * 0.5);
    }
    fn main() -> int {
      a = pair(3);
      b = pair(-1);
      println(a); println(b);
      println(a.1 + " " + b.1);
      println(a.2); println(b.0);
      println(Str<bool>(a == pair(3)));
      println(Str<bool>(a == b));
      println(Str<bool>(b == pair(-2)));
      n, s, f = pair(4);
      println(n); println(s); println(f);
      t: (char?, bool?) = ('x', None);
      println(t);
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "(3, pos, 1.5)\n(None, neg, None)\npos neg\n1.5\nNone\n"
                      "True\nFalse\nTrue\n4\npos\n2\n(x, None)\n");
  g.expectNoLeaks("TuplesHoldOptionalsInline");
}

// #96's generics case: a `Stack<int>` whose `pop()` returns `T?`, and an
// array of `T?` in a template.
TEST(OptionalPrimitive, GenericStackPopsAnOptional) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    class Stack<T> {
      items: T[8];
      n: int;
      fn push(x: T) {
        self.items[self.n] = x;
        self.n = self.n + 1;
      }
      fn pop() -> T? {
        if (self.n == 0) { return None; }
        self.n = self.n - 1;
        return self.items[self.n];
      }
    }

    impl Stack<T> {
      fn __init__(fill: T) {
        self.items = [fill; 8];
        self.n = 0;
      }
    }
    class Slots<T> {
      s: T?[3];
      fn put(i: int, x: T) { self.s[i] = x; }
      fn get(i: int) -> T? { return self.s[i]; }
    }

    impl Slots<T> {
      fn __init__() { self.s = [None, None, None]; }
    }
    fn main() -> int {
      st = Stack<int>(0);
      i = 0;
      while (i < 8) { st.push(i * 10); i = i + 1; }
      total = 0;
      going = True;
      while (going) {
        match st.pop() { n: int { total = total + n; } None { going = False; } }
      }
      println(Str<int>(total));
      sl = Slots<float>();
      sl.put(1, 2.5);
      println(sl.get(0)); println(sl.get(1));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "280\nNone\n2.5\n");
  g.expectNoLeaks("GenericStackPopsAnOptional");
}

// -- PIR inline optionals (#96)

// `some`, `none`, `is_some` and `unwrap` on both backends, with optionals in
// locals, parameters, results, fields (a new object's are absent; an
// opt_char and an opt_bool field are 16 bytes like the others), a vtable
// slot's signature, `select` and through addresses; `unwrap` of an absent
// optional is its payload type's zero.
TEST(OptionalPrimitive, PIRInlineOptionals) {
  LeakGuard g;
  auto r = compileAndRunPIR(R"(module "t"
class C {
  field c: opt_char
  field n: opt_i64
  field f: opt_f64
  field b: opt_bool
  vtable {
    destroy = @C.destroy : (obj) -> void
    pick = @C.pick : (obj, opt_char, bool) -> opt_char
  }
}

fn @C.destroy(%self: obj) -> void {
  ret
}

fn @C.pick(%self: obj, %c: opt_char, %keep: bool) -> opt_char {
  %n = none opt_char
  %r = select %keep, %c, %n
  ret %r
}

fn @inc(%o: opt_i64) -> i64 {
  %has = is_some %o
  %v = unwrap %o
  %w = add %v, 1
  %r = select %has, %w, 100
  ret %r
}

fn @dbl(%p: ptr) -> void {
  %o = ptr.load opt_i64, %p
  %v = unwrap %o
  %w = mul %v, 2
  %s = some %w
  ptr.store %p, %s
  ret
}

fn @main() -> i64 {
  local %x: opt_i64
  local %b: opt_bool
  %five = some 5
  store %x, %five
  %px = local.addr %x
  call @dbl(%px)
  %x2 = load %x
  %a = call @inc(%x2)
  %none = none opt_i64
  %z = unwrap %none
  %c = call @inc(%none)
  %o = new C
  %fn = field.load %o, C.n
  %d = call @inc(%fn)
  field.store %o, C.n, %five
  %pn = field.addr %o, C.n
  call @dbl(%pn)
  %fn2 = field.load %o, C.n
  %e = call @inc(%fn2)
  %ff = field.load %o, C.f
  %hasf = is_some %ff
  %fsome = some 2.5
  field.store %o, C.f, %fsome
  %ff2 = field.load %o, C.f
  %bt = some true
  field.store %o, C.b, %bt
  %cz = some 'z'
  field.store %o, C.c, %cz
  %fv = unwrap %ff2
  %fi = ftoi %fv
  %ch = some 'a'
  %k1 = vcall %o : C [1] (%ch, true)
  %k2 = vcall %o : C [1] (%ch, false)
  %hk1 = is_some %k1
  %hk2 = is_some %k2
  %kc = unwrap %k1
  %kci = cast %kc to i64
  %tru = some true
  store %b, %tru
  %bl = load %b
  %bv0 = unwrap %bl
  %fb = field.load %o, C.b
  %fbv = unwrap %fb
  %bv = select %fbv, %bv0, false
  free %o
  %s1 = add %a, %z
  %c0 = sub %c, 100
  %d0 = sub %d, 100
  %s2 = add %s1, %c0
  %s3 = add %s2, %d0
  %s4 = add %s3, %e
  %s5 = add %s4, %fi
  %k7 = sub %kci, 90
  %s6 = add %s5, %k7
  %t1 = select %hk1, 1, 0
  %t2 = select %hk2, 50, 0
  %t3 = select %hasf, 50, 0
  %t4 = select %bv, 3, 0
  %s7 = add %s6, %t1
  %s8 = add %s7, %t2
  %s9 = add %s8, %t3
  %s10 = add %s9, %t4
  ret %s10
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  // a 11 + z 0 + c 0 + d 0 + e 11 + fi 2 + 'a' - 90 7 + hk1 1 + bv 3.
  EXPECT_EQ(r.ExitCode, 35) << r.StdErr;
  g.expectNoLeaks("inline optionals");
}
