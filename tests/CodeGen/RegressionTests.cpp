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

// -- Ref-typed (array / tuple) parameters of user methods

TEST(Regression, MethodArrayParam) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Bag {
      fn take(ys: int[]) -> int { return ys.len(); }
    }
    fn main() -> int {
      b = Bag();
      ys: int[] = [1, 2, 3];
      println(Str<int>(b.take(ys)));
      println(Str<int>(b.take([4, 5])));
      println(Str<int>(ys.len()));
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
      println(Str<int>(b.total(ps)));
      println(Str<int>(b.total([P(3), P(4)])));
      println(Str<int>(b.count(["a", "b", "c"])));
      println(Str<int>(ps[1].x));
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
      println(Str<int>(b.first(t)));
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
      println(Str<bool>(b == c));
      println(Str<bool>(b.equals(c)));
      println(Str<bool>(ys == zs));
      println(Str<bool>(ys.equals([1, 2, 3])));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  // Array equality is reference identity.
  EXPECT_EQ(r.StdOut, "True\nTrue\nTrue\nFalse\n");
  g.expectNoLeaks("MethodEqualsOverrideAndArrayEquals");
}

// -- Inferred-type variables initialised from a ref-typed ternary

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
      println(Str<int>(pick(True)));
      println(Str<int>(pick(False)));
      c = False;
      r = if c then Point(3, 3) else Point(4, 4);
      println(Str<int>(r.sum()));
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
      println(Str<int>(run(True)));
      println(Str<int>(run(False)));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "1116\n2028\n");
  g.expectNoLeaks("InferredArrayTupleOptionalTernary");
}
// -- Array subscript: char elements and temporary (call-rooted) receivers

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
      println(Str<char>(cs[1]));
      if (cs[0] == 'a') { println("eq"); }
      println(Str<bool>(isA(cs[0])));
      c: char = cs[1];
      d = cs[0];
      println(Str<char>(c) + Str<char>(d));
      x = 'k';
      ds: char[] = [x, 'm'];
      ds.push('z');
      ds[0] = 'q';
      println(Str<char>(ds[0]) + Str<char>(second(ds)) + Str<char>(ds.pop()));
      println(Str<int>(ds.len()));
      w = Word();
      println(Str<char>(w.first()) + Str<char>(w.cs[1]));
      s: Str = "yo";
      println(Str<char>(s[1]));
      bs: bool[] = [True, False];
      bs.push(True);
      println(Str<bool>(bs.pop()) + Str<bool>(bs[1]));
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
      println(Str<int>(mk()[0].x));
      println(Str<int>(mk()[1].get()));
      p = mk()[0];
      q: Point = mk()[1];
      println(Str<int>(p.x + q.x));
      q = mk()[0];
      println(Str<int>(q.x));
      println(Str<int>(take(mk()[0])));
      s = mks()[0];
      println(s + mks()[1]);
      println(Str<int>(mkn()[0][1]));
      println(Str<int>(mkn()[1].len()));
      println(Str<int>(mkh().items[0].x));
      println(Str<int>(ret().x));
      ps: Point[] = [mk()[1]];
      println(Str<int>(ps[0].x));
      match mk()[0] {
        pp: Point { println(Str<int>(pp.x)); }
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
// -- Class functions referenced before their class body is emitted

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
      println(Str<int>(d.v) + " " + Str<int>(b.n) + " " + Str<int>(b.cell().v));
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
      println(Str<int>(s.area()) + " " + s.tag);
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
      println(a.name + Str<int>(a.p.x) + Str<int>(a.xs.len()));
      b = Cat("c");
      println(b.name + Str<int>(b.p.x));
      q = Point(3);
      s: Str = "fwd";
      ys: int[] = [7];
      c = Fwd(q, s, ys);
      println(c.name + Str<int>(c.p.x) + Str<int>(q.x) + s + Str<int>(ys.len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "literal12\nc!2\nfwd33fwd1\n");
  g.expectNoLeaks("SuperArgumentsAreBoxed");
}
// -- Empty array literals take their element type from the destination

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
      println(Str<int>(s.popP().x));
      println(Str<int>(s.items.len()) + " " + Str<int>(s.ps.len()));
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
  println(Str<int>(b.items[0].v));   // subscript via local receiver
  println(Str<int>(b.second()));     // subscript via self.field inside a method
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
  println(Str<int>(g.at(0, 0)));   // 0
  println(Str<int>(g.at(1, 2)));   // 5
  println(Str<int>(g.at(2, 1)));   // 7
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
      println(Str<int>(fill([])));
      println(Str<int>(Bag().fill([])));
      ps = none();
      ps.push(P(3));
      grid: Str[][] = [["a"]];
      grid[0] = [];
      grid[0].push("b");
      println(Str<int>(ps.len()) + grid[0][0]);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "1\n2\n1b\n");
  g.expectNoLeaks("EmptyArrayArgumentAndReturn");
}

// #72: `match` on a literal subject (string, and the primitive literals) is a
// plain value match; the string subject is released on every exit.
TEST(Regression, MatchOnALiteralSubject) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn pick() -> int {
      match "r" { "r" { return 7; } _ { return 0; } }
    }
    fn main() -> int {
      match "s" { "t" { println("t"); } _ { println("other"); } }
      match "s" { "s" { println("s"); } _ { println("other"); } }
      match "" { "" { println("empty"); } _ { println("other"); } }
      match "q" { _ { println("wild"); } }
      match 3 { 1 { println("1"); } 3 { println("3"); } _ { println("?"); } }
      match 2.5 { 2.5 { println("2.5"); } _ { println("?"); } }
      match 'c' { 'c' { println("c"); } _ { println("?"); } }
      match True { True { println("T"); } False { println("F"); } }
      i = 0;
      while (i < 3) {
        i = i + 1;
        match "x" { "x" { if (i == 2) { break; } println("x"); } _ { } }
      }
      return pick();
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 7);
  EXPECT_EQ(r.StdOut, "other\ns\nempty\nwild\n3\n2.5\nc\nT\nx\n");
  g.expectNoLeaks("MatchOnALiteralSubject");
}

