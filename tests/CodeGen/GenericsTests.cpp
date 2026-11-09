// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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
    view fn get() -> view T { return self.v; }
    fn set(v: T) { self.v = move v; }
  }

  impl Box<T> {
    fn __init__(v: T) { self.v = move v; }
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
    b: mut Box<Str> = Box<Str>("hello");
    io::println(b.get());
    b.set("world");
    io::println(b.get());
    c: mut = Box<Str>(b.get() + "!");
    io::println(c.get());
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "hello\nworld\nworld!\n");
  g.expectNoLeaks("BoxStrGetSet");
}

TEST(Generics, BoxIntAndBoxStrCoexist) {
  LeakGuard g;
  auto r = compileAndRun(withMain(kBox, R"(
    a: mut Box<int> = Box<int>(3);
    b: mut Box<Str> = Box<Str>("s");
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
      view fn key() -> view K { return self.k; }
      view fn value() -> view V { return self.v; }
      view fn toString() -> Str { return self.k + "=" + Str<int>(self.v); }
    }

    impl Pair<K, V> {
      fn __init__(k: K, v: V) { self.k = move k; self.v = move v; }
    }
  )",
                                  R"(
    p: mut Pair<Str, int> = Pair<Str, int>("age", 42);
    io::println(p.key() + ":" + Str<int>(p.value()));
    io::println(p.toString());
    io::println(p);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "age:42\nage=42\nage=42\n");
  g.expectNoLeaks("PairStrInt");
}

