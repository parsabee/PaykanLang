// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Codegen / E2E tests for structs (docs/language/11-structs.md): values held
// in place with named fields, assigned in place, with methods from an `impl`
// block whose `self` is the receiver's storage.  Every runtime test runs
// under the tracking allocator and asserts zero live heap blocks afterwards,
// so each case checks both the observable output and that every reference a
// struct holds is released exactly once.

#include "CodeGenTestUtils.h"
#include "Names.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

using namespace paykan::test;

// The layout kind a struct's layout marks a nested struct with.
static_assert(int(paykan::names::kNativeKindStruct) == int(PAYKAN_KIND_STRUCT),
              "struct layout kind ABI drift");

namespace {

std::string writeTempFile(const std::filesystem::path &dir,
                          const std::string &relPath,
                          const std::string &content) {
  auto full = dir / relPath;
  std::filesystem::create_directories(full.parent_path());
  std::ofstream ofs(full);
  ofs << content;
  return full.string();
}

} // namespace

// -- Fields, copies and printing

TEST(Struct, FieldsAreReadAndAssignedInPlace) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    struct Point { x: int; y: int; }
    fn main() -> int {
      p = Point(1, 2);
      p.x = 5;
      p.y = p.x * 2;
      q = p;            // a copy
      q.x = 0;
      io::println(p);
      io::println(q);
      io::println(Str(p == Point(5, 10)) + " " + Str(p == q) + " " + Str(p != q));
      return p.x + p.y + q.x + q.y;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "Point(x: 5, y: 10)\nPoint(x: 0, y: 10)\nTrue False True\n");
  EXPECT_EQ(r.ExitCode, 25);
  g.expectNoLeaks("FieldsAreReadAndAssignedInPlace");
}

TEST(Struct, EveryFieldTypeAndCopiesShareReferences) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Node { n: int; view fn toString() -> Str { return "N" + Str(self.n); } }
    struct Rec { name: Str; node: Node; maybe: Node?; t: (int, Str); oi: int?; }
    struct Outer { r: Rec; k: int; }
    class Holder { r: Rec; rs: Rec[2]; }
    fn mk(i: int) -> Rec { return Rec("r" + Str(i), Node(i), None, (i, "t"), None); }
    fn first(xs: view Rec[]) -> view Rec { return xs[0]; }
    fn label(r: view Rec) -> view Str { return r.name; }
    fn bump(r: inout Rec) { r.name = r.name + "!"; r.t = (r.t.0 + 1, r.t.1); }
    fn main() -> int {
      r = mk(1);
      s = r;                      // a copy: shares the objects
      s.name = "s";
      s.node = Node(2);
      s.maybe = Node(3);
      s.oi = 7;
      io::println(r);
      io::println(s);
      o = Outer(r, 5);
      o.r.name = "o";
      io::println(o.r.name + " " + r.name + " " + Str(o.r.node == r.node));
      h = Holder(mk(4), [mk(5), mk(6)]);
      h.r.name = "h";
      h.rs[1].name = "h6";
      io::println(h.r.name + " " + h.rs[0].name + " " + h.rs[1].name);
      io::println(mk(7).name + " " + Str(mk(8).node.n) + " " + mk(9).t.1);
      xs: Rec[2] = [r, s];
      io::println(first(xs).name + " " + label(xs[1]));
      bump(r);
      bump(s);
      io::println(r.name + " " + Str(r.t.0) + " " + xs[0].name);
      t = (r, 1);
      io::println(t);
      u = t;
      io::println(Str(u == t) + " " + Str(r == s) + " " + Str(r == r));
      r2 = r;
      r2.node = Node(1);          // another object: unequal by identity
      io::println(Str(r2 == r));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "Rec(name: r1, node: N1, maybe: None, t: (1, t), oi: None)\n"
            "Rec(name: s, node: N2, maybe: N3, t: (1, t), oi: 7)\n"
            "o r1 True\n"
            "h r5 h6\n"
            "r7 8 t\n"
            "r1 s\n"
            "r1! 2 r1\n"
            "(Rec(name: r1!, node: N1, maybe: None, t: (2, t), oi: None), 1)\n"
            "True False True\n"
            "False\n");
  EXPECT_EQ(r.ExitCode, 0);
  g.expectNoLeaks("EveryFieldTypeAndCopiesShareReferences");
}

