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
