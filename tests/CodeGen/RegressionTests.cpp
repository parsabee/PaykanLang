// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Regression tests for crashers and miscompiles found after the fact.  Every
// runtime test runs under the tracking allocator and asserts zero live heap
// blocks, since most of these bugs were ownership imbalances.

#include "TestUtils.h"
#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

using namespace paykan::test;

namespace {

// RAII guard: enable tracking allocator before test, check zero leaks after.
struct LeakGuard {
  LeakGuard() {
    // Flush harness output still buffered in stdout, so it is not captured as
    // the program's output once compileAndRun redirects the descriptor.
    fflush(stdout);
    Paykan_heap_set_tracking(1);
    Paykan_heap_reset();
  }
  ~LeakGuard() { Paykan_heap_set_tracking(0); }
  void expectNoLeaks(const char *label = "") const {
    int64_t live = Paykan_heap_live_blocks();
    EXPECT_EQ(live, 0) << "heap leak in: " << label << " (" << live
                       << " live blocks)";
  }
};

} // namespace

// ============================================================================
// Ref-typed (array / tuple) parameters of user methods
// ============================================================================

TEST(Regression, MethodArrayParam) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Bag {
      fn take(ys: int[]) -> int { return ys.len(); }
    }
    fn main() -> int {
      b = Bag();
      ys: int[] = [1, 2, 3];
      println(StrInt(b.take(ys)));
      println(StrInt(b.take([4, 5])));
      println(StrInt(ys.len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "3\n2\n3\n");
  g.expectNoLeaks("MethodArrayParam");
}

TEST(Regression, MethodObjectArrayParam) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class P {
      x: int;
      fn __init__(x: int) { self.x = x; }
    }
    class Bag {
      fn total(ps: P[]) -> int {
        s = 0;
        i = 0;
        while (i < ps.len()) { s = s + ps[i].x; i = i + 1; }
        return s;
      }
      fn count(ss: Str[]) -> int { return ss.len(); }
    }
    fn main() -> int {
      b = Bag();
      ps: P[] = [P(1), P(2)];
      println(StrInt(b.total(ps)));
      println(StrInt(b.total([P(3), P(4)])));
      println(StrInt(b.count(["a", "b", "c"])));
      println(StrInt(ps[1].x));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "3\n7\n3\n2\n");
  g.expectNoLeaks("MethodObjectArrayParam");
}

TEST(Regression, MethodTupleParam) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Bag {
      fn second(t: (int, Str)) -> Str { return t.1; }
      fn first(t: (int, Str)) -> int { return t.0; }
    }
    fn main() -> int {
      b = Bag();
      t: (int, Str) = (1, "one");
      println(b.second(t));
      println(b.second((2, "two")));
      println(StrInt(b.first(t)));
      println(t.1);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "one\ntwo\n1\none\n");
  g.expectNoLeaks("MethodTupleParam");
}

TEST(Regression, MethodEqualsOverrideAndArrayEquals) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Bag {
      n: int;
      fn __init__(n: int) { self.n = n; }
      fn equals(o: Obj) -> bool { return True; }
    }
    fn main() -> int {
      b = Bag(1);
      c = Bag(2);
      ys: int[] = [1, 2, 3];
      zs: int[] = ys;
      println(StrBool(b == c));
      println(StrBool(b.equals(c)));
      println(StrBool(ys == zs));
      println(StrBool(ys.equals([1, 2, 3])));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  // Array equality is reference identity.
  EXPECT_EQ(r.StdOut, "True\nTrue\nTrue\nFalse\n");
  g.expectNoLeaks("MethodEqualsOverrideAndArrayEquals");
}

// ============================================================================
// Inferred-type variables initialised from a ref-typed ternary
// ============================================================================

TEST(Regression, InferredClassTernaryReceiver) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Point {
      x: int;
      y: int;
      fn __init__(x: int, y: int) { self.x = x; self.y = y; }
      fn sum() -> int { return self.x + self.y; }
    }
    class Animal { fn sound() -> Str { return "..."; } }
    class Dog : Animal { fn sound() -> Str { return "woof"; } }
    class Cat : Animal { fn sound() -> Str { return "meow"; } }
    fn pick(c: bool) -> int {
      p = Point(1, 2);
      q = Point(10, 20);
      r = if c then p else q;
      return r.sum() + r.x;
    }
    fn main() -> int {
      println(StrInt(pick(True)));
      println(StrInt(pick(False)));
      c = False;
      r = if c then Point(3, 3) else Point(4, 4);
      println(StrInt(r.sum()));
      a = if c then Dog() else Cat();
      println(a.sound());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "4\n40\n8\nmeow\n");
  g.expectNoLeaks("InferredClassTernaryReceiver");
}

