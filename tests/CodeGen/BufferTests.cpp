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
      b: mutable = Buffer<int>();
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
      b: mutable = Buffer<float>();
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
    impl Cloneable for P {}
    fn main() -> int {
      s: mutable = Buffer<Str>();
      s.set(0, "a");
      s.set(1, "b");
      s.set(2, "c");
      s.set(1, "B");
      io::println(s.get(0) + s.get(1) + s.get(2));
      s.shrink(2);
      ps: mutable = Buffer<P>();
      p = P(42);
      ps.set(1, p.clone());
      ps.set(2, move p);
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
      b: mutable = Buffer<Str?>();
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
    fn num(o: view int?) -> Str {
      match o { v: int { return Str<int>(v); } None { return "-"; } }
    }
    fn main() -> int {
      cs: mutable = Buffer<char?>();
      ns: mutable = Buffer<int?>();
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
      while i < 7 { s = s + c(cs.get(i)) + num(ns.get(i)) + " "; i = i + 1; }
      io::println(s);
      io::println(c(cs.copy(21)) + num(ns.copy(21)) + " " + c(cs.get(22)));
      return 0;
    }
  )",
             "a0 -- -- a3000 -- -- a6000 \na21000 -\n");
}

// A bool takes one byte; a copy outlives the value it was taken from.
TEST(Buffer, HoldsBoolsAndCopiesOut) {
  expectRuns(R"(import ::io;
    fn main() -> int {
      b: mutable = Buffer<bool>();
      b.grow(13);
      b.set(1, True);
      b.set(15, True);
      io::println(Str<bool>(b.get(0)) + " " + Str<bool>(b.get(1)) + " " +
              Str<bool>(b.copy(15)));
      s: mutable = Buffer<Str>();
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
// owned by the buffer (a tuple's elements hold no `shared` reference), its
// `int?` is inline, and a copy outlives the value it was taken from.  A tuple
// of plain values is copied out of a `get`.
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
      b: mutable = Buffer<(Str, Node, int?)>();
      b.set(0, ("a", Node(1), 1));
      b.set(1, ("b" + "b", Node(2), None));
      b.set(2, ("c", Node(1), 3));
      io::println(b.get(0));
      io::println(b.get(1));
      io::println(b.get(1).0 + " " + opt(b.get(1).2) + " " + opt(b.get(2).2));
      t = b.copy(2);
      b.set(2, ("d", Node(4), 4));
      io::println(t);
      io::println(b.get(2));
      b.grow(2);
      b.set(4, move t);
      io::println(Str<int>(b.len()) + " " + b.get(4).0);
      b.shrink(3);
      io::println(b.get(0));
      ps: mutable = Buffer<(int, float)>();
      ps.set(1, (2, 0.5));
      p = ps.get(1);
      io::println(p);
      io::println(ps.get(0));
      return 0;
    }
  )",
             "(a, N1, 1)\n(bb, N2, None)\nbb - 3\n(c, N1, 3)\n(d, N4, 4)\n5 c\n"
             "(a, N1, 1)\n(2, 0.5)\n(0, 0)\n");
}