TEST(Struct, NestedStructsAndTuplesPrintAndCompareInPlace) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    struct Point { x: int; y: int; }
    struct Line { a: Point; b: Point; label: Str; }
    struct Tagged { t: (Point, Str); f: float; c: char; b: bool; }
    fn main() -> int {
      l = Line(Point(1, 2), Point(3, 4), "l");
      l.a.x = 9;
      m = l;
      m.b.y = 0;
      io::println(l);
      io::println(m);
      io::println(Str(l == m) + " " + Str(l.a == m.a) + " " + Str(l == Line(Point(9, 2), Point(3, 4), "l")));
      g = Tagged((Point(1, 1), "p"), 2.5, 'c', True);
      io::println(g);
      io::println(Str(g == Tagged((Point(1, 1), "p"), 2.5, 'c', True)) + " " + Str(g.t.0.x));
      return l.b.y + m.b.y;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "Line(a: Point(x: 9, y: 2), b: Point(x: 3, y: 4), label: l)\n"
            "Line(a: Point(x: 9, y: 2), b: Point(x: 3, y: 0), label: l)\n"
            "False True True\n"
            "Tagged(t: (Point(x: 1, y: 1), p), f: 2.5, c: c, b: True)\n"
            "True 1\n");
  EXPECT_EQ(r.ExitCode, 4);
  g.expectNoLeaks("NestedStructsAndTuplesPrintAndCompareInPlace");
}

// -- Methods

TEST(Struct, MethodsChangeTheReceiverInPlace) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    struct Counter { n: int; name: Str; }
    impl Counter {
      fn __init__(name: Str) { self.n = 0; self.name = name; }
      fn tick() { self.n = self.n + 1; }
      view fn get() -> int { return self.n; }
      view fn label() -> view Str { return self.name; }
      fn rename(s: Str) { self.name = s; }
      view fn copyWith(n: int) -> Counter { c = Counter("copy"); c.n = n; return c; }
      fn slot() -> inout int { return self.n; }
      static fn named(s: Str) -> Counter { return Counter(s); }
      view fn same(other: view Counter) -> bool { return self.n == other.n; }
      fn tickTwice() { self.tick(); self.tick(); }
    }
    class Box { c: Counter; }
    fn mk() -> Counter { return Counter("tmp"); }
    fn addOne(n: inout int) { n = n + 1; }
    fn main() -> int {
      c = Counter::named("a");
      c.tick();
      c.tickTwice();
      b = Box(c);                 // a copy in the field
      b.c.tick();
      cs: Counter[2] = [c, c];
      cs[1].tick();
      cs[1].rename("second");
      addOne(c.slot());
      k: inout int = b.c.slot();
      k = k + 10;
      mk().tick();                // a temporary: changed, then dropped
      io::println(Str(c.get()) + " " + Str(b.c.get()) + " " + Str(cs[0].get()) + " " +
              Str(cs[1].get()) + " " + cs[1].label() + " " + c.label() + " " +
              Str(mk().get()));
      d = c.copyWith(9);
      io::println(Str(d.get()) + Str(c.get()) + Str(c.same(d)) + Str(c.same(c)));
      v: view = c;
      io::println(Str(v.get()) + v.label());
      return c.get();
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "4 14 3 4 second a 0\n94FalseTrue\n4a\n");
  EXPECT_EQ(r.ExitCode, 4);
  g.expectNoLeaks("MethodsChangeTheReceiverInPlace");
}

