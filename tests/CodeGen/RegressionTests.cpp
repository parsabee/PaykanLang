// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Regression tests for crashers and miscompiles found after the fact.  Every
// runtime test runs under the tracking allocator and asserts zero live heap
// blocks, since most of these bugs were ownership imbalances.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <unistd.h>

extern "C" {
#include "Runtime.h"
}

using namespace paykan::test;

// ============================================================================
// Ref-typed (array / tuple) parameters of user methods
// ============================================================================

TEST(Regression, MethodArrayParam) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Bag {
      fn take(ys: int[]) -> int { return ys.len(); }
    }
    fn main() -> int {
      b = Bag();
      ys: int[] = [1, 2, 3];
      println(StrInt(b.take(ys)));
      println(StrInt(b.take([4, 5])));
      println(StrInt(ys.len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "3\n2\n3\n");
  g.expectNoLeaks("MethodArrayParam");
}

TEST(Regression, MethodObjectArrayParam) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class P {
      x: int;
      fn __init__(x: int) { self.x = x; }
    }
    class Bag {
      fn total(ps: P[]) -> int {
        s = 0;
        i = 0;
        while (i < ps.len()) { s = s + ps[i].x; i = i + 1; }
        return s;
      }
      fn count(ss: Str[]) -> int { return ss.len(); }
    }
    fn main() -> int {
      b = Bag();
      ps: P[] = [P(1), P(2)];
      println(StrInt(b.total(ps)));
      println(StrInt(b.total([P(3), P(4)])));
      println(StrInt(b.count(["a", "b", "c"])));
      println(StrInt(ps[1].x));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "3\n7\n3\n2\n");
  g.expectNoLeaks("MethodObjectArrayParam");
}

TEST(Regression, MethodTupleParam) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Bag {
      fn second(t: (int, Str)) -> Str { return t.1; }
      fn first(t: (int, Str)) -> int { return t.0; }
    }
    fn main() -> int {
      b = Bag();
      t: (int, Str) = (1, "one");
      println(b.second(t));
      println(b.second((2, "two")));
      println(StrInt(b.first(t)));
      println(t.1);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "one\ntwo\n1\none\n");
  g.expectNoLeaks("MethodTupleParam");
}

TEST(Regression, MethodEqualsOverrideAndArrayEquals) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Bag {
      n: int;
      fn __init__(n: int) { self.n = n; }
      fn equals(o: Obj) -> bool { return True; }
    }
    fn main() -> int {
      b = Bag(1);
      c = Bag(2);
      ys: int[] = [1, 2, 3];
      zs: int[] = ys;
      println(StrBool(b == c));
      println(StrBool(b.equals(c)));
      println(StrBool(ys == zs));
      println(StrBool(ys.equals([1, 2, 3])));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  // Array equality is reference identity.
  EXPECT_EQ(r.StdOut, "True\nTrue\nTrue\nFalse\n");
  g.expectNoLeaks("MethodEqualsOverrideAndArrayEquals");
}

// ============================================================================
// Inferred-type variables initialised from a ref-typed ternary
// ============================================================================

TEST(Regression, InferredClassTernaryReceiver) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Point {
      x: int;
      y: int;
      fn __init__(x: int, y: int) { self.x = x; self.y = y; }
      fn sum() -> int { return self.x + self.y; }
    }
    class Animal { fn sound() -> Str { return "..."; } }
    class Dog : Animal { fn sound() -> Str { return "woof"; } }
    class Cat : Animal { fn sound() -> Str { return "meow"; } }
    fn pick(c: bool) -> int {
      p = Point(1, 2);
      q = Point(10, 20);
      r = if c then p else q;
      return r.sum() + r.x;
    }
    fn main() -> int {
      println(StrInt(pick(True)));
      println(StrInt(pick(False)));
      c = False;
      r = if c then Point(3, 3) else Point(4, 4);
      println(StrInt(r.sum()));
      a = if c then Dog() else Cat();
      println(a.sound());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "4\n40\n8\nmeow\n");
  g.expectNoLeaks("InferredClassTernaryReceiver");
}

