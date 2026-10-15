// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen tests: traits.  The methods of an `impl ... for` block run like any
// `impl` block's, `Self` among their types, and a subclass has them too.

#include "CodeGenTestUtils.h"
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
