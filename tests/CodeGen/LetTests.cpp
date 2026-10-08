// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen tests: `let` locals.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// A `let` local holds a value or a reference like any variable: inferred and
// annotated, primitives, a string, an object whose fields change, an array,
// an optional, inside a loop and shadowed in a block.
TEST(Let, LocalsHoldValuesAndReferences) {
  LeakGuard guard;
  auto r = compileAndRun(R"(
    class Point { x: int; fn __init__(x: int) { self.x = x; } }
    fn main() -> int {
      let n = 3;
      let f: float = 2;
      let s = "n=" + Str(n);
      let p = Point(1);
      let base: Obj = Point(5);
      let xs = [n, 4];
      let o: int? = None;
      p.x = p.x + n;
      xs.push(p.x);
      { let n = 10; println(Str(n)); }
      i = 0;
      while (i < 2) { let k = i * 2; print(Str(k) + " "); i = i + 1; }
      println(s + " " + Str(f) + " " + Str(p.x) + " " + Str(xs.len()) + " " +
              Str(o == None) + " " + Str(base != None));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "10\n0 2 n=3 2 4 3 True True\n");
  guard.expectNoLeaks("let locals");
}
