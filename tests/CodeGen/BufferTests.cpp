// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Codegen / E2E tests for the PaykanBuffer_* runtime functions, called the
// way the standard library's memory::Buffer<T> calls them: an ordinary
// generic class whose methods are generic native functions.  Every run is
// checked for zero live heap blocks.

#include "CodeGenTestUtils.h"
#include <filesystem>
#include <fstream>
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

// -- A program's own types with the standard library's names and shapes

// The lowering reads the numbers of a `mem::Buffer<T>` in place: the
// runtime's buffer object out of `__data`, its length and its storage
// loaded, with no call of `at`, `get` or `len`.  That is the standard
// library's struct, which Sema tells it (SemaContext::BufferTypes).  A
// struct of a program's own with the name, the field and the methods holds
// whatever object the program gives it, and its methods are called.  (The
// lowering took one for a buffer by its name and shape, and read a `Thing`
// as the runtime's buffer object: an index out of bounds of a length that
// was the object's bytes, or a crash.)
TEST(Buffer, AStructOfTheNameAndShapeIsTheProgramsOwn) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
class Thing { n: int; }
struct Buffer<T> { __data: Obj; }
impl Buffer<T> {
  view fn at(i: unsigned) -> int { return 7; }
  view fn get(i: unsigned) -> int { return 8; }
  view fn len() -> unsigned { return 3; }
}
fn total<T>(b: view Buffer<T>) -> int { return b.at(1) + b.get(2); }
fn main() -> int {
  let a = Buffer<int>(Thing(1));
  io::println(a.at(0));
  io::println(a.get(9));
  io::println(a.len());
  io::println(total(a) + total(Buffer<float>(Thing(2))));
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "7\n8\n3\n30\n");
  g.expectNoLeaks("AStructOfTheNameAndShapeIsTheProgramsOwn");
}

// So is one a module of the program declares, spelled `mem::Buffer<int>`
// where it is imported as the standard library's is: `memory` is not
// `::memory`.
TEST(Buffer, AStructOfAModuleOfTheProgramIsTheProgramsOwn) {
  auto dir = paykan::test::tempDir() / "pkn_own_buffer_module";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "memory.pkn") << R"pkn(
class Thing { n: int; }
struct Buffer<T> { __data: Obj; }
impl Buffer<T> {
  view fn at(i: unsigned) -> int { return 7; }
  view fn get(i: unsigned) -> int { return 8; }
  view fn len() -> unsigned { return 3; }
}
fn ints() -> Buffer<int> { return Buffer<int>(Thing(1)); }
fn sum(b: view Buffer<int>) -> int { return b.at(0) + b.get(1); }
)pkn";
  const std::string mainPath = (dir / "main.pkn").string();
  std::ofstream(mainPath) << R"pkn(import ::io;
import memory as mem;
fn main() -> int {
  let a = mem::ints();
  io::println(a.at(0));
  io::println(a.get(9));
  io::println(a.len());
  let b = mem::Buffer<float>(mem::Thing(2));
  io::println(b.at(0) + b.get(0) + mem::sum(a));
  return 0;
}
)pkn";
  LeakGuard g;
  auto r = compileAndRunFile(mainPath);
  std::filesystem::remove_all(dir);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "7\n8\n3\n30\n");
  g.expectNoLeaks("AStructOfAModuleOfTheProgramIsTheProgramsOwn");
}

// A read through `bx.value()` or `bx.get()` of a `mem::Box<mem::Buffer<T>>`
// takes the box's field `__data` itself, with no call: the standard
// library's box (SemaContext::BoxTypes), whose methods are that field.  A
// class of a program's own with a `__data` and methods of those names does
// what its methods do, whether it is named `Box` and has that one field or
// not.  (The lowering read `__data` for any class with such a field: the
// first buffer of `Holder`, where its methods give the second, and
// `Box<float>`'s without the call that shortens it.)
TEST(Buffer, AClassWithABoxsFieldAndMethodsIsTheProgramsOwn) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
import ::memory as mem;
class Holder {
  __data: mem::Buffer<int>;
  other: mem::Buffer<int>;
}
impl Holder {
  view fn value() -> view mem::Buffer<int> { return self.other; }
  fn get() -> inout mem::Buffer<int> { return self.other; }
}
class Box<T> { __data: mem::Buffer<T>; }
impl Box<T> {
  fn get() -> inout mem::Buffer<T> {
    let e = self.__data.shrink(1);
    return self.__data;
  }
  view fn value() -> view mem::Buffer<T> { return self.__data; }
}
fn main() -> int {
  a = mem::Buffer<int>();
  let e1 = a.push(1);
  b = mem::Buffer<int>();
  let e2 = b.push(42);
  let e3 = b.push(43);
  let h: mutable = Holder(move a, move b);
  io::println(h.value().at(0));
  io::println(h.value().len());
  io::println(h.get().at(1));
  io::println(h.get().len());
  xs = mem::Buffer<float>();
  let e4 = xs.push(1.5);
  let e5 = xs.push(2.5);
  let e6 = xs.push(3.5);
  let own: mutable = Box<float>(move xs);
  io::println(own.get().len());
  io::println(own.get().at(0));
  io::println(own.value().len());
  // The standard library's, beside them: read in place, as before.
  c = mem::Buffer<int>();
  let e7 = c.push(5);
  let bx: mutable = mem::Box(move c);
  io::println(bx.value().at(0));
  io::println(bx.get().len());
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "42\n2\n43\n2\n2\n1.5\n1\n5\n1\n");
  g.expectNoLeaks("AClassWithABoxsFieldAndMethodsIsTheProgramsOwn");
}