// The standard library's memory::Buffer<T>, whose reads, stores, moves and
// exchanges the lowering emits in place (#364): values of every shape (an
// object, an optional one, a Str, a struct, a tuple, a tagged union and a
// function value held in memory, a number, a bool) go in by a move, are
// read where they are, and come out by a move, across a growth from nothing
// and a pop to empty, where the allocation changes and the runtime's
// functions take over.  The output is the previous compiler's.
TEST(Buffer, ValuesOfEveryShapeMoveInAndOut) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
import ::memory as mem;
class Node { id: int; }
struct Pair { name: Str; node: Node; }
enum Shape { Named(s: Str), Sized(n: int), Empty }
fn show(s: view Shape) -> Str {
  match s {
    n: Named { return "named " + n.s; }
    z: Sized { return "sized " + Str(z.n); }
    _ { return "empty"; }
  }
}
fn main() -> int {
  // objects: stores over a value, a pop to empty and a growth from nothing
  nodes = mem::Buffer<Node>();
  for i in 0..40 {
    nodes.push(Node(i));
  }
  nodes.set(3, Node(300));
  nodes.swap(0, 39);
  nodes.swap(7, 7);
  total = 0;
  while nodes.len() > 0 {
    let n = nodes.pop();
    total = total + n.id;
  }
  for i in 0..3 {
    nodes.push(Node(i + 1));
  }
  io::println(Str(total) + " " + Str(nodes.len()) + " " + Str(nodes.at(2).id));
  // optional objects: None is a value, and a slot grown is None
  opts = mem::Buffer<Node?>();
  opts.grow(2);
  opts.set(1, Node(5));
  opts.push(None);
  opts.swap(0, 1);
  let o = opts.pop();
  match opts.at(0) {
    n: Node { io::println("some " + Str(n.id) + " " + Str(o == None) + " " + Str(opts.at(1) == None)); }
    _ { io::println("none"); }
  }
  // strings: a literal, a string built, a clone read out
  strs = mem::Buffer<Str>();
  strs.push("lit");
  strs.push("built " + Str(1));
  strs.set(0, "set");
  strs.swap(0, 1);
  let got = strs.get(1);
  let top = strs.pop();
  io::println(strs.at(0) + "|" + got + "|" + top + "|" + Str(strs.len()));
  // values held in memory: a struct, a tuple, a tagged union, a function value
  pairs = mem::Buffer<Pair>();
  pairs.push(Pair("p" + Str(1), Node(7)));
  pairs.push(Pair("q", Node(8)));
  pairs.set(0, Pair("r", Node(9)));
  pairs.swap(0, 1);
  let last = pairs.pop();
  io::println(last.name + Str(last.node.id) + " " + pairs.at(0).name + Str(pairs.at(0).node.id));
  tuples = mem::Buffer<(Str, int)>();
  tuples.push(("one", 1));
  tuples.push(("two", 2));
  tuples.swap(0, 1);
  let t = tuples.pop();
  io::println(t.0 + Str(t.1) + " " + tuples.at(0).0);
  shapes = mem::Buffer<Shape>();
  shapes.push(Shape::Named("a name"));
  shapes.push(Shape::Sized(4));
  shapes.push(Shape::Empty);
  shapes.set(1, Shape::Named("renamed"));
  shapes.swap(0, 2);
  let s = shapes.pop();
  io::println(show(s) + ", " + show(shapes.at(0)) + ", " + show(shapes.at(1)));
  base = 10;
  fns = mem::Buffer<(int) -> int>();
  fns.push(fn(x: int) => x + base);
  fns.push(fn(x: int) => x * 2);
  fns.swap(0, 1);
  let f = fns.pop();
  io::println(Str(f(1)) + " " + Str(fns.at(0)(4)));
  // numbers and flags
  ints = mem::Buffer<int>();
  for i in 0..20 {
    ints.push(i * i);
  }
  ints.set(2, -4);
  ints.swap(2, 19);
  flags = mem::Buffer<bool>();
  flags.push(True);
  flags.push(False);
  flags.swap(0, 1);
  io::println(Str(ints.pop()) + " " + Str(ints.at(2)) + " " + Str(ints.len()) + " " + Str(flags.pop()) + " " + Str(flags.at(0)));
  // a clone holds clones
  copy = strs.clone();
  io::println(copy.at(0) + " " + Str(copy.len()));
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1077 3 3\n"
                      "some 5 True True\n"
                      "built 1|set|set|1\n"
                      "r9 q8\n"
                      "one1 two\n"
                      "named a name, empty, named renamed\n"
                      "11 8\n"
                      "-4 361 19 True False\n"
                      "built 1 1\n");
  g.expectNoLeaks("ValuesOfEveryShapeMoveInAndOut");
}