// The inner Box moves into the outer one, and is changed through it.
TEST(Generics, BoxOfBoxInt) {
  LeakGuard g;
  auto r = compileAndRun(withMain(kBox, R"(
    inner: mut Box<int> = Box<int>(5);
    outer: mut Box<Box<int>> = Box<Box<int>>(move inner);
    io::println(Str<int>(outer.get().get()));
    outer.v.set(9);
    io::println(Str<int>(outer.get().get()));
    fresh: mut = Box<Box<int>>(Box<int>(1));
    io::println(Str<int>(fresh.get().get()));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "5\n9\n1\n");
  g.expectNoLeaks("BoxOfBoxInt");
}

// Stack<T> over a fixed-capacity T[4] field and a count, with Str and int
// elements: the methods write and read the field in place (`pop` returns a
// clone, since an element stays its array's), and an element still on the
// stack is released with it.
TEST(Generics, StackOfStrAndInt) {
  LeakGuard g;
  auto r = compileAndRun(withMain(R"(
    class Stack<T> {
      items: T[4];
      n: int;
      fn push(v: T) {
        self.items[self.n] = move v;
        self.n = self.n + 1;
      }
      fn pop() -> T {
        self.n = self.n - 1;
        return self.items[self.n].clone();
      }
      view fn size() -> int { return self.n; }
      view fn empty() -> bool { return self.n == 0; }
    }

    impl Stack<T> {
      fn __init__(fill: T) {
        self.items = [fill.clone(), fill.clone(), fill.clone(), move fill];
        self.n = 0;
      }
    }
  )",
                                  R"(
    s: mut Stack<Str> = Stack<Str>("");
    s.push("a");
    s.push("b");
    s.push("c");
    io::println(Str<int>(s.size()));
    while !s.empty() { io::println(s.pop()); }
    n: mut Stack<int> = Stack<int>(0);
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
    fn unbox<T>(b: Box<T>) -> T { return move b.v; }
    fn rebox<T>(b: Box<T>) -> Box<T> { return Box<T>(move b.v); }
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
        b: Box<int> { return "Box<int> holding " + Str<int>(b.v); }
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
    fn consume(b: mut Box<Str>) -> Str { return b.get().clone(); }
  )",
                                  R"(
    a: mut Box<Str> = Box<Str>("moved");
    b: mut Box<Str> = move a;
    io::println(b.get());
    io::println(consume(move b));
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
      view fn get() -> view T { return self.v; }
      view fn equals(other: Obj) -> bool {
        match other {
          o: Box<T> { return o.v == self.v; }
          _ { return False; }
        }
      }
    }

    impl Box<T> {
      fn __init__(v: T) { self.v = move v; }
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
      view fn get() -> view Box<int> { return self.b; }
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
    h: mut Holder = Holder(40);
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
    b: mut = Box(5);
    io::println(Str<int>(b.get()));
    s: mut = Box("inferred");
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
      view fn name() -> Str { return "base"; }
    }

    impl Base {
      fn __init__(n: int) { self.n = n; }
    }
    class Wrap<T> : Base {
      v: T;
      view fn name() -> Str { return "wrap"; }
      view fn get() -> view T { return self.v; }
    }

    impl Wrap<T> {
      fn __init__(v: T, n: int) { __super__(n); self.v = move v; }
    }
  )",
                                  R"(
    w: shared mut Wrap<Str> = Wrap<Str>("payload", 5);
    b: shared mut Base = w;
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
class Box<T: Copyable> { v: T; fn get() -> T { return self.v; } }
impl Box<T> { fn __init__(v: T) { self.v = move v; } }
fn mk() -> Box<int> { return Box<int>(41); }
)";
  }
  auto mainPath = (dir / "main.pkn").string();
  {
    std::ofstream mainFile(mainPath);
    mainFile << R"(
import lib::gen;
fn main() -> int { b: mut = gen::mk(); return b.get() + 1; }
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
      fn first() -> A { x, _ = self.p.clone(); return x; }
      fn second() -> B { return self.p.1.clone(); }
      fn swap() -> (B, A) { return (self.p.1.clone(), self.p.0.clone()); }
    }

    impl Pair<A, B> {
      fn __init__(a: A, b: B) { self.p = (move a, move b); }
    }
    fn firstOf<A: Copyable, B>(p: (A, B)) -> A { return p.0; }
    fn dup<T>(x: T) -> (T, T) { return (x.clone(), move x); }
    fn main() -> int {
      q: mut = Pair<Str, int>("a", 1);
      io::println(q.first());
      io::println(Str<int>(q.second()));
      io::println(q.swap());
      r: mut Pair<int, Str> = Pair<int, Str>(2, "b");
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

// `get` and `orElse` borrow: the slot keeps its value.
TEST(GenericsTypes, OptionalFieldsAndUnwrapInTemplates) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Node { v: int; }
    impl Node { fn __init__(x: int) { self.v = x; } }
    class Slot<T> {
      v: T?;
      fn set(x: T) { self.v = move x; }
      view fn get() -> view T? { return self.v; }
    }

    impl Slot<T> {
      fn __init__() { }
    }
    fn orElse<T>(x: view T?, d: view T) -> view T {
      match x {
        v: T { return v; }
        None { return d; }
      }
    }
    fn ident<T>(x: T) -> T { return x; }
    fn main() -> int {
      b: mut = Slot<Node>();
      io::println(Str<int>(orElse(b.get(), Node(0)).v));
      b.set(Node(5));
      io::println(Str<int>(orElse(b.get(), Node(0)).v));
      io::println(Str<int>(orElse(None, Node(9)).v));
      s: mut = Slot<Str>();
      io::println(orElse(s.get(), "dflt"));
      n: Node? = None;
      m = ident<Node?>(move n);
      io::println(Str<bool>(m == None));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n5\n9\ndflt\nTrue\n");
  g.expectNoLeaks("OptionalFieldsAndUnwrapInTemplates");
}

// When T is optional, a template's `T?` is T, and its arm `v: T` on it is
// the arm on what T wraps: it matches a present value and binds it as a T,
// so a template that unwraps a `T?` instantiates with any T.
TEST(GenericsTypes, UnwrapArmOfAnOptionalTypeArgument) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Node { v: Str; }
    impl Node { fn __init__(x: Str) { self.v = move x; } }
    class Slot<T> {
      v: T?;
      view fn describe() -> Str {
        match self.v { x: T { return "has"; } None { return "empty"; } }
      }
    }
    impl Slot<T> {
      fn __init__(v: T?) { self.v = move v; }
    }
    fn orElse<T>(x: T?, d: T) -> T {
      match move x {
        v: T { return v; }
        None { return d; }
      }
    }
    fn main() -> int {
      io::println(Slot<Str?>(None).describe() + " " +
                  Slot<Str?>("a" + "b").describe());
      y: int? = 5;
      io::println(orElse<int?>(y, None).toString() + " " +
                  orElse<int?>(None, 7).toString());
      n: Node? = Node("n" + "1");
      match orElse<Node?>(move n, None) { m: Node { io::print(m.v); } None { } }
      io::println(" " + orElse<Node?>(None, None).toString());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "empty has\n5 7\nn1 None\n");
  g.expectNoLeaks("UnwrapArmOfAnOptionalTypeArgument");
}

