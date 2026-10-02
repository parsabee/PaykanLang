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

// RAII guard: enable tracking allocator before the run, check zero live
// blocks after (same pattern as LeakTests.cpp).
struct LeakGuard {
  LeakGuard() {
    Paykan_heap_set_tracking(1);
    Paykan_heap_reset();
  }
  ~LeakGuard() { Paykan_heap_set_tracking(0); }
  void expectNoLeaks(const char *label = "") const {
    int64_t live = Paykan_heap_live_blocks();
    EXPECT_EQ(live, 0) << "heap leak in: " << label << " (" << live
                       << " live blocks)";
  }
};

const char *kBox = R"(
  class Box<T> {
    v: T;
    fn __init__(v: T) { self.v = v; }
    fn get() -> T { return self.v; }
    fn set(v: T) { self.v = v; }
  }
)";

std::string withMain(const std::string &decls, const std::string &body) {
  return decls + "\nfn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

} // namespace

TEST(Generics, BoxStrGetSet) {
  LeakGuard g;
  auto r = compileAndRun(withMain(kBox, R"(
    b: Box<Str> = Box<Str>("hello");
    println(b.get());
    b.set("world");
    println(b.get());
    c = Box<Str>(b.get() + "!");
    println(c.get());
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
    println(StrInt(a.get()));
    println(b.get());
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
      fn __init__(k: K, v: V) { self.k = k; self.v = v; }
      fn key() -> K { return self.k; }
      fn value() -> V { return self.v; }
      fn toString() -> Str { return self.k + "=" + StrInt(self.v); }
    }
  )",
                                  R"(
    p: Pair<Str, int> = Pair<Str, int>("age", 42);
    println(p.key() + ":" + StrInt(p.value()));
    println(p.toString());
    println(p);
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
    println(StrInt(outer.get().get()));
    outer.get().set(9);
    println(StrInt(inner.get()));
    fresh = Box<Box<int>>(Box<int>(1));
    println(StrInt(fresh.get().get()));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "5\n9\n1\n");
  g.expectNoLeaks("BoxOfBoxInt");
}

// Stack<T> over T[] with Str and int elements.  Every element is popped
// before main returns: pushing a Str into an array FIELD from inside a method
// and leaving it there leaks the element even for a hand-written class (a
// pre-existing ownership bug unrelated to generics).
TEST(Generics, StackOfStrAndInt) {
  LeakGuard g;
  auto r = compileAndRun(withMain(R"(
    class Stack<T> {
      items: T[];
      fn __init__() { self.items = []; }
      fn push(v: T) { self.items.push(v); }
      fn pop() -> T { return self.items.pop(); }
      fn size() -> int { return self.items.len(); }
      fn empty() -> bool { return self.items.len() == 0; }
    }
  )",
                                  R"(
    s: Stack<Str> = Stack<Str>();
    s.push("a");
    s.push("b");
    s.push("c");
    println(StrInt(s.size()));
    while (!s.empty()) { println(s.pop()); }
    n: Stack<int> = Stack<int>();
    n.push(1);
    n.push(2);
    total: int = 0;
    while (!n.empty()) { total = total + n.pop(); }
    println(StrInt(total));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3\nc\nb\na\n3\n");
  g.expectNoLeaks("StackOfStrAndInt");
}

TEST(Generics, GenericFirstOnIntAndStrArrays) {
  LeakGuard g;
  auto r = compileAndRun(withMain(R"(
    fn first<T>(xs: T[]) -> T { return xs[0]; }
    fn last<T>(xs: T[]) -> T { return xs[xs.len() - 1]; }
  )",
                                  R"(
    println(StrInt(first([10, 20, 30])));
    println(first(["x", "y"]));
    words: Str[] = ["p", "q", "r"];
    println(last(words));
    println(StrInt(last<int>([7])));
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
    println(StrInt(unbox(Box<int>(4))));
    println(unbox(rebox(Box<Str>("z"))));
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
        b: Box<int> { return "Box<int> holding " + StrInt(b.get()); }
        Box<Str> { return "Box<Str>"; }
        _ { return "other"; }
      }
    }
  )",
                                  R"(
    println(describe(Box<int>(3)));
    println(describe(Box<Str>("s")));
    println(describe("plain"));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Box<int> holding 3\nBox<Str>\nother\n");
  g.expectNoLeaks("MatchOnInstantiation");
}

TEST(Generics, MovOfInstantiation) {
  LeakGuard g;
  auto r = compileAndRun(withMain(std::string(kBox) + R"(
    fn consume(b: Box<Str>) -> Str { return b.get(); }
  )",
                                  R"(
    a: Box<Str> = Box<Str>("moved");
    b: Box<Str> = mov a;
    println(b.get());
    println(consume(mov b));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "moved\nmoved\n");
  g.expectNoLeaks("MovOfInstantiation");
}

TEST(Generics, EqualityDispatchesToEquals) {
  LeakGuard g;
  auto r = compileAndRun(withMain(R"(
    class Box<T> {
      v: T;
      fn __init__(v: T) { self.v = v; }
      fn get() -> T { return self.v; }
      fn equals(other: Obj) -> bool {
        match other {
          o: Box<T> { return o.get() == self.v; }
          _ { return False; }
        }
      }
    }
  )",
                                  R"(
    a: Box<int> = Box<int>(1);
    b: Box<int> = Box<int>(1);
    c: Box<int> = Box<int>(2);
    println(StrBool(a == b));
    println(StrBool(a == c));
    println(StrBool(a != c));
    s: Box<Str> = Box<Str>("x");
    println(StrBool(s == Box<Str>("x")));
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
      fn __init__(n: int) { self.b = Box<int>(n); }
      fn get() -> Box<int> { return self.b; }
    }
  )",
                                  R"(
    arr: Box<int>[] = [Box<int>(1), Box<int>(2)];
    arr.push(Box<int>(3));
    total: int = 0;
    i: int = 0;
    while (i < arr.len()) { total = total + arr[i].get(); i = i + 1; }
    println(StrInt(total));
    h: Holder = Holder(40);
    println(StrInt(h.get().get() + 2));
    names: Box<Str>[] = [Box<Str>("n")];
    println(names[0].get());
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "6\n42\nn\n");
  g.expectNoLeaks("ArrayOfInstantiationsAndFieldOfInstantiation");
}