// -- An index that runs code

// A number read in place takes the buffer from the receiver's storage, its
// length and its element; an index that may run code runs before any of
// them is loaded, and a receiver it may write is copied first, as the
// receiver of the call `at` would be: the read is of the buffer the
// receiver had, whatever the index puts in its place through another name
// for the object (a field, a `view` parameter, a method's `self`, a
// closure's capture, a box in a field, a buffer read in the index).  (The
// lowering loaded the buffer's object and length before the index, and
// read the element from the object the index had freed.)
TEST(Buffer, AnIndexThatReplacesTheBufferReadsTheOneItHad) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
import ::memory as mem;
class Own { buf: mem::Buffer<int>; }
impl Own {
  fn reset(n: unsigned) -> unsigned {
    nb = mem::Buffer<int>();
    let e = nb.push(7);
    self.buf = move nb;
    return n;
  }
  view fn readVia(other: shared mutable Own) -> int { return self.buf.get(other.reset(1)); }
}
class Holder { bx: mem::Box<mem::Buffer<int>>; }
impl Holder {
  fn replace() -> unsigned {
    nb = mem::Buffer<int>();
    let e = nb.push(7);
    self.bx = mem::Box(move nb);
    return 2;
  }
}
fn filled(from: int) -> mem::Buffer<int> {
  b = mem::Buffer<int>();
  for i in 0..8 { let e = b.push(from + i); }
  return b;
}
fn read(b: view mem::Buffer<int>, p: shared mutable Own) -> int { return b.at(p.reset(3)); }
fn main() -> int {
  let o: shared mutable = Own(filled(100));
  let p = o;
  io::println(o.buf.at(p.reset(0)));
  o.buf = filled(100);
  io::println(read(o.buf, p));
  o.buf = filled(100);
  io::println(o.readVia(p));
  o.buf = filled(100);
  let f = fn() -> unsigned { return p.reset(4); };
  io::println(o.buf.at(f()));
  o.buf = filled(100);
  c = filled(0);
  io::println(o.buf.at(unsigned(c.at(p.reset(5)))));
  io::println(o.buf.at(0));
  let h: shared mutable = Holder(mem::Box(filled(100)));
  let q = h;
  io::println(h.bx.value().at(q.replace()));
  io::println(h.bx.value().at(0));
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "100\n103\n101\n104\n105\n7\n102\n7\n");
  g.expectNoLeaks("AnIndexThatReplacesTheBufferReadsTheOneItHad");
}

// The buffer's length and storage are the ones it has once the index has
// run: an index that grows it reads the slot it grew into, one that shrinks
// it is checked against the new length.  (The lowering checked the index
// against the length before: a slot past the end, or a panic for one in
// it.)
TEST(Buffer, AnIndexThatResizesTheBufferReadsItsNewLength) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
import ::memory as mem;
fn grown(bx: shared mutable mem::Box<mem::Buffer<int>>) -> unsigned {
  for i in 0..1000 { let e = bx.get().push(i); }
  return 500;
}
fn shrunk(bx: shared mutable mem::Box<mem::Buffer<int>>) -> unsigned {
  let e = bx.get().shrink(bx.get().len() - 3);
  return 2;
}
fn main() -> int {
  b = mem::Buffer<int>();
  let e = b.push(1);
  let bx: shared mutable = mem::Box(move b);
  io::println(bx.value().at(grown(bx)));
  io::println(bx.get().len());
  io::println(bx.get().get(shrunk(bx)));
  io::println(bx.value().len());
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "499\n1001\n1\n3\n");
  g.expectNoLeaks("AnIndexThatResizesTheBufferReadsItsNewLength");
}

