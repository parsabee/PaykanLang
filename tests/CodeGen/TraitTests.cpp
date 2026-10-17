// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen tests: traits.  The methods of an `impl ... for` block run like any
// `impl` block's, `Self` among their types, and a subclass has them too.  A
// generic body calls what its bounds require, also in a template of another
// module.

#include "CodeGenTestUtils.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Traits, ImplForMethodsRun) {
  LeakGuard guard;
  auto r = compileAndRun(R"pkn(
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
      println(a.name() + " " + b.name() + " " + Str(a.into() + b.into()) +
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
  auto r = compileAndRun(R"pkn(
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
      println(greet(a) + " " + greet(b) + " " + Str(total(ab)) + " " +
              h.label() + " " + Str(a.same(b)) + " " + Str(a.same(A(1))));
      println(show("s"));
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
  std::ofstream(dir / "main.pkn") << R"pkn(
import fmt;
class P { view fn toString() -> Str { return "P"; } }
impl P { fn __init__() {} }
fn main() -> int {
  b = fmt::Box<P>(P());
  println(fmt::show(P()) + " " + b.text() + " " + fmt::show("s"));
  return 0;
}
)pkn";
  auto r = compileAndRunFile((dir / "main.pkn").string());
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "<P> <P> <s>\n");
  guard.expectNoLeaks("Show across modules");
  std::filesystem::remove_all(dir);
}

TEST(Traits, ScalarsShow) {
  LeakGuard guard;
  auto r = compileAndRun(R"pkn(
    fn show<T: Show>(x: view T) -> Str { return "<" + x.toString() + ">"; }
    fn main() -> int {
      n = 42;
      f = 2.5;
      b = True;
      c = 'z';
      println(n.toString() + " " + f.toString() + " " + b.toString() + " " +
              c.toString());
      println(show(n) + show(f) + show(b) + show(c) + show("s"));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "42 2.5 True z\n<42><2.5><True><z><s>\n");
  guard.expectNoLeaks("scalar Show");
}

// `hash()` of int, bool, char and Str (the builtin `Hashable`): equal values
// hash alike (a char and a bool as their codes), and a class's own `hash`
// meets a `T: Hashable` bound.
TEST(Traits, BuiltinHashable) {
  LeakGuard guard;
  auto r = compileAndRun(R"pkn(
    class Key { id: int; }
    impl Hashable for Key { view fn hash() -> int { return self.id.hash(); } }
    fn h<T: Hashable>(x: view T) -> int { return x.hash(); }
    fn main() -> int {
      n = 97;
      one = 1;
      s = "ab";
      println(Str(h(n) == n.hash()) + " " + Str(h(1) == h(2)) + " " +
              Str(h('a') == h(n)) + " " + Str(True.hash() == one.hash()));
      println(Str((s + "c").hash() == "abc".hash()) + " " +
              Str(h(s) == h("ba")) + " " + Str(h(Key(7)) == h(7)));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True False True True\nTrue False True\n");
  guard.expectNoLeaks("builtin Hashable");
}
