// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Codegen tests: traits.  The methods of an `impl ... for` block run like any
// `impl` block's, `Self` among their types, and a subclass has them too.  A
// generic body calls what its bounds require, also in a template of another
// module, and a type's `Hashable` crosses modules with it.

#include "CodeGenTestUtils.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Traits, ImplForMethodsRun) {
  LeakGuard guard;
  auto r = compileAndRun(R"pkn(import ::io;
    trait Named { view fn name() -> Str; }
    trait Into<T> { view fn into() -> T; }
    trait Same { view fn same(other: view Self) -> bool; }
    class A {
      n: int;
      view fn name() -> Str { return "A" + Str(self.n); }
    }

    impl A {
      fn __init__(n: int) { self.n = n; }
    }
    impl Named for A {}
    impl Into<int> for A { view fn into() -> int { return self.n; } }
    impl Same for A {
      view fn same(other: view Self) -> bool { return self.n == other.n; }
    }
    class B : A {}
    impl B { fn __init__() { __super__(7); } }
    fn main() -> int {
      a = A(1);
      b = B();
      io::println(a.name() + " " + b.name() + " " + Str(a.into() + b.into()) +
              " " + Str(a.same(b)) + " " + Str(a.same(A(1))));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "A1 A7 8 False True\n");
  guard.expectNoLeaks("traits");
}
TEST(Traits, BoundedGenericsRun) {
  LeakGuard guard;
  auto r = compileAndRun(R"pkn(import ::io;
    trait Named { view fn name() -> Str; }
    trait Into<T> { view fn into() -> T; }
    trait Same { view fn same(other: view Self) -> bool; }
    class A {
      n: int;
      view fn name() -> Str { return "A" + Str(self.n); }
    }

    impl A {
      fn __init__(n: int) { self.n = n; }
    }
    impl Named for A {}
    impl Into<int> for A { view fn into() -> int { return self.n; } }
    impl Same for A {
      view fn same(other: view Self) -> bool { return self.n == other.n; }
    }
    class B : A {}
    impl B { fn __init__() { __super__(7); } }
    fn greet<T: Named>(x: view T) -> Str { return "hi " + x.name(); }
    fn total<T: Into<int>>(xs: view T[]) -> int {
      t = 0;
      i = 0;
      while (i < xs.len()) { t = t + xs[i].into(); i = i + 1; }
      return t;
    }
    fn show<T: Show>(x: view T) -> Str { return x.toString(); }
    class Holder<T: Named> {
      v: T;
      view fn label() -> Str { return "[" + self.v.name() + "]"; }
    }

    impl Holder<T> {
      fn __init__(v: T) { self.v = v; }
    }
    fn main() -> int {
      a = A(1);
      b = B();
      h = Holder<A>(b);
      ab: A[2] = [a, b];
      io::println(greet(a) + " " + greet(b) + " " + Str(total(ab)) + " " +
              h.label() + " " + Str(a.same(b)) + " " + Str(a.same(A(1))));
      io::println(show("s"));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "hi A1 hi A7 8 [A7] False True\ns\n");
  guard.expectNoLeaks("traits");
}

// A template of another module bounded by `Show` takes the importer's class,
// whose `toString` override it calls.
TEST(Traits, ShowBoundAcrossModules) {
  LeakGuard guard;
  auto dir = tempDir() / "pkn_trait_modules";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "fmt.pkn") << R"pkn(
fn show<T: Show>(x: view T) -> Str { return "<" + x.toString() + ">"; }
class Box<T: Show> {
  v: T;
  view fn text() -> Str { return show(self.v); }
}

impl Box<T> {
  fn __init__(v: T) { self.v = v; }
}
)pkn";
  std::ofstream(dir / "main.pkn") << R"pkn(import ::io;
import fmt;
class P { view fn toString() -> Str { return "P"; } }
impl P { fn __init__() {} }
fn main() -> int {
  b = fmt::Box<P>(P());
  io::println(fmt::show(P()) + " " + b.text() + " " + fmt::show("s"));
  return 0;
}
)pkn";
  auto r = compileAndRunFile((dir / "main.pkn").string());
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "<P> <P> <s>\n");
  guard.expectNoLeaks("Show across modules");
  std::filesystem::remove_all(dir);
}

