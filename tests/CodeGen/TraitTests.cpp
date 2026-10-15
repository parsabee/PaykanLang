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
      fn __init__(n: int) { self.n = n; }
      view fn name() -> Str { return "A" + Str(self.n); }
    }
    impl Named for A {}
    impl Into<int> for A { view fn into() -> int { return self.n; } }
    impl Same for A {
      view fn same(other: view Self) -> bool { return self.n == other.n; }
    }
    class B : A { fn __init__() { __super__(7); } }
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
      fn __init__(n: int) { self.n = n; }
      view fn name() -> Str { return "A" + Str(self.n); }
    }
    impl Named for A {}
    impl Into<int> for A { view fn into() -> int { return self.n; } }
    impl Same for A {
      view fn same(other: view Self) -> bool { return self.n == other.n; }
    }
    class B : A { fn __init__() { __super__(7); } }
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
      fn __init__(v: T) { self.v = v; }
      view fn label() -> Str { return "[" + self.v.name() + "]"; }
    }
    fn main() -> int {
      a = A(1);
      b = B();
      h = Holder<A>(b);
      println(greet(a) + " " + greet(b) + " " + Str(total([a, b])) + " " +
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
  fn __init__(v: T) { self.v = v; }
  view fn text() -> Str { return show(self.v); }
}
)pkn";
  std::ofstream(dir / "main.pkn") << R"pkn(
import fmt;
class P { fn __init__() {} view fn toString() -> Str { return "P"; } }
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