TEST(Regression, InferredArrayTupleOptionalTernary) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Point {
      x: int;
      fn __init__(x: int) { self.x = x; }
      fn get() -> int { return self.x; }
    }
    fn run(c: bool) -> int {
      p = Point(1);
      q = Point(2);
      ps: Point[] = [p];
      arr = if c then ps else [q, q];
      t: (int, Point) = (5, p);
      tup = if c then t else (6, q);
      opt = if c then p else None;
      n = 0;
      if (opt != None) { n = 100; }
      return arr.len() * 1000 + arr[0].get() * 10 + tup.1.get() + tup.0 + n;
    }
    fn main() -> int {
      println(StrInt(run(True)));
      println(StrInt(run(False)));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "1116\n2028\n");
  g.expectNoLeaks("InferredArrayTupleOptionalTernary");
}
// ============================================================================
// Array subscript: char elements and temporary (call-rooted) receivers
// ============================================================================

TEST(Regression, CharArrayElements) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Word {
      cs: char[];
      fn __init__() { self.cs = ['h', 'i']; }
      fn first() -> char { return self.cs[0]; }
    }
    fn isA(c: char) -> bool { return c == 'a'; }
    fn second(cs: char[]) -> char { return cs[1]; }
    fn main() -> int {
      cs: char[] = ['a', 'b'];
      println(StrChar(cs[1]));
      if (cs[0] == 'a') { println("eq"); }
      println(StrBool(isA(cs[0])));
      c: char = cs[1];
      d = cs[0];
      println(StrChar(c) + StrChar(d));
      x = 'k';
      ds: char[] = [x, 'm'];
      ds.push('z');
      ds[0] = 'q';
      println(StrChar(ds[0]) + StrChar(second(ds)) + StrChar(ds.pop()));
      println(StrInt(ds.len()));
      w = Word();
      println(StrChar(w.first()) + StrChar(w.cs[1]));
      s: Str = "yo";
      println(StrChar(s[1]));
      bs: bool[] = [True, False];
      bs.push(True);
      println(StrBool(bs.pop()) + StrBool(bs[1]));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "b\neq\nTrue\nba\nqmz\n2\nhi\no\nTrueFalse\n");
  g.expectNoLeaks("CharArrayElements");
}

TEST(Regression, CallRootedObjectSubscript) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Point {
      x: int;
      fn __init__(x: int) { self.x = x; }
      fn get() -> int { return self.x; }
    }
    class Holder {
      items: Point[];
      fn __init__() { self.items = [Point(7)]; }
    }
    fn mk() -> Point[] { return [Point(1), Point(2)]; }
    fn mks() -> Str[] { return ["a", "b"]; }
    fn mkn() -> int[][] { return [[1, 2], [3]]; }
    fn mkh() -> Holder { return Holder(); }
    fn take(p: Point) -> int { return p.x; }
    fn ret() -> Point { return mk()[1]; }
    fn main() -> int {
      println(StrInt(mk()[0].x));
      println(StrInt(mk()[1].get()));
      p = mk()[0];
      q: Point = mk()[1];
      println(StrInt(p.x + q.x));
      q = mk()[0];
      println(StrInt(q.x));
      println(StrInt(take(mk()[0])));
      s = mks()[0];
      println(s + mks()[1]);
      println(StrInt(mkn()[0][1]));
      println(StrInt(mkn()[1].len()));
      println(StrInt(mkh().items[0].x));
      println(StrInt(ret().x));
      ps: Point[] = [mk()[1]];
      println(StrInt(ps[0].x));
      match mk()[0] {
        pp: Point { println(StrInt(pp.x)); }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "1\n2\n3\n1\n1\nab\n2\n1\n7\n2\n2\n1\n");
  g.expectNoLeaks("CallRootedObjectSubscript");
}

TEST(Regression, CallRootedSubscriptAssign) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Point {
      x: int;
      fn __init__(x: int) { self.x = x; }
    }
    fn mk() -> Point[] { return [Point(1), Point(2)]; }
    fn mki() -> int[] { return [1, 2]; }
    fn mkn() -> int[][] { return [[1, 2], [3]]; }
    fn main() -> int {
      mk()[0] = Point(9);
      mki()[1] = 5;
      mkn()[0] = [5];
      mkn()[1][0] = 4;
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  g.expectNoLeaks("CallRootedSubscriptAssign");
}
// ============================================================================
// Class functions referenced before their class body is emitted
// ============================================================================

