// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen tests: handing a reference on in the positions `mov` used to mark
// (retired in #145).  Output and leak balance must hold without the keyword.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

using namespace paykan::test;

TEST(Transfer, PrimitiveForwardsValue) {
  auto r = compileAndRun(R"(
    fn main() -> int {
      a: int = 42;
      b = a;
      println(Str<int>(b));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "42\n");
}

TEST(Transfer, StringOwnershipTransfer) {
  auto r = compileAndRun(R"(
    fn main() -> int {
      s: Str = "Hello world";
      t = s;
      println(t);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Hello world\n");
}

TEST(Transfer, MoveTemporaryIntoVariable) {
  auto r = compileAndRun(R"(
    fn make() -> Str { return "made"; }
    fn main() -> int {
      t = make();
      println(t);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "made\n");
}

TEST(Transfer, MoveClassObject) {
  auto r = compileAndRun(R"(
    class Point { x: int; y: int;
      fn __init__(a: int, b: int) { self.x = a; self.y = b; }
      fn sum() -> int { return self.x + self.y; }
    }
    fn main() -> int {
      p: Point = Point(3, 4);
      q = p;
      println(Str<int>(q.sum()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "7\n");
}

TEST(Transfer, MoveThenRevive) {
  auto r = compileAndRun(R"(
    fn main() -> int {
      s: Str = "first";
      t = s;
      s = "second";
      println(t);
      println(s);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "first\nsecond\n");
}

TEST(Transfer, MoveIntoFunctionArgument) {
  auto r = compileAndRun(R"(
    fn shout(s: Str) { println(s); }
    fn main() -> int {
      s: Str = "loud";
      shout(s);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "loud\n");
}

TEST(Transfer, MoveOutOfFunctionReturn) {
  auto r = compileAndRun(R"(
    fn passthrough(s: Str) -> Str { return s; }
    fn main() -> int {
      out: Str = passthrough("relayed");
      println(out);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "relayed\n");
}

TEST(Transfer, ConditionalMove) {
  auto r = compileAndRun(R"(
    fn main() -> int {
      s: Str = "cond";
      if (True) {
        t = s;
        println(t);
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "cond\n");
}

// -- A hand-off inside a ternary branch
//
// Whichever branch runs, the result must own exactly one reference and the
// source variable must be released exactly once overall: never twice (a
// double free) and never zero times (a leak).  The un-taken branch is never
// executed, so it must not release anything either.

TEST(Transfer, TernaryMoveTakenBranch) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      x: Str = "moved";
      c: bool = True;
      y = if c then x else x;
      println(y);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "moved\n");
  g.expectNoLeaks("TernaryMoveTakenBranch");
}

TEST(Transfer, TernaryMoveUntakenBranch) {
  // The else branch shares x (retain); x is still owned by its own slot and
  // released at scope exit, so both y and x drop their reference exactly once.
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      x: Str = "shared";
      c: bool = False;
      y = if c then x else x;
      println(y);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "shared\n");
  g.expectNoLeaks("TernaryMoveUntakenBranch");
}

TEST(Transfer, TernaryMoveInElseBranch) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      x: Str = "else";
      c: bool = False;
      y = if c then x else x;
      println(y);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "else\n");
  g.expectNoLeaks("TernaryMoveInElseBranch");
}

TEST(Transfer, TernaryMoveInBothBranches) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn pick(c: bool) -> Str {
      x: Str = "both";
      y = if c then x else x;
      return y;
    }
    fn main() -> int {
      println(pick(True));
      println(pick(False));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "both\nboth\n");
  g.expectNoLeaks("TernaryMoveInBothBranches");
}

TEST(Transfer, TernaryMoveClassObjectBothWays) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Point { x: int; y: int;
      fn __init__(a: int, b: int) { self.x = a; self.y = b; }
      fn sum() -> int { return self.x + self.y; }
    }
    fn pick(c: bool) -> int {
      p: Point = Point(1, 2);
      q: Point = Point(10, 20);
      r: Point = if c then p else q;
      return r.sum();
    }
    fn main() -> int {
      println(Str<int>(pick(True)));
      println(Str<int>(pick(False)));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "3\n30\n");
  g.expectNoLeaks("TernaryMoveClassObjectBothWays");
}

TEST(Transfer, TernaryMoveInCondition) {
  // The condition hands x on, on every path; the result is a primitive.
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn take(s: Str) -> bool { println(s); return True; }
    fn main() -> int {
      x: Str = "cond";
      n: int = if take(x) then 1 else 2;
      println(Str<int>(n));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "cond\n1\n");
  g.expectNoLeaks("TernaryMoveInCondition");
}

TEST(Transfer, TernaryMoveIntoCallInBranch) {
  // A primitive-typed ternary whose branch hands a ref var to a call.
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn take(s: Str) -> int { println(s); return 1; }
    fn run(c: bool) -> int {
      x: Str = "arg";
      n: int = if c then take(x) else 0;
      return n;
    }
    fn main() -> int {
      println(Str<int>(run(True)));
      println(Str<int>(run(False)));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "arg\n1\n0\n");
  g.expectNoLeaks("TernaryMoveIntoCallInBranch");
}

// -- A hand-off inside the short-circuit RHS of `&&` / `||`

TEST(Transfer, AndMoveInRhsEvaluatedAndSkipped) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn take(s: Str) -> bool { println(s); return True; }
    fn run(c: bool) -> bool {
      x: Str = "rhs";
      ok: bool = c && take(x);
      return ok;
    }
    fn main() -> int {
      println(Str<bool>(run(True)));
      println(Str<bool>(run(False)));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "rhs\nTrue\nFalse\n");
  g.expectNoLeaks("AndMoveInRhsEvaluatedAndSkipped");
}

TEST(Transfer, OrMoveInRhsEvaluatedAndSkipped) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn take(s: Str) -> bool { println(s); return False; }
    fn run(c: bool) -> bool {
      x: Str = "rhs";
      ok: bool = c || take(x);
      return ok;
    }
    fn main() -> int {
      println(Str<bool>(run(False)));
      println(Str<bool>(run(True)));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "rhs\nFalse\nTrue\n");
  g.expectNoLeaks("OrMoveInRhsEvaluatedAndSkipped");
}

TEST(Transfer, AndMoveInLhsThenRhsRuns) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn take(s: Str) -> bool { println(s); return True; }
    fn main() -> int {
      x: Str = "lhs";
      c: bool = True;
      ok: bool = take(x) && c;
      println(Str<bool>(ok));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "lhs\nTrue\n");
  g.expectNoLeaks("AndMoveInLhsThenRhsRuns");
}

// `None` where `mov None` was is still the absent optional (#119).
TEST(Transfer, MovNoneIsTheAbsentOptional) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Box { s: Str?; fn __init__() { self.s = None; } }
    fn show(s: Str?) {
      match s { v: Str { println(v); } None { println("none"); } }
    }
    fn give() -> int? { return None; }
    fn main() -> int {
      x: Str? = None;
      show(x);
      x = "some";
      show(x);
      x = None;
      show(x);
      b = Box();
      show(b.s);
      show(None);
      n: int? = give();
      match n { v: int { println("int"); } None { println("none"); } }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "none\nsome\nnone\nnone\nnone\nnone\n");
  g.expectNoLeaks("MovNoneIsTheAbsentOptional");
}

// `None` inside a literal, where `mov None` was, is the absent value of the
// slot's element type (#132).
TEST(Transfer, MovNoneInsideALiteralIsTheAbsentOptional) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn show(s: Str?) {
      match s { v: Str { println(v); } None { println("none"); } }
    }
    fn main() -> int {
      xs: Str?[] = [None, "a"];
      show(xs[0]);
      show(xs[1]);
      t: (Str?, int) = (None, 7);
      show(t.0);
      println(Str(t.1));
      n: (int?, Str?)[] = [(None, None)];
      match n[0].0 { v: int { println("int"); } None { println("none"); } }
      show(n[0].1);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "none\na\nnone\n7\nnone\nnone\n");
  g.expectNoLeaks("MovNoneInsideALiteralIsTheAbsentOptional");
}