// A struct's and a class's `impl Hashable for` blocks cross modules: the
// importer makes them keys of `HashMap` and `HashSet` and arguments of a
// `T: Hashable` function, also when it reaches them through another module
// (mid) or a template body of theirs (keys::distinct over the private
// `_Priv`), and so are the instantiations of keys' generic types, those keys
// made and those main makes (`Keyed<Str>`, `GBox<Str>`, whose `impl Trait
// for` blocks go with the templates; `impl Named for GBox<T>` gives
// `GBox<Str>` its `name`).  An importer's subclass is Hashable through its
// superclass, whose `hash` it may override when the class body declares it.
TEST(Traits, HashableAcrossModules) {
  LeakGuard guard;
  auto dir = tempDir() / "pkn_trait_hashable";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / "lib");
  std::ofstream(dir / "lib" / "keys.pkn") << R"pkn(import ::collections;
struct Key { a: int; b: int; }
impl Hashable for Key { view fn hash() -> int { return (self.a * 31 + self.b).hash(); } }
class CK {
  v: int;
  view fn equals(other: Obj) -> bool {
    match other {
      p: CK { return p.v == self.v; }
      _ { return False; }
    }
  }
}
impl CK { fn __init__(v: int) { self.v = v; } }
impl Hashable for CK { view fn hash() -> int { return self.v.hash(); } }
struct _Priv { n: int; }
impl Hashable for _Priv { view fn hash() -> int { return self.n.hash(); } }
fn distinct<T>(x: T) -> unsigned {
  s = collections::HashSet<_Priv>(); s.add(_Priv(1)); s.add(_Priv(1)); s.add(_Priv(2));
  return s.len();
}
trait Named { view fn name() -> Str; }
struct Keyed<K: Hashable> { k: K; }
impl Hashable for Keyed<K> { view fn hash() -> int { return self.k.hash(); } }
class GBox<T> { v: T; }
impl GBox<T> { fn __init__(v: T) { self.v = v; } }
impl Hashable for GBox<T> { view fn hash() -> int { return 3; } }
impl Named for GBox<T> { view fn name() -> Str { return "box"; } }
fn mkKeyed() -> Keyed<int> { return Keyed<int>(7); }
fn mkBox() -> GBox<int> { return GBox<int>(1); }
class CK2 { n: int; view fn hash() -> int { return self.n; } }
impl CK2 { fn __init__(n: int) { self.n = n; } }
impl Hashable for CK2 {}
)pkn";
  std::ofstream(dir / "lib" / "mid.pkn") << R"pkn(import lib::keys;
fn mk() -> keys::Key { return keys::Key(1, 2); }
fn mkc() -> keys::CK { return keys::CK(4); }
)pkn";
  std::ofstream(dir / "main.pkn") << R"pkn(import ::io;
import ::collections;
import lib::keys;
import lib::mid;
class Sub : keys::CK {
  view fn equals(other: Obj) -> bool { match other { p: Sub { return p.v == self.v; } _ { return False; } } }
}
impl Sub { fn __init__(v: int) { __super__(v); } }
class Sub2 : keys::CK2 { view fn hash() -> int { return 99; } }
impl Sub2 { fn __init__(n: int) { __super__(n); } }
fn h<T: Hashable>(x: view T) -> int { return x.hash(); }
fn main() -> int {
  m = collections::HashMap<keys::Key, int>();
  m.put(keys::Key(1, 2), 5);
  m.put(mid::mk(), 6);
  s = collections::HashSet<keys::CK>();
  s.add(keys::CK(3)); s.add(keys::CK(3)); s.add(mid::mkc());
  t = collections::HashSet<Sub>();
  t.add(Sub(1)); t.add(Sub(1));
  io::println(Str(m.len()) + " " + Str(s.len()) + " " + Str(t.len()));
  io::println(Str(h(keys::mkKeyed()) == h(7)) + " " + Str(h(keys::Keyed<Str>("a")) == h("a")));
  io::println(Str(h(keys::mkBox())) + " " + Str(h(keys::GBox<Str>("x"))) + " " + keys::GBox<Str>("x").name());
  io::println(Str(keys::distinct(0)));
  c: keys::CK2 = Sub2(3);
  io::println(Str(h(c)) + " " + Str(h(Sub2(1))) + " " + Str(h(keys::CK2(5))));
  return 0;
}
)pkn";
  auto r = compileAndRunFile((dir / "main.pkn").string());
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1 2 1\nTrue True\n3 3 box\n2\n99 99 5\n");
  guard.expectNoLeaks("Hashable across modules");
  std::filesystem::remove_all(dir);
}