namespace {

// Write @p content to @p relPath under @p dir (creating directories).
std::string writeProjectFile(const std::filesystem::path &dir,
                             const std::string &relPath,
                             const std::string &content) {
  auto full = dir / relPath;
  std::filesystem::create_directories(full.parent_path());
  std::ofstream ofs(full);
  ofs << content;
  return full.string();
}

// A fresh project directory; Sema caches analysed modules by path, so every
// test uses its own.
std::filesystem::path freshProjectDir(const std::string &name) {
  auto dir = paykan::test::tempDir() /
             ("pkn_regression_" + name + "_" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

} // namespace

TEST(Regression, MethodConstructsOwnAndLaterClass) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Cell {
      v: int;
      fn __init__(v: int) { self.v = v; }
      fn clone() -> Cell { return Cell(self.v + 1); }
      fn wrap() -> Box { return Box(self.v * 2); }
    }
    class Box {
      n: int;
      fn __init__(n: int) { self.n = n; }
      fn cell() -> Cell { return Cell(self.n); }
    }
    fn main() -> int {
      c = Cell(3);
      d = c.clone();
      b = c.wrap();
      println(StrInt(d.v) + " " + StrInt(b.n) + " " + StrInt(b.cell().v));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "4 6 6\n");
  g.expectNoLeaks("MethodConstructsOwnAndLaterClass");
}

TEST(Regression, ImportedClassConstructsItself) {
  auto dir = freshProjectDir("self_ctor");
  writeProjectFile(dir, "lib/cell.pkn", R"(
class Cell {
  v: int;
  fn __init__(v: int) { self.v = v; }
  fn clone() -> Cell { return Cell(self.v + 1); }
}
)");
  auto mainPath = writeProjectFile(dir, "main.pkn", R"(
import lib::cell;
fn main() -> int {
  c = cell::Cell(3);
  d = c.clone();
  return d.v;
}
)");
  LeakGuard g;
  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 4);
  g.expectNoLeaks("ImportedClassConstructsItself");
  std::filesystem::remove_all(dir);
}

TEST(Regression, SubclassDeclaredBeforeBase) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Square : Rect {
      fn __init__(s: int) { __super__(s, s, "square"); }
    }
    class Rect {
      w: int;
      h: int;
      tag: Str;
      fn __init__(w: int, h: int, tag: Str) {
        self.w = w;
        self.h = h;
        self.tag = tag;
      }
      fn area() -> int { return self.w * self.h; }
    }
    fn main() -> int {
      s = Square(4);
      println(StrInt(s.area()) + " " + s.tag);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "16 square\n");
  g.expectNoLeaks("SubclassDeclaredBeforeBase");
}

TEST(Regression, SuperInitOfImportedBase) {
  auto dir = freshProjectDir("imported_base");
  writeProjectFile(dir, "shapes/rect.pkn", R"(
class Rect {
  w: int;
  h: int;
  tag: Str;
  fn __init__(w: int, h: int, tag: Str) {
    self.w = w;
    self.h = h;
    self.tag = tag;
  }
  fn area() -> int { return self.w * self.h; }
}
class Unit {
  n: int;
  fn __init__() { self.n = 7; }
}
)");
  auto mainPath = writeProjectFile(dir, "main.pkn", R"(
import shapes::rect;
class Square : rect::Rect {
  fn __init__(s: int) { __super__(s, s, "sq"); }
}
class One : rect::Unit {
  fn __init__() { __super__(); }
}
fn main() -> int {
  s = Square(4);
  o = One();
  if (s.tag != "sq") { return 1; }
  return s.area() + o.n;
}
)");
  LeakGuard g;
  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 23);
  g.expectNoLeaks("SuperInitOfImportedBase");
  std::filesystem::remove_all(dir);
}

