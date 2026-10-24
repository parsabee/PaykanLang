// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Fixed-size arrays, `T[n]` (docs/language/09-arrays.md): values held in `mem`
// slots, copied whole, read and written in place.  Every test runs on the
// backend the suite is built for (llvm and c) and checks zero live heap blocks.
// The index panic is a death test in DriverTests.cpp.

#include "CodeGenTestUtils.h"

#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

std::string wrapMain(const std::string &body) {
  return "import ::io; enum Color { Red, Green, Blue }\nfn main() -> int {\n" +
         body + "\n  return 0;\n}\n";
}

void expectOutput(const std::string &body, const std::string &expected) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(body));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, expected);
  g.expectNoLeaks(body.c_str());
}

} // namespace

TEST(FixedArray, ElementsOfEveryScalarType) {
  expectOutput(R"(
    xs: int[3] = [1, 2, 3];
    fs: float[2] = [1, 2.5];
    bs: bool[2] = [True, False];
    cs: char[3] = ['a', 'b', 'c'];
    es: Color[2] = [Color::Blue, Color::Red];
    xs[1] = xs[0] + xs[2];
    fs[0] = fs[0] / 4.0;
    bs[1] = !bs[1];
    cs[0] = 'z';
    io::println(Str(xs[0]) + " " + Str(xs[1]) + " " + Str(xs[2]));
    io::println(Str(fs[0]) + " " + Str(fs[1]));
    io::println(Str(bs[0]) + " " + Str(bs[1]) + " " + Str(cs[0]) + Str(cs[2]));
    match es[0] {
      Blue { io::println("blue"); }
      _ { io::println("not blue"); }
    }
  )",
               "1 4 3\n0.25 2.5\nTrue True zc\nblue\n");
}

// A copy is independent of its source: initialising, assigning and
// declaring by assignment all copy the bytes.
TEST(FixedArray, CopiesAreIndependent) {
  expectOutput(R"(
    a: int[3] = [1, 2, 3];
    b = a;
    b[0] = 10;
    c: int[3] = b;
    c[1] = 20;
    a = c;
    a[2] = 30;
    io::println(Str(a[0]) + " " + Str(a[1]) + " " + Str(a[2]));
    io::println(Str(b[0]) + " " + Str(b[1]) + " " + Str(b[2]));
    io::println(Str(c[0]) + " " + Str(c[1]) + " " + Str(c[2]));
  )",
               "10 20 30\n10 2 3\n10 20 3\n");
}

// A literal assigned to an array is built first, so it can read the array it
// replaces.
TEST(FixedArray, AssignedLiteralReadsTheOldValue) {
  expectOutput(R"(
    xs: int[3] = [1, 2, 3];
    xs = [xs[2], xs[0], xs[1]];
    io::println(Str(xs[0]) + " " + Str(xs[1]) + " " + Str(xs[2]));
  )",
               "3 1 2\n");
}

// `int[2][3]` is two `int[3]`s: grid[i][j] with i < 2 and j < 3.  A row is a
// value too.
TEST(FixedArray, ArraysOfArrays) {
  expectOutput(R"(
    grid: int[2][3] = [[1, 2, 3], [4, 5, 6]];
    row = grid[1];
    row[0] = 40;
    grid[0] = row;
    grid[1][2] = 60;
    io::println(Str(grid.len()) + " " + Str(grid[0].len()));
    i = 0;
    while i < grid.len() {
      j = 0;
      line = "";
      while j < grid[i].len() {
        line = line + Str(grid[i][j]) + " ";
        j = j + 1;
      }
      io::println(line);
      i = i + 1;
    }
    io::println(Str(row[0]) + " " + Str(row[2]));
  )",
               "2 3\n40 5 6 \n4 5 60 \n40 6\n");
}

// An `inout` local names the array itself; a `view` local holds a copy that
// cannot change while it is in use.
TEST(FixedArray, LocalBorrows) {
  expectOutput(R"(
    xs: int[3] = [1, 2, 3];
    r: inout = xs;
    r[0] = 100;
    r = [7, 8, 9];
    v: view = xs;
    io::println(Str(v[0]) + " " + Str(v[1]) + " " + Str(v.len()));
    xs[2] = 0;
    io::println(Str(xs[0]) + " " + Str(xs[2]));
  )",
               "7 8 3\n7 0\n");
}

