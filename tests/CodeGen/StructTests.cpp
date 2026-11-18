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
    struct Rec { name: Str; node: shared Node; maybe: shared Node?; t: (int, Str); oi: int?; }
    struct Outer { r: Rec; k: int; }
    class Holder { r: Rec; rs: Rec[2]; }
    fn mk(i: int) -> Rec { return Rec("r" + Str(i), Node(i), None, (i, "t"), None); }
    fn first(xs: view Rec[]) -> view Rec { return xs[0]; }
    fn label(r: view Rec) -> view Str { return r.name; }
    fn bump(r: inout Rec) { r.name = r.name + "!"; r.t = (r.t.0 + 1, r.t.1.clone()); }
    fn main() -> int {
      r = mk(1);
      s = r.clone();              // a copy: shares the objects
      s.name = "s";
      s.node = Node(2);
      s.maybe = Node(3);
      s.oi = 7;
      io::println(r);
      io::println(s);
      o = Outer(r.clone(), 5);
      o.r.name = "o";
      io::println(o.r.name + " " + r.name + " " + Str(o.r.node == r.node));
      h: mut = Holder(mk(4), [mk(5), mk(6)]);
      h.r.name = "h";
      h.rs[1].name = "h6";
      io::println(h.r.name + " " + h.rs[0].name + " " + h.rs[1].name);
      io::println(mk(7).name + " " + Str(mk(8).node.n) + " " + mk(9).t.1);
      xs: Rec[2] = [r.clone(), s.clone()];
      io::println(first(xs).name + " " + label(xs[1]));
      bump(r);
      bump(s);
      io::println(r.name + " " + Str(r.t.0) + " " + xs[0].name);
      t = (r.clone(), 1);
      io::println(t);
      u = t.clone();
      io::println(Str(u == t) + " " + Str(r == s) + " " + Str(r == r));
      r2 = r.clone();
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
      m = l.clone();
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
      fn __init__(name: Str) { self.n = 0; self.name = move name; }
      fn tick() { self.n = self.n + 1; }
      view fn get() -> int { return self.n; }
      view fn label() -> view Str { return self.name; }
      fn rename(s: Str) { self.name = move s; }
      view fn copyWith(n: int) -> Counter { c = Counter("copy"); c.n = n; return c; }
      fn slot() -> inout int { return self.n; }
      static fn named(s: Str) -> Counter { return Counter(move s); }
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
      b: mut = Box(c.clone());    // a copy in the field
      b.c.tick();
      cs: Counter[2] = [c.clone(), c.clone()];
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
    fn show<T: Showable>(x: view T) -> Str { return x.toString(); }
    fn twice<T: ValueType + Copyable>(x: T) -> (T, T) { return (x, x); }
    fn greet<T: Named>(x: view T) -> Str { return x.name(); }
    fn hashOf<T: Hashable>(x: view T) -> bool { return x.hash() == x.hash(); }
    class Box<T> { v: T; view fn get() -> view T { return self.v; } fn set(v: T) { self.v = move v; } }
    impl Box<T> { fn __init__(v: T) { self.v = move v; } }
    fn pick<T>(first: bool, a: T, b: T) -> T { return if first then a else b; }
    fn main() -> int {
      p = Point(1, 2);
      p.shift(1);
      h: mut = Holder(p);
      h.p.y = 3;
      io::println(show(p) + " " + greet(p) + " " + Str(hashOf(p)) + " " + Str(p.hash() == Point(2, 3).hash()));
      pair = twice(p);
      m = Point::origin().moved(2);
      b: mut = Box(Point(1, 2));
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
      a = byValue(p.clone());
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
    impl Cloneable for Node {}
    struct P { node: Node; name: Str; }
    fn mk(i: int) -> P { return P(Node(i), "p" + Str(i)); }
    fn main() -> int {
      total = 0;
      i = 0;
      while i < 3 {
        total = total + mk(i).node.n + mk(i).name.len();
        xs: P[2] = [mk(i), mk(i + 1)];
        xs[0] = xs[1].clone();
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
    import ::memory as mem;
    struct Pair<A, B> { a: A; b: B; }
    impl Pair<A, B> {
      view fn first() -> view A { return self.a; }
      fn swapWith(other: inout Pair<A, B>) { mem::swap(self.a, other.a); }
      fn flipped() -> Pair<B, A> { return Pair<B, A>(self.b.clone(), self.a.clone()); }
    }
    struct Box<T> { v: T; }
    impl Box<T> {
      fn __init__(v: T) { self.v = move v; }
      fn set(v: T) { self.v = move v; }
      view fn get() -> view T { return self.v; }
    }
    impl Hashable for Box<T> { view fn hash() -> int { return 7; } }
    trait Named { view fn name() -> Str; }
    impl Named for Pair<A, B> { view fn name() -> Str { return "pair"; } }
    class Holder<T> { p: Pair<T, Str>; }
    impl Holder<T> { fn __init__(x: T) { self.p = Pair<T, Str>(move x, "h"); } }
    fn sumInts(p: view Pair<int, int>) -> int { return p.a + p.b; }
    fn firstOf<A, B>(p: view Pair<A, B>) -> view A { return p.a; }
    fn mkBox<T>(v: T) -> Box<T> { return Box(move v); }
    fn greet<T: Named>(x: view T) -> Str { return x.name(); }
    fn show<T: Showable>(x: view T) -> Str { return x.toString(); }
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
      h: mut = Holder(9);
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
    impl Cloneable for Node {}
    struct Slot<T> { v: T; label: Str; }
    impl Slot<T> { fn put(v: T) { self.v = move v; } }
    struct Two<A, B> { a: Slot<A>; b: Slot<B>; }
    fn mk(i: int) -> Slot<Node> { return Slot<Node>(Node(i), "s" + Str(i)); }
    fn main() -> int {
      s = mk(1);
      t = s.clone();                       // a copy of the Node
      t.put(Node(2));                      // t's own
      two = Two<Node, Str>(s.clone(), Slot<Str>("x", "y"));
      two.a.label = "two";
      two.b.put("z");
      xs: Slot<Node>[2] = [mk(3), mk(4)];
      xs[0] = xs[1].clone();
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
  fn bump(d: int) { self.a = self.a.clone(); }
}
struct Box<T> { v: T; }
impl Box<T> { fn __init__(v: T) { self.v = move v; } view fn get() -> view T { return self.v; } }
fn mk() -> Pair<int, Str> { return Pair<int, Str>(41, "x"); }
fn boxed<T>(v: T) -> Box<T> { return Box<T>(move v); }
)");
  auto mainPath = writeTempFile(tmpDir, "main.pkn", R"(import ::io;
import lib::gen;
fn main() -> int {
  p = gen::mk();                      // the module's own Pair<int, Str>
  q = gen::Pair<int, Str>(1, "q");    // the same struct
  r = gen::Pair("s", 2.5);            // a new instantiation, here
  p.a = p.a + q.a;
  b: gen::Box<Str> = gen::boxed("b"); // an instantiation of a function
  c = gen::Box(r.clone());
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

// -- A struct's `__deinit__` (#378)

// It runs once where a value of the struct is destroyed: at the end of its
// scope, where it is overwritten, as a temporary, and as a part of a
// struct (after that one's own), an array, a tuple, an optional and an
// object; with the fields still there.
TEST(Struct, DeinitRunsWhereAValueIsDestroyed) {
  LeakGuard g;
  auto r = compileAndRun(R"(
import ::io;
struct Guard { id: int; tag: Str; }
impl Guard {
  fn __deinit__() { io::println("release " + Str(self.id) + self.tag); }
}
struct Pair { a: Guard; b: Guard; }
impl Pair { fn __deinit__() { io::println("pair of " + Str(self.a.id)); } }
class Holder { g: Guard; o: Guard?; }
fn make(i: int) -> Guard { return Guard(i, "!"); }
fn main() -> int {
  {
    let a = Guard(1, "");
    io::println("scope");
  }
  b = make(2);
  b = make(3);
  io::println("overwritten");
  {
    let p = Pair(make(4), make(5));
    let arr = [make(6), make(7)];
    let t = (make(8), 1);
    io::println("parts");
  }
  {
    h: mut = Holder(make(9), make(10));
    h.o = None;
    io::println("none");
    h.g = make(11);
    io::println("object");
  }
  make(12);
  io::println("end");
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "scope\nrelease 1\nrelease 2!\noverwritten\nparts\nrelease 8!\n"
            "release 6!\nrelease 7!\npair of 4\nrelease 4!\nrelease 5!\n"
            "release 10!\nnone\nrelease 9!\nobject\nrelease 11!\n"
            "release 12!\nend\nrelease 3!\n");
  g.expectNoLeaks("DeinitRunsWhereAValueIsDestroyed");
}

// A value that is moved is destroyed by its last owner and not where it
// left: handed to a function (which destroys it when it returns), returned
// (a conditional's branch too), taken apart, taken out by `mem::replace`
// and `mem::take`, given to a task, and bound by a `match` that moves it.
// One moved on some paths only is destroyed where it still is.
TEST(Struct, DeinitFollowsAMovedValue) {
  LeakGuard g;
  auto r = compileAndRun(R"(
import ::io;
import ::memory as mem;
struct Guard { id: int; }
impl Guard { fn __deinit__() { io::println("release " + Str(self.id)); } }
class Holder { g: Guard; o: Guard?; }
enum Slot { Empty, Full(g: Guard, n: int) }
fn make(i: int) -> Guard { let g = Guard(i); return g; }
fn pass(g: Guard) -> Guard { return g; }
fn eat(g: Guard) -> int { io::println("eating " + Str(g.id)); return g.id; }
fn pick(c: bool) -> Guard {
  let a = Guard(21);
  let b = Guard(22);
  return if c then a else b;
}
fn first(t: (Guard, Guard)) -> Guard {
  a, b = move t;
  return a;
}
async fn work(g: Guard) -> int { return g.id; }
fn main() -> int {
  let a = make(1);
  let b = pass(move a);
  eat(move b);
  io::println("eaten");
  let c = make(2);
  if c.id > 5 { eat(move c); }
  let p = pick(False);
  io::println("picked " + Str(p.id));
  let f = first((make(31), make(32)));
  io::println("first " + Str(f.id));
  h: mut = Holder(make(4), make(5));
  let old = mem::replace(h.g, make(6));
  let t = mem::take(h.o);
  io::println("taken " + Str(old.id) + " " + Str(h.o == None));
  let n = await work(make(7));
  io::println("worked " + Str(n));
  let e = Slot::Full(make(8), 2);
  match move e {
    s: Full { io::println("got " + Str(s.g.id)); }
    Empty { }
  }
  io::println("end");
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "eating 1\nrelease 1\neaten\nrelease 21\npicked 22\nrelease 32\n"
            "first 31\ntaken 4 True\nrelease 7\nworked 7\ngot 8\nrelease 8\n"
            "end\nrelease 5\nrelease 4\nrelease 6\nrelease 31\nrelease 22\n"
            "release 2\n");
  g.expectNoLeaks("DeinitFollowsAMovedValue");
}

// A scope's variables go in the reverse of the order they were declared
// in, an object among the values as it was declared among them: what the
// `__deinit__`s do is in one order.
TEST(Struct, DeinitOrderIsTheReverseOfDeclaration) {
  LeakGuard g;
  auto r = compileAndRun(R"(
import ::io;
class Loud { name: Str; fn __deinit__() { io::println("object " + self.name); } }
struct Val { name: Str; }
impl Val { fn __deinit__() { io::println("value " + self.name); } }
fn main() -> int {
  let a = Loud("a");
  let b = Val("b");
  let c = Loud("c");
  let d = Val("d");
  let e = (Loud("e"), 1);
  {
    let f = Val("f");
    let h = Loud("h");
  }
  io::println("end");
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "object h\nvalue f\nend\nobject e\nvalue d\nobject c\n"
                      "value b\nobject a\n");
  g.expectNoLeaks("DeinitOrderIsTheReverseOfDeclaration");
}

// An `inout` arm's binding is the value while the arm uses it: changing it
// destroys nothing, assigning it destroys the value it replaces there, and
// the place has it again after.  A binding that only reads is a look at
// the value where it is.
TEST(Struct, DeinitAndMatchBindings) {
  LeakGuard g;
  auto r = compileAndRun(R"(
import ::io;
struct Guard { id: int; }
impl Guard { fn __deinit__() { io::println("release " + Str(self.id)); } }
class Holder { o: Guard?; }
enum Slot { Empty, Full(g: Guard, n: int) }
fn bump(g: inout Guard) { g.id = g.id + 100; }
fn main() -> int {
  h: mut = Holder(Guard(1));
  match h.o {
    g: inout Guard { g.id = g.id + 1; bump(g); }
    None { }
  }
  io::println("changed");
  match h.o {
    g: inout Guard { g = Guard(2); io::println("assigned"); }
    None { }
  }
  match h.o {
    g: Guard { io::println("see " + Str(g.id)); }
    None { }
  }
  s = Slot::Full(Guard(3), 1);
  match s {
    f: inout Full { f.n = f.n + 1; f.g.id = f.g.id + 1; }
    Empty { }
  }
  match s {
    f: Full { io::println("full " + Str(f.g.id) + " " + Str(f.n)); }
    Empty { }
  }
  s = Slot::Empty;
  io::println("end");
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "changed\nrelease 102\nassigned\nsee 2\nfull 4 2\n"
                      "release 4\nend\nrelease 2\n");
  g.expectNoLeaks("DeinitAndMatchBindings");
}

// A struct with a `__deinit__` is cloned by the `clone` it writes, in a
// value and in an object that holds it; and what its `__deinit__` does to
// what a caller reads is seen by the read's own reference.
TEST(Struct, DeinitClonesAndWhatItReaches) {
  LeakGuard g;
  auto r = compileAndRun(R"(
import ::io;
class Board { top: Str; }
struct Guard { id: int; b: shared mut Board?; }
impl Guard {
  fn __deinit__() {
    io::println("release " + Str(self.id));
    match self.b {
      b: Board { b.top = "replaced by " + Str(self.id); }
      None { }
    }
  }
}
impl Cloneable for Guard {
  view fn clone() -> Self { return Guard(self.id + 100, None); }
}
class Owner { g: Guard; gs: (Guard, int); }
impl Cloneable for Owner {}
fn show(s: view Str, n: int) -> int { io::println(s + " / " + Str(n)); return n; }
fn eat(g: Guard) -> int { return g.id; }
fn mk(b: shared mut Board, i: int) -> Guard { return Guard(i, b); }
fn main() -> int {
  {
    let o = Owner(Guard(1, None), (Guard(2, None), 0));
    let c = o.clone();
    io::println("cloned " + Str(c.g.id) + " " + Str(c.gs.0.id));
  }
  let board: shared mut Board = Board("a board text long enough for the heap");
  let g = mk(board, 7);
  show(board.top, eat(move g));
  show(board.top, mk(board, 8).id);
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "cloned 101 102\nrelease 101\nrelease 102\nrelease 1\nrelease 2\n"
            "release 7\na board text long enough for the heap / 7\n"
            "release 8\nreplaced by 7 / 8\n");
  g.expectNoLeaks("DeinitClonesAndWhatItReaches");
}

// A part read out of a temporary leaves it: the variable, the field, the
// element or the arm it goes to has the value, which is destroyed there,
// and the temporary (a value, or an object nothing else holds) lets go of
// the part without its `__deinit__`.
TEST(Struct, DeinitFollowsAPartThatLeavesATemporary) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
import ::io;
struct G { id: int; }
impl G { fn __deinit__() { io::println("release " + Str(self.id)); } }
struct H { g: G; n: int; }
struct O { o: G?; n: int; }
class C { f: G; }
enum Slot { Empty, Full(g: G, n: int) }
fn mkh(i: int) -> H { return H(G(i), i); }
fn mko(i: int) -> O { return O(G(i), i); }
fn arr(i: int) -> G[2] { return [G(i), G(i + 1)]; }
fn tup(i: int) -> (G, int) { return (G(i), i); }
async fn task(i: int) -> H { return H(G(i), i); }
fn main() -> int {
  x = G(1);
  x = mkh(2).g;                       io::println("assigned " + Str(x.id));
  xs = [G(3), G(4)];
  xs[0] = mkh(5).g;                   io::println("element " + Str(xs[0].id));
  let c: mut = C(G(6));
  c.f = mkh(7).g;                     io::println("field " + Str(c.f.id));
  let o: G? = mkh(8).g;               io::println("optional " + Str(o == None));
  let s = Slot::Full(mkh(9).g, 1);    io::println("variant");
  let ys = [mkh(10).g, mkh(11).g];    io::println("array " + Str(ys[1].id));
  let e = arr(12)[1];                 io::println("elem " + Str(e.id));
  let t = tup(14).0;                  io::println("tuple " + Str(t.id));
  let a = (await task(15)).g;         io::println("awaited " + Str(a.id));
  let q = C(G(17)).f;                 io::println("object " + Str(q.id));
  match mko(16).o {
    g: G { io::println("arm " + Str(g.id)); }
    None { }
  }
  io::println("end");
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "release 1\nassigned 2\nrelease 3\nelement 5\nrelease 6\n"
            "field 7\noptional False\nvariant\narray 11\nrelease 12\n"
            "elem 13\ntuple 14\nawaited 15\nobject 17\narm 16\n"
            "release 16\nend\nrelease 17\nrelease 15\nrelease 14\n"
            "release 13\nrelease 10\nrelease 11\nrelease 9\nrelease 8\n"
            "release 7\nrelease 5\nrelease 4\nrelease 2\n");
  g.expectNoLeaks("DeinitFollowsAPartThatLeavesATemporary");
}

// A value that is replaced is destroyed once the new one is in its place,
// from a slot of its own: its `__deinit__` is the program's code, and may
// reach the storage the value was in through another reference, to clear
// it, to store there or to take what is there.  An `inout` arm's
// write-back does the same with what an alias stored meanwhile.
TEST(Struct, DeinitMayReachTheStorageItsValueWasIn) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
import ::io;
import ::memory as mem;
class Reg { slot: G?; spare: G?; }
struct G { r: shared mut Reg; id: int; }
impl G {
  fn __deinit__() {
    io::println("deinit " + Str(self.id));
    if self.id == 1 {
      self.r.slot = None;
      io::println("still " + Str(self.id));
    }
    if self.id == 2 {
      self.r.slot = G(self.r, 3);
      io::println("still " + Str(self.id));
    }
    if self.id == 4 {
      self.r.spare = mem::take(self.r.slot);
      io::println("still " + Str(self.id));
    }
  }
}
fn main() -> int {
  let r: shared mut = Reg(None, None);
  r.slot = G(r, 1);
  r.slot = None;
  io::println("cleared");
  r.slot = G(r, 2);
  r.slot = None;
  io::println("slot holds one: " + Str(r.slot != None));
  r.slot = G(r, 4);
  io::println("replaced");
  r.slot = None;
  io::println("spare holds none: " + Str(r.spare == None));
  r.slot = G(r, 5);
  let other = r;
  match r.slot {
    g: inout G {
      other.slot = G(other, 6);
      io::println("in the arm");
      g.id = 50;
    }
    None { }
  }
  io::println("written back");
  r.slot = None;
  io::println("end");
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "deinit 1\nstill 1\ncleared\ndeinit 2\nstill 2\n"
                      "slot holds one: True\ndeinit 3\nreplaced\ndeinit 4\n"
                      "still 4\nspare holds none: True\nin the arm\ndeinit 6\n"
                      "written back\ndeinit 50\nend\n");
  g.expectNoLeaks("DeinitMayReachTheStorageItsValueWasIn");
}

// A `match` that only looks at a value that runs a `__deinit__` destroys
// nothing: a part of what a call lent (a `view` result) stays in its
// object, and what the subject was read from on the way (an object a call
// made, the value a `view` result is in) lasts until the match ends.  A
// branch of a conditional subject that leaves its temporary is the match's.
TEST(Struct, DeinitWaitsForAMatchThatLooksAtIt) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
import ::io;
struct Lock { name: Str; }
impl Lock { fn __deinit__() { io::println("unlock " + self.name); } }
struct H { o: Lock?; n: int; }
class Session { held: Lock?; }
impl Session { view fn me() -> view Session { return self; } }
fn mkh(s: Str) -> H { return H(Lock(move s), 0); }
fn mks(s: Str) -> shared mut Session { return Session(Lock(move s)); }
fn part(h: view H) -> view Lock? { return h.o; }
fn main() -> int {
  let s = Session(Lock("a"));
  match s.me().held { l: Lock { io::println("looking at " + l.name); } None { } }
  io::println("still held: " + Str(s.held != None));
  let c = True;
  match (if c then mkh("b").o else mkh("c").o) {
    l: Lock { io::println("holding " + l.name); }
    None { }
  }
  match mks("d").held { l: Lock { io::println("holding " + l.name); } None { } }
  match part(mkh("e")) { l: Lock { io::println("holding " + l.name); } None { } }
  io::println("end");
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "looking at a\nstill held: True\nholding b\nunlock b\n"
            "holding d\nunlock d\nholding e\nunlock e\nend\nunlock a\n");
  g.expectNoLeaks("DeinitWaitsForAMatchThatLooksAtIt");
}
