// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: `mov` is retired (#145) and parses as its operand, so every
// former move-checking program here now type-checks.  They stay as the shapes
// the last-use ownership pass (#186) must handle.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// -- Well-formed moves

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

// -- Use after a former move

TEST(Mov, UseAfterMoveReadAccepted) {
  auto r = semaCheck(R"(
    fn main() -> int {
      a: int = 3;
      b = mov a;
      c = a;
      return c;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, UseAfterMoveInExprAccepted) {
  auto r = semaCheck(R"(
    fn main() -> int {
      a: int = 3;
      b = mov a;
      return a + 1;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, DoubleMoveAccepted) {
  auto r = semaCheck(R"(
    fn main() -> int {
      s: Str = "hi";
      t = mov s;
      u = mov s;
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Revival on re-assignment

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

// -- Aggregate-slot operands

TEST(Mov, MoveMemberVariableAccepted) {
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
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, MoveArrayElementAccepted) {
  auto r = semaCheck(R"(
    fn main() -> int {
      a: int[] = [1, 2, 3];
      x = mov a[0];
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- `mov self`

TEST(Mov, MoveSelfAccepted) {
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
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
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

// -- Loops

TEST(Mov, MoveInLoopDeclaredOutsideAccepted) {
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
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, MoveInLoopConditionAccepted) {
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
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
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

TEST(Mov, MoveInLoopReassignedOnlySomePathsAccepted) {
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
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
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

TEST(Mov, MoveBeforeLoopStaysMovedInsideAccepted) {
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
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Branches

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

TEST(Mov, MoveInBranchStillMovedAfterConstructAccepted) {
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
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
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

TEST(Mov, MoveRevivedInOnlyThenBranchAccepted) {
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
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Ternary expression: then/else are sibling branches

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

TEST(Mov, TernaryMoveInBothBranchesThenUseAccepted) {
  auto r = semaCheck(R"(
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      y = if c then mov x else mov x;
      println(x);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, TernaryMoveInOneBranchThenUseAccepted) {
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
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, TernaryMoveInElseThenUseAccepted) {
  auto r = semaCheck(R"(
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      y = if c then x else mov x;
      println(x);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, TernaryMoveInConditionVisibleInBothBranchesAccepted) {
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
  EXPECT_TRUE(r.Ok) << r.Diagnostics;

  auto r2 = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      y = if take(mov x) then "a" else x;
      println(y);
      return 0;
    }
  )");
  EXPECT_TRUE(r2.Ok) << r2.Diagnostics;
}

TEST(Mov, TernaryMoveInConditionStaysMovedAfterAccepted) {
  auto r = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      n: int = if take(mov x) then 1 else 2;
      println(x);
      return n;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
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

// -- Short-circuit `&&` / `||`

TEST(Mov, AndMoveInLhsVisibleInRhsAccepted) {
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
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, OrMoveInLhsVisibleInRhsAccepted) {
  auto r = semaCheck(R"(
    fn take(s: Str) -> bool { return False; }
    fn peek(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      ok: bool = take(mov x) || peek(x);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, AndMoveInLhsStaysMovedAfterAccepted) {
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
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, AndMoveInRhsStaysMovedAfterAccepted) {
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
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, OrMoveInRhsStaysMovedAfterAccepted) {
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
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
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

TEST(Mov, AndRhsMoveDoesNotPoisonSiblingTernaryBranchAccepted) {
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
  EXPECT_TRUE(r.Ok) << r.Diagnostics;

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

// -- Call arguments are evaluated left to right

TEST(Mov, MoveThenReuseInSameCallAccepted) {
  auto r = semaCheck(R"(
    fn two(a: Str, b: Str) { println(a); println(b); }
    fn main() -> int {
      x: Str = "x";
      two(mov x, x);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
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

// -- `mov` keeps the contextual type of its operand (#119)

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

// `mov None` inside an array or tuple literal takes the slot's element type,
// as `None` there does (#132).
TEST(Mov, MovNoneInsideALiteralTakesTheElementType) {
  auto r = semaCheck(R"(
    fn take(xs: Str?[]) -> int { return xs.len(); }
    fn main() -> int {
      xs: Str?[] = [mov None];
      t: (Str?, int) = (mov None, 1);
      ys: Str?[] = [mov None, "a", None];
      n: (int?, Str?)[] = [(mov None, mov None), (1, "b")];
      zs: Str?[][] = [[mov None], mov [mov None]];
      xs = [mov None, mov None];
      return take([mov None]);
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
}

TEST(Mov, MovNoneInsideALiteralForANonOptionalSlotIsOneError) {
  for (const char *decl :
       {"xs: Str[] = [mov None];", "t: (Str, int) = (mov None, 1);"}) {
    auto r =
        semaCheck(std::string("fn main() -> int { ") + decl + " return 0; }");
    EXPECT_FALSE(r.Ok) << decl;
    EXPECT_EQ(r.ErrorCount, 1u) << decl << "\n" << r.Diagnostics;
  }
}
