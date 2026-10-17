// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen / E2E tests for the builtin Buffer<T> (docs/language/05-arrays.md,
// "Buffer"), whose methods are the runtime's PaykanBuffer_* functions.
// Every run is checked for zero live heap blocks.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

void expectRuns(const std::string &main, const std::string &expected) {
  LeakGuard g;
  auto r = compileAndRun(main);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, expected);
  g.expectNoLeaks("buffer");
}

} // namespace

TEST(Buffer, StartsWithThreeZeroedSlots) {
  expectRuns(R"(
    fn main() -> int {
      b = Buffer<int>();
      println(Str<int>(b.len()));
      b.set(0, 10);
      b.set(2, 30);
      n: int = b.get(0) + b.get(1) + b.get(2);
      println(Str<int>(n));
      return 0;
    }
  )",
             "3\n40\n");
}

TEST(Buffer, ReserveGrowAndShrinkChangeTheLength) {
  expectRuns(R"(
    fn main() -> int {
      b = Buffer<float>();
      b.grow(2);
      b.set(4, 2.5);
      println(Str<int>(b.len()) + " " + Str<float>(b.get(4)));
      b.shrink(4);
      println(Str<int>(b.len()));
      b.reserve(8);
      b.reserve(2);
      println(Str<int>(b.len()) + " " + Str<float>(b.get(4)));
      return 0;
    }
  )",
             "5 2.5\n1\n8 0\n");
}

TEST(Buffer, HoldsReferences) {
  expectRuns(R"(
    class P {
      x: int;
    }

    impl P {
      fn __init__(x: int) { self.x = x; }
    }
    fn main() -> int {
      s = Buffer<Str>();
      s.set(0, "a");
      s.set(1, "b");
      s.set(2, "c");
      s.set(1, "B");
      println(s.get(0) + s.get(1) + s.get(2));
      s.shrink(2);
      ps = Buffer<P>();
      p = P(42);
      ps.set(1, p);
      ps.set(2, p);
      println(Str<int>(ps.get(1).x + ps.get(2).x));
      return 0;
    }
  )",
             "aBc\n84\n");
}

TEST(Buffer, OptionalSlotsStartAsNone) {
  expectRuns(R"(
    fn show(o: view Str?) {
      match o {
        v: Str { println(v); }
        None { println("None"); }
      }
    }
    fn main() -> int {
      b = Buffer<Str?>();
      b.set(2, "x");
      show(b.get(0));
      show(b.get(2));
      b.set(2, None);
      show(b.get(2));
      return 0;
    }
  )",
             "None\nx\nNone\n");
}

// An optional value is its presence flag and payload: a `char?` takes two
// bytes and an `int?` sixteen, a new one is None, and both survive growing.
TEST(Buffer, HoldsOptionalValuesOfEverySize) {
  expectRuns(R"(
    fn c(o: view char?) -> Str {
      match o { v: char { return Str<char>(v); } None { return "-"; } }
    }
    fn n(o: view int?) -> Str {
      match o { v: int { return Str<int>(v); } None { return "-"; } }
    }
    fn main() -> int {
      cs = Buffer<char?>();
      ns = Buffer<int?>();
      cs.grow(20);
      ns.grow(20);
      i: int = 0;
      while (i < 23) {
        if (i % 3 == 0) { cs.set(i, 'a'); ns.set(i, i * 1000); }
        i = i + 1;
      }
      cs.set(1, None);
      s = "";
      i = 0;
      while (i < 7) { s = s + c(cs.get(i)) + n(ns.get(i)) + " "; i = i + 1; }
      println(s);
      println(c(cs.copy(21)) + n(ns.copy(21)) + " " + c(cs.get(22)));
      return 0;
    }
  )",
             "a0 -- -- a3000 -- -- a6000 \na21000 -\n");
}

// A bool takes one byte; a copy outlives the value it was taken from.
TEST(Buffer, HoldsBoolsAndCopiesOut) {
  expectRuns(R"(
    fn main() -> int {
      b = Buffer<bool>();
      b.grow(13);
      b.set(1, True);
      b.set(15, True);
      println(Str<bool>(b.get(0)) + " " + Str<bool>(b.get(1)) + " " +
              Str<bool>(b.copy(15)));
      s = Buffer<Str>();
      s.set(0, "x");
      t: Str = s.copy(0);
      s.set(0, "y");
      println(t + s.get(0));
      return 0;
    }
  )",
             "False True True\nxy\n");
}

// A buffer is shared like any object: what one variable changes, another
// sees.  It can be a field of a generic class, a parameter whose element
// type is inferred, and a result.
TEST(Buffer, IsSharedAndGeneric) {
  expectRuns(R"(
    class Stack<T> {
      items: Buffer<T>;
    }
    impl Stack<T> {
      fn __init__() {
        self.items = Buffer<T>();
        self.items.shrink(self.items.len());
      }
      fn push(v: view T) {
        self.items.grow(1);
        self.items.set(self.items.len() - 1, v);
      }
      fn pop() -> T {
        v: T = self.items.copy(self.items.len() - 1);
        self.items.shrink(1);
        return v;
      }
    }
    fn count<T>(b: view Buffer<T>) -> int { return b.len(); }
    fn squares(n: int) -> Buffer<int> {
      b = Buffer<int>();
      b.reserve(n);
      i = 0;
      while (i < n) { b.set(i, i * i); i = i + 1; }
      return b;
    }
    fn main() -> int {
      b = squares(4);
      alias = b;
      alias.grow(1);
      alias.set(4, 16);
      s = Stack<Str>();
      s.push("a");
      s.push("b" + "c");
      top = s.pop();
      println(Str(b.len()) + " " + Str(b.get(4)) + " " + Str(count(b)) + " " +
              top + Str(count(s.items)) + " " + Str(b == alias));
      return 0;
    }
  )",
             "5 16 5 bc1 True\n");
}
