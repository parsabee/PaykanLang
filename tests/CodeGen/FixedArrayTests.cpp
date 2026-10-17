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
