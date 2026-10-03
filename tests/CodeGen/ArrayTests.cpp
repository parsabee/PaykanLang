// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// CodeGen tests: array subscript read/write end-to-end.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// ============================================================================
// int[] subscript assignment
// ============================================================================

TEST(ArrayAssign, IntArray) {
  auto r = compileAndRun(wrapMain(R"(
    arr: int[] = [10, 20, 30];
    arr[0] = 99;
    arr[2] = 42;
    println(Str<int>(arr[0]));
    println(Str<int>(arr[1]));
    println(Str<int>(arr[2]));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "99\n20\n42\n");
}

TEST(ArrayAssign, IntArrayLoopOverwrite) {
  auto r = compileAndRun(wrapMain(R"(
    arr: int[] = [0, 0, 0, 0, 0];
    i: int = 0;
    while (i < 5) {
      arr[i] = i * i;
      i = i + 1;
    }
    i = 0;
    while (i < 5) {
      println(Str<int>(arr[i]));
      i = i + 1;
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n1\n4\n9\n16\n");
}

// ============================================================================
// float[] subscript assignment
// ============================================================================

TEST(ArrayAssign, FloatArray) {
  auto r = compileAndRun(wrapMain(R"(
    arr: float[] = [1.0, 2.0, 3.0];
    arr[1] = 9.5;
    println(Str<float>(arr[0]));
    println(Str<float>(arr[1]));
    println(Str<float>(arr[2]));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1\n9.5\n3\n");
}

// ============================================================================
// bool[] subscript assignment
// ============================================================================

TEST(ArrayAssign, BoolArray) {
  auto r = compileAndRun(wrapMain(R"(
    arr: bool[] = [True, False, True];
    arr[0] = False;
    arr[2] = False;
    println(Str<bool>(arr[0]));
    println(Str<bool>(arr[1]));
    println(Str<bool>(arr[2]));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "False\nFalse\nFalse\n");
}

// ============================================================================
// Str[] subscript assignment
// ============================================================================

TEST(ArrayAssign, StrArray) {
  auto r = compileAndRun(wrapMain(R"(
    arr: Str[] = ["a", "b", "c"];
    arr[1] = "hello";
    println(arr[0]);
    println(arr[1]);
    println(arr[2]);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "a\nhello\nc\n");
}

TEST(ArrayAssign, StrArrayMultipleWrites) {
  auto r = compileAndRun(wrapMain(R"(
    arr: Str[] = ["x", "y", "z"];
    arr[0] = "first";
    arr[0] = "second";
    println(arr[0]);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "second\n");
}

// ============================================================================
// String subscript read: s[i] -> Str (single char string)
// ============================================================================

TEST(ArraySubscript, StringIndexRead) {
  // s[i] on a Str yields a char — wrap with Str<char> to print.
  auto r = compileAndRun(wrapMain(R"(
    s: Str = "paykan";
    println(Str<char>(s[0]));
    println(Str<char>(s[5]));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "p\nn\n");
}

TEST(ArraySubscript, StringIndexBuildResult) {
  auto r = compileAndRun(wrapMain(R"(
    s: Str = "abcdef";
    result: Str = Str<char>(s[2]) + Str<char>(s[1]) + Str<char>(s[0]);
    println(result);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "cba\n");
}

// ============================================================================
// push/pop still work after subscript assign
// ============================================================================

TEST(ArrayAssign, PushAfterAssign) {
  auto r = compileAndRun(wrapMain(R"(
    arr: int[] = [1, 2, 3];
    arr[0] = 10;
    arr.push(99);
    println(Str<int>(arr[0]));
    println(Str<int>(arr.len()));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "10\n4\n");
}

// ============================================================================
// Reference-typed arrays reached through a member access / call (regression).
//
// Subscripting or passing an array that comes from a member access
// (self.field), a ref-returning call, or a ternary must unwrap the
// PaykanShared* box to the raw PaykanArray* first.  These used to read
// length/data from the box struct (len=0 traps, garbage elements) or
// over-release the object.
// ============================================================================

TEST(RefField, SubscriptObjectArrayField) {
  auto r = compileAndRun(R"(
class Foo { v: int; fn __init__(x: int){ self.v = x; } }
class Box {
  items: Foo[];
  fn __init__(){ self.items = []; self.items.push(Foo(41)); self.items.push(Foo(42)); }
  fn second() -> int { return self.items[1].v; }   // subscript a member-access array
}
fn main() -> int {
  b: Box = Box();
  println(Str<int>(b.items[0].v));   // subscript via local receiver
  println(Str<int>(b.second()));     // subscript via self.field inside a method
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "41\n42\n");
}

TEST(RefField, NestedArrayOfArraysField) {
  // An array-of-arrays field (T[][]) — the outer element is itself a
  // reference-typed array, so the object-element read must re-box/unwrap.
  auto r = compileAndRun(R"(
class Grid {
  rows: int[][];
  fn __init__() {
    self.rows = [];
    i: int = 0;
    while (i < 3) {
      row: int[] = [];
      j: int = 0;
      while (j < 3) { row.push(i * 3 + j); j = j + 1; }
      self.rows.push(row);
      i = i + 1;
    }
  }
  fn at(r: int, c: int) -> int {
    row: int[] = self.rows[r];   // subscript the outer 2-D field
    return row[c];
  }
}
fn main() -> int {
  g: Grid = Grid();
  println(Str<int>(g.at(0, 0)));   // 0
  println(Str<int>(g.at(1, 2)));   // 5
  println(Str<int>(g.at(2, 1)));   // 7
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n5\n7\n");
}

TEST(RefField, ThreeDimArrayLiteralsAndSubscripts) {
  // int[][][] spelled at several use sites (annotations, a parameter, a
  // return type, nested literals) must all agree on one canonical type so the
  // nested element reads/writes lower consistently at every depth.
  auto r = compileAndRun(R"(
fn corner(c: int[][][]) -> int { return c[1][1][1]; }
fn build() -> int[][][] {
  cube: int[][][] = [];
  cube.push([[1, 2], [3, 4]]);
  cube.push([[5, 6], [7, 8]]);
  return cube;
}
fn main() -> int {
  c: int[][][] = build();
  println(Str<int>(corner(c)));         // 8
  c[0][1][0] = 30;
  plane: int[][] = c[0];
  println(Str<int>(plane[1][0]));       // 30
  c[1] = [[9]];
  println(Str<int>(c[1][0][0]));        // 9
  println(Str<int>(c.len()));           // 2
  println(Str<int>(c[1].len()));        // 1
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "8\n30\n9\n2\n1\n");
}

TEST(RefField, MemberAccessPassedAsClassArg) {
  // Passing a member access (borrowed box) as a class-typed argument to a
  // user method must retain it: the callee consumes (releases) it, and the
  // holder releases it on destruction, so without the retain the object is
  // freed one time too many and a later use reads freed memory.
  auto r = compileAndRun(R"(
class Named { name: Str; fn __init__(n: Str){ self.name = n; } }
class Store {
  saved: Str;
  fn __init__(){ self.saved = ""; }
  fn keep(s: Str) { self.saved = s; }
  fn get() -> Str { return self.saved; }
}
fn stash(st: Store) {
  nm: Named = Named("hello");
  st.keep(nm.name);   // member access as class arg; nm dies at return
}
fn main() -> int {
  st: Store = Store();
  stash(st);
  stash(st);                 // second call would crash if the first over-freed
  println(st.get());         // "hello"
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "hello\n");
}

TEST(RefField, ObjectElementFieldSubscriptAssign) {
  // Assigning into an object-element array field (self.items[i] = obj) must
  // unwrap the field box and use the object-set path (element type derived
  // from the array expression, not just bare identifiers).
  auto r = compileAndRun(R"(
class Foo { v: int; fn __init__(x: int){ self.v = x; } }
class Box {
  items: Foo[];
  fn __init__(){ self.items = [Foo(1), Foo(2)]; }
  fn set1(f: Foo) { self.items[1] = f; }
  fn get1() -> int { return self.items[1].v; }
}
fn main() -> int {
  b: Box = Box();
  b.set1(Foo(99));
  println(Str<int>(b.get1()));   // 99
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "99\n");
}

// ============================================================================
// int -> float promotion into float[] element slots.  Every scalar store
// promotes; the element paths (literal, push, subscript store) used to store
// the integer bit pattern, which read back as a denormal double.
// ============================================================================

TEST(ArrayPromotion, IntElementsIntoFloatArray) {
  auto r = compileAndRun(R"(
    class C {
      fs: float[];
      fn __init__() { self.fs = [1, 2]; }
      fn sum(ys: float[]) -> float { return ys[0] + ys[1]; }
    }
    fn total(zs: float[]) -> float { return zs[0] + zs[1]; }
    fn mk() -> float[] { return [7, 8]; }
    fn main() -> int {
      xs: float[] = [1, 2];            // all-int literal into float[]
      println(Str<float>(xs[0] + xs[1]));
      ys: float[] = [1, 2.5];          // mixed literal
      println(Str<float>(ys[0] + ys[1]));
      n: int = 4;
      zs: float[] = [n, n + 1];        // non-constant int elements
      println(Str<float>(zs[0] + zs[1]));
      xs.push(3);                      // push(int)
      println(Str<float>(xs[2]));
      xs[0] = 10;                      // subscript store
      println(Str<float>(xs[0]));
      xs = [5, 6];                     // reassignment
      println(Str<float>(xs[1]));
      c: C = C();
      println(Str<float>(c.fs[1]));      // field store
      println(Str<float>(total([3, 4]))); // function argument
      println(Str<float>(c.sum([5, 6]))); // method argument
      println(Str<float>(mk()[1]));      // returned literal
      grid: float[][] = [[1], [2]];    // nested literals
      println(Str<float>(grid[0][0] + grid[1][0]));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3\n3.5\n9\n3\n10\n6\n2\n7\n11\n8\n3\n");
}
