// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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

// -- Array and tuple parameters of user methods

TEST(Regression, MethodArrayParam) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Bag {
      fn take(ys: view int[]) -> int { return ys.len(); }
    }
    fn main() -> int {
      b = Bag();
      ys: int[3] = [1, 2, 3];
      io::println(Str<int>(b.take(ys)));
      io::println(Str<int>(b.take([4, 5])));
      io::println(Str<int>(ys.len()));
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
  auto r = compileAndRun(R"(import ::io;
    class P {
      x: int;
    }

    impl P {
      fn __init__(x: int) { self.x = x; }
    }
    class Bag {
      fn total(ps: view P[]) -> int {
        s = 0;
        i = 0;
        while i < ps.len() { s = s + ps[i].x; i = i + 1; }
        return s;
      }
      fn count(ss: view Str[]) -> int { return ss.len(); }
    }
    fn main() -> int {
      b = Bag();
      ps: P[2] = [P(1), P(2)];
      io::println(Str<int>(b.total(ps)));
      io::println(Str<int>(b.total([P(3), P(4)])));
      io::println(Str<int>(b.count(["a", "b", "c"])));
      io::println(Str<int>(ps[1].x));
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
  auto r = compileAndRun(R"(import ::io;
    class Bag {
      fn second(t: (int, Str)) -> Str { return t.1; }
      fn first(t: (int, Str)) -> int { return t.0; }
    }
    fn main() -> int {
      b = Bag();
      t: (int, Str) = (1, "one");
      io::println(b.second(t));
      io::println(b.second((2, "two")));
      io::println(Str<int>(b.first(t)));
      io::println(t.1);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "one\ntwo\n1\none\n");
  g.expectNoLeaks("MethodTupleParam");
}

TEST(Regression, MethodEqualsOverride) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Bag {
      n: int;
      view fn equals(o: Obj) -> bool { return True; }
    }

    impl Bag {
      fn __init__(n: int) { self.n = n; }
    }
    fn main() -> int {
      b = Bag(1);
      c = Bag(2);
      io::println(Str<bool>(b == c));
      io::println(Str<bool>(b.equals(c)));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "True\nTrue\n");
  g.expectNoLeaks("MethodEqualsOverride");
}

// -- Inferred-type variables initialised from a ref-typed ternary

TEST(Regression, InferredClassTernaryReceiver) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Point {
      x: int;
      y: int;
      fn sum() -> int { return self.x + self.y; }
    }

    impl Point {
      fn __init__(x: int, y: int) { self.x = x; self.y = y; }
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
      io::println(Str<int>(pick(True)));
      io::println(Str<int>(pick(False)));
      c = False;
      r = if c then Point(3, 3) else Point(4, 4);
      io::println(Str<int>(r.sum()));
      a = if c then Dog() else Cat();
      io::println(a.sound());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "4\n40\n8\nmeow\n");
  g.expectNoLeaks("InferredClassTernaryReceiver");
}

TEST(Regression, InferredTupleOptionalTernary) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Point {
      x: int;
      fn get() -> int { return self.x; }
    }

    impl Point {
      fn __init__(x: int) { self.x = x; }
    }
    fn run(c: bool) -> int {
      p = Point(1);
      q = Point(2);
      t: (int, Point) = (5, p);
      tup = if c then t else (6, q);
      opt = if c then p else None;
      n = 0;
      if opt != None { n = 100; }
      return tup.1.get() * 10 + tup.0 + n;
    }
    fn main() -> int {
      io::println(Str<int>(run(True)));
      io::println(Str<int>(run(False)));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "115\n26\n");
  g.expectNoLeaks("InferredTupleOptionalTernary");
}
// -- Array subscript: char elements and temporary (call-rooted) receivers

TEST(Regression, CharArrayElements) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Word {
      cs: char[2];
      fn first() -> char { return self.cs[0]; }
    }

    impl Word {
      fn __init__() { self.cs = ['h', 'i']; }
    }
    fn isA(c: char) -> bool { return c == 'a'; }
    fn second(cs: view char[]) -> char { return cs[1]; }
    fn main() -> int {
      cs: char[2] = ['a', 'b'];
      io::println(Str<char>(cs[1]));
      if cs[0] == 'a' { io::println("eq"); }
      io::println(Str<bool>(isA(cs[0])));
      c: char = cs[1];
      d = cs[0];
      io::println(Str<char>(c) + Str<char>(d));
      x = 'k';
      ds: char[3] = [x, 'm', x];
      ds[2] = 'z';
      ds[0] = 'q';
      io::println(Str<char>(ds[0]) + Str<char>(second(ds)) + Str<char>(ds[2]));
      io::println(Str<int>(ds.len()));
      w = Word();
      io::println(Str<char>(w.first()) + Str<char>(w.cs[1]));
      s: Str = "yo";
      io::println(Str<char>(s[1]));
      bs: bool[3] = [True, False, False];
      bs[2] = True;
      io::println(Str<bool>(bs[2]) + Str<bool>(bs[1]));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "b\neq\nTrue\nba\nqmz\n3\nhi\no\nTrueFalse\n");
  g.expectNoLeaks("CharArrayElements");
}