// A value a store overwrites, or a shrink drops, has left the buffer when
// it is let go: its `__deinit__`, reading the buffer through another
// reference, finds the new value in the slot, and a buffer already shorter.
// (A store used to drop the old value where it was: the `__deinit__` read
// the object it was running for.)
TEST(Buffer, AValueDroppedHasLeftItsBuffer) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
import ::memory as mem;
class Holder { items: mem::Buffer<Tracked>; }
class Tracked {
  id: int;
  home: weak mutable Holder?;
}
impl Tracked {
  fn __deinit__() {
    match self.home {
      h: Holder { io::println("drop " + Str(self.id) + ": slot 0 holds " + Str(h.items.at(0).id) + " of " + Str(h.items.len())); }
      _ { io::println("drop " + Str(self.id)); }
    }
  }
}
fn main() -> int {
  let h: shared mutable Holder = Holder(mem::Buffer<Tracked>());
  h.items.push(Tracked(1, h));
  h.items.set(0, Tracked(2, h));
  h.items.push(Tracked(3, h));
  h.items.swap(0, 1);
  h.items.shrink(1);
  io::println("end " + Str(h.items.len()));
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "drop 1: slot 0 holds 2 of 1\n"
                      "drop 2: slot 0 holds 3 of 1\n"
                      "end 1\n"
                      "drop 3\n");
  g.expectNoLeaks("AValueDroppedHasLeftItsBuffer");
}

// A native function bound to one of the runtime's Buffer functions with a
// result ::memory does not declare (a `bool` for `isSet`, none for `get`
// and `tryGrow`, an `int` for `set`) calls the runtime, as before: what is
// emitted in place is ::memory's functions, by their signatures.  (The
// first version emitted its own result for each: PIR that failed to
// verify, or a crash of the compiler.)
TEST(Buffer, AFunctionBoundAnotherWayIsTheRuntimes) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
native fn bNew<T>(n: int) -> Obj = "PaykanBuffer_new";
native fn bLen<T>(b: view Obj) -> int = "PaykanBuffer_len";
native fn bIsSet<T>(b: view Obj, i: int) -> bool = "PaykanBuffer_isSet";
native fn bCheck<T>(b: view Obj, i: int) = "PaykanBuffer_get";
native fn bGrow<T>(b: inout Obj, by: int) = "PaykanBuffer_tryGrow";
native fn bPut<T>(b: inout Obj, i: int, v: view T) -> int = "PaykanBuffer_set";
fn main() -> int {
  // results ::memory does not declare: a bool, none, an int
  s: mutable Obj = bNew<Str>(2);
  if bIsSet<Str>(s, 0) { io::println("set"); } else { io::println("unset"); }
  n: mutable Obj = bNew<int>(2);
  bCheck<int>(n, 1);
  bGrow<int>(n, 1);
  r = bPut<int>(n, 2, 7);
  io::println("checked " + Str(bLen<int>(n)) + " " + Str(r == r));
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "unset\n"
                      "checked 3 True\n");
  g.expectNoLeaks("AFunctionBoundAnotherWayIsTheRuntimes");
}

// So does a store of a struct with a `__deinit__`, which ::memory refuses
// a buffer of: the runtime's drop runs no `__deinit__`, and one emitted in
// place ran the first value's twice and the second's never.
TEST(Buffer, AStructWithADeinitIsTheRuntimesToStore) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
native fn bNew<T>(n: int) -> Obj = "PaykanBuffer_new";
native fn bSet<T>(b: inout Obj, i: int, v: view T) = "PaykanBuffer_set";
struct Guard { id: int; }
impl Guard {
  fn __deinit__() { io::println("guard " + Str(self.id)); }
}
fn main() -> int {
  g: mutable Obj = bNew<Guard>(1);
  one = Guard(1);
  bSet<Guard>(g, 0, one);
  io::println("stored " + Str(one.id));
  bSet<Guard>(g, 0, Guard(2));
  io::println("end");
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "stored 1\n"
                      "guard 2\n"
                      "end\n"
                      "guard 1\n");
  g.expectNoLeaks("AStructWithADeinitIsTheRuntimesToStore");
}