TEST(Traits, ScalarsShow) {
  LeakGuard guard;
  auto r = compileAndRun(R"pkn(import ::io;
    fn show<T: Show>(x: view T) -> Str { return "<" + x.toString() + ">"; }
    fn main() -> int {
      n = 42;
      f = 2.5;
      b = True;
      c = 'z';
      io::println(n.toString() + " " + f.toString() + " " + b.toString() + " " +
              c.toString());
      io::println(show(n) + show(f) + show(b) + show(c) + show("s"));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "42 2.5 True z\n<42><2.5><True><z><s>\n");
  guard.expectNoLeaks("scalar Show");
}

// A tuple, struct, enum and optional whose every part shows itself print
// as before, with `io::println`, `toString()` and through a `T: Show`
// bound: a function value inside an object (which shows itself through
// its class) does not stop the struct holding the object from showing
// itself.
TEST(Traits, AggregatesOfShowPartsShow) {
  LeakGuard guard;
  auto r = compileAndRun(R"pkn(import ::io;
    class Handler {
      f: (int) -> int;
      view fn toString() -> Str { return "h" + Str(self.f(1)); }
    }
    impl Handler { fn __init__() { self.f = fn(x: int) => x + 1; } }
    struct P { x: int; h: Handler; }
    struct Q { p: P; at: (float, char?); }
    enum E { A(q: Q), B(n: int?), N }
    enum Maybe<X> { Just(v: X), Nothing }
    fn show<X: Show>(x: view X) -> Str { return "<" + x.toString() + ">"; }
    fn main() -> int {
      q = Q(P(1, Handler()), (2.5, None));
      io::println(q);
      io::println(q.p.toString() + " " + (1, 'c').toString());
      io::println(E::A(q));
      io::println(E::B(None).toString() + " " + E::N.toString());
      o: Q? = None;
      io::println(o.toString());
      o = q;
      io::println(o);
      m: Maybe<P> = Maybe::Just(q.p);
      io::println(show(m) + show((q.at, E::B(3))));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Q(p: P(x: 1, h: h2), at: (2.5, None))\n"
                      "P(x: 1, h: h2) (1, c)\n"
                      "A(q: Q(p: P(x: 1, h: h2), at: (2.5, None)))\n"
                      "B(n: None) N\n"
                      "None\n"
                      "Q(p: P(x: 1, h: h2), at: (2.5, None))\n"
                      "<Just(v: P(x: 1, h: h2))><((2.5, None), B(n: 3))>\n");
  guard.expectNoLeaks("aggregates of Show parts");
}

// `hash()` of int, bool, char and Str (the builtin `Hashable`): equal values
// hash alike (a char and a bool as their codes), and a class's own `hash`
// meets a `T: Hashable` bound.
TEST(Traits, BuiltinHashable) {
  LeakGuard guard;
  auto r = compileAndRun(R"pkn(import ::io;
    class Key { id: int; }
    impl Hashable for Key { view fn hash() -> int { return self.id.hash(); } }
    fn h<T: Hashable>(x: view T) -> int { return x.hash(); }
    fn main() -> int {
      n = 97;
      one = 1;
      s = "ab";
      io::println(Str(h(n) == n.hash()) + " " + Str(h(1) == h(2)) + " " +
              Str(h('a') == h(n)) + " " + Str(True.hash() == one.hash()));
      io::println(Str((s + "c").hash() == "abc".hash()) + " " +
              Str(h(s) == h("ba")) + " " + Str(h(Key(7)) == h(7)));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True False True True\nTrue False True\n");
  guard.expectNoLeaks("builtin Hashable");
}
