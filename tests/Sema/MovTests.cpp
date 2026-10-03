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

// -- Ternary expression: then/else are sibling branches ----------------------

TEST(Mov, TernaryMoveInThenDoesNotPoisonElse) {
  // The else branch can only run instead of the then branch, so a `mov` in
  // the then branch must not be visible there.
  auto r = semaCheck(R"(
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      y = if c then mov x else x;
      println(y);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, TernaryMoveInElseDoesNotPoisonThen) {
  auto r = semaCheck(R"(
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      y = if c then x else mov x;
      println(y);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, TernaryMoveInBothBranchesOk) {
  // Moving the same variable on both paths is fine: exactly one path runs.
  auto r = semaCheck(R"(
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      y = if c then mov x else mov x;
      println(y);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, TernaryMoveInBothBranchesThenUseRejected) {
  auto r = semaCheck(R"(
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      y = if c then mov x else mov x;
      println(x);
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("moved variable 'x'"), std::string::npos);
}

TEST(Mov, TernaryMoveInOneBranchThenUseRejected) {
  // Moved on ANY path => moved after the expression (conservative union).
  auto r = semaCheck(R"(
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      y = if c then mov x else x;
      println(x);
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("moved variable 'x'"), std::string::npos);
}

TEST(Mov, TernaryMoveInElseThenUseRejected) {
  auto r = semaCheck(R"(
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      y = if c then x else mov x;
      println(x);
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("moved variable 'x'"), std::string::npos);
}

TEST(Mov, TernaryMoveInConditionVisibleInBothBranches) {
  // The condition always runs before either branch, so its moves poison both.
  auto r = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      y = if take(mov x) then x else "b";
      println(y);
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("moved variable 'x'"), std::string::npos);

  auto r2 = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      y = if take(mov x) then "a" else x;
      println(y);
      return 0;
    }
  )");
  EXPECT_FALSE(r2.Ok);
  EXPECT_NE(r2.Diagnostics.find("moved variable 'x'"), std::string::npos);
}

TEST(Mov, TernaryMoveInConditionStaysMovedAfter) {
  auto r = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      n: int = if take(mov x) then 1 else 2;
      println(x);
      return n;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("moved variable 'x'"), std::string::npos);
}

TEST(Mov, TernaryNestedBranchesIsolated) {
  // Nested ternaries: every leaf is its own path from the outer condition.
  auto r = semaCheck(R"(
    fn main() -> int {
      x: Str = "x";
      a: bool = True;
      b: bool = False;
      y = if a then mov x else if b then mov x else x;
      println(y);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, TernaryMoveInsideStatementBranchIsolated) {
  // A ternary move inside one if-branch does not leak into the sibling
  // statement branch either.
  auto r = semaCheck(R"(
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      if (c) {
        y = if c then mov x else x;
        println(y);
      } else {
        println(x);
      }
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Short-circuit `&&` / `||`: the RHS is a conditional branch ---------------

TEST(Mov, AndMoveInLhsVisibleInRhs) {
  // The LHS always runs before the RHS.
  auto r = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn peek(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      ok: bool = take(mov x) && peek(x);
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("moved variable 'x'"), std::string::npos);
}

TEST(Mov, OrMoveInLhsVisibleInRhs) {
  auto r = semaCheck(R"(
    fn take(s: Str) -> bool { return False; }
    fn peek(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      ok: bool = take(mov x) || peek(x);
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("moved variable 'x'"), std::string::npos);
}

TEST(Mov, AndMoveInLhsStaysMovedAfter) {
  // A move in the LHS is definite.
  auto r = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      ok: bool = take(mov x) && c;
      println(x);
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("moved variable 'x'"), std::string::npos);
}

TEST(Mov, AndMoveInRhsStaysMovedAfter) {
  // A move in the RHS may or may not have happened, so it counts as moved.
  auto r = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      ok: bool = c && take(mov x);
      println(x);
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("moved variable 'x'"), std::string::npos);
}

TEST(Mov, OrMoveInRhsStaysMovedAfter) {
  auto r = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      c: bool = False;
      ok: bool = c || take(mov x);
      println(x);
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("moved variable 'x'"), std::string::npos);
}

TEST(Mov, AndMoveInRhsNoLaterUseOk) {
  auto r = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      ok: bool = c && take(mov x);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, OrMoveInRhsNoLaterUseOk) {
  auto r = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      c: bool = False;
      ok: bool = c || take(mov x);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, AndMoveInRhsThenRevivedOk) {
  // Re-assignment after the expression revives the name as usual.
  auto r = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      ok: bool = c && take(mov x);
      x = "again";
      println(x);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, AndRhsMoveDoesNotPoisonSiblingTernaryBranch) {
  // An `&&` inside a ternary's then-branch stays confined to that branch.
  auto r = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      n: int = if (c && take(mov x)) then 1 else 2;
      println(x);
      return n;
    }
  )");
  // The `&&` is in the CONDITION here, so x is moved for both branches and
  // afterwards.
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("moved variable 'x'"), std::string::npos);

  auto r2 = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      ok: bool = if c then (c && take(mov x)) else take(x);
      return 0;
    }
  )");
  EXPECT_TRUE(r2.Ok) << r2.Diagnostics;
}

// -- Call arguments are evaluated left to right ------------------------------

TEST(Mov, MoveThenReuseInSameCallRejected) {
  auto r = semaCheck(R"(
    fn two(a: Str, b: Str) { println(a); println(b); }
    fn main() -> int {
      x: Str = "x";
      two(mov x, x);
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("moved variable 'x'"), std::string::npos);
}

TEST(Mov, UseThenMoveInSameCallOk) {
  auto r = semaCheck(R"(
    fn two(a: Str, b: Str) { println(a); println(b); }
    fn main() -> int {
      x: Str = "x";
      two(x, mov x);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- `mov` keeps the contextual type of its operand (#119) -------------------

TEST(Mov, MovNoneIntoAnOptionalSlotOk) {
  auto r = semaCheck(R"(
    class Box { s: Str?; fn __init__() { self.s = mov None; } }
    fn take(s: Str?) -> int { return 0; }
    fn give() -> int? { return mov None; }
    fn main() -> int {
      x: Str? = mov None;
      x = mov None;
      n: int? = mov None;
      xs: Str?[] = mov [None, "a"];
      t: (int?, Str) = mov (None, "b");
      b = Box();
      b.s = mov None;
      return take(mov None);
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
}

TEST(Mov, MovNoneIntoANonOptionalSlotIsOneError) {
  auto r = semaCheck(R"(
    fn main() -> int {
      x: Str = mov None;
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
}