// A value that a store overwrites, or a shrink drops, has left the buffer
// when it is let go, so its `__deinit__` may change the buffer: push to
// it, store in it, pop it, exchange two values, grow it past its
// allocation, empty it, or put another buffer in its place (the shrink
// holds the one it is shrinking).  Each leaves a buffer of whole values and
// leaks nothing.  (The runtime dropped a value where it was: a push from a
// `__deinit__` wrote over values a shrink had still to drop, and a pop
// under a store read the storage freed.)
TEST(Buffer, ADeinitMayChangeItsBuffer) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
import ::memory as mem;
class Holder { items: mem::Buffer<Tracked>; }
class Tracked {
  id: int;
  act: int;
  home: weak mutable Holder?;
}
impl Tracked {
  fn __deinit__() {
    match self.home {
      h: Holder {
        io::println("drop " + Str(self.id) + " act " + Str(self.act) + " len " + Str(h.items.len()));
        if self.act == 1 { e = h.items.push(Tracked(self.id * 10, 0, None)); }
        if self.act == 2 && h.items.len() > 0 { e = h.items.set(0, Tracked(self.id * 10, 0, None)); }
        if self.act == 3 && h.items.len() > 1 { let p = h.items.pop(); io::println("popped " + Str(p.id)); }
        if self.act == 4 && h.items.len() > 1 { e = h.items.swap(0, h.items.len() - 1); }
        if self.act == 5 { for k in 0..20 { e = h.items.push(Tracked(self.id * 100 + k, 0, None)); } }
        if self.act == 6 { e = h.items.shrink(h.items.len()); }
        if self.act == 7 { h.items = mem::Buffer<Tracked>(); }
      }
      _ {}
    }
  }
}
fn show(what: Str, h: shared mutable Holder) {
  s = "";
  i: unsigned = 0;
  while i < h.items.len() {
    s = s + Str(h.items.at(i).id) + " ";
    i = i + 1;
  }
  io::println(what + ": [" + s + "]");
}
fn main() -> int {
  let h: shared mutable Holder = Holder(mem::Buffer<Tracked>());
  for act in 1..8 {
    // a store over a value whose __deinit__ changes the buffer
    e = h.items.shrink(h.items.len());
    e = h.items.push(Tracked(1, 0, None));
    e = h.items.push(Tracked(2, act, h));
    e = h.items.push(Tracked(3, 0, None));
    e = h.items.set(1, Tracked(4, 0, None));
    show("set", h);
    // a shrink that drops it, with a value after it still to drop
    e = h.items.shrink(h.items.len());
    e = h.items.push(Tracked(5, 0, None));
    e = h.items.push(Tracked(6, act, h));
    e = h.items.push(Tracked(7, 0, None));
    e = h.items.shrink(2);
    show("shrink", h);
  }
  e = h.items.shrink(h.items.len());
  io::println("end");
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "drop 2 act 1 len 3\n"
                      "set: [1 4 3 20 ]\n"
                      "drop 6 act 1 len 1\n"
                      "shrink: [5 60 ]\n"
                      "drop 2 act 2 len 3\n"
                      "set: [20 4 3 ]\n"
                      "drop 6 act 2 len 1\n"
                      "shrink: [60 ]\n"
                      "drop 2 act 3 len 3\n"
                      "popped 3\n"
                      "set: [1 4 ]\n"
                      "drop 6 act 3 len 1\n"
                      "shrink: [5 ]\n"
                      "drop 2 act 4 len 3\n"
                      "set: [3 4 1 ]\n"
                      "drop 6 act 4 len 1\n"
                      "shrink: [5 ]\n"
                      "drop 2 act 5 len 3\n"
                      "set: [1 4 3 200 201 202 203 204 205 206 207 208 209 210 "
                      "211 212 213 214 215 216 217 218 219 ]\n"
                      "drop 6 act 5 len 1\n"
                      "shrink: [5 600 601 602 603 604 605 606 607 608 609 610 "
                      "611 612 613 614 615 616 617 618 619 ]\n"
                      "drop 2 act 6 len 3\n"
                      "set: []\n"
                      "drop 6 act 6 len 1\n"
                      "shrink: []\n"
                      "drop 2 act 7 len 3\n"
                      "set: []\n"
                      "drop 6 act 7 len 1\n"
                      "shrink: []\n"
                      "end\n");
  g.expectNoLeaks("ADeinitMayChangeItsBuffer");
}