// The box's buffer, read through `value()` or `get()`, is held across an
// index that may store: moving it out there is moving out a value still
// borrowed, as for the call `at` of a buffer of strings.  (The lowering
// read the element from the buffer the swap had freed.)
TEST(BufferDeathTest, AnIndexThatMovesTheBoxsBufferOutPanics) {
  const std::string reset = R"pkn(import ::io;
import ::memory as mem;
fn reset(bx: shared mutable mem::Box<mem::Buffer<int>>) -> unsigned {
  nb = mem::Buffer<int>();
  mem::swap(bx.get(), nb);
  return 0;
}
fn main() -> int {
  b = mem::Buffer<int>();
  for i in 0..8 { let e = b.push(i + 100); }
  let bx: shared mutable = mem::Box(move b);
)pkn";
  EXPECT_DEATH(compileAndRunUncaptured(
                   reset + "  io::println(bx.value().at(reset(bx)));\n"
                           "  return 0;\n}\n"),
               "a value moved out of 'bx.get\\(\\)' is still borrowed");
  EXPECT_DEATH(
      compileAndRunUncaptured(reset + "  io::println(bx.get().at(reset(bx)));\n"
                                      "  return 0;\n}\n"),
      "a value moved out of 'bx.get\\(\\)' is still borrowed");
}

// (The lowering checked the index against the length before the shrink,
// and read the slot it had dropped.)
TEST(BufferDeathTest, AnIndexPastTheEndOfTheShrunkBufferPanics) {
  EXPECT_DEATH(compileAndRunUncaptured(R"pkn(import ::io;
import ::memory as mem;
fn shrinkTo(bx: shared mutable mem::Box<mem::Buffer<int>>, keep: unsigned) -> unsigned {
  let e = bx.get().shrink(bx.get().len() - keep);
  return keep + 1;
}
fn main() -> int {
  b = mem::Buffer<int>();
  for i in 0..8 { let e = b.push(i + 100); }
  let bx: shared mutable = mem::Box(move b);
  io::println(bx.value().at(shrinkTo(bx, 1)));
  return 0;
}
)pkn"),
               "buffer index 2 out of bounds \\(len=1\\)");
}

// The iterations of a `parallel for` read one buffer of the enclosing
// function in place, through a `view` parameter too, with indexes that
// call code: no count of it is lost, and none frees it.  A function whose
// `view` parameter is such a buffer holds the one it is given, by an atomic
// count, across an index that replaces it.  (The read copied its receiver,
// a plain count from every thread: a leak, or a buffer freed under the
// read.)
TEST(Buffer, AParallelForsReadsOfItsBufferLoseNoCount) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
import ::memory as mem;
class Own { buf: mem::Buffer<int>; }
impl Own {
  fn reset() -> unsigned {
    nb = mem::Buffer<int>();
    let e = nb.push(7);
    self.buf = move nb;
    return 0;
  }
}
fn bump(x: inout int) -> unsigned { x += 1; return 0; }
fn readIt(b: view mem::Buffer<int>, y: inout int) -> int { return b.at(bump(y)); }
fn resetIt(b: view mem::Buffer<int>, p: shared mutable Own) -> int { return b.at(p.reset()); }
fn main() -> int {
  b = mem::Buffer<int>();
  let e = b.push(1);
  ys = [0; 4096];
  t = 0;
  for r in 0..20 {
    s = 0;
    parallel for i in 0..4096 { s += b.at(bump(ys[i])) + readIt(b, ys[i]); }
    t += s;
  }
  io::println(t);
  let o: shared mutable = Own(mem::Buffer<int>());
  let e2 = o.buf.push(3);
  io::println(resetIt(o.buf, o));
  io::println(o.buf.at(0));
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "163840\n3\n7\n");
  g.expectNoLeaks("AParallelForsReadsOfItsBufferLoseNoCount");
}

// A call in the index that changes nothing the receiver reaches takes no
// copy of it, which would count the buffer's object: in a `parallel for`
// body, whose iterations read a buffer of the enclosing function in place,
// and on a frozen one there (LoweringTests's
// ABufferReadWithAHarmlessCallInItsIndexCountsNothing).
TEST(Buffer, AnIndexWithACallThatChangesNothingReadsTheBuffer) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
import ::memory as mem;
fn pick(i: int) -> unsigned { return unsigned(i % 3); }
class Own { buf: mem::Buffer<float>; }
fn main() -> int {
  b = mem::Buffer<int>();
  for i in 0..8 { let e = b.push(i + 100); }
  s = 0;
  parallel for i in 0..8 { s += b.at(pick(i)) + b.get(pick(i + 1)); }
  io::println(s);
  let bx: atomic shared = mem::Box(move b);
  t = 0;
  parallel for i in 0..8 { t += bx.value().at(pick(i)); }
  io::println(t);
  xs = mem::Buffer<float>();
  for i in 0..4 { let e = xs.push(float(i) + 0.5); }
  let o: shared mutable = Own(move xs);
  io::println(o.buf.at(pick(5)) + o.buf.get(pick(4)));
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1616\n807\n4\n");
  g.expectNoLeaks("AnIndexWithACallThatChangesNothingReadsTheBuffer");
}
