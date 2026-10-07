// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: programs that `mov` used to reject or constrain (retired in
// #145) all type-check now.  Each test name records the old move it stood
// for; the programs stay as the shapes the last-use pass (#186) must handle.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// -- Plain hand-offs

TEST(Mov, MovePrimitiveOk) {
  auto r = semaCheck(R"(
    fn main() -> int {
      a: int = 3;
      b = a;
      return b;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, MoveStringOk) {
  auto r = semaCheck(R"(
    fn main() -> int {
      s: Str = "hi";
      t = s;
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
      t = make();
      println(t);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Use after a hand-off

TEST(Mov, UseAfterMoveReadAccepted) {
  auto r = semaCheck(R"(
    fn main() -> int {
      a: int = 3;
      b = a;
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
      b = a;
      return a + 1;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, DoubleMoveAccepted) {
  auto r = semaCheck(R"(
    fn main() -> int {
      s: Str = "hi";
      t = s;
      u = s;
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Re-assignment after a hand-off

TEST(Mov, ReassignRevivesVariable) {
  auto r = semaCheck(R"(
    fn main() -> int {
      s: Str = "hi";
      t = s;
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
      x = b.v;
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, MoveArrayElementAccepted) {
  auto r = semaCheck(R"(
    fn main() -> int {
      a: int[] = [1, 2, 3];
      x = a[0];
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- `self` and parameters

TEST(Mov, MoveSelfAccepted) {
  auto r = semaCheck(R"(
    class A {
      x: int;
      fn __init__() { self.x = 7; }
      fn grab() -> int {
        y = self;
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
  auto r = semaCheck(R"(
    fn consume(s: Str) { println(s); }
    fn pass(s: Str) { consume(s); }
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
        consume(s);
        i = i + 1;
      }
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, MoveInLoopConditionAccepted) {
  auto r = semaCheck(R"(
    fn check(s: Str) -> bool { return False; }
    fn main() -> int {
      s: Str = "x";
      while (check(s)) {
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
        consume(s);
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
  auto r = semaCheck(R"(
    fn consume(s: Str) { println(s); }
    fn main() -> int {
      s: Str = "x";
      i: int = 0;
      while (i < 2) {
        consume(s);
        if (i > 0) { s = "again"; }
        i = i + 1;
      }
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, MoveInLoopDeclaredInsideOk) {
  auto r = semaCheck(R"(
    fn consume(s: Str) { println(s); }
    fn main() -> int {
      i: int = 0;
      while (i < 2) {
        s: Str = "x";
        consume(s);
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
      t = s;
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
        consume(s);
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
        1 { consume(s); }
        _ { println(s); }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, MoveInBranchStillMovedAfterConstructAccepted) {
  auto r = semaCheck(R"(
    fn consume(s: Str) { println(s); }
    fn main() -> int {
      s: Str = "x";
      f: bool = True;
      if (f) { consume(s); }
      println(s);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, MoveRevivedInBothBranchesOk) {
  auto r = semaCheck(R"(
    fn main() -> int {
      s: Str = "x";
      t = s;
      f: bool = True;
      if (f) { s = "a"; } else { s = "b"; }
      println(s);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, MoveRevivedInOnlyThenBranchAccepted) {
  auto r = semaCheck(R"(
    fn main() -> int {
      s: Str = "x";
      t = s;
      f: bool = True;
      if (f) { s = "a"; }
      println(s);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Ternary expression

TEST(Mov, TernaryMoveInThenDoesNotPoisonElse) {
  auto r = semaCheck(R"(
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      y = if c then x else x;
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
      y = if c then x else x;
      println(y);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, TernaryMoveInBothBranchesOk) {
  auto r = semaCheck(R"(
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      y = if c then x else x;
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
      y = if c then x else x;
      println(x);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, TernaryMoveInOneBranchThenUseAccepted) {
  auto r = semaCheck(R"(
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      y = if c then x else x;
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
      y = if c then x else x;
      println(x);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, TernaryMoveInConditionVisibleInBothBranchesAccepted) {
  auto r = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      y = if take(x) then x else "b";
      println(y);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;

  auto r2 = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      y = if take(x) then "a" else x;
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
      n: int = if take(x) then 1 else 2;
      println(x);
      return n;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, TernaryNestedBranchesIsolated) {
  auto r = semaCheck(R"(
    fn main() -> int {
      x: Str = "x";
      a: bool = True;
      b: bool = False;
      y = if a then x else if b then x else x;
      println(y);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, TernaryMoveInsideStatementBranchIsolated) {
  auto r = semaCheck(R"(
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      if (c) {
        y = if c then x else x;
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
  auto r = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn peek(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      ok: bool = take(x) && peek(x);
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
      ok: bool = take(x) || peek(x);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, AndMoveInLhsStaysMovedAfterAccepted) {
  auto r = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      ok: bool = take(x) && c;
      println(x);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, AndMoveInRhsStaysMovedAfterAccepted) {
  auto r = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      ok: bool = c && take(x);
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
      ok: bool = c || take(x);
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
      ok: bool = c && take(x);
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
      ok: bool = c || take(x);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, AndMoveInRhsThenRevivedOk) {
  auto r = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      ok: bool = c && take(x);
      x = "again";
      println(x);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Mov, AndRhsMoveDoesNotPoisonSiblingTernaryBranchAccepted) {
  auto r = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      n: int = if (c && take(x)) then 1 else 2;
      println(x);
      return n;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;

  auto r2 = semaCheck(R"(
    fn take(s: Str) -> bool { return True; }
    fn main() -> int {
      x: Str = "x";
      c: bool = True;
      ok: bool = if c then (c && take(x)) else take(x);
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
      two(x, x);
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
      two(x, x);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- `None` where `mov None` was: still contextually typed (#119)

TEST(Mov, MovNoneIntoAnOptionalSlotOk) {
  auto r = semaCheck(R"(
    class Box { s: Str?; fn __init__() { self.s = None; } }
    fn take(s: Str?) -> int { return 0; }
    fn give() -> int? { return None; }
    fn main() -> int {
      x: Str? = None;
      x = None;
      n: int? = None;
      xs: Str?[] = [None, "a"];
      t: (int?, Str) = (None, "b");
      b = Box();
      b.s = None;
      return take(None);
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
}

TEST(Mov, MovNoneIntoANonOptionalSlotIsOneError) {
  auto r = semaCheck(R"(
    fn main() -> int {
      x: Str = None;
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
}

// `None` inside an array or tuple literal, where `mov None` was, takes the
// slot's element type (#132).
TEST(Mov, MovNoneInsideALiteralTakesTheElementType) {
  auto r = semaCheck(R"(
    fn take(xs: Str?[]) -> int { return xs.len(); }
    fn main() -> int {
      xs: Str?[] = [None];
      t: (Str?, int) = (None, 1);
      ys: Str?[] = [None, "a", None];
      n: (int?, Str?)[] = [(None, None), (1, "b")];
      zs: Str?[][] = [[None], [None]];
      xs = [None, None];
      return take([None]);
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
}

TEST(Mov, MovNoneInsideALiteralForANonOptionalSlotIsOneError) {
  for (const char *decl :
       {"xs: Str[] = [None];", "t: (Str, int) = (None, 1);"}) {
    auto r =
        semaCheck(std::string("fn main() -> int { ") + decl + " return 0; }");
    EXPECT_FALSE(r.Ok) << decl;
    EXPECT_EQ(r.ErrorCount, 1u) << decl << "\n" << r.Diagnostics;
  }
}
