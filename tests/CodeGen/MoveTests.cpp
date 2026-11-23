// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Codegen tests: a value swapped with another, or moved out of its place,
// by `::memory`'s `swap`, `replace` and `take` and the containers' move out
// of a node.  The lowering emits both in place (#365,
// ModuleLowering::emitMoveNative), where the runtime's Paykan_mem_swap and
// Paykan_mem_move_out copied byte by byte to the size a type descriptor
// gave.  Every run is checked for zero live heap blocks.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// `mem::swap`, `mem::replace`, `mem::take` and a container's move out of a
// node, over every shape of value: an object and an optional one (the
// Towers benchmark's disks, which leave a pile and join another by moves),
// a Str, a number, a struct, a tuple, a tagged union and a function value
// held in memory.  The output is the previous compiler's, which called the
// runtime for each.
TEST(Moves, ValuesOfEveryShapeTradePlacesAndLeaveThem) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
import ::memory as mem;
import ::collections as cl;
class Disk {
  size: int;
  next: Disk?;
}
struct Pair { name: Str; disk: Disk; }
enum Shape { Named(s: Str), Sized(n: int), Empty }
class Towers {
  piles: Disk?[3];
  moves: int;
}
impl Towers {
  fn pushDisk(disk: mutable Disk, pile: int) {
    let top = mem::take(self.piles[pile]);
    disk.next = move top;
    self.piles[pile] = move disk;
  }
  fn popDiskFrom(pile: int) -> Disk {
    let top = mem::take(self.piles[pile]);
    match move top {
      d: Disk {
        out: mutable = move d;
        self.piles[pile] = mem::take(out.next);
        return move out;
      }
      _ { return Disk(-1, None); }
    }
  }
  fn moveDisks(disks: int, from: int, to: int) {
    if disks == 1 {
      self.pushDisk(self.popDiskFrom(from), to);
      self.moves = self.moves + 1;
    } else {
      let other = 3 - from - to;
      self.moveDisks(disks - 1, from, other);
      self.pushDisk(self.popDiskFrom(from), to);
      self.moves = self.moves + 1;
      self.moveDisks(disks - 1, other, to);
    }
  }
  view fn height(pile: int) -> int {
    n = 0;
    match self.piles[pile] {
      d: Disk { n = d.size; }
      _ {}
    }
    return n;
  }
}
fn show(s: view Shape) -> Str {
  match s {
    n: Named { return "named " + n.s; }
    z: Sized { return "sized " + Str(z.n); }
    _ { return "empty"; }
  }
}
fn main() -> int {
  // the Towers shape: a disk leaves a pile and joins another by moves
  t: mutable = Towers([None, None, None], 0);
  i = 7;
  while i > 0 {
    t.pushDisk(Disk(i, None), 0);
    i = i - 1;
  }
  t.moveDisks(7, 0, 1);
  io::println(Str(t.moves) + " " + Str(t.height(0)) + " " + Str(t.height(1)));
  // objects and strings
  a: mutable Str = "a" + Str(1);
  b: mutable Str = "b";
  mem::swap(a, b);
  let was = mem::replace(a, "c");
  o: mutable Disk? = Disk(5, None);
  let got = mem::take(o);
  match got {
    d: Disk { io::println(a + b + was + " " + Str(d.size) + " " + Str(o == None)); }
    _ {}
  }
  // numbers, and values held in memory
  x = 1;
  y = 2;
  mem::swap(x, y);
  f = 1.5;
  let g = mem::replace(f, 2.5);
  p = Pair("p", Disk(1, None));
  q = Pair("q", Disk(2, None));
  mem::swap(p, q);
  let r = mem::replace(p, Pair("r", Disk(3, None)));
  u = ("one", 1);
  v = ("two", 2);
  mem::swap(u, v);
  s1 = Shape::Named("a name");
  s2 = Shape::Sized(4);
  mem::swap(s1, s2);
  fa = fn(n: int) => n + x;
  fb = fn(n: int) => n * 10;
  mem::swap(fa, fb);
  io::println(Str(x) + Str(y) + " " + Str(g) + Str(f) + " " + p.name + Str(p.disk.size) + q.name + Str(q.disk.size) + r.name + Str(r.disk.size) + " " + u.0 + Str(v.1) + " " + show(s1) + ", " + show(s2) + " " + Str(fa(3)) + " " + Str(fb(3)));
  // a container's value moved out of its node
  l: mutable = cl::LinkedList<Pair>();
  l.pushBack(Pair("first", Disk(8, None)));
  l.pushBack(Pair("second", Disk(9, None)));
  let front = l.popFront();
  let back = l.popBack();
  io::println(front.name + Str(front.disk.size) + " " + back.name + Str(back.disk.size) + " " + Str(l.len()));
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "127 0 1\n"
                      "ca1b 5 True\n"
                      "21 1.52.5 r3p1q2 two1 sized 4, named a name 30 5\n"
                      "first8 second9 0\n");
  g.expectNoLeaks("ValuesOfEveryShapeTradePlacesAndLeaveThem");
}

// A native function bound to the runtime's swap with a result ::memory
// does not declare calls the runtime, as before (what its `int` holds is
// not defined; the values are swapped).
TEST(Moves, ASwapBoundAnotherWayIsTheRuntimes) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
native fn sw<T>(a: inout T, b: inout T) -> int = "Paykan_mem_swap";
fn main() -> int {
  x = 1;
  y = 2;
  let r = sw<int>(x, y);
  a: mutable Str = "a" + Str(1);
  b: mutable Str = "b";
  let q = sw<Str>(a, b);
  io::println(Str(x) + Str(y) + a + b + Str(r == r) + Str(q == q));
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "21ba1TrueTrue\n");
  g.expectNoLeaks("ASwapBoundAnotherWayIsTheRuntimes");
}