// A by-value parameter is the callee's own copy, a `view` one reads the
// caller's array and an `inout` one changes it; a result is a value like any
// other, also from a method and as an element or a receiver.
TEST(FixedArray, ParametersAndResults) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn total(xs: view int[3]) -> int { return xs[0] + xs[1] + xs[2]; }
    fn bumped(xs: int[3]) -> int[3] {
      xs[0] = xs[0] + 100;
      return xs;
    }
    fn swap(a: inout int[3], b: inout int[3]) {
      tmp = a;
      a = b;
      b = tmp;
    }
    fn unit() -> float[2][2] { return [[1, 0], [0, 1]]; }
    class Stats { n: int; }
    impl Stats {
      fn __init__(xs: view int[3]) { self.n = total(xs); }
      view fn doubled(xs: int[3]) -> int[3] {
        return [xs[0] * 2, xs[1] * 2, self.n];
      }
    }
    fn main() -> int {
      xs: int[3] = [1, 2, 3];
      ys = bumped(xs);
      io::println(Str(xs[0]) + " " + Str(ys[0]) + " " + Str(total(ys)) + " " +
              Str(total([4, 5, 6])));
      swap(xs, ys);
      io::println(Str(xs[0]) + " " + Str(ys[0]));
      io::println(Str(unit()[1][1]) + " " + Str(unit().len()));
      s = Stats(ys);
      d = s.doubled(xs);
      io::println(Str(d[0]) + " " + Str(d[2]) + " " + Str(s.doubled(xs)[1]));
      xs = bumped(bumped(xs));
      return xs[0] - 300;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 1); // 101 + 200 - 300
  EXPECT_EQ(r.StdOut, "1 101 106 15\n101 1\n1 2\n202 6 4\n");
  g.expectNoLeaks("ParametersAndResults");
}

TEST(FixedArray, IndexesComputedAtRunTime) {
  expectOutput(R"(
    sq: int[5] = [0; 5];
    i = 0;
    while i < sq.len() {
      sq[i] = i * i;
      i = i + 1;
    }
    total = 0;
    k = sq.len() - 1;
    while k >= 0 {
      total = total + sq[k];
      k = k - 1;
    }
    io::println(Str(total));
  )",
               "30\n");
}

// An element of a reference type is a reference: a copy of the array
// shares its elements' objects, and an assigned element or array lets go of
// the ones it replaces.
TEST(FixedArray, ElementsOfReferenceTypes) {
  expectOutput(R"(
    ss: Str[3] = ["a", "b", "c"];
    ss[1] = ss[0] + ss[2];
    ss[2] = ss[2];
    copy = ss;
    copy[0] = "z";
    ss = ss;
    io::println(ss[0] + ss[1] + ss[2] + " " + copy[0] + copy[1]);
    grid: Str[2][3] = [["a", "b", "c"], ["d", "e", "f"]];
    row = grid[1];
    grid[0] = row;
    grid = [grid[1], [row[2], row[1], row[0]]];
    grid[1][1] = "E";
    io::println(grid[0][0] + grid[1][1] + grid[1][2] + row[1]);
    os: Str?[2] = [None, "x"];
    match os[1] {
      s: Str { io::println("some " + s); }
      None { io::println("none"); }
    }
    ts: (int, Str)[2] = [(1, "one"), (2, "two")];
    io::println(ts[1].1);
    big: Str[6] = ["1", "2", "3", "4", "5", "6"];
    b2 = big;
    b2[5] = "x";
    r: inout = big;
    r[0] = "R";
    io::println(big[0] + big[5] + b2[0] + b2[5]);
  )",
               "aacc zac\ndEde\nsome x\ntwo\nR61x\n");
}