// #86: methods were lowered to `<Class>_<method>`, so a user function spelled
// that way (`K_w`, `K_destroy`) collided with the method or the generated
// destructor.  They are now `<Class>.<method>`, which no identifier can spell.
TEST(Regression, MethodAndUserFunctionWithTheOldMangledName) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class K { fn w() -> int { return 1; } }
    fn K_w() -> int { return 2; }
    fn main() -> int { k: K = K(); println(Str<int>(k.w() + K_w())); return 0; }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "3\n");
  g.expectNoLeaks("MethodAndUserFunctionWithTheOldMangledName");
}

TEST(Regression, DestructorAndUserFunctionWithTheOldMangledName) {
  LeakGuard g;
  // The destructor of K (which releases `name`) and the user `K_destroy`
  // are distinct; so are a method `vtable` and the class's vtable (LLVM)
  // and `K_vtable`.
  auto r = compileAndRun(R"(
    class K {
      name: Str;
      fn __init__(name: Str) { self.name = name; }
      fn vtable() -> int { return 10; }
    }
    class D : K {
      fn __init__() { __super__("d"); }
      fn vtable() -> int { return 20; }
    }
    fn K_destroy(k: K) -> Str { return "user " + k.name; }
    fn K_vtable() -> int { return 30; }
    fn D_vtable() -> int { return 40; }
    fn main() -> int {
      k: K = K("k");
      b: K = D();
      println(K_destroy(k) + " " + K_destroy(b));
      println(Str<int>(k.vtable() + b.vtable() + K_vtable() + D_vtable()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "user k user d\n100\n");
  g.expectNoLeaks("DestructorAndUserFunctionWithTheOldMangledName");
}

// The C backend escapes the '.' of `K.w` as `_2E`; a user function named
// `K_2Ew` sanitizes to the same text and must still get its own C symbol.
TEST(Regression, MethodAndUserFunctionWithItsCSpelling) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class K { fn w() -> int { return 1; } fn destroy2() -> int { return 4; } }
    fn K_2Ew() -> int { return 2; }
    fn K_2Edestroy() -> int { return 8; }
    fn K_2Edestroy2() -> int { return 16; }
    fn main() -> int {
      k: K = K();
      println(Str<int>(k.w() + K_2Ew() + k.destroy2() + K_2Edestroy() +
                       K_2Edestroy2()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "31\n");
  g.expectNoLeaks("MethodAndUserFunctionWithItsCSpelling");
}

// An imported class's methods keep their names in the importer, beside the
// importer's own `Cell_clone`.
TEST(Regression, ImportedClassMethodAndImporterFunctionWithTheOldName) {
  auto dir = freshProjectDir("method_mangling");
  writeProjectFile(dir, "lib/cell.pkn", R"(
class Cell {
  v: int;
  fn __init__(v: int) { self.v = v; }
  fn clone() -> Cell { return Cell(self.v + 1); }
}
)");
  auto mainPath = writeProjectFile(dir, "main.pkn", R"(
import lib::cell;
fn Cell_clone() -> int { return 100; }
fn Cell_destroy() -> int { return 1000; }
fn main() -> int {
  c = cell::Cell(3);
  d = c.clone();
  println(Str<int>(d.v + Cell_clone() + Cell_destroy()));
  return 0;
}
)");
  LeakGuard g;
  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "1104\n");
  g.expectNoLeaks("ImportedClassMethodAndImporterFunctionWithTheOldName");
  std::filesystem::remove_all(dir);
}

// Issue #95: push/pop at the array capacity boundary thrashed (two reallocs
// per pair).  The issue's repro, scaled down: peak 100,000 (cap 131,072),
// then 20,000 push/pop pairs at len 65,536 = cap/2.

TEST(Regression, ArrayPushPopAtCapacityBoundary) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Item {}
    fn main() -> int {
      xs: int[] = [];
      os: Item[] = [];
      i = 0;
      while (i < 100000) { xs.push(i); os.push(Item()); i = i + 1; }
      while (xs.len() > 65536) { xs.pop(); os.pop(); }
      k = 0;
      sum = 0;
      while (k < 20000) {
        xs.push(k);
        sum = sum + xs.pop();
        os.push(Item());
        os.pop();
        k = k + 1;
      }
      println(Str(xs.len()));
      println(Str(os.len()));
      println(Str(sum));
      println(Str(xs[65535]));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "65536\n65536\n199990000\n65535\n");
  g.expectNoLeaks("array push/pop at capacity boundary");
  // The JIT runs in process, so the realloc counter is the program's own:
  // growth to 131,072 is 15 doublings per array, and the boundary loop adds
  // none (it was 2 x 2 x 20,000 before the fix).  The C backend runs the same
  // runtime in a child; tests/Runtime/ArrayTests.cpp covers it directly.
  if (testBackend() == "llvm") {
    EXPECT_LE(Paykan_heap_total_reallocs(), 100);
  }
}
