// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen tests: generic classes and functions run end to end through the
// JIT, and every allocation is released (instantiations are ordinary
// classes, so ARC applies to them unchanged).

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

using namespace paykan::test;

namespace {

const char *kBox = R"(
  class Box<T> {
    v: T;
    fn get() -> T { return self.v; }
    fn set(v: T) { self.v = v; }
  }

  impl Box<T> {
    fn __init__(v: T) { self.v = v; }
  }
)";

std::string withMain(const std::string &decls, const std::string &body) {
  return decls + "import ::io; \nfn main() -> int {\n" + body +
         "\n  return 0;\n}\n";
}

} // namespace

TEST(Generics, BoxStrGetSet) {
  LeakGuard g;
  auto r = compileAndRun(withMain(kBox, R"(
    b: Box<Str> = Box<Str>("hello");
    io::println(b.get());
    b.set("world");
    io::println(b.get());
    c = Box<Str>(b.get() + "!");
    io::println(c.get());
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "hello\nworld\nworld!\n");
  g.expectNoLeaks("BoxStrGetSet");
}

TEST(Generics, BoxIntAndBoxStrCoexist) {
  LeakGuard g;
  auto r = compileAndRun(withMain(kBox, R"(
    a: Box<int> = Box<int>(3);
    b: Box<Str> = Box<Str>("s");
    a.set(a.get() * 2);
    io::println(Str<int>(a.get()));
    io::println(b.get());
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "6\ns\n");
  g.expectNoLeaks("BoxIntAndBoxStrCoexist");
}

TEST(Generics, PairStrInt) {
  LeakGuard g;
  auto r = compileAndRun(withMain(R"(
    class Pair<K, V> {
      k: K;
      v: V;
      fn key() -> K { return self.k; }
      fn value() -> V { return self.v; }
      view fn toString() -> Str { return self.k + "=" + Str<int>(self.v); }
    }

    impl Pair<K, V> {
      fn __init__(k: K, v: V) { self.k = k; self.v = v; }
    }
  )",
                                  R"(
    p: Pair<Str, int> = Pair<Str, int>("age", 42);
    io::println(p.key() + ":" + Str<int>(p.value()));
    io::println(p.toString());
    io::println(p);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "age:42\nage=42\nage=42\n");
  g.expectNoLeaks("PairStrInt");
}

TEST(Generics, BoxOfBoxInt) {
  LeakGuard g;
  auto r = compileAndRun(withMain(kBox, R"(
    inner: Box<int> = Box<int>(5);
    outer: Box<Box<int>> = Box<Box<int>>(inner);
    io::println(Str<int>(outer.get().get()));
    outer.get().set(9);
    io::println(Str<int>(inner.get()));
    fresh = Box<Box<int>>(Box<int>(1));
    io::println(Str<int>(fresh.get().get()));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "5\n9\n1\n");
  g.expectNoLeaks("BoxOfBoxInt");
}

// Stack<T> over a fixed-capacity T[4] field and a count, with Str and int
// elements: the methods write and read the field in place, and an element
// still on the stack is released with it.
TEST(Generics, StackOfStrAndInt) {
  LeakGuard g;
  auto r = compileAndRun(withMain(R"(
    class Stack<T> {
      items: T[4];
      n: int;
      fn push(v: T) {
        self.items[self.n] = v;
        self.n = self.n + 1;
      }
      fn pop() -> T {
        self.n = self.n - 1;
        return self.items[self.n];
      }
      view fn size() -> int { return self.n; }
      view fn empty() -> bool { return self.n == 0; }
    }

    impl Stack<T> {
      fn __init__(fill: T) {
        self.items = [fill; 4];
        self.n = 0;
      }
    }
  )",
                                  R"(
    s: Stack<Str> = Stack<Str>("");
    s.push("a");
    s.push("b");
    s.push("c");
    io::println(Str<int>(s.size()));
    while !s.empty() { io::println(s.pop()); }
    n: Stack<int> = Stack<int>(0);
    n.push(1);
    n.push(2);
    total: int = 0;
    while !n.empty() { total = total + n.pop(); }
    io::println(Str<int>(total));
    s.push("left");               // released with the stack
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3\nc\nb\na\n3\n");
  g.expectNoLeaks("StackOfStrAndInt");
}

TEST(Generics, GenericFirstOnIntAndStrArrays) {
  LeakGuard g;
  auto r = compileAndRun(withMain(R"(
    fn first<T>(xs: view T[]) -> view T { return xs[0]; }
    fn last<T>(xs: view T[]) -> view T { return xs[xs.len() - 1]; }
  )",
                                  R"(
    nums: int[3] = [10, 20, 30];
    io::println(Str<int>(first(nums)));
    letters: Str[2] = ["x", "y"];
    io::println(first(letters));
    words: Str[3] = ["p", "q", "r"];
    io::println(last(words));
    io::println(Str<int>(last<int>([7])));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "10\nx\nr\n7\n");
  g.expectNoLeaks("GenericFirstOnIntAndStrArrays");
}

TEST(Generics, GenericFunctionOverInstantiation) {
  LeakGuard g;
  auto r = compileAndRun(withMain(std::string(kBox) + R"(
    fn unbox<T>(b: Box<T>) -> T { return b.get(); }
    fn rebox<T>(b: Box<T>) -> Box<T> { return Box<T>(b.get()); }
  )",
                                  R"(
    io::println(Str<int>(unbox(Box<int>(4))));
    io::println(unbox(rebox(Box<Str>("z"))));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "4\nz\n");
  g.expectNoLeaks("GenericFunctionOverInstantiation");
}

TEST(Generics, MatchOnInstantiation) {
  LeakGuard g;
  auto r = compileAndRun(withMain(std::string(kBox) + R"(
    fn describe(o: Obj) -> Str {
      match o {
        b: Box<int> { return "Box<int> holding " + Str<int>(b.get()); }
        Box<Str> { return "Box<Str>"; }
        _ { return "other"; }
      }
    }
  )",
                                  R"(
    io::println(describe(Box<int>(3)));
    io::println(describe(Box<Str>("s")));
    io::println(describe("plain"));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Box<int> holding 3\nBox<Str>\nother\n");
  g.expectNoLeaks("MatchOnInstantiation");
}

TEST(Generics, InstantiationTransfer) {
  LeakGuard g;
  auto r = compileAndRun(withMain(std::string(kBox) + R"(
    fn consume(b: Box<Str>) -> Str { return b.get(); }
  )",
                                  R"(
    a: Box<Str> = Box<Str>("moved");
    b: Box<Str> = a;
    io::println(b.get());
    io::println(consume(b));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "moved\nmoved\n");
  g.expectNoLeaks("InstantiationTransfer");
}

TEST(Generics, EqualityDispatchesToEquals) {
  LeakGuard g;
  auto r = compileAndRun(withMain(R"(
    class Box<T> {
      v: T;
      fn get() -> T { return self.v; }
      view fn equals(other: Obj) -> bool {
        match other {
          o: Box<T> { return o.get() == self.v; }
          _ { return False; }
        }
      }
    }

    impl Box<T> {
      fn __init__(v: T) { self.v = v; }
    }
  )",
                                  R"(
    a: Box<int> = Box<int>(1);
    b: Box<int> = Box<int>(1);
    c: Box<int> = Box<int>(2);
    io::println(Str<bool>(a == b));
    io::println(Str<bool>(a == c));
    io::println(Str<bool>(a != c));
    s: Box<Str> = Box<Str>("x");
    io::println(Str<bool>(s == Box<Str>("x")));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\nFalse\nTrue\nTrue\n");
  g.expectNoLeaks("EqualityDispatchesToEquals");
}

TEST(Generics, ArrayOfInstantiationsAndFieldOfInstantiation) {
  LeakGuard g;
  auto r = compileAndRun(withMain(std::string(kBox) + R"(
    class Holder {
      b: Box<int>;
      fn get() -> Box<int> { return self.b; }
    }

    impl Holder {
      fn __init__(n: int) { self.b = Box<int>(n); }
    }
  )",
                                  R"(
    arr: Box<int>[3] = [Box<int>(1), Box<int>(2), Box<int>(0)];
    arr[2] = Box<int>(3);
    total: int = 0;
    i: int = 0;
    while i < arr.len() { total = total + arr[i].get(); i = i + 1; }
    io::println(Str<int>(total));
    h: Holder = Holder(40);
    io::println(Str<int>(h.get().get() + 2));
    names: Box<Str>[1] = [Box<Str>("n")];
    io::println(names[0].get());
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "6\n42\nn\n");
  g.expectNoLeaks("ArrayOfInstantiationsAndFieldOfInstantiation");
}

TEST(Generics, ConstructorInference) {
  LeakGuard g;
  auto r = compileAndRun(withMain(kBox, R"(
    b = Box(5);
    io::println(Str<int>(b.get()));
    s = Box("inferred");
    io::println(s.get());
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "5\ninferred\n");
  g.expectNoLeaks("ConstructorInference");
}

// Instantiations are merged into the class list so that a superclass is
// emitted before its (instantiated) subclass — `__super__(n)` must find
// Base___init__ — and a constructed class before the class constructing it.
TEST(Generics, GenericWithConcreteSuperclassVirtualDispatch) {
  LeakGuard g;
  auto r = compileAndRun(withMain(R"(
    class Base {
      n: int;
      fn name() -> Str { return "base"; }
    }

    impl Base {
      fn __init__(n: int) { self.n = n; }
    }
    class Wrap<T> : Base {
      v: T;
      fn name() -> Str { return "wrap"; }
      fn get() -> T { return self.v; }
    }

    impl Wrap<T> {
      fn __init__(v: T, n: int) { __super__(n); self.v = v; }
    }
  )",
                                  R"(
    w: Wrap<Str> = Wrap<Str>("payload", 5);
    b: Base = w;
    io::println(b.name() + " " + Str<int>(b.n) + " " + w.get());
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "wrap 5 payload\n");
  g.expectNoLeaks("GenericWithConcreteSuperclassVirtualDispatch");
}

// An exported instantiation crosses the module boundary as a concrete class.
TEST(Generics, ExportedInstantiationAcrossModules) {
  LeakGuard g;
  auto dir = paykan::test::tempDir() / "pkn_cg_generics_exp";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / "lib");
  {
    std::ofstream lib(dir / "lib" / "gen.pkn");
    lib << R"(
class Box<T> { v: T; fn get() -> T { return self.v; } }
impl Box<T> { fn __init__(v: T) { self.v = v; } }
fn mk() -> Box<int> { return Box<int>(41); }
)";
  }
  auto mainPath = (dir / "main.pkn").string();
  {
    std::ofstream mainFile(mainPath);
    mainFile << R"(
import lib::gen;
fn main() -> int { b = gen::mk(); return b.get() + 1; }
)";
  }
  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 42);
  std::filesystem::remove_all(dir);
  g.expectNoLeaks("ExportedInstantiationAcrossModules");
}

// -- Interaction with tuples (#4) and optionals (#5)

TEST(GenericsTypes, TupleFieldLiteralIndexAndDestructuring) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Pair<A, B> {
      p: (A, B);
      fn first() -> A { x, _ = self.p; return x; }
      fn second() -> B { return self.p.1; }
      fn swap() -> (B, A) { return (self.p.1, self.p.0); }
    }

    impl Pair<A, B> {
      fn __init__(a: A, b: B) { self.p = (a, b); }
    }
    fn firstOf<A, B>(p: (A, B)) -> A { return p.0; }
    fn dup<T>(x: T) -> (T, T) { return (x, x); }
    fn main() -> int {
      q = Pair<Str, int>("a", 1);
      io::println(q.first());
      io::println(Str<int>(q.second()));
      io::println(q.swap());
      r: Pair<int, Str> = Pair<int, Str>(2, "b");
      io::println(r.swap());
      io::println(Str<int>(firstOf((7, "x"))));
      a, b = dup("z");
      io::println(a + b);
      io::println(dup((1, "q")));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "a\n1\n(1, a)\n(b, 2)\n7\nzz\n((1, q), (1, q))\n");
  g.expectNoLeaks("TupleFieldLiteralIndexAndDestructuring");
}

TEST(GenericsTypes, OptionalFieldsAndUnwrapInTemplates) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Node { v: int; }
    impl Node { fn __init__(x: int) { self.v = x; } }
    class Slot<T> {
      v: T?;
      fn set(x: T) { self.v = x; }
      fn get() -> T? { return self.v; }
    }

    impl Slot<T> {
      fn __init__() { }
    }
    fn orElse<T>(x: T?, d: T) -> T {
      match x {
        v: T { return v; }
        None { return d; }
      }
    }
    fn ident<T>(x: T) -> T { return x; }
    fn main() -> int {
      b = Slot<Node>();
      io::println(Str<int>(orElse(b.get(), Node(0)).v));
      b.set(Node(5));
      io::println(Str<int>(orElse(b.get(), Node(0)).v));
      io::println(Str<int>(orElse(None, Node(9)).v));
      s = Slot<Str>();
      io::println(orElse(s.get(), "dflt"));
      n: Node? = None;
      m = ident<Node?>(n);
      io::println(Str<bool>(m == None));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n5\n9\ndflt\nTrue\n");
  g.expectNoLeaks("OptionalFieldsAndUnwrapInTemplates");
}

TEST(GenericsTypes, ExportedInstantiationNamesWithCommasAndTuples) {
  LeakGuard g;
  auto dir = paykan::test::tempDir() / "pkn_cg_generics_names";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  {
    std::ofstream lib(dir / "lib.pkn");
    lib << R"(
class Pair<A, B> { a: A; b: B; fn first() -> A { return self.a; } }
impl Pair<A, B> { fn __init__(a: A, b: B) { self.a = a; self.b = b; } }
class Box<T> { v: T; fn get() -> T { return self.v; } }
impl Box<T> { fn __init__(v: T) { self.v = v; } }
fn mkPair() -> Pair<Str, int> { return Pair<Str, int>("k", 1); }
fn mkBoxTuple() -> Box<(Str, int)> { return Box<(Str, int)>(("t", 3)); }
)";
  }
  auto mainPath = (dir / "main.pkn").string();
  {
    std::ofstream mainFile(mainPath);
    mainFile << R"(import ::io;
import lib;
fn main() -> int {
  p = lib::mkPair();
  io::println(p.first());
  b = lib::mkBoxTuple();
  return b.get().1;
}
)";
  }
  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "k\n");
  EXPECT_EQ(r.ExitCode, 3);
  std::filesystem::remove_all(dir);
  g.expectNoLeaks("ExportedInstantiationNamesWithCommasAndTuples");
}