// A call's result or a literal owns its elements: one that initialises a
// place hands them over, one that is only read, passed or dropped lets go
// of them; so do the variables a loop iteration or a return leaves.
TEST(FixedArray, TemporariesAndScopesLetGoOfTheirElements) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Animal {
      name: Str;
      view fn speak() -> Str { return "it " + self.name; }
    }
    impl Animal {
      fn __init__(n: Str) { self.name = n; }
    }
    class Dog : Animal {
      view fn speak() -> Str { return "woof " + self.name; }
    }
    impl Dog {
      fn __init__(n: Str) { __super__(n); }
    }
    class Kennel { first: Str; }
    impl Kennel {
      fn __init__(dogs: view Animal[2]) { self.first = dogs[0].speak(); }
      fn pair(extra: Str[2]) -> Str[2] { return [self.first, extra[1]]; }
    }
    fn names() -> Str[2] { return ["ann", "bob"]; }
    fn shout(xs: Str[2]) -> Str[2] {
      xs[1] = xs[1] + "!";
      return xs;
    }
    fn rename(xs: inout Str[2]) { xs[0] = "q"; }
    fn find(xs: Str[5], s: Str) -> int {
      i = 0;
      while i < xs.len() {
        pair: Str[2] = [xs[i], s];
        if pair[0] == pair[1] {
          return i;
        }
        i = i + 1;
      }
      return -1;
    }
    fn main() -> int {
      i = 0;
      hits = 0;
      while names()[0] == "ann" && i < 3 {
        pair: Str[2] = [Str(i), "x"];
        i = i + 1;
        if i == 2 {
          continue;
        }
        if pair[0] == "9" {
          break;
        }
        hits = hits + pair[1].len();
      }
      io::println(Str(hits) + " " + Str(find(["a", "b", "c", "d", "e"], "c")) +
              " " + Str(find(["a", "b", "c", "d", "e"], "z")));
      names();
      ns = shout(names());
      io::println(ns[0] + ns[1] + " " + shout(ns)[1] + " " +
              Str(names().len()) + " " + Str(names()[1].len()));
      rename(ns);
      zoo: Animal[2] = [Dog("rex"), Animal("cat")];
      zoo[1] = Dog("fido");
      k = Kennel(zoo);
      p = k.pair(ns);
      io::println(zoo[1].speak() + ", " + p[0] + ", " + p[1] + ", " +
              k.pair(["x", "y"])[1]);
      match names()[1] {
        "bob" { return 7; }
        _ { return 0; }
      }
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 7);
  EXPECT_EQ(r.StdOut, "2 2 -1\nannbob! bob!! 2 3\n"
                      "woof fido, woof rex, bob!, y\n");
  g.expectNoLeaks("TemporariesAndScopesLetGoOfTheirElements");
}

// A field is held in its object, which releases its references when it
// dies; it is read and written in place, also through a call's result
// (`mk().cells[1][1]`), passed by address and named by an `inout` local.
TEST(FixedArray, Fields) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Grid { cells: int[2][3]; tags: Str[2]; }
    impl Grid {
      fn set(r: int, c: int, v: int) { self.cells[r][c] = v; }
      view fn at(r: int, c: int) -> int { return self.cells[r][c]; }
      fn rename(i: int, s: Str) { self.tags[i] = s; }
      view fn row(r: int) -> int[3] { return self.cells[r]; }
    }
    class Named { names: Str[6]; }
    impl Named {
      fn __init__() { self.names = ["a", "b", "c", "d", "e", "f"]; }
    }
    class More : Named { extra: Str[1]; }
    impl More {
      fn __init__() {
        __super__();
        self.extra = ["x"];
      }
    }
    class Box<T> { xs: T[2]; }
    fn mk() -> Grid { return Grid([[1, 2, 3], [4, 5, 6]], ["p", "q"]); }
    fn total(xs: view int[3]) -> int { return xs[0] + xs[1] + xs[2]; }
    fn clear(ts: inout Str[2]) { ts = ["", ts[0]]; }
    fn main() -> int {
      g = Grid([[1, 2, 3], [4, 5, 6]], ["p", "q"]);
      g.set(1, 2, 60);
      g.rename(0, "P" + g.tags[1]);
      io::println(Str(g.at(1, 2)) + " " + g.tags[0] + " " +
              Str(total(g.cells[0])) + " " + Str(g.cells.len()));
      g.cells[1] = [0, 0, 0];
      r = g.row(0);
      g.cells[0] = [7, 8, 9];
      copy = g.tags;
      g.tags = ["new", g.tags[0]];
      io::println(Str(r[0]) + " " + Str(g.cells[0][0]) + " " +
              Str(g.cells[1][2]) + " " + copy[0] + " " + g.tags[1]);
      io::println(mk().tags[1] + " " + Str(mk().cells[1][1]) + " " +
              Str(total(mk().cells[1])) + " " + Str(mk().tags.len()));
      t = mk().tags;
      mk().tags[0] = "lost";
      m = More();
      clear(g.tags);
      inner: inout = g.tags;
      inner[0] = "in";
      b = Box<Str>(["box", t[0]]);
      io::println(t[0] + m.names[5] + m.extra[0] + " " + g.tags[0] + g.tags[1] +
              " " + b.xs[0] + b.xs[1]);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "60 Pq 6 2\n1 7 0 Pq Pq\nq 5 15 2\npfx innew boxp\n");
  g.expectNoLeaks("Fields");
}

