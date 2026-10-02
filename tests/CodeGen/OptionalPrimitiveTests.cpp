// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// CodeGen / E2E tests for optional primitives `int?`, `float?`, `bool?` and
// `char?` (#66).  A present value is boxed in the runtime's Int / Float /
// Bool / Char object, so every test runs under the tracking allocator and
// asserts zero live heap blocks: each boxing site must hand its sink exactly
// one reference, and each sink must release it.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

using namespace paykan::test;

// ============================================================================
// Declarations, None, printing, match
// ============================================================================

TEST(OptionalPrimitive, EveryPrimitiveSomeAndNone) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn si(o: int?) -> Str {
      match o { n: int { return "int " + StrInt(n); } None { return "no int"; } }
    }
    fn sf(o: float?) -> Str {
      match o { f: float { return "float " + StrFloat(f); } None { return "no float"; } }
    }
    fn sb(o: bool?) -> Str {
      match o { b: bool { return "bool " + StrBool(b); } None { return "no bool"; } }
    }
    fn sc(o: char?) -> Str {
      match o { c: char { return "char " + StrChar(c); } None { return "no char"; } }
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
      match o { f: float { return StrFloat(f / 2.0); } None { return "-"; } }
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
      println(StrInt(back(big)));
      println(StrInt(back(small)));
      println(StrBool(back(small) == small));
      nan: float = 0.0 / 0.0;
      inf: float = 1.0 / 0.0;
      println(StrFloat(backf(nan)));
      println(StrFloat(backf(inf)));
      println(StrFloat(backf(-inf)));
      println(StrFloat(backf(-0.0)));
      println(StrInt(StrChar(backc('~')).len()));
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
          println(StrInt(n));
        }
        None { println("none"); }
      }
      println(o);
      total = 0;
      xs: int?[] = [1, 2, 3];
      xs.push(None);
      i = 0;
      while (i < xs.len()) {
        match xs[i] {
          v: int { if (v == 2) { i = i + 1; continue; } total = total + v; }
          None { break; }
        }
        i = i + 1;
      }
      println(StrInt(total));
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
      println(StrInt(sign(-4)));
      println(StrInt(sign(4)));
      println(StrInt(sign(None)));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "-1\n1\n0\n");
  g.expectNoLeaks("WildcardAndNoneArmsAndEarlyReturn");
}

// ============================================================================
// Comparisons
// ============================================================================

TEST(OptionalPrimitive, CompareAgainstNoneAndEachOther) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn main() -> int {
      a: int? = 5;  b: int? = 5;  c: int? = 6;  n: int? = None;  m: int? = None;
      println(StrBool(a == None));
      println(StrBool(None != a));
      println(StrBool(n == None));
      println(StrBool(a == b));
      println(StrBool(a != c));
      println(StrBool(a == n));
      println(StrBool(n == m));
      t: bool? = True;  u: bool? = True;
      println(StrBool(t == u));
      x: char? = 'a';  y: char? = 'b';
      println(StrBool(x == y));
      z: float? = -0.0;  w: float? = 0.0;
      println(StrBool(z == w));
      nan: float = 0.0 / 0.0;
      p: float? = nan;  q: float? = nan;
      println(StrBool(p == q));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "False\nTrue\nTrue\nTrue\nTrue\nFalse\nTrue\nTrue\n"
                      "False\nTrue\nFalse\n");
  g.expectNoLeaks("CompareAgainstNoneAndEachOther");
}

// ============================================================================
// Sinks: params/returns, fields, arrays, tuples, ternary, mov, Obj
// ============================================================================

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
      println(StrInt(orZero(pass(half(8)))));
      println(StrInt(orZero(pass(None))));
      i = 0;
      sum = 0;
      while (i < 20) { sum = sum + orZero(half(i)); i = i + 1; }
      println(StrInt(sum));
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
      fn __init__() { }
      fn get() -> int? { return self.count; }
      fn bump() {
        match self.count {
          n: int { self.count = n + 1; }
          None { self.count = 1; }
        }
      }
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

TEST(OptionalPrimitive, ArraysOfOptionalPrimitives) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn main() -> int {
      xs: int?[] = [1, 2];
      xs.push(None);
      xs.push(4);
      xs[1] = None;
      xs[2] = 30;
      i = 0;
      while (i < xs.len()) { println(xs[i]); i = i + 1; }
      last = xs.pop();
      println(last);
      fs: float?[] = [];
      fs.push(1);
      fs.push(None);
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
    fn find(xs: int[], key: int) -> (int?, Str) {
      i = 0;
      while (i < xs.len()) {
        if (xs[i] == key) { return (i, "found"); }
        i = i + 1;
      }
      none: int? = None;
      return (none, "missing");
    }
    fn main() -> int {
      xs: int[] = [4, 5, 6];
      a = find(xs, 6);
      b = find(xs, 9);
      println(a.0); println(a.1);
      println(b.0); println(b.1);
      idx, msg = find(xs, 4);
      match idx { n: int { println(StrInt(n) + " " + msg); } None { } }
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "2\nfound\nNone\nmissing\n0 found\n");
  g.expectNoLeaks("TupleElements");
}

TEST(OptionalPrimitive, TernaryMovAndReassignment) {
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
      m = mov a;
      println(m);
      x: int? = 1;
      y: int? = x;                              // shares x's box
      x = 2;
      println(y);
      println(x);
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\nNone\n2\nNone\n1\n1\n1\n2\n");
  g.expectNoLeaks("TernaryMovAndReassignment");
}

TEST(OptionalPrimitive, IntoObjAsBoxedClasses) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn kind(o: Obj) -> Str {
      match o {
        i: Int   { return "Int"; }
        f: Float { return "Float"; }
        b: Bool  { return "Bool"; }
        c: Char  { return "Char"; }
        _        { return "other"; }
      }
    }
    fn main() -> int {
      a: int? = 1;  f: float? = 1.5;  b: bool? = True;  c: char? = 'c';
      n: int? = None;
      println(kind(a)); println(kind(f)); println(kind(b)); println(kind(c));
      println(kind(n));
      o: Obj = c;
      println(o);
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Int\nFloat\nBool\nChar\nother\nc\n");
  g.expectNoLeaks("IntoObjAsBoxedClasses");
}

// ============================================================================
// Generics
// ============================================================================

TEST(OptionalPrimitive, GenericArgumentsAndOptionalOfTypeParameter) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    class Cell<T> {
      v: T;
      fn __init__(x: T) { self.v = x; }
      fn get() -> T { return self.v; }
    }
    class Slot<T> {
      v: T?;
      fn __init__() { }
      fn put(x: T) { self.v = x; }
      fn take() -> T? { r = self.v; self.v = None; return r; }
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
      println(StrInt(orElse(wrap(6), 0)));
      none: char? = None;
      println(StrChar(orElse(none, '?')));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "None\n8\nNone\n2\nNone\n5\n6\n?\n");
  g.expectNoLeaks("GenericArgumentsAndOptionalOfTypeParameter");
}