TEST(Generics, ConstructorInferenceAndInstantiationOfArray) {
  LeakGuard g;
  auto r = compileAndRun(withMain(kBox, R"(
    b = Box(5);
    println(StrInt(b.get()));
    s = Box("inferred");
    println(s.get());
    xs: Box<int[]> = Box<int[]>([1, 2, 3]);
    println(StrInt(xs.get().len()));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "5\ninferred\n3\n");
  g.expectNoLeaks("ConstructorInferenceAndInstantiationOfArray");
}

// Instantiations are merged into the class list so that a superclass is
// emitted before its (instantiated) subclass — `__super__(n)` must find
// Base___init__ — and a constructed class before the class constructing it.
TEST(Generics, GenericWithConcreteSuperclassVirtualDispatch) {
  LeakGuard g;
  auto r = compileAndRun(withMain(R"(
    class Base {
      n: int;
      fn __init__(n: int) { self.n = n; }
      fn name() -> Str { return "base"; }
    }
    class Wrap<T> : Base {
      v: T;
      fn __init__(v: T, n: int) { __super__(n); self.v = v; }
      fn name() -> Str { return "wrap"; }
      fn get() -> T { return self.v; }
    }
  )",
                                  R"(
    w: Wrap<Str> = Wrap<Str>("payload", 5);
    b: Base = w;
    println(b.name() + " " + StrInt(b.n) + " " + w.get());
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
class Box<T> { v: T; fn __init__(v: T) { self.v = v; } fn get() -> T { return self.v; } }
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

// ============================================================================
// Interaction with tuples (#4) and optionals (#5)
// ============================================================================

TEST(GenericsTypes, TupleFieldLiteralIndexAndDestructuring) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Pair<A, B> {
      p: (A, B);
      fn __init__(a: A, b: B) { self.p = (a, b); }
      fn first() -> A { x, _ = self.p; return x; }
      fn second() -> B { return self.p.1; }
      fn swap() -> (B, A) { return (self.p.1, self.p.0); }
    }
    fn firstOf<A, B>(p: (A, B)) -> A { return p.0; }
    fn dup<T>(x: T) -> (T, T) { return (x, x); }
    fn main() -> int {
      q = Pair<Str, int>("a", 1);
      println(q.first());
      println(StrInt(q.second()));
      println(q.swap());
      r: Pair<int, Str> = Pair<int, Str>(2, "b");
      println(r.swap());
      println(StrInt(firstOf((7, "x"))));
      a, b = dup("z");
      println(a + b);
      println(dup((1, "q")));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "a\n1\n(1, a)\n(b, 2)\n7\nzz\n((1, q), (1, q))\n");
  g.expectNoLeaks("TupleFieldLiteralIndexAndDestructuring");
}

TEST(GenericsTypes, OptionalFieldsAndUnwrapInTemplates) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Node { v: int; fn __init__(x: int) { self.v = x; } }
    class Slot<T> {
      v: T?;
      fn __init__() { }
      fn set(x: T) { self.v = x; }
      fn get() -> T? { return self.v; }
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
      println(StrInt(orElse(b.get(), Node(0)).v));
      b.set(Node(5));
      println(StrInt(orElse(b.get(), Node(0)).v));
      println(StrInt(orElse(None, Node(9)).v));
      s = Slot<Str>();
      println(orElse(s.get(), "dflt"));
      n: Node? = None;
      m = ident<Node?>(n);
      println(StrBool(m == None));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n5\n9\ndflt\nTrue\n");
  g.expectNoLeaks("OptionalFieldsAndUnwrapInTemplates");
}

TEST(GenericsTypes, ExportedInstantiationNamesWithCommasAndArrays) {
  LeakGuard g;
  auto dir = paykan::test::tempDir() / "pkn_cg_generics_names";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  {
    std::ofstream lib(dir / "lib.pkn");
    lib << R"(
class Pair<A, B> { a: A; b: B; fn __init__(a: A, b: B) { self.a = a; self.b = b; } fn first() -> A { return self.a; } }
class Box<T> { v: T; fn __init__(v: T) { self.v = v; } fn get() -> T { return self.v; } }
fn mkPair() -> Pair<Str, int> { return Pair<Str, int>("k", 1); }
fn mkBoxArr() -> Box<int[]> { return Box<int[]>([1, 2, 3]); }
)";
  }
  auto mainPath = (dir / "main.pkn").string();
  {
    std::ofstream mainFile(mainPath);
    mainFile << R"(
import lib;
fn main() -> int {
  p = lib::mkPair();
  println(p.first());
  b = lib::mkBoxArr();
  return b.get().len();
}
)";
  }
  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "k\n");
  EXPECT_EQ(r.ExitCode, 3);
  std::filesystem::remove_all(dir);
  g.expectNoLeaks("ExportedInstantiationNamesWithCommasAndArrays");
}
