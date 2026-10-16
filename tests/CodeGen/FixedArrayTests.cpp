// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Fixed-size arrays, `T[n]` (docs/language/05-arrays.md, "Fixed-size
// arrays"): values held in `mem` slots, copied whole, read and written in
// place.  Every test runs on the backend the suite is built for (llvm and c)
// and checks zero live heap blocks.  The index panic is a death test in
// DriverTests.cpp.

#include "CodeGenTestUtils.h"

#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

std::string wrapMain(const std::string &body) {
  return "enum Color { Red, Green, Blue }\nfn main() -> int {\n" + body +
         "\n  return 0;\n}\n";
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
    println(Str(xs[0]) + " " + Str(xs[1]) + " " + Str(xs[2]));
    println(Str(fs[0]) + " " + Str(fs[1]));
    println(Str(bs[0]) + " " + Str(bs[1]) + " " + Str(cs[0]) + Str(cs[2]));
    match es[0] {
      Blue { println("blue"); }
      _ { println("not blue"); }
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
    println(Str(a[0]) + " " + Str(a[1]) + " " + Str(a[2]));
    println(Str(b[0]) + " " + Str(b[1]) + " " + Str(b[2]));
    println(Str(c[0]) + " " + Str(c[1]) + " " + Str(c[2]));
  )",
               "10 20 30\n10 2 3\n10 20 3\n");
}

// A literal assigned to an array is built first, so it can read the array it
// replaces.
TEST(FixedArray, AssignedLiteralReadsTheOldValue) {
  expectOutput(R"(
    xs: int[3] = [1, 2, 3];
    xs = [xs[2], xs[0], xs[1]];
    println(Str(xs[0]) + " " + Str(xs[1]) + " " + Str(xs[2]));
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
    println(Str(grid.len()) + " " + Str(grid[0].len()));
    i = 0;
    while (i < grid.len()) {
      j = 0;
      line = "";
      while (j < grid[i].len()) {
        line = line + Str(grid[i][j]) + " ";
        j = j + 1;
      }
      println(line);
      i = i + 1;
    }
    println(Str(row[0]) + " " + Str(row[2]));
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
    println(Str(v[0]) + " " + Str(v[1]) + " " + Str(v.len()));
    xs[2] = 0;
    println(Str(xs[0]) + " " + Str(xs[2]));
  )",
               "7 8 3\n7 0\n");
}

// A by-value parameter is the callee's own copy, a `view` one reads the
// caller's array and an `inout` one changes it; a result is a value like any
// other, also from a method and as an element or a receiver.
TEST(FixedArray, ParametersAndResults) {
  LeakGuard g;
  auto r = compileAndRun(R"(
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
      println(Str(xs[0]) + " " + Str(ys[0]) + " " + Str(total(ys)) + " " +
              Str(total([4, 5, 6])));
      swap(xs, ys);
      println(Str(xs[0]) + " " + Str(ys[0]));
      println(Str(unit()[1][1]) + " " + Str(unit().len()));
      s = Stats(ys);
      d = s.doubled(xs);
      println(Str(d[0]) + " " + Str(d[2]) + " " + Str(s.doubled(xs)[1]));
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
    sq: int[5] = [0, 0, 0, 0, 0];
    i = 0;
    while (i < sq.len()) {
      sq[i] = i * i;
      i = i + 1;
    }
    total = 0;
    k = sq.len() - 1;
    while (k >= 0) {
      total = total + sq[k];
      k = k - 1;
    }
    println(Str(total));
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
    println(ss[0] + ss[1] + ss[2] + " " + copy[0] + copy[1]);
    grid: Str[2][3] = [["a", "b", "c"], ["d", "e", "f"]];
    row = grid[1];
    grid[0] = row;
    grid = [grid[1], [row[2], row[1], row[0]]];
    grid[1][1] = "E";
    println(grid[0][0] + grid[1][1] + grid[1][2] + row[1]);
    os: Str?[2] = [None, "x"];
    match os[1] {
      s: Str { println("some " + s); }
      None { println("none"); }
    }
    ts: (int, Str)[2] = [(1, "one"), (2, "two")];
    arrs: int[][2] = [[1, 2], [3]];
    arrs[1].push(4);
    println(ts[1].1 + " " + Str(arrs[1].len()) + " " + Str(arrs[0][1]));
    big: Str[6] = ["1", "2", "3", "4", "5", "6"];
    b2 = big;
    b2[5] = "x";
    r: inout = big;
    r[0] = "R";
    println(big[0] + big[5] + b2[0] + b2[5]);
  )",
               "aacc zac\ndEde\nsome x\ntwo 2 2\nR61x\n");
}

// A call's result or a literal owns its elements: one that initialises a
// place hands them over, one that is only read, passed or dropped lets go
// of them; so do the variables a loop iteration or a return leaves.
TEST(FixedArray, TemporariesAndScopesLetGoOfTheirElements) {
  LeakGuard g;
  auto r = compileAndRun(R"(
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
      view fn pair(extra: Str[2]) -> Str[2] { return [self.first, extra[1]]; }
    }
    fn names() -> Str[2] { return ["ann", "bob"]; }
    fn shout(xs: Str[2]) -> Str[2] {
      xs[1] = xs[1] + "!";
      return xs;
    }
    fn rename(xs: inout Str[2]) { xs[0] = "q"; }
    fn find(xs: view Str[5], s: Str) -> int {
      i = 0;
      while (i < xs.len()) {
        pair: Str[2] = [xs[i], s];
        if (pair[0] == pair[1]) {
          return i;
        }
        i = i + 1;
      }
      return -1;
    }
    fn main() -> int {
      i = 0;
      hits = 0;
      while (names()[0] == "ann" && i < 3) {
        pair: Str[2] = [Str(i), "x"];
        i = i + 1;
        if (i == 2) {
          continue;
        }
        if (pair[0] == "9") {
          break;
        }
        hits = hits + pair[1].len();
      }
      println(Str(hits) + " " + Str(find(["a", "b", "c", "d", "e"], "c")) +
              " " + Str(find(["a", "b", "c", "d", "e"], "z")));
      names();
      ns = shout(names());
      println(ns[0] + ns[1] + " " + shout(ns)[1] + " " +
              Str(names().len()) + " " + Str(names()[1].len()));
      rename(ns);
      zoo: Animal[2] = [Dog("rex"), Animal("cat")];
      zoo[1] = Dog("fido");
      k = Kennel(zoo);
      p = k.pair(ns);
      println(zoo[1].speak() + ", " + p[0] + ", " + p[1] + ", " +
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
