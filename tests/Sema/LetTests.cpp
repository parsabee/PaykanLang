// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: `let` locals cannot be reassigned.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Let, CannotBeReassigned) {
  auto r = semaCheck(R"(fn main() -> int {
  let n = 3;
  n = 4;
  b = 0;
  n, b = (5, 6);
  {
    n = 7;
  }
  return n;
})");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
  for (const char *diag : {":3:3: error: 'n' is declared with 'let' and "
                           "cannot be reassigned",
                           ":5:3: error: 'n' is declared with 'let' and "
                           "cannot be reassigned",
                           ":7:5: error: 'n' is declared with 'let' and "
                           "cannot be reassigned"})
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos)
        << diag << "\n"
        << r.Diagnostics;
}

// A `let` local is otherwise an ordinary variable: it takes its
// initializer's type (or its annotation), an inner block may declare its own
// variable of the same name, and the object a `let` reference holds can
// still be changed.
TEST(Let, IsAnOrdinaryLocalOtherwise) {
  auto ok = semaCheck(R"(
    class Point { x: int; fn __init__(x: int) { self.x = x; } }
    fn main() -> int {
      let n = 3;
      let f: float = 2;
      let p = Point(1);
      let xs = [1, 2];
      p.x = n;
      xs[0] = p.x;
      xs.push(n);
      { n: int = 4; n = 5; }
      i = 0;
      while (i < 2) { let k = i; i = i + k + 1; }
      return n + p.x;
    }
  )");
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;

  auto r = semaCheck(R"(fn main() -> int {
  let s = "a";
  t: int = s;
  let u: int = "b";
  return 0;
})");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find(":3:3: error: initializer of type 'Str' does "
                               "not match declared type 'int' for variable 't'"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(":4:3: error: initializer of type 'Str' does "
                               "not match declared type 'int' for variable 'u'"),
            std::string::npos)
      << r.Diagnostics;
}
