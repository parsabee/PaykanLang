// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: if/else, while, for, logical operators, ternary,
// break/continue.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "import ::io; fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// -- if / else

TEST(ControlFlow, IfBoolCondition) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 10;
    if x > 5 { io::println("big"); }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, IfElse) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 10;
    if x > 5 { io::println("big"); } else { io::println("small"); }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, IfNonBoolConditionRejected) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 10;
    if x { io::println("nope"); }
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("bool"), std::string::npos);
}

TEST(ControlFlow, IfElseIfChain) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 42;
    if x > 100 { io::println("large"); }
    elif x > 10 { io::println("medium"); }
    else { io::println("small"); }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, IfStringConditionRejected) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 1;
    if x { io::println("nope"); }
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("bool"), std::string::npos);
}

// -- while

TEST(ControlFlow, WhileBoolCondition) {
  auto r = semaCheck(wrapMain(R"(
    i: int = 0;
    while i < 5 { i = i + 1; }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, WhileNonBoolConditionRejected) {
  auto r = semaCheck(wrapMain(R"(
    i: int = 0;
    while i { i = i + 1; }
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("bool"), std::string::npos);
}

TEST(ControlFlow, WhileTrueLiteral) {
  auto r = semaCheck(wrapMain(R"(
    while True { return 0; }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- for

TEST(ControlFlow, ForRangeTypes) {
  // A literal end takes the other end's type, as an operand of `<` does;
  // the annotation types both; the step is an `int` whatever they are.
  auto r = semaCheck(wrapMain(R"(
    n: unsigned = 3;
    for i in 0..n { u: unsigned = i; }
    for b: u8 in 0..=255 step 5 { x: u8 = b; }
    for k in n..0 step -1 { }
    by = 2;
    for j in 10..0 step -by { y: int = j; }
    for i in 0..3 { for i in i..3 { } }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// The bounds are checked outside the loop: `i` there is the outer one.
TEST(ControlFlow, ForBoundsSeeTheOuterScope) {
  auto r = semaCheck(wrapMain(R"(
    i: Str = "outer";
    for i in 0..3 { n: int = i; }
    s: Str = i;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  auto bad = semaCheck(wrapMain("for i in 0..i { }"));
  EXPECT_FALSE(bad.Ok);
  EXPECT_NE(bad.Diagnostics.find("'i'"), std::string::npos) << bad.Diagnostics;
}

// `for _` binds nothing: it hides no outer name and nests.
TEST(ControlFlow, ForDiscardsItsVariable) {
  auto r = semaCheck(wrapMain(R"(
    n: unsigned = 3;
    count: unsigned = 0;
    for _ in 0..n { count = count + 1; }
    for _: u8 in 0..=255 step 5 { }
    for _ in 0..3 { for _ in 0..2 { for i in 0..1 { y: int = i; } } }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, ForVariableIsReadOnly) {
  auto r = semaCheck("fn bump(x: inout int) { x = x + 1; }\n" + wrapMain(R"(
    for i in 0..3 { i = 2; }
    for i in 0..3 { bump(i); }
    for i in 0..3 { f = fn() -> int { return i; }; }
  )"));
  EXPECT_FALSE(r.Ok);
  for (const char *e :
       {"'i' is the variable of a 'for' loop and cannot be assigned",
        "'i' is the variable of a 'for' loop and cannot be passed to 'inout' "
        "parameter 'x'"})
    EXPECT_NE(r.Diagnostics.find(e), std::string::npos) << e << "\n"
                                                        << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
}

TEST(ControlFlow, ForRangeErrors) {
  struct Case {
    const char *Loop;
    const char *Error;
  };
  for (const Case &c :
       {Case{"for f in 0.5..3.0 { }",
             "a 'for' loop's range is of integers, not 'float'"},
        Case{"for s in 0..\"x\" { }",
             "a 'for' loop's range is of integers, not 'Str'"},
        Case{"n: unsigned = 1; m = 4; for i in n..m { }",
             "the ends of the range have types 'unsigned' and 'int'; a 'for' "
             "loop's range has one type"},
        Case{"m = 4; for b: u8 in 0..m { }",
             "the end of the range has type 'int', but 'b' is a 'u8'"},
        Case{"m = 4; for b: u8 in m..9 { }",
             "the start of the range has type 'int', but 'b' is a 'u8'"},
        Case{"for b: u8 in 0..=300 { }",
             "the literal 300 does not fit in 'u8'"},
        Case{"for f: float in 0..3 { }",
             "'f' has type 'float': a 'for' loop's variable is an integer"},
        Case{"for i in 0..3 step 0 { }", "a 'for' loop's step cannot be 0"},
        Case{"for i in 0..3 step -0 { }", "a 'for' loop's step cannot be 0"},
        Case{"for i in 0..3 step 1.5 { }",
             "a 'for' loop's step is an 'int', not 'float'"},
        Case{"k: u8 = 1; for i in 0..3 step k { }",
             "a 'for' loop's step is an 'int', not 'u8'"},
        Case{"for int in 0..3 { }", "'int'"},
        Case{"m = 4; for _: u8 in 0..m { }",
             "the end of the range has type 'int', but '_' is a 'u8'"},
        Case{"for _: Str in 0..3 { }",
             "'_' has type 'Str': a 'for' loop's variable is an integer"}}) {
    auto r = semaCheck(wrapMain(c.Loop));
    EXPECT_FALSE(r.Ok) << c.Loop;
    EXPECT_NE(r.Diagnostics.find(c.Error), std::string::npos)
        << c.Loop << ": " << r.Diagnostics;
  }
}

TEST(ControlFlow, BreakAndContinueInFor) {
  auto r = semaCheck(wrapMain(R"(
    for i in 0..10 {
      if i == 2 { continue; }
      if i == 5 { break; }
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Logical operators && ||

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
    if x > 0 && y > 0 { io::println("both positive"); }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, LogicalInWhileCondition) {
  auto r = semaCheck(wrapMain(R"(
    i: int = 0;
    j: int = 10;
    while i < 5 && j > 0 { i = i + 1; j = j - 1; }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Ternary expressions

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

TEST(ControlFlow, TernaryArrayMismatchedBranchesRejected) {
  auto r = semaCheck(wrapMain(R"(
    c: bool = True;
    a: int[1] = [1];
    b: Str[1] = ["x"];
    xs: int[1] = if c then a else b;
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("incompatible types"), std::string::npos);
}

TEST(ControlFlow, TernaryArrayDimensionMismatchRejected) {
  auto r = semaCheck(wrapMain(R"(
    c: bool = True;
    a: int[1] = [1];
    m: int[1][1] = [[1]];
    xs: int[1] = if c then a else m;
  )"));
  EXPECT_FALSE(r.Ok);
}

// -- break / continue

TEST(ControlFlow, BreakInsideLoop) {
  auto r = semaCheck(wrapMain(R"(
    while True { break; }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, ContinueInsideLoop) {
  auto r = semaCheck(wrapMain(R"(
    i: int = 0;
    while i < 10 { i = i + 1; continue; }
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
    while i < 10 {
      if i == 5 { break; }
      i = i + 1;
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ControlFlow, BreakInIfOutsideLoopRejected) {
  auto r = semaCheck(wrapMain(R"(
    if True { break; }
  )"));
  EXPECT_FALSE(r.Ok);
}

TEST(ControlFlow, BreakInNestedLoop) {
  auto r = semaCheck(wrapMain(R"(
    while True {
      while True { break; }
      break;
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Return-path analysis: bool-exhaustive match (True + False literal arms)

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
  auto r = semaCheck(R"(import ::io;
    fn pick(b: bool) -> int {
      match b {
        True  { return 1; }
        False { io::println("no"); }
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
