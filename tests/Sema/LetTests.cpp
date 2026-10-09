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
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
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
      q = Point(p.x);
      q.x = n;
      ys = [xs[0], xs.len()];
      ys.push(n);
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
  EXPECT_NE(
      r.Diagnostics.find(":3:3: error: initializer of type 'Str' does "
                         "not match declared type 'int' for variable 't'"),
      std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(
      r.Diagnostics.find(":4:3: error: initializer of type 'Str' does "
                         "not match declared type 'int' for variable 'u'"),
      std::string::npos)
      << r.Diagnostics;
}

// A `let` local's value cannot change either: no field or element writes, no
// call of a method that is not a `view fn` (`push` included), no `inout`
// argument reached through it.  A `view fn` can be called.
TEST(Let, ValueCannotChange) {
  auto r = semaCheck(R"(class Point {
  x: int;
  fn __init__(x: int) { self.x = x; }
  fn move(dx: int) { self.x = self.x + dx; }
  view fn at() -> int { return self.x; }
}
fn bump(n: inout int) { n = n + 1; }
fn main() -> int {
  let p = Point(1);
  let xs = [1, 2];
  p.x = 2;
  xs[0] = 3;
  xs.push(4);
  p.move(1);
  bump(p.x);
  return p.at() + xs.len();
}
)");
  EXPECT_FALSE(r.Ok);
  for (const char *diag : {
           ":11:3: error: 'p' is 'let'; cannot assign to its field 'x'",
           ":12:3: error: 'xs' is 'let'; cannot assign to its elements",
           ":13:3: error: 'xs' is 'let'; 'push' is not a 'view fn'",
           ":14:3: error: 'p' is 'let'; 'move' is not a 'view fn'",
           ":15:8: error: 'p' is 'let'; it cannot be passed to 'inout' "
           "parameter 'n'",
       })
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find(":16:"), std::string::npos) << r.Diagnostics;
}
