// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: `mov` move semantics — use-after-move, revival, restrictions.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// -- Well-formed moves -------------------------------------------------------

TEST(Mov, MovePrimitiveOk) {
  auto r = semaCheck(R"(
    fn main() -> int {
      a: int = 3;
      b = mov a;
      return b;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, MoveStringOk) {
  auto r = semaCheck(R"(
    fn main() -> int {
      s: Str = "hi";
      t = mov s;
      println(t);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, MoveTemporaryOk) {
  auto r = semaCheck(R"(
    fn make() -> Str { return "x"; }
    fn main() -> int {
      t = mov make();
      println(t);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Use-after-move errors ---------------------------------------------------

TEST(Mov, UseAfterMoveRead) {
  auto r = semaCheck(R"(
    fn main() -> int {
      a: int = 3;
      b = mov a;
      c = a;
      return c;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("moved variable 'a'"), std::string::npos);
}

TEST(Mov, UseAfterMoveInExpr) {
  auto r = semaCheck(R"(
    fn main() -> int {
      a: int = 3;
      b = mov a;
      return a + 1;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("moved variable 'a'"), std::string::npos);
}

TEST(Mov, DoubleMoveRejected) {
  auto r = semaCheck(R"(
    fn main() -> int {
      s: Str = "hi";
      t = mov s;
      u = mov s;
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("moved variable 's'"), std::string::npos);
}

// -- Revival on re-assignment ------------------------------------------------

TEST(Mov, ReassignRevivesVariable) {
  auto r = semaCheck(R"(
    fn main() -> int {
      s: Str = "hi";
      t = mov s;
      s = "again";
      println(s);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Aggregate-slot moves are rejected ---------------------------------------

TEST(Mov, MoveMemberVariableRejected) {
  auto r = semaCheck(R"(
    class Box { v: Str;
      fn __init__(s: Str) { self.v = s; }
    }
    fn main() -> int {
      b: Box = Box("hi");
      x = mov b.v;
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("member variable"), std::string::npos);
}

TEST(Mov, MoveArrayElementRejected) {
  auto r = semaCheck(R"(
    fn main() -> int {
      a: int[] = [1, 2, 3];
      x = mov a[0];
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("array element"), std::string::npos);
}

// -- `mov self` is rejected --------------------------------------------------

TEST(Mov, MoveSelfRejected) {
  auto r = semaCheck(R"(
    class A {
      x: int;
      fn __init__() { self.x = 7; }
      fn grab() -> int {
        y = mov self;
        return 0;
      }
    }
    fn main() -> int {
      a: A = A();
      return a.grab();
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("cannot 'mov' 'self'"), std::string::npos);
}

TEST(Mov, MoveParameterOk) {
  // Ordinary parameters are owned by the callee frame and may be moved
  // (unlike `self`, which is a borrowed reference).
  auto r = semaCheck(R"(
    fn consume(s: Str) { println(s); }
    fn pass(s: Str) { consume(mov s); }
    fn main() -> int {
      pass("hi");
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Loops: back-edge soundness ----------------------------------------------

TEST(Mov, MoveInLoopDeclaredOutsideRejected) {
  auto r = semaCheck(R"(
    fn consume(s: Str) { println(s); }
    fn main() -> int {
      s: Str = "x";
      i: int = 0;
      while (i < 2) {
        consume(mov s);
        i = i + 1;
      }
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("declared outside the loop"), std::string::npos);
}

TEST(Mov, MoveInLoopConditionRejected) {
  // The condition also re-executes every iteration.
  auto r = semaCheck(R"(
    fn check(s: Str) -> bool { return False; }
    fn main() -> int {
      s: Str = "x";
      while (check(mov s)) {
        println("body");
      }
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("declared outside the loop"), std::string::npos);
}

TEST(Mov, MoveInLoopReassignedBeforeBackEdgeOk) {
  auto r = semaCheck(R"(
    fn consume(s: Str) { println(s); }
    fn main() -> int {
      s: Str = "x";
      i: int = 0;
      while (i < 2) {
        consume(mov s);
        s = "again";
        i = i + 1;
      }
      println(s);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, MoveInLoopReassignedOnlySomePathsRejected) {
  // The re-assignment is conditional, so the back edge may still see the
  // variable moved.
  auto r = semaCheck(R"(
    fn consume(s: Str) { println(s); }
    fn main() -> int {
      s: Str = "x";
      i: int = 0;
      while (i < 2) {
        consume(mov s);
        if (i > 0) { s = "again"; }
        i = i + 1;
      }
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("declared outside the loop"), std::string::npos);
}

TEST(Mov, MoveInLoopDeclaredInsideOk) {
  // A loop-local variable is re-declared fresh on every iteration.
  auto r = semaCheck(R"(
    fn consume(s: Str) { println(s); }
    fn main() -> int {
      i: int = 0;
      while (i < 2) {
        s: Str = "x";
        consume(mov s);
        i = i + 1;
      }
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, MoveBeforeLoopStaysMovedInsideRejected) {
  auto r = semaCheck(R"(
    fn main() -> int {
      s: Str = "x";
      t = mov s;
      i: int = 0;
      while (i < 2) {
        println(s);
        i = i + 1;
      }
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("moved variable 's'"), std::string::npos);
}

// -- Branches: per-path move state -------------------------------------------

TEST(Mov, MoveInThenDoesNotPoisonElse) {
  auto r = semaCheck(R"(
    fn consume(s: Str) { println(s); }
    fn main() -> int {
      s: Str = "x";
      f: bool = True;
      if (f) {
        consume(mov s);
      } else {
        println(s);
      }
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, MoveInMatchArmDoesNotPoisonSiblingArm) {
  auto r = semaCheck(R"(
    fn consume(s: Str) { println(s); }
    fn main() -> int {
      s: Str = "x";
      n: int = 1;
      match n {
        1 { consume(mov s); }
        _ { println(s); }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, MoveInBranchStillMovedAfterConstruct) {
  // Moved on ANY path => moved after the construct (conservative union).
  auto r = semaCheck(R"(
    fn consume(s: Str) { println(s); }
    fn main() -> int {
      s: Str = "x";
      f: bool = True;
      if (f) { consume(mov s); }
      println(s);
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("moved variable 's'"), std::string::npos);
}

TEST(Mov, MoveRevivedInBothBranchesOk) {
  // If BOTH branches of an if/else re-assign, the union is revived.
  auto r = semaCheck(R"(
    fn main() -> int {
      s: Str = "x";
      t = mov s;
      f: bool = True;
      if (f) { s = "a"; } else { s = "b"; }
      println(s);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, MoveRevivedInOnlyThenBranchRejected) {
  // Without an else, the skip path keeps the variable moved.
  auto r = semaCheck(R"(
    fn main() -> int {
      s: Str = "x";
      t = mov s;
      f: bool = True;
      if (f) { s = "a"; }
      println(s);
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("moved variable 's'"), std::string::npos);
}