// A slice parameter reads and writes the caller's array in place, whatever
// its length: a variable, a literal, a row, a field, or a slice passed on.
TEST(FixedArray, SliceParameters) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Counter { n: int; tags: Str[2]; }
    impl Counter {
      fn __init__(xs: view int[]) {
        self.n = xs.len();
        self.tags = ["a", "b"];
      }
      view fn total(xs: view int[]) -> int { return self.n + sum(xs); }
    }
    fn sum(xs: view int[]) -> int {
      t = 0;
      i = 0;
      while i < xs.len() {
        t = t + xs[i];
        i = i + 1;
      }
      return t;
    }
    fn double(xs: inout int[]) {
      i = 0;
      while i < xs.len() {
        xs[i] = xs[i] * 2;
        i = i + 1;
      }
    }
    fn first(ss: view Str[]) -> int { return ss[0].len() + ss.len(); }
    fn rename(ss: inout Str[], s: Str) { ss[ss.len() - 1] = s; }
    fn count<T>(xs: view T[]) -> int { return xs.len(); }
    fn pass(xs: view int[]) -> int { return sum(xs) + count(xs); }
    fn main() -> int {
      a: int[3] = [1, 2, 3];
      b: int[5] = [1; 5];
      double(a);
      io::println(Str(sum(a)) + " " + Str(sum(b)) + " " + Str(sum([10, 20])) +
              " " + Str(pass(a)) + " " + Str(count(b)));
      ss: Str[2] = ["ab", "c"];
      rename(ss, "zzz");
      io::println(Str(first(ss)) + " " + ss[1] + " " + Str(first(["hello"])));
      c = Counter(b);
      rename(c.tags, "B" + c.tags[1]);
      grid: int[2][3] = [[1, 2, 3], [4, 5, 6]];
      io::println(Str(c.total(a)) + " " + Str(c.total([1])) + " " + c.tags[1] +
              " " + Str(sum(grid[1])));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "12 5 30 15 5\n4 zzz 6\n17 6 Bb 15\n");
  g.expectNoLeaks("SliceParameters");
}

// An element can be an optional value: held inline (a zeroed one is None),
// matched, compared with None and printed, also through a slice.
TEST(FixedArray, OptionalElements) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn total(xs: view int?[]) -> int {
      t = 0;
      i = 0;
      while i < xs.len() {
        match xs[i] {
          n: int { t = t + n; }
          None { t = t + 100; }
        }
        i = i + 1;
      }
      return t;
    }
    fn main() -> int {
      os: int?[3] = [1, None, 3];
      os[0] = None;
      os[2] = os[2];
      fs: float?[2] = [2, None];
      bs: bool?[2] = [None, True];
      cs: char?[1] = ['x'];
      copy = os;
      copy[1] = 7;
      io::println(Str(total(os)) + " " + Str(total(copy)) + " " +
              Str(total([None, 5])));
      io::println(fs[0]);
      io::println(fs[1]);
      io::println(bs[1]);
      io::println(cs[0]);
      if os[1] == None && copy[1] != None { io::println("eq ok"); }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "203 110 105\n2\nNone\nTrue\nx\neq ok\n");
  g.expectNoLeaks("OptionalElements");
}

// A `_` length is the initializer's: a literal's element count (its first
// element's lengths for a nested `_`) or the length of the array a call
// returns.
TEST(FixedArray, InferredLengths) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class A {
      n: int;
      view fn tag() -> Str { return "A" + Str(self.n); }
    }
    impl A {
      fn __init__(n: int) { self.n = n; }
    }
    class B : A {
      view fn tag() -> Str { return "B" + Str(self.n); }
    }
    impl B {
      fn __init__(n: int) { __super__(n); }
    }
    fn fill<T>(v: T) -> T[3] { return [v, v, v]; }
    fn rows() -> int[2][3] { return [[1, 2, 3], [4, 5, 6]]; }
    fn main() -> int {
      x: int[_][_] = [[1, 2, 3], [4, 5, 6]];
      y: A[_] = [B(1), A(2), B(3)];
      z: float[_] = fill<float>(1.5);
      w: int[2][_] = rows();
      let v: char[_] = ['h', 'i'];
      f: float[_] = [1, 2];
      copy: int[_][_] = [x[1], x[0], [7, 8, 9]];
      io::println(Str(x.len()) + " " + Str(x[1].len()) + " " + Str(x[1][2]) + " " +
              y[0].tag() + y[1].tag() + y[2].tag() + " " + Str(z.len()) +
              Str(z[2]) + " " + Str(w[1][0]) + " " + Str(v[1]) + " " +
              Str(f[0] / 4.0) + " " + Str(copy[0][0]) + Str(copy[2][2]));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "2 3 6 B1A2B3 31.5 4 i 0.25 49\n");
  g.expectNoLeaks("InferredLengths");
}