TEST(Regression, CallRootedObjectSubscript) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Point {
      x: int;
      fn get() -> int { return self.x; }
    }

    impl Point {
      fn __init__(x: int) { self.x = x; }
    }
    class Holder {
      items: Point[1];
    }

    impl Holder {
      fn __init__() { self.items = [Point(7)]; }
    }
    fn mk() -> Point[2] { return [Point(1), Point(2)]; }
    fn mks() -> Str[2] { return ["a", "b"]; }
    fn mkn() -> int[2][2] { return [[1, 2], [3, 4]]; }
    fn mkh() -> Holder { return Holder(); }
    fn take(p: Point) -> int { return p.x; }
    fn ret() -> Point { return mk()[1]; }
    fn main() -> int {
      io::println(Str<int>(mk()[0].x));
      io::println(Str<int>(mk()[1].get()));
      p = mk()[0];
      q: Point = mk()[1];
      io::println(Str<int>(p.x + q.x));
      q = mk()[0];
      io::println(Str<int>(q.x));
      io::println(Str<int>(take(mk()[0])));
      s = mks()[0];
      io::println(s + mks()[1]);
      io::println(Str<int>(mkn()[0][1]));
      io::println(Str<int>(mkn()[1].len()));
      io::println(Str<int>(mkh().items[0].x));
      io::println(Str<int>(ret().x));
      ps: Point[1] = [mk()[1]];
      io::println(Str<int>(ps[0].x));
      match mk()[0] {
        pp: Point { io::println(Str<int>(pp.x)); }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "1\n2\n3\n1\n1\nab\n2\n2\n7\n2\n2\n1\n");
  g.expectNoLeaks("CallRootedObjectSubscript");
}