TEST(Struct, ImplBlocksTraitsAndGenerics) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    struct Point { x: int; y: int; }
    class Holder { p: Point; }
    impl Point {
      fn __init__(x: int, y: int) { self.x = x; self.y = y; }
      static fn origin() -> Point { return Point(0, 0); }
      view fn sum() -> int { return self.x + self.y; }
      fn shift(d: int) { self.x = self.x + d; self.y = self.y + d; }
      view fn moved(d: int) -> Point { q = self; q.shift(d); return q; }
    }
    impl Hashable for Point { view fn hash() -> int { return (self.x * 31 + self.y).hash(); } }
    trait Named { view fn name() -> Str; }
    impl Named for Point { view fn name() -> Str { return "Point"; } }
    fn show<T: Show>(x: view T) -> Str { return x.toString(); }
    fn twice<T: ValueType>(x: T) -> (T, T) { return (x, x); }
    fn greet<T: Named>(x: view T) -> Str { return x.name(); }
    fn hashOf<T: Hashable>(x: view T) -> bool { return x.hash() == x.hash(); }
    class Box<T> { v: T; view fn get() -> view T { return self.v; } fn set(v: T) { self.v = v; } }
    impl Box<T> { fn __init__(v: T) { self.v = v; } }
    fn pick<T>(first: bool, a: T, b: T) -> T { return if first then a else b; }
    fn main() -> int {
      p = Point(1, 2);
      p.shift(1);
      h = Holder(p);
      h.p.y = 3;
      io::println(show(p) + " " + greet(p) + " " + Str(hashOf(p)) + " " + Str(p.hash() == Point(2, 3).hash()));
      pair = twice(p);
      m = Point::origin().moved(2);
      b = Box(Point(1, 2));
      b.set(Point(3, 4));
      q: Point = b.get();
      io::println(show(q) + " " + show(b.get()) + " " + Str(pick(True, Point(1, 1), Point(2, 2)).sum()));
      return p.sum() + pair.1.x + m.x + h.p.y;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Point(x: 2, y: 3) Point True True\n"
                      "Point(x: 3, y: 4) Point(x: 3, y: 4) 2\n");
  EXPECT_EQ(r.ExitCode, 12);
  g.expectNoLeaks("ImplBlocksTraitsAndGenerics");
}

// -- Parameter modes and `let`

TEST(Struct, ParameterModesAndLet) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    struct P { x: int; s: Str; }
    fn byValue(p: P) -> int { p.x = 100; return p.x; }
    fn byView(p: view P) -> int { return p.x + p.s.len(); }
    fn byInout(p: inout P) { p.x = p.x + 1; p.s = p.s + "!"; }
    fn replace(p: inout P) { p = P(0, "new"); }
    fn main() -> int {
      p = P(1, "a");
      let k = P(7, "k");
      a = byValue(p);
      b = byView(p);
      byInout(p);
      io::println(Str(a) + " " + Str(b) + " " + p.s + Str(p.x) + " " + k.s);
      replace(p);
      v: view = p;
      io::println(v.s + " " + Str(byView(k)));
      return p.x + k.x;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "100 2 a!2 k\nnew 8\n");
  EXPECT_EQ(r.ExitCode, 7);
  g.expectNoLeaks("ParameterModesAndLet");
}

TEST(Struct, FieldsOfTemporariesAndElementsAreReleased) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Node { n: int; }
    struct P { node: Node; name: Str; }
    fn mk(i: int) -> P { return P(Node(i), "p" + Str(i)); }
    fn main() -> int {
      total = 0;
      i = 0;
      while i < 3 {
        total = total + mk(i).node.n + mk(i).name.len();
        xs: P[2] = [mk(i), mk(i + 1)];
        xs[0] = xs[1];
        total = total + xs[0].node.n;
        i = i + 1;
      }
      return total;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 15);
  g.expectNoLeaks("FieldsOfTemporariesAndElementsAreReleased");
}