// `[v; n]` is n copies of v: a zero fills the whole array (all 16384 slots
// of the largest), any other value is stored in every slot, and an `int`
// into a `float[n]` is promoted.
TEST(FixedArray, RepeatLiteralsOfScalars) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn sum(xs: view int[]) -> int {
      t = 0;
      i = 0;
      while i < xs.len() {
        t = t + xs[i];
        i = i + 1;
      }
      return t;
    }
    fn main() -> int {
      zs: int[16384] = [0; 16384];
      zs[16383] = 5;
      io::println(Str(zs.len()) + " " + Str(zs[0]) + " " + Str(zs[8191]) + " " +
              Str(sum(zs)));
      x = 3;
      sevens = [x + 4; 6];
      sevens[0] = 0;
      fives = [5; 5];
      io::println(Str(sum(sevens)) + " " + Str(sum(fives)) + " " +
              Str(sum([2; 3])) + " " + Str(sevens.len()));
      fs: float[4] = [0; 4];
      fs[1] = 1.5;
      gs = [2.5; 3];
      bs = [True; 5];
      bs[4] = False;
      cs = ['x'; 2];
      zc = ['\0'; 3];
      zb = [False; 2];
      io::println(Str(fs[0] + fs[3]) + " " + Str(fs[1]) + " " + Str(gs[2]) + " " +
              Str(bs[0]) + Str(bs[4]) + " " + Str(cs[0]) + Str(cs[1]) + " " +
              Str(int(zc[2])) + " " + Str(zb[1]));
      grid = [[1; 3]; 2];
      grid[0][0] = 9;
      io::println(Str(grid[0][0]) + " " + Str(grid[1][0]) + " " +
              Str(sum(grid[1])));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "16384 0 0 5\n35 25 6 6\n0 1.5 2.5 TrueFalse xx 0 "
                      "False\n9 1 3\n");
  g.expectNoLeaks("RepeatLiteralsOfScalars");
}

// v is evaluated once: every slot holds the same reference (one `Node`
// three times, one string twice), a nested array is copied into every row,
// and a call runs once.
TEST(FixedArray, RepeatLiteralsOfReferences) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Node { v: int; }
    impl Node { fn __init__(v: int) { self.v = v; } }
    class Counter {
      n: int;
      fn next() -> int {
        self.n = self.n + 1;
        return self.n * 10;
      }
    }
    fn main() -> int {
      s = "ab";
      ss = [s; 3];
      ss[1] = "c";
      ts = [Str(7); 2];
      io::println(ss[0] + ss[1] + ss[2] + " " + ts[0] + ts[1]);
      nodes = [Node(1); 3];
      nodes[0].v = 42;
      io::println(Str(nodes[1].v) + " " + Str(nodes[2].v));
      grid: Str[3][3] = [[s + "!"; 3]; 3];
      grid[1][1] = "x";
      grid[2] = [grid[1][1]; 3];
      io::println(grid[0][1] + grid[1][1] + grid[1][0] + grid[2][2]);
      os: int?[4] = [None; 4];
      os[2] = 5;
      ps: float?[2] = [1; 2];
      io::println(os[0]);
      io::println(os[2]);
      io::println(ps[1]);
      ns: Node?[3] = [None; 3];
      ns[1] = nodes[2];
      match ns[1] {
        n: Node { io::println("node " + Str(n.v)); }
        None { io::println("none"); }
      }
      match ns[2] {
        n: Node { io::println("node " + Str(n.v)); }
        None { io::println("none"); }
      }
      c = Counter(0);
      tens = [c.next(); 4];
      io::println(Str(c.n) + " " + Str(tens[0] + tens[3]));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, "abcab 77\n42 42\nab!xab!x\nNone\n5\n1\nnode 42\nnone\n"
                      "1 20\n");
  g.expectNoLeaks("RepeatLiteralsOfReferences");
}