// The binding of an arm naming an optional T on a `T?` is a T, as the
// template's other values of type T are: it is compared with a T and put in
// a `(T, T)`, and through `x: inout T` (directly, or passed on to an `inout
// T` parameter) it is assigned a T, None included, that goes back to the
// subject (the functions that make it borrow the old value).  So for an
// optional of a class, of a primitive, of a struct and of a tagged union,
// next to the same templates with a T that is not one.
TEST(GenericsTypes, ArmOfAnOptionalTypeArgumentBindsAT) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    struct P { s: Str; n: int; }
    enum Sh { E, L(s: Str) }
    class Slot<T> {
      v: T?;
      fn poke(f: (view T) -> T) { match self.v { x: inout T { x = f(x); } None { } } }
      fn pass(f: (view T) -> T) { match self.v { x: inout T { bump(x, move f); } None { } } }
    }
    fn holds<T>(x: view T?, d: view T) -> bool {
      match x { v: T { return v == d; } None { return False; } }
    }
    fn pairWith<T>(x: T?, d: T) -> (T, T)? {
      match move x { v: T { return (move v, move d); } None { return None; } }
    }
    fn bump<T>(p: inout T, f: (view T) -> T) { p = f(p); }
    fn twice(s: view Str) -> Str { return s + s; }
    fn twiceO(s: view Str?) -> Str? { match s { t: Str { return t + t; } None { return "-"; } } }
    fn gone(s: view Str?) -> Str? { return None; }
    fn incO(n: view int?) -> int? { match n { k: int { return k + 1; } None { return 0; } } }
    fn noInt(n: view int?) -> int? { return None; }
    fn grow(p: view P) -> P { return P(p.s + "+", p.n + 1); }
    fn growO(p: view P?) -> P? { match p { q: P { return P(q.s + "?", q.n * 10); } None { return None; } } }
    fn shO(s: view Sh?) -> Sh? {
      match s { t: Sh { match t { l: L { return Sh::L(l.s + "!"); } _ { return None; } } } None { return None; } }
    }
    fn main() -> int {
      a: mut = Slot<Str>("a" + "");
      a.poke(twice);
      a.pass(twice);
      b: mut = Slot<Str?>("b" + "");
      b.poke(twiceO);
      b.pass(twiceO);
      io::print(a.v.toString() + " " + b.v.toString() + " ");
      b.poke(gone);
      d: mut = Slot<int?>(5);
      d.poke(incO);
      d.pass(incO);
      io::println(b.v.toString() + " " + d.v.toString());
      d.poke(noInt);
      e: mut = Slot<P>(P("e" + "", 1));
      e.poke(grow);
      f: mut = Slot<P?>(P("f" + "", 2));
      f.poke(growO);
      f.pass(growO);
      h: mut = Slot<Sh?>(Sh::L("h" + ""));
      h.poke(shO);
      h.pass(shO);
      io::println(d.v.toString() + " " + e.v.toString() + " " + f.v.toString() + " " +
                  h.v.toString());
      s: Str? = "s" + "";
      n: int? = 1;
      q: P? = P("q" + "", 3);
      io::println(Str(holds<Str?>(s, "s")) + " " + Str(holds<Str?>(s, None)) + " " +
                  Str(holds<int>(n, 1)) + " " + Str(holds<int?>(n, 1)) + " " +
                  Str(holds<int?>(n, None)) + " " + Str(holds<P?>(q, P("q", 3))));
      io::println(pairWith<Str?>(move s, None).toString() + " " + pairWith<int?>(n, 8).toString() +
                  " " + pairWith<P?>(move q, None).toString() + " " +
                  pairWith<int>(n, 2).toString());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "aaaa bbbb None 7\n"
                      "None P(s: e+, n: 2) P(s: f??, n: 200) L(s: h!!)\n"
                      "True False True True False True\n"
                      "(s, None) (1, 8) (P(s: q, n: 3), None) (1, 2)\n");
  g.expectNoLeaks("ArmOfAnOptionalTypeArgumentBindsAT");
}

TEST(GenericsTypes, ExportedInstantiationNamesWithCommasAndTuples) {
  LeakGuard g;
  auto dir = paykan::test::tempDir() / "pkn_cg_generics_names";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  {
    std::ofstream lib(dir / "lib.pkn");
    lib << R"(
class Pair<A, B> { a: A; b: B; view fn first() -> view A { return self.a; } }
impl Pair<A, B> { fn __init__(a: A, b: B) { self.a = move a; self.b = move b; } }
class Box<T> { v: T; view fn get() -> view T { return self.v; } }
impl Box<T> { fn __init__(v: T) { self.v = move v; } }
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
  p: mut = lib::mkPair();
  io::println(p.first());
  b: mut = lib::mkBoxTuple();
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
  fn __init__(v: T) { self.__v = move v; }
  view fn get() -> view T { return self.__v; }
  static fn of(v: T) -> Box<T> { return Box<T>(move v); }
  static fn twice(v: T) -> (Box<T>, Box<T>) {
    b = Box<T>(move v);
    c = Box<T>(b.__v.clone());
    return (move b, move c);
  }
}
struct Pair<A, B> { a: A; b: B; }
impl Pair<A, B> {
  static fn make(a: A, b: B) -> Pair<A, B> { return Pair<A, B>(move a, move b); }
  static fn swapped(p: Pair<B, A>) -> Pair<A, B> { return Pair<A, B>(p.b.clone(), p.a.clone()); }
}
fn main() -> int {
  b = Box<int>::of(1);
  s = Box<Str>::of("s");
  t = Box<Str>::twice("t");
  p = Pair<int, Str>::make(2, "p");
  q = Pair<Str, int>::swapped(p.clone());
  n: Box<Pair<int, Str>> = Box<Pair<int, Str>>::of(p.clone());
  io::println(Str(b.get()) + " " + s.get() + " " + t.0.get() + t.1.get() + " " + p.toString() + " " + q.toString() + " " + Str(n.get().a));
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "1 s tt Pair<int, Str>(a: 2, b: p) Pair<Str, int>(a: p, b: 2) 2\n");
  g.expectNoLeaks("StaticFnOfAnInstantiation");
}