// -- Generic structs

TEST(Struct, GenericStructsInstantiateLikeClasses) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    struct Pair<A, B> { a: A; b: B; }
    impl Pair<A, B> {
      view fn first() -> view A { return self.a; }
      fn swapWith(other: inout Pair<A, B>) { t = self.a; self.a = other.a; other.a = t; }
      fn flipped() -> Pair<B, A> { return Pair<B, A>(self.b, self.a); }
    }
    struct Box<T> { v: T; }
    impl Box<T> {
      fn __init__(v: T) { self.v = v; }
      fn set(v: T) { self.v = v; }
      view fn get() -> view T { return self.v; }
    }
    impl Hashable for Box<T> { view fn hash() -> int { return 7; } }
    trait Named { view fn name() -> Str; }
    impl Named for Pair<A, B> { view fn name() -> Str { return "pair"; } }
    class Holder<T> { p: Pair<T, Str>; }
    impl Holder<T> { fn __init__(x: T) { self.p = Pair<T, Str>(x, "h"); } }
    fn sumInts(p: view Pair<int, int>) -> int { return p.a + p.b; }
    fn firstOf<A, B>(p: view Pair<A, B>) -> view A { return p.a; }
    fn mkBox<T>(v: T) -> Box<T> { return Box(v); }
    fn greet<T: Named>(x: view T) -> Str { return x.name(); }
    fn show<T: Show>(x: view T) -> Str { return x.toString(); }
    fn main() -> int {
      p = Pair(1, "one");               // inferred Pair<int, Str>
      q: Pair<int, Str> = Pair<int, Str>(2, "two");
      p.swapWith(q);
      f = p.flipped();
      io::println(p);
      io::println(f);
      io::println(show(q) + " " + greet(p) + " " + Str(p.first()) + Str(firstOf(f).len()));
      b = Box(Pair(3, 4));
      b.set(Pair(5, 6));
      io::println(Str(sumInts(b.get())) + " " + Str(b.hash()) + " " + show(mkBox("s")));
      h = Holder(9);
      h.p.a = 10;
      bs: Box<int>[2] = [Box(1), Box(2)];
      bs[1].v = 7;
      t = (Pair(1.5, 'c'), Box(True));
      io::println(Str(h.p.a) + h.p.b + " " + Str(bs[0].v + bs[1].v) + " " + Str(t.0.b) + Str(t.1.v));
      io::println(Str(p == Pair(2, "two")) + Str(b == Box(Pair(5, 6))) + Str(Box(1) == Box(2)));
      return p.a + q.a;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Pair<int, Str>(a: 2, b: one)\n"
                      "Pair<Str, int>(a: one, b: 2)\n"
                      "Pair<int, Str>(a: 1, b: two) pair 23\n"
                      "11 7 Box<Str>(v: s)\n"
                      "10h 8 cTrue\n"
                      "FalseTrueFalse\n");
  EXPECT_EQ(r.ExitCode, 3);
  g.expectNoLeaks("GenericStructsInstantiateLikeClasses");
}

TEST(Struct, GenericStructsHoldingReferencesAreReleased) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Node { n: int; view fn toString() -> Str { return "N" + Str(self.n); } }
    struct Slot<T> { v: T; label: Str; }
    impl Slot<T> { fn put(v: T) { self.v = v; } }
    struct Two<A, B> { a: Slot<A>; b: Slot<B>; }
    fn mk(i: int) -> Slot<Node> { return Slot<Node>(Node(i), "s" + Str(i)); }
    fn main() -> int {
      s = mk(1);
      t = s;                               // the same Node
      t.put(Node(2));                      // t's own
      two = Two<Node, Str>(s, Slot<Str>("x", "y"));
      two.a.label = "two";
      two.b.put("z");
      xs: Slot<Node>[2] = [mk(3), mk(4)];
      xs[0] = xs[1];
      io::println(s);
      io::println(t);
      io::println(two);
      io::println(Str(xs[0].v.n + xs[1].v.n) + " " + Str(mk(5).v.n) + " " + mk(6).label);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Slot<Node>(v: N1, label: s1)\n"
                      "Slot<Node>(v: N2, label: s1)\n"
                      "Two<Node, Str>(a: Slot<Node>(v: N1, label: two), b: "
                      "Slot<Str>(v: z, label: y))\n"
                      "8 5 s6\n");
  EXPECT_EQ(r.ExitCode, 0);
  g.expectNoLeaks("GenericStructsHoldingReferencesAreReleased");
}

