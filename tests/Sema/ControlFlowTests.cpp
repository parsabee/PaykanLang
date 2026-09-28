// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: if/else, while, logical operators, ternary, break/continue.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// ============================================================================
// if / else
// ============================================================================

TEST(ControlFlow, IfBoolCondition) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 10;
    if (x > 5) { println("big"); }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, IfElse) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 10;
    if (x > 5) { println("big"); } else { println("small"); }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, IfNonBoolConditionRejected) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 10;
    if (x) { println("nope"); }
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("bool"), std::string::npos);
}

TEST(ControlFlow, IfElseIfChain) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 42;
    if (x > 100) { println("large"); }
    else if (x > 10) { println("medium"); }
    else { println("small"); }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, IfStringConditionRejected) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 1;
    if (x) { println("nope"); }
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("bool"), std::string::npos);
}

// ============================================================================
// while
// ============================================================================

TEST(ControlFlow, WhileBoolCondition) {
  auto r = semaCheck(wrapMain(R"(
    i: int = 0;
    while (i < 5) { i = i + 1; }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, WhileNonBoolConditionRejected) {
  auto r = semaCheck(wrapMain(R"(
    i: int = 0;
    while (i) { i = i + 1; }
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("bool"), std::string::npos);
}

TEST(ControlFlow, WhileTrueLiteral) {
  auto r = semaCheck(wrapMain(R"(
    while (True) { return 0; }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// Logical operators && ||
// ============================================================================

TEST(ControlFlow, LogicalAndBool) {
  auto r = semaCheck(wrapMain("a: bool = True && False;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, LogicalOrBool) {
  auto r = semaCheck(wrapMain("a: bool = True || False;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, LogicalAndWithRelational) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 5;
    a: bool = x > 0 && x < 10;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, LogicalOrWithRelational) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 5;
    a: bool = x < 0 || x > 0;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, LogicalAndNonBoolLhsRejected) {
  auto r = semaCheck(wrapMain("a: bool = 42 && True;"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("bool"), std::string::npos);
}

TEST(ControlFlow, LogicalOrNonBoolRhsRejected) {
  auto r = semaCheck(wrapMain("a: bool = True || 42;"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("bool"), std::string::npos);
}

TEST(ControlFlow, LogicalChained) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 5;
    a: bool = x > 0 && x < 10 || x == 0;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, LogicalInIfCondition) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 5;
    y: int = 10;
    if (x > 0 && y > 0) { println("both positive"); }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, LogicalInWhileCondition) {
  auto r = semaCheck(wrapMain(R"(
    i: int = 0;
    j: int = 10;
    while (i < 5 && j > 0) { i = i + 1; j = j - 1; }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// Ternary expressions
// ============================================================================

TEST(ControlFlow, TernaryIntBranches) {
  auto r = semaCheck(wrapMain("x: int = if True then 1 else 2;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, TernaryBoolBranches) {
  auto r = semaCheck(wrapMain("x: bool = if True then True else False;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, TernaryFloatBranches) {
  auto r = semaCheck(wrapMain("x: float = if True then 1.0 else 2.0;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, TernaryWithRelationalCondition) {
  auto r = semaCheck(wrapMain(R"(
    a: int = 5;
    x: int = if a > 3 then 10 else 20;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, TernaryNonBoolConditionRejected) {
  auto r = semaCheck(wrapMain("x: int = if 1 then 2 else 3;"));
  EXPECT_FALSE(r.Ok);
}

TEST(ControlFlow, TernaryMismatchedBranchesRejected) {
  auto r = semaCheck(wrapMain("x: int = if True then 1 else 2.0;"));
  EXPECT_FALSE(r.Ok);
}

TEST(ControlFlow, TernaryNestedOk) {
  auto r = semaCheck(
      wrapMain("x: int = if True then 1 else if False then 2 else 3;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// break / continue
// ============================================================================

TEST(ControlFlow, BreakInsideLoop) {
  auto r = semaCheck(wrapMain(R"(
    while (True) { break; }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, ContinueInsideLoop) {
  auto r = semaCheck(wrapMain(R"(
    i: int = 0;
    while (i < 10) { i = i + 1; continue; }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, BreakOutsideLoopRejected) {
  auto r = semaCheck(wrapMain("break;"));
  EXPECT_FALSE(r.Ok);
}

TEST(ControlFlow, ContinueOutsideLoopRejected) {
  auto r = semaCheck(wrapMain("continue;"));
  EXPECT_FALSE(r.Ok);
}

TEST(ControlFlow, BreakInIfInsideLoopOk) {
  auto r = semaCheck(wrapMain(R"(
    i: int = 0;
    while (i < 10) {
      if (i == 5) { break; }
      i = i + 1;
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, BreakInIfOutsideLoopRejected) {
  auto r = semaCheck(wrapMain(R"(
    if (True) { break; }
  )"));
  EXPECT_FALSE(r.Ok);
}

TEST(ControlFlow, BreakInNestedLoop) {
  auto r = semaCheck(wrapMain(R"(
    while (True) {
      while (True) { break; }
      break;
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// Return-path analysis: bool-exhaustive match (True + False literal arms)
// ============================================================================

TEST(ControlFlow, BoolMatchTrueFalseArmsIsExhaustiveReturn) {
  auto r = semaCheck(R"(
    fn pick(b: bool) -> int {
      match b {
        True  { return 1; }
        False { return 0; }
      }
    }
    fn main() -> int { return pick(True); }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, BoolMatchMissingFalseArmRejected) {
  auto r = semaCheck(R"(
    fn pick(b: bool) -> int {
      match b {
        True { return 1; }
      }
    }
    fn main() -> int { return pick(True); }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("does not always return"), std::string::npos);
}

TEST(ControlFlow, BoolMatchArmWithoutReturnRejected) {
  auto r = semaCheck(R"(
    fn pick(b: bool) -> int {
      match b {
        True  { return 1; }
        False { println("no"); }
      }
    }
    fn main() -> int { return pick(True); }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("does not always return"), std::string::npos);
}

TEST(ControlFlow, IntMatchLiteralArmsNotExhaustive) {
  // Literal coverage is only decidable for bool subjects.
  auto r = semaCheck(R"(
    fn pick(n: int) -> int {
      match n {
        0 { return 1; }
        1 { return 0; }
      }
    }
    fn main() -> int { return pick(0); }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("does not always return"), std::string::npos);
}
