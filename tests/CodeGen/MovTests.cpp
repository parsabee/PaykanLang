// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen tests: `mov` lowering — value/ownership transfer behavior.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Mov, PrimitiveForwardsValue) {
  auto r = compileAndRun(R"(
    fn main() -> int {
      a: int = 42;
      b = mov a;
      println(StrInt(b));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "42\n");
}

TEST(Mov, StringOwnershipTransfer) {
  auto r = compileAndRun(R"(
    fn main() -> int {
      s: Str = "Hello world";
      t = mov s;
      println(t);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Hello world\n");
}

TEST(Mov, MoveTemporaryIntoVariable) {
  auto r = compileAndRun(R"(
    fn make() -> Str { return "made"; }
    fn main() -> int {
      t = mov make();
      println(t);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "made\n");
}

TEST(Mov, MoveClassObject) {
  auto r = compileAndRun(R"(
    class Point { x: int; y: int;
      fn __init__(a: int, b: int) { self.x = a; self.y = b; }
      fn sum() -> int { return self.x + self.y; }
    }
    fn main() -> int {
      p: Point = Point(3, 4);
      q = mov p;
      println(StrInt(q.sum()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "7\n");
}

TEST(Mov, MoveThenRevive) {
  auto r = compileAndRun(R"(
    fn main() -> int {
      s: Str = "first";
      t = mov s;
      s = "second";
      println(t);
      println(s);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "first\nsecond\n");
}

TEST(Mov, MoveIntoFunctionArgument) {
  auto r = compileAndRun(R"(
    fn shout(s: Str) { println(s); }
    fn main() -> int {
      s: Str = "loud";
      shout(mov s);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "loud\n");
}

TEST(Mov, MoveOutOfFunctionReturn) {
  auto r = compileAndRun(R"(
    fn passthrough(s: Str) -> Str { return mov s; }
    fn main() -> int {
      out: Str = passthrough("relayed");
      println(out);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "relayed\n");
}

TEST(Mov, ConditionalMove) {
  auto r = compileAndRun(R"(
    fn main() -> int {
      s: Str = "cond";
      if (True) {
        t = mov s;
        println(t);
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "cond\n");
}