TEST(Struct, GenericStructsImportedFromAnotherModule) {
  LeakGuard g;
  auto tmpDir = paykan::test::tempDir() / "pkn_generic_struct_module_test";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);
  writeTempFile(tmpDir, "lib/gen.pkn", R"(
struct Pair<A, B> { a: A; b: B; }
impl Pair<A, B> {
  view fn first() -> view A { return self.a; }
  fn bump(d: int) { self.a = self.a; }
}
struct Box<T> { v: T; }
impl Box<T> { fn __init__(v: T) { self.v = v; } view fn get() -> view T { return self.v; } }
fn mk() -> Pair<int, Str> { return Pair<int, Str>(41, "x"); }
fn boxed<T>(v: T) -> Box<T> { return Box<T>(v); }
)");
  auto mainPath = writeTempFile(tmpDir, "main.pkn", R"(import ::io;
import lib::gen;
fn main() -> int {
  p = gen::mk();                      // the module's own Pair<int, Str>
  q = gen::Pair<int, Str>(1, "q");    // the same struct
  r = gen::Pair("s", 2.5);            // a new instantiation, here
  p.a = p.a + q.a;
  b: gen::Box<Str> = gen::boxed("b"); // an instantiation of a function
  c = gen::Box(r);
  io::println(p.toString() + " " + r.first() + " " + b.get() + " " + c.get().b.toString() + " " + Str(p == q));
  return p.a;
}
)");
  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Pair<int, Str>(a: 42, b: x) s b 2.5 False\n");
  EXPECT_EQ(r.ExitCode, 42);
  g.expectNoLeaks("GenericStructsImportedFromAnotherModule");
  std::filesystem::remove_all(tmpDir);
}

// -- Modules

TEST(Struct, ImportedFromAnotherModule) {
  LeakGuard g;
  auto tmpDir = paykan::test::tempDir() / "pkn_struct_module_test";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);
  writeTempFile(tmpDir, "geo/shapes.pkn", R"(
struct Point { x: int; y: int; }
impl Point {
  fn __init__(x: int, y: int) { self.x = x; self.y = y; }
  static fn origin() -> Point { return Point(0, 0); }
  view fn sum() -> int { return self.x + self.y; }
  fn shift(d: int) { self.x = self.x + d; }
}
struct Named { p: Point; name: Str; }
class Shape { at: Point; view fn where() -> Point { return self.at; } }
fn make(n: int) -> Named { return Named(Point(n, n), "n" + Str(n)); }
)");
  auto mainPath = writeTempFile(tmpDir, "main.pkn", R"(import ::io;
import geo::shapes;
fn main() -> int {
  p = shapes::Point(1, 2);
  p.shift(3);
  o = shapes::Point::origin();
  n = shapes::make(4);
  n.p.shift(1);
  s = shapes::Shape(p);
  q: shapes::Point = s.where();
  io::println(p.toString() + " " + Str(p.sum()) + " " + Str(o == p) + " " + n.name + Str(n.p.x) + " " + Str(q.sum()));
  return 0;
}
)");
  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Point(x: 4, y: 2) 6 False n45 6\n");
  EXPECT_EQ(r.ExitCode, 0);
  g.expectNoLeaks("ImportedFromAnotherModule");
  std::filesystem::remove_all(tmpDir);
}
