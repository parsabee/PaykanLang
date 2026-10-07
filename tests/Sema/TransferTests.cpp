// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: programs that `mov` used to reject or constrain (retired in
// #145) all type-check now.  Each test name records the old move it stood
// for; the programs stay as the shapes the last-use pass (#186) must handle.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// -- Short-circuit `&&` / `||`

TEST(Transfer, AndMoveInLhsVisibleInRhsAccepted) {
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

TEST(Transfer, OrMoveInLhsVisibleInRhsAccepted) {
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

TEST(Transfer, AndMoveInLhsStaysMovedAfterAccepted) {
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

TEST(Transfer, AndMoveInRhsStaysMovedAfterAccepted) {
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

TEST(Transfer, OrMoveInRhsStaysMovedAfterAccepted) {
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

TEST(Transfer, AndMoveInRhsNoLaterUseOk) {
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

TEST(Transfer, OrMoveInRhsNoLaterUseOk) {
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

TEST(Transfer, AndMoveInRhsThenRevivedOk) {
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

TEST(Transfer, AndRhsMoveDoesNotPoisonSiblingTernaryBranchAccepted) {
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

TEST(Transfer, MoveThenReuseInSameCallAccepted) {
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

TEST(Transfer, UseThenMoveInSameCallOk) {
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

TEST(Transfer, MovNoneIntoAnOptionalSlotOk) {
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

TEST(Transfer, MovNoneIntoANonOptionalSlotIsOneError) {
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
TEST(Transfer, MovNoneInsideALiteralTakesTheElementType) {
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

TEST(Transfer, MovNoneInsideALiteralForANonOptionalSlotIsOneError) {
  for (const char *decl :
       {"xs: Str[] = [None];", "t: (Str, int) = (None, 1);"}) {
    auto r =
        semaCheck(std::string("fn main() -> int { ") + decl + " return 0; }");
    EXPECT_FALSE(r.Ok) << decl;
    EXPECT_EQ(r.ErrorCount, 1u) << decl << "\n" << r.Diagnostics;
  }
}