TEST(Regression, SuperArgumentsAreBoxed) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Point {
      x: int;
      fn __init__(x: int) { self.x = x; }
    }
    class Base {
      p: Point;
      name: Str;
      xs: int[];
      fn __init__(p: Point, name: Str, xs: int[]) {
        self.p = p;
        self.name = name;
        self.xs = xs;
      }
    }
    class Lit : Base {
      fn __init__() { __super__(Point(1), "literal", [1, 2]); }
    }
    class Cat : Base {
      fn __init__(n: Str) { __super__(Point(2), n + "!", [3]); }
    }
    class Fwd : Base {
      fn __init__(p: Point, n: Str, xs: int[]) { __super__(p, n, xs); }
    }
    fn main() -> int {
      a = Lit();
      println(a.name + StrInt(a.p.x) + StrInt(a.xs.len()));
      b = Cat("c");
      println(b.name + StrInt(b.p.x));
      q = Point(3);
      s: Str = "fwd";
      ys: int[] = [7];
      c = Fwd(q, s, ys);
      println(c.name + StrInt(c.p.x) + StrInt(q.x) + s + StrInt(ys.len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "literal12\nc!2\nfwd33fwd1\n");
  g.expectNoLeaks("SuperArgumentsAreBoxed");
}
// ============================================================================
// Empty array literals take their element type from the destination
// ============================================================================

TEST(Regression, PushOntoEmptyArrayFieldFromMethod) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class P {
      x: int;
      fn __init__(x: int) { self.x = x; }
    }
    class SStack {
      items: Str[];
      ps: P[];
      fn __init__() { self.items = []; self.ps = []; }
      fn push(v: Str) { self.items.push(v); }
      fn pushP(p: P) { self.ps.push(p); }
      fn pop() -> Str { return self.items.pop(); }
      fn popP() -> P { return self.ps.pop(); }
    }
    fn main() -> int {
      s = SStack();
      s.push("a");
      s.push("b" + "c");
      s.push("d");
      s.pushP(P(1));
      q = P(2);
      s.pushP(q);
      s.pushP(P(3));
      println(s.pop());
      println(StrInt(s.popP().x));
      println(StrInt(s.items.len()) + " " + StrInt(s.ps.len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "d\n3\n2 2\n");
  g.expectNoLeaks("PushOntoEmptyArrayFieldFromMethod");
}

// The same program as ArrayTests' RefField.SubscriptObjectArrayField.
TEST(Regression, SubscriptObjectArrayFieldIsLeakFree) {
  LeakGuard g;
  auto r = compileAndRun(R"(
class Foo { v: int; fn __init__(x: int){ self.v = x; } }
class Box {
  items: Foo[];
  fn __init__(){ self.items = []; self.items.push(Foo(41)); self.items.push(Foo(42)); }
  fn second() -> int { return self.items[1].v; }   // subscript a member-access array
}
fn main() -> int {
  b: Box = Box();
  println(StrInt(b.items[0].v));   // subscript via local receiver
  println(StrInt(b.second()));     // subscript via self.field inside a method
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "41\n42\n");
  g.expectNoLeaks("SubscriptObjectArrayFieldIsLeakFree");
}

// The same program as ArrayTests' RefField.NestedArrayOfArraysField.
TEST(Regression, NestedArrayOfArraysFieldIsLeakFree) {
  LeakGuard g;
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
  println(StrInt(g.at(0, 0)));   // 0
  println(StrInt(g.at(1, 2)));   // 5
  println(StrInt(g.at(2, 1)));   // 7
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n5\n7\n");
  g.expectNoLeaks("NestedArrayOfArraysFieldIsLeakFree");
}

TEST(Regression, EmptyArrayArgumentAndReturn) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class P {
      x: int;
      fn __init__(x: int) { self.x = x; }
    }
    class Bag {
      fn fill(ps: P[]) -> int { ps.push(P(1)); ps.push(P(2)); return ps.len(); }
    }
    fn fill(ss: Str[]) -> int { ss.push("x"); return ss.len(); }
    fn none() -> P[] { return []; }
    fn main() -> int {
      println(StrInt(fill([])));
      println(StrInt(Bag().fill([])));
      ps = none();
      ps.push(P(3));
      grid: Str[][] = [["a"]];
      grid[0] = [];
      grid[0].push("b");
      println(StrInt(ps.len()) + grid[0][0]);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "1\n2\n1b\n");
  g.expectNoLeaks("EmptyArrayArgumentAndReturn");
}