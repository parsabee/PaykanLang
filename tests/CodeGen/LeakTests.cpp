// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Heap leak tests: verify every allocation site releases to zero live blocks.
// Uses the tracking allocator (Paykan_heap_set_tracking / Paykan_heap_live_blocks).

#include "TestUtils.h"
#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

using namespace paykan::test;

// RAII guard: enable tracking allocator before test, check zero leaks after.
struct LeakGuard {
  LeakGuard()  { Paykan_heap_set_tracking(1); Paykan_heap_reset(); }
  ~LeakGuard() { Paykan_heap_set_tracking(0); }
  void expectNoLeaks(const char *label = "") const {
    int64_t live = Paykan_heap_live_blocks();
    EXPECT_EQ(live, 0) << "heap leak in: " << label
                       << " (" << live << " live blocks)";
  }
};

// ============================================================================
// String literal (samples/leak-check/01)
// ============================================================================

TEST(Leak, StringLiteral) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      s: Str = "hello";
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("StringLiteral");
}

// ============================================================================
// String concatenation (samples/leak-check/02)
// ============================================================================

TEST(Leak, StringConcat) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      a: Str = "foo";
      b: Str = "bar";
      c: Str = a + b;
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("StringConcat");
}

// ============================================================================
// StrInt conversion (samples/leak-check/03)
// ============================================================================

TEST(Leak, StrInt) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      n: int = 42;
      s: Str = StrInt(n);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("StrInt");
}

// ============================================================================
// int[] array literal (samples/leak-check/04)
// ============================================================================

TEST(Leak, IntArrayLiteral) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      a: int[] = [1, 2, 3];
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("IntArrayLiteral");
}

// ============================================================================
// Str[] array with push (samples/leak-check/05)
// ============================================================================

TEST(Leak, StrArrayPush) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      arr: Str[] = [];
      arr.push("one");
      arr.push("two");
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("StrArrayPush");
}

// ============================================================================
// Object array (samples/leak-check/06)
// ============================================================================

TEST(Leak, ObjArray) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      arr: Obj[] = [];
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("ObjArray");
}

// ============================================================================
// Fieldless class instance (samples/leak-check/07)
// ============================================================================

TEST(Leak, ClassEmpty) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Box {
      fn __init__() {}
    }
    fn main() -> int {
      b: Box = Box();
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("ClassEmpty");
}

// ============================================================================
// Class with Str field (samples/leak-check/08)
// ============================================================================

TEST(Leak, ClassStrField) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Person {
      name: Str;
      fn __init__(n: Str) { name = n; }
    }
    fn main() -> int {
      p: Person = Person("Alice");
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("ClassStrField");
}

// ============================================================================
// Class inheritance (samples/leak-check/09)
// ============================================================================

TEST(Leak, ClassInherit) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Animal {
      name: Str;
      fn __init__(n: Str) { name = n; }
    }
    class Dog : Animal {
      fn __init__(n: Str) { __super__(n); }
    }
    fn main() -> int {
      d: Dog = Dog("Rex");
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("ClassInherit");
}

// ============================================================================
// String assignment rebind (ensures no double-wrap leak — regression for #22)
// ============================================================================

TEST(Leak, StringRebind) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      s: Str = "first";
      s = "second";
      s = "third";
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("StringRebind");
}

// ============================================================================
// String returned from function
// ============================================================================

TEST(Leak, StringReturn) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn greet(name: Str) -> Str {
      return "Hello " + name;
    }
    fn main() -> int {
      s: Str = greet("World");
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("StringReturn");
}

// ============================================================================
// Early return from function with string locals
// ============================================================================

TEST(Leak, EarlyReturn) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn maybe(flag: bool) -> int {
      s: Str = "allocated";
      if (flag) { return 1; }
      return 0;
    }
    fn main() -> int {
      maybe(True);
      maybe(False);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("EarlyReturn");
}