TEST(Regression, InferredArrayTupleOptionalTernary) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Point {
      x: int;
      fn __init__(x: int) { self.x = x; }
      fn get() -> int { return self.x; }
    }
    fn run(c: bool) -> int {
      p = Point(1);
      q = Point(2);
      ps: Point[] = [p];
      arr = if c then ps else [q, q];
      t: (int, Point) = (5, p);
      tup = if c then t else (6, q);
      opt = if c then p else None;
      n = 0;
      if (opt != None) { n = 100; }
      return arr.len() * 1000 + arr[0].get() * 10 + tup.1.get() + tup.0 + n;
    }
    fn main() -> int {
      println(StrInt(run(True)));
      println(StrInt(run(False)));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "1116\n2028\n");
  g.expectNoLeaks("InferredArrayTupleOptionalTernary");
}
// ============================================================================
// Array subscript: char elements and temporary (call-rooted) receivers
// ============================================================================

TEST(Regression, CharArrayElements) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Word {
      cs: char[];
      fn __init__() { self.cs = ['h', 'i']; }
      fn first() -> char { return self.cs[0]; }
    }
    fn isA(c: char) -> bool { return c == 'a'; }
    fn second(cs: char[]) -> char { return cs[1]; }
    fn main() -> int {
      cs: char[] = ['a', 'b'];
      println(StrChar(cs[1]));
      if (cs[0] == 'a') { println("eq"); }
      println(StrBool(isA(cs[0])));
      c: char = cs[1];
      d = cs[0];
      println(StrChar(c) + StrChar(d));
      x = 'k';
      ds: char[] = [x, 'm'];
      ds.push('z');
      ds[0] = 'q';
      println(StrChar(ds[0]) + StrChar(second(ds)) + StrChar(ds.pop()));
      println(StrInt(ds.len()));
      w = Word();
      println(StrChar(w.first()) + StrChar(w.cs[1]));
      s: Str = "yo";
      println(StrChar(s[1]));
      bs: bool[] = [True, False];
      bs.push(True);
      println(StrBool(bs.pop()) + StrBool(bs[1]));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "b\neq\nTrue\nba\nqmz\n2\nhi\no\nTrueFalse\n");
  g.expectNoLeaks("CharArrayElements");
}

TEST(Regression, CallRootedObjectSubscript) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Point {
      x: int;
      fn __init__(x: int) { self.x = x; }
      fn get() -> int { return self.x; }
    }
    class Holder {
      items: Point[];
      fn __init__() { self.items = [Point(7)]; }
    }
    fn mk() -> Point[] { return [Point(1), Point(2)]; }
    fn mks() -> Str[] { return ["a", "b"]; }
    fn mkn() -> int[][] { return [[1, 2], [3]]; }
    fn mkh() -> Holder { return Holder(); }
    fn take(p: Point) -> int { return p.x; }
    fn ret() -> Point { return mk()[1]; }
    fn main() -> int {
      println(StrInt(mk()[0].x));
      println(StrInt(mk()[1].get()));
      p = mk()[0];
      q: Point = mk()[1];
      println(StrInt(p.x + q.x));
      q = mk()[0];
      println(StrInt(q.x));
      println(StrInt(take(mk()[0])));
      s = mks()[0];
      println(s + mks()[1]);
      println(StrInt(mkn()[0][1]));
      println(StrInt(mkn()[1].len()));
      println(StrInt(mkh().items[0].x));
      println(StrInt(ret().x));
      ps: Point[] = [mk()[1]];
      println(StrInt(ps[0].x));
      match mk()[0] {
        pp: Point { println(StrInt(pp.x)); }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "1\n2\n3\n1\n1\nab\n2\n1\n7\n2\n2\n1\n");
  g.expectNoLeaks("CallRootedObjectSubscript");
}

TEST(Regression, CallRootedSubscriptAssign) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Point {
      x: int;
      fn __init__(x: int) { self.x = x; }
    }
    fn mk() -> Point[] { return [Point(1), Point(2)]; }
    fn mki() -> int[] { return [1, 2]; }
    fn mkn() -> int[][] { return [[1, 2], [3]]; }
    fn main() -> int {
      mk()[0] = Point(9);
      mki()[1] = 5;
      mkn()[0] = [5];
      mkn()[1][0] = 4;
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  g.expectNoLeaks("CallRootedSubscriptAssign");
}