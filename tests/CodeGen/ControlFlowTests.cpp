// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen tests: if/else, while, logical operators, ternary, break/continue.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// -- if / else

TEST(ControlFlow, IfTrue) {
  auto r = compileAndRun(wrapMain(R"(
    if (True) {
      println("yes");
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "yes\n");
}

TEST(ControlFlow, IfFalse) {
  auto r = compileAndRun(wrapMain(R"(
    if (False) {
      println("no");
    }
    println("done");
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "done\n");
}

TEST(ControlFlow, IfElseTrueBranch) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = 10;
    if (x > 5) {
      println("big");
    } else {
      println("small");
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "big\n");
}

TEST(ControlFlow, IfElseFalseBranch) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = 3;
    if (x > 5) {
      println("big");
    } else {
      println("small");
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "small\n");
}

TEST(ControlFlow, IfElseIfChain) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = 42;
    if (x > 100) {
      println("large");
    } else if (x > 10) {
      println("medium");
    } else {
      println("small");
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "medium\n");
}

TEST(ControlFlow, IfElseIfLastBranch) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = 3;
    if (x > 100) {
      println("large");
    } else if (x > 10) {
      println("medium");
    } else {
      println("small");
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "small\n");
}

TEST(ControlFlow, NestedIf) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = 5;
    if (x > 0) {
      if (x < 10) {
        println("single digit");
      } else {
        println("big");
      }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "single digit\n");
}