TEST(Regression, CallRootedSubscriptAssign) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Point {
      x: int;
    }

    impl Point {
      fn __init__(x: int) { self.x = x; }
    }
    fn mk() -> Point[2] { return [Point(1), Point(2)]; }
    fn mki() -> int[2] { return [1, 2]; }
    fn mkn() -> int[2][2] { return [[1, 2], [3, 4]]; }
    fn main() -> int {
      mk()[0] = Point(9);
      mki()[1] = 5;
      mkn()[0] = [5, 6];
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
  auto r = compileAndRun(R"(import ::io;
    class Cell {
      v: int;
      fn clone() -> Cell { return Cell(self.v + 1); }
      fn wrap() -> Box { return Box(self.v * 2); }
    }

    impl Cell {
      fn __init__(v: int) { self.v = v; }
    }
    class Box {
      n: int;
      fn cell() -> Cell { return Cell(self.n); }
    }

    impl Box {
      fn __init__(n: int) { self.n = n; }
    }
    fn main() -> int {
      c = Cell(3);
      d = c.clone();
      b = c.wrap();
      io::println(Str<int>(d.v) + " " + Str<int>(b.n) + " " + Str<int>(b.cell().v));
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
  fn clone() -> Cell { return Cell(self.v + 1); }
}

impl Cell {
  fn __init__(v: int) { self.v = v; }
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
  auto r = compileAndRun(R"(import ::io;
    class Square : Rect {}

    impl Square {
      fn __init__(s: int) { __super__(s, s, "square"); }
    }
    class Rect {
      w: int;
      h: int;
      tag: Str;
      fn area() -> int { return self.w * self.h; }
    }

    impl Rect {
      fn __init__(w: int, h: int, tag: Str) {
        self.w = w;
        self.h = h;
        self.tag = tag;
      }
    }
    fn main() -> int {
      s = Square(4);
      io::println(Str<int>(s.area()) + " " + s.tag);
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
  fn area() -> int { return self.w * self.h; }
}

impl Rect {
  fn __init__(w: int, h: int, tag: Str) {
    self.w = w;
    self.h = h;
    self.tag = tag;
  }
}
class Unit {
  n: int;
}

impl Unit {
  fn __init__() { self.n = 7; }
}
)");
  auto mainPath = writeProjectFile(dir, "main.pkn", R"(
import shapes::rect;
class Square : rect::Rect {}

impl Square {
  fn __init__(s: int) { __super__(s, s, "sq"); }
}
class One : rect::Unit {}

impl One {
  fn __init__() { __super__(); }
}
fn main() -> int {
  s = Square(4);
  o = One();
  if s.tag != "sq" { return 1; }
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

// A literal passed to `__super__` takes the superclass `__init__`'s
// parameter type: `[1, 2]` into a `float[2]`, `None` into an optional,
// `(None, 3)` into a `(Node?, int)`.
TEST(Regression, SuperLiteralArgumentsTakeTheParameterTypes) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Node { v: int; }
    class Base {
      xs: float[2];
      link: Node?;
      pair: (Node?, int);
      q: int?;
    }
    impl Base {
      fn __init__(xs: float[2], link: Node?, pair: (Node?, int), q: int?) {
        self.xs = xs;
        self.link = link;
        self.pair = pair;
        self.q = q;
      }
    }
    class Derived : Base {}
    impl Derived {
      fn __init__() { __super__([1, 2], None, (None, 3), 4); }
    }
    fn main() -> int {
      d = Derived();
      io::println(Str(d.xs[0] + d.xs[1]) + " " + Str(d.pair.1) + " " +
              Str(d.link == None));
      match d.q { n: int { io::println(Str(n)); } None { io::println("none"); } }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3 3 True\n4\n");
  g.expectNoLeaks("super literal arguments");
}

TEST(Regression, SuperArgumentsAreBoxed) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Point {
      x: int;
    }

    impl Point {
      fn __init__(x: int) { self.x = x; }
    }
    class Base {
      p: Point;
      name: Str;
      xs: int[2];
    }

    impl Base {
      fn __init__(p: Point, name: Str, xs: int[2]) {
        self.p = p;
        self.name = name;
        self.xs = xs;
      }
    }
    fn two(a: int, b: int) -> int[2] { return [a, b]; }
    class Lit : Base {}

    impl Lit {
      fn __init__() { __super__(Point(1), "literal", two(1, 2)); }
    }
    class Cat : Base {}

    impl Cat {
      fn __init__(n: Str) { __super__(Point(2), n + "!", two(3, 4)); }
    }
    class Fwd : Base {}

    impl Fwd {
      fn __init__(p: Point, n: Str, xs: int[2]) { __super__(p, n, xs); }
    }
    fn main() -> int {
      a = Lit();
      io::println(a.name + Str<int>(a.p.x) + Str<int>(a.xs.len()));
      b = Cat("c");
      io::println(b.name + Str<int>(b.p.x));
      q = Point(3);
      s: Str = "fwd";
      ys: int[2] = [7, 8];
      c = Fwd(q, s, ys);
      io::println(c.name + Str<int>(c.p.x) + Str<int>(q.x) + s + Str<int>(ys.len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "literal12\nc!2\nfwd33fwd2\n");
  g.expectNoLeaks("SuperArgumentsAreBoxed");
}
// -- Array fields subscripted through a member access

// The same program as ArrayTests' RefField.SubscriptObjectArrayField.
TEST(Regression, SubscriptObjectArrayFieldIsLeakFree) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
class Foo { v: int; }
impl Foo { fn __init__(x: int){ self.v = x; } }
class Box {
  items: Foo[2];
  fn second() -> int { return self.items[1].v; }   // subscript a member-access array
}

impl Box {
  fn __init__(){ self.items = [Foo(41), Foo(42)]; }
}
fn main() -> int {
  b: Box = Box();
  io::println(Str<int>(b.items[0].v));   // subscript via local receiver
  io::println(Str<int>(b.second()));     // subscript via self.field inside a method
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
  auto r = compileAndRun(R"(import ::io;
class Grid {
  rows: int[3][3];
  fn at(r: int, c: int) -> int {
    row: int[3] = self.rows[r];   // subscript the outer 2-D field
    return row[c];
  }
}

impl Grid {
  fn __init__() {
    self.rows = [[0; 3]; 3];
    i: int = 0;
    while i < 3 {
      row: int[3] = [0; 3];
      j: int = 0;
      while j < 3 { row[j] = i * 3 + j; j = j + 1; }
      self.rows[i] = row;
      i = i + 1;
    }
  }
}
fn main() -> int {
  g: Grid = Grid();
  io::println(Str<int>(g.at(0, 0)));   // 0
  io::println(Str<int>(g.at(1, 2)));   // 5
  io::println(Str<int>(g.at(2, 1)));   // 7
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n5\n7\n");
  g.expectNoLeaks("NestedArrayOfArraysFieldIsLeakFree");
}

// #72: `match` on a literal subject (string, and the primitive literals) is a
// plain value match; the string subject is released on every exit.
TEST(Regression, MatchOnALiteralSubject) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn pick() -> int {
      match "r" { "r" { return 7; } _ { return 0; } }
    }
    fn main() -> int {
      match "s" { "t" { io::println("t"); } _ { io::println("other"); } }
      match "s" { "s" { io::println("s"); } _ { io::println("other"); } }
      match "" { "" { io::println("empty"); } _ { io::println("other"); } }
      match "q" { _ { io::println("wild"); } }
      match 3 { 1 { io::println("1"); } 3 { io::println("3"); } _ { io::println("?"); } }
      match 2.5 { 2.5 { io::println("2.5"); } _ { io::println("?"); } }
      match 'c' { 'c' { io::println("c"); } _ { io::println("?"); } }
      match True { True { io::println("T"); } False { io::println("F"); } }
      i = 0;
      while i < 3 {
        i = i + 1;
        match "x" { "x" { if i == 2 { break; } io::println("x"); } _ { } }
      }
      return pick();
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 7);
  EXPECT_EQ(r.StdOut, "other\ns\nempty\nwild\n3\n2.5\nc\nT\nx\n");
  g.expectNoLeaks("MatchOnALiteralSubject");
}

// A value match with an arm of 9223372036854775807 (INT64_MAX) and any other
// arm failed the MLIR pipeline ("'scf.index_switch' op has duplicate case
// value"): the DenseSet of the switch's cases takes INT64_MAX for its empty
// key.  The arm second, with and without a default, each arm's value and
// INT64_MIN, which is no arm's.
TEST(Regression, MatchArmOfInt64Max) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn cls(n: int) -> int {
      match n { 0 { return 1; } 9223372036854775807 { return 2; } _ { return 3; } }
    }
    fn set(n: int) -> int {
      r = 5;
      match n { 0 { r = 1; } 9223372036854775807 { r = 2; } }
      return r;
    }
    fn main() -> int {
      max = 9223372036854775807;
      min = -max - 1;
      io::println(Str(cls(max)) + Str(cls(0)) + Str(cls(min)) + Str(cls(7)));
      io::println(Str(set(max)) + Str(set(0)) + Str(set(min)) + Str(set(7)));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "2133\n2155\n");
  g.expectNoLeaks("MatchArmOfInt64Max");
}

// #86: methods were lowered to `<Class>_<method>`, so a user function spelled
// that way (`K_w`, `K_destroy`) collided with the method or the generated
// destructor.  They are now `<Class>.<method>`, which no identifier can spell.
TEST(Regression, MethodAndUserFunctionWithTheOldMangledName) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class K { fn w() -> int { return 1; } }
    fn K_w() -> int { return 2; }
    fn main() -> int { k: K = K(); io::println(Str<int>(k.w() + K_w())); return 0; }
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
  auto r = compileAndRun(R"(import ::io;
    class K {
      name: Str;
      fn vtable() -> int { return 10; }
    }

    impl K {
      fn __init__(name: Str) { self.name = name; }
    }
    class D : K {
      fn vtable() -> int { return 20; }
    }

    impl D {
      fn __init__() { __super__("d"); }
    }
    fn K_destroy(k: K) -> Str { return "user " + k.name; }
    fn K_vtable() -> int { return 30; }
    fn D_vtable() -> int { return 40; }
    fn main() -> int {
      k: K = K("k");
      b: K = D();
      io::println(K_destroy(k) + " " + K_destroy(b));
      io::println(Str<int>(k.vtable() + b.vtable() + K_vtable() + D_vtable()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "user k user d\n100\n");
  g.expectNoLeaks("DestructorAndUserFunctionWithTheOldMangledName");
}

// A link name escapes the '.' of `K.w` as `_2E`; a user function named
// `K_2Ew` sanitizes to the same text and must still get its own C symbol.
TEST(Regression, MethodAndUserFunctionWithItsCSpelling) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class K { fn w() -> int { return 1; } fn destroy2() -> int { return 4; } }
    fn K_2Ew() -> int { return 2; }
    fn K_2Edestroy() -> int { return 8; }
    fn K_2Edestroy2() -> int { return 16; }
    fn main() -> int {
      k: K = K();
      io::println(Str<int>(k.w() + K_2Ew() + k.destroy2() + K_2Edestroy() +
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
  fn clone() -> Cell { return Cell(self.v + 1); }
}

impl Cell {
  fn __init__(v: int) { self.v = v; }
}
)");
  auto mainPath = writeProjectFile(dir, "main.pkn", R"(import ::io;
import lib::cell;
fn Cell_clone() -> int { return 100; }
fn Cell_destroy() -> int { return 1000; }
fn main() -> int {
  c = cell::Cell(3);
  d = c.clone();
  io::println(Str<int>(d.v + Cell_clone() + Cell_destroy()));
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