// A `static fn` of a generic class's or struct's `impl` block is the
// instantiation's function: `Box<int>::of(1)`.
TEST(Generics, StaticFnOfAnInstantiation) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
class Box<T> { __v: T; }
impl Box<T> {
  fn __init__(v: T) { self.__v = v; }
  view fn get() -> view T { return self.__v; }
  static fn of(v: T) -> Box<T> { return Box<T>(v); }
  static fn twice(v: T) -> (Box<T>, Box<T>) { b = Box<T>(v); return (b, Box<T>(b.__v)); }
}
struct Pair<A, B> { a: A; b: B; }
impl Pair<A, B> {
  static fn make(a: A, b: B) -> Pair<A, B> { return Pair<A, B>(a, b); }
  static fn swapped(p: Pair<B, A>) -> Pair<A, B> { return Pair<A, B>(p.b, p.a); }
}
fn main() -> int {
  b = Box<int>::of(1);
  s = Box<Str>::of("s");
  t = Box<Str>::twice("t");
  p = Pair<int, Str>::make(2, "p");
  q = Pair<Str, int>::swapped(p);
  n: Box<Pair<int, Str>> = Box<Pair<int, Str>>::of(p);
  io::println(Str(b.get()) + " " + s.get() + " " + t.0.get() + t.1.get() + " " + p.toString() + " " + q.toString() + " " + Str(n.get().a));
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "1 s tt Pair<int, Str>(a: 2, b: p) Pair<Str, int>(a: p, b: 2) 2\n");
  g.expectNoLeaks("StaticFnOfAnInstantiation");
}