TEST(ControlFlow, IfWithReturn) {
  auto r = compileAndRun(R"(
    fn classify(x: int) -> int {
      if (x > 0) {
        return 1;
      } else if (x < 0) {
        return -1;
      } else {
        return 0;
      }
    }
    fn main() -> int {
      println(Str<int>(classify(42)));
      println(Str<int>(classify(-5)));
      println(Str<int>(classify(0)));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1\n-1\n0\n");
}

// -- while

TEST(ControlFlow, WhileCountUp) {
  auto r = compileAndRun(wrapMain(R"(
    i: int = 0;
    while (i < 5) {
      println(Str<int>(i));
      i = i + 1;
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n1\n2\n3\n4\n");
}

TEST(ControlFlow, WhileNeverExecutes) {
  auto r = compileAndRun(wrapMain(R"(
    while (False) {
      println("nope");
    }
    println("done");
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "done\n");
}

TEST(ControlFlow, WhileSum) {
  auto r = compileAndRun(wrapMain(R"(
    i: int = 1;
    sum: int = 0;
    while (i <= 10) {
      sum = sum + i;
      i = i + 1;
    }
    println(Str<int>(sum));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "55\n");
}

TEST(ControlFlow, WhileWithIf) {
  auto r = compileAndRun(wrapMain(R"(
    i: int = 0;
    while (i < 6) {
      if (i % 2 == 0) {
        println(Str<int>(i));
      }
      i = i + 1;
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n2\n4\n");
}

TEST(ControlFlow, NestedWhile) {
  auto r = compileAndRun(wrapMain(R"(
    i: int = 0;
    count: int = 0;
    while (i < 3) {
      j: int = 0;
      while (j < 3) {
        count = count + 1;
        j = j + 1;
      }
      i = i + 1;
    }
    println(Str<int>(count));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "9\n");
}

// -- Logical operators && ||

TEST(ControlFlow, LogicalAndTrueTrue) {
  auto r = compileAndRun(wrapMain(R"(
    a: bool = True && True;
    println(Str<bool>(a));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\n");
}

TEST(ControlFlow, LogicalAndTrueFalse) {
  auto r = compileAndRun(wrapMain(R"(
    a: bool = True && False;
    println(Str<bool>(a));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "False\n");
}

TEST(ControlFlow, LogicalAndFalseShortCircuit) {
  auto r = compileAndRun(wrapMain(R"(
    a: bool = False && True;
    println(Str<bool>(a));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "False\n");
}

TEST(ControlFlow, LogicalOrFalseFalse) {
  auto r = compileAndRun(wrapMain(R"(
    a: bool = False || False;
    println(Str<bool>(a));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "False\n");
}

TEST(ControlFlow, LogicalOrTrueShortCircuit) {
  auto r = compileAndRun(wrapMain(R"(
    a: bool = True || False;
    println(Str<bool>(a));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\n");
}

TEST(ControlFlow, LogicalOrFalseTrue) {
  auto r = compileAndRun(wrapMain(R"(
    a: bool = False || True;
    println(Str<bool>(a));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\n");
}

TEST(ControlFlow, LogicalWithRelational) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = 5;
    a: bool = x > 0 && x < 10;
    println(Str<bool>(a));
    b: bool = x < 0 || x > 3;
    println(Str<bool>(b));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\nTrue\n");
}

TEST(ControlFlow, LogicalInIfCondition) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = 5;
    y: int = 10;
    if (x > 0 && y > 0) {
      println("both positive");
    } else {
      println("not both positive");
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "both positive\n");
}

TEST(ControlFlow, LogicalInWhileCondition) {
  auto r = compileAndRun(wrapMain(R"(
    i: int = 0;
    j: int = 10;
    while (i < 5 && j > 5) {
      i = i + 1;
      j = j - 1;
    }
    println(Str<int>(i) + Str<int>(j));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "55\n");
}

TEST(ControlFlow, LogicalChained) {
  auto r = compileAndRun(wrapMain(R"(
    a: bool = True && True || False;
    println(Str<bool>(a));
    b: bool = False || False && True;
    println(Str<bool>(b));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\nFalse\n");
}

TEST(ControlFlow, LogicalNot) {
  auto r = compileAndRun(wrapMain(R"(
    a: bool = !True && False;
    println(Str<bool>(a));
    b: bool = !False || False;
    println(Str<bool>(b));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "False\nTrue\n");
}

// -- Ternary expressions

TEST(ControlFlow, TernaryTrueBranch) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = if True then 10 else 20;
    println(Str<int>(x));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "10\n");
}

TEST(ControlFlow, TernaryFalseBranch) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = if False then 10 else 20;
    println(Str<int>(x));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "20\n");
}

TEST(ControlFlow, TernaryWithCondition) {
  auto r = compileAndRun(wrapMain(R"(
    a: int = 5;
    x: int = if a > 3 then 100 else 200;
    println(Str<int>(x));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "100\n");
}

TEST(ControlFlow, TernaryNested) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = if False then 1 else if True then 2 else 3;
    println(Str<int>(x));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "2\n");
}

TEST(ControlFlow, TernaryBoolResult) {
  auto r = compileAndRun(wrapMain(R"(
    x: bool = if True then False else True;
    println(Str<bool>(x));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "False\n");
}

// -- break / continue

TEST(ControlFlow, BreakExitsLoop) {
  auto r = compileAndRun(wrapMain(R"(
    i: int = 0;
    while (True) {
      if (i == 3) { break; }
      i = i + 1;
    }
    println(Str<int>(i));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3\n");
}

TEST(ControlFlow, ContinueSkipsRest) {
  auto r = compileAndRun(wrapMain(R"(
    i: int = 0;
    sum: int = 0;
    while (i < 5) {
      i = i + 1;
      if (i == 3) { continue; }
      sum = sum + i;
    }
    println(Str<int>(sum));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "12\n");
}

TEST(ControlFlow, BreakInnerLoopOnly) {
  auto r = compileAndRun(wrapMain(R"(
    i: int = 0;
    count: int = 0;
    while (i < 3) {
      j: int = 0;
      while (True) {
        if (j == 2) { break; }
        j = j + 1;
        count = count + 1;
      }
      i = i + 1;
    }
    println(Str<int>(count));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "6\n");
}

TEST(ControlFlow, ContinueSumEvens) {
  auto r = compileAndRun(wrapMain(R"(
    i: int = 0;
    result: int = 0;
    while (i < 10) {
      if (i % 2 != 0) {
        i = i + 1;
        continue;
      }
      result = result + i;
      i = i + 1;
    }
    println(Str<int>(result));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "20\n");
}

TEST(ControlFlow, BreakWhileTrue) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = 100;
    while (True) {
      x = x - 7;
      if (x < 50) { break; }
    }
    println(Str<int>(x));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "44\n");
}
