// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Codegen / E2E tests for the PaykanBuffer_* runtime functions, called the
// way the standard library's memory::Buffer<T> calls them: an ordinary
// generic class whose methods are generic native functions.  Every run is
// checked for zero live heap blocks.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

// The standard library's declaration, verbatim.
const std::string kBuffer = R"(
native fn _bufferNew<T>(n: int) -> Obj = "PaykanBuffer_new";
native fn _bufferLen<T>(b: view Obj) -> int = "PaykanBuffer_len";
native fn _bufferGet<T>(b: view Obj, i: int) -> view T = "PaykanBuffer_get";
native fn _bufferCopy<T>(b: view Obj, i: int) -> T = "PaykanBuffer_copy";
native fn _bufferSet<T>(b: inout Obj, i: int, v: view T) = "PaykanBuffer_set";
native fn _bufferReserve<T>(b: inout Obj, n: int) = "PaykanBuffer_reserve";
native fn _bufferGrow<T>(b: inout Obj, by: int) = "PaykanBuffer_grow";
native fn _bufferShrink<T>(b: inout Obj, by: int) = "PaykanBuffer_shrink";

class Buffer<T> {
  __data: Obj;
  view fn len() -> int { return _bufferLen<T>(self.__data); }
  view fn get(i: int) -> view T { return _bufferGet<T>(self.__data, i); }
  fn copy(i: int) -> T { return _bufferCopy<T>(self.__data, i); }
  fn set(i: int, v: T) { _bufferSet<T>(self.__data, i, v); }
  fn reserve(n: int) { _bufferReserve<T>(self.__data, n); }
  fn grow(by: int) { _bufferGrow<T>(self.__data, by); }
  fn shrink(by: int) { _bufferShrink<T>(self.__data, by); }
}

impl Buffer<T> {
  fn __init__() { self.__data = _bufferNew<T>(3); }
}
)";

void expectRuns(const std::string &main, const std::string &expected) {
  LeakGuard g;
  auto r = compileAndRun(kBuffer + main);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, expected);
  g.expectNoLeaks("buffer");
}

} // namespace

TEST(Buffer, StartsWithThreeZeroedSlots) {
  expectRuns(R"(import ::io;
    fn main() -> int {
      b = Buffer<int>();
      io::println(Str<int>(b.len()));
      b.set(0, 10);
      b.set(2, 30);
      n: int = b.get(0) + b.get(1) + b.get(2);
      io::println(Str<int>(n));
      return 0;
    }
  )",
             "3\n40\n");
}

TEST(Buffer, ReserveGrowAndShrinkChangeTheLength) {
  expectRuns(R"(import ::io;
    fn main() -> int {
      b = Buffer<float>();
      b.grow(2);
      b.set(4, 2.5);
      io::println(Str<int>(b.len()) + " " + Str<float>(b.get(4)));
      b.shrink(4);
      io::println(Str<int>(b.len()));
      b.reserve(8);
      b.reserve(2);
      io::println(Str<int>(b.len()) + " " + Str<float>(b.get(4)));
      return 0;
    }
  )",
             "5 2.5\n1\n8 0\n");
}

TEST(Buffer, HoldsReferences) {
  expectRuns(R"(import ::io;
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
      io::println(s.get(0) + s.get(1) + s.get(2));
      s.shrink(2);
      ps = Buffer<P>();
      p = P(42);
      ps.set(1, p);
      ps.set(2, p);
      io::println(Str<int>(ps.get(1).x + ps.get(2).x));
      return 0;
    }
  )",
             "aBc\n84\n");
}

TEST(Buffer, OptionalSlotsStartAsNone) {
  expectRuns(R"(import ::io;
    fn show(o: view Str?) {
      match o {
        v: Str { io::println(v); }
        None { io::println("None"); }
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
  expectRuns(R"(import ::io;
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
      while i < 23 {
        if i % 3 == 0 { cs.set(i, 'a'); ns.set(i, i * 1000); }
        i = i + 1;
      }
      cs.set(1, None);
      s = "";
      i = 0;
      while i < 7 { s = s + c(cs.get(i)) + n(ns.get(i)) + " "; i = i + 1; }
      io::println(s);
      io::println(c(cs.copy(21)) + n(ns.copy(21)) + " " + c(cs.get(22)));
      return 0;
    }
  )",
             "a0 -- -- a3000 -- -- a6000 \na21000 -\n");
}

// A bool takes one byte; a copy outlives the value it was taken from.
TEST(Buffer, HoldsBoolsAndCopiesOut) {
  expectRuns(R"(import ::io;
    fn main() -> int {
      b = Buffer<bool>();
      b.grow(13);
      b.set(1, True);
      b.set(15, True);
      io::println(Str<bool>(b.get(0)) + " " + Str<bool>(b.get(1)) + " " +
              Str<bool>(b.copy(15)));
      s = Buffer<Str>();
      s.set(0, "x");
      t: Str = s.copy(0);
      s.set(0, "y");
      io::println(t + s.get(0));
      return 0;
    }
  )",
             "False True True\nxy\n");
}

// A tuple is held in place: its references (here a Str and a Node) are
// retained by the buffer, its `int?` is inline, and a copy outlives the value
// it was taken from.  A tuple of plain values is copied out of a `get`.
TEST(Buffer, HoldsTuples) {
  expectRuns(R"(import ::io;
    class Node {
      v: int;
      view fn toString() -> Str { return "N" + Str<int>(self.v); }
    }
    impl Node { fn __init__(v: int) { self.v = v; } }
    fn opt(o: view int?) -> Str {
      match o { v: int { return Str<int>(v); } None { return "-"; } }
    }
    fn main() -> int {
      b = Buffer<(Str, Node, int?)>();
      n = Node(1);
      b.set(0, ("a", n, 1));
      b.set(1, ("b" + "b", Node(2), None));
      b.set(2, ("c", n, 3));
      io::println(b.get(0));
      io::println(b.get(1));
      io::println(b.get(1).0 + " " + opt(b.get(1).2) + " " + opt(b.get(2).2));
      t = b.copy(2);
      b.set(2, ("d", Node(4), 4));
      io::println(t);
      io::println(b.get(2));
      b.grow(2);
      b.set(4, t);
      io::println(Str<int>(b.len()) + " " + b.get(4).0);
      b.shrink(3);
      n.v = 9;
      io::println(b.get(0));
      ps = Buffer<(int, float)>();
      ps.set(1, (2, 0.5));
      p = ps.get(1);
      io::println(p);
      io::println(ps.get(0));
      return 0;
    }
  )",
             "(a, N1, 1)\n(bb, N2, None)\nbb - 3\n(c, N1, 3)\n(d, N4, 4)\n5 c\n"
             "(a, N9, 1)\n(2, 0.5)\n(0, 0)\n");
}
