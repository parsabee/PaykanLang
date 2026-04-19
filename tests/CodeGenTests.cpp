// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// End-to-end codegen + JIT tests (compile → run → check stdout).

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

static std::string withFns(const std::string &fns, const std::string &body) {
  return fns + "\n" + wrapMain(body);
}

// ============================================================================
// Arithmetic
// ============================================================================

TEST(CodeGen, IntArithmetic) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = 2 + 3 * 4;
    out(StringInt(x));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "14\n");
}

TEST(CodeGen, FloatArithmetic) {
  auto r = compileAndRun(wrapMain(R"(
    x: float = 1.5 + 2.5;
    out(StringFloat(x));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "4\n");
}

TEST(CodeGen, BoolLiterals) {
  auto r = compileAndRun(wrapMain(R"(
    a: bool = True;
    b: bool = False;
    out(StringBool(a), StringBool(b));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "TrueFalse\n");
}

// ============================================================================
// Strings
// ============================================================================

TEST(CodeGen, StringLiteral) {
  auto r = compileAndRun(wrapMain(R"(out("hello world");)"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "hello world\n");
}

TEST(CodeGen, StringConcat) {
  auto r = compileAndRun(wrapMain(R"(
    a: String = "hello";
    b: String = " world";
    c: String = a + b;
    out(&c);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "hello world\n");
}

TEST(CodeGen, StringBuiltins) {
  auto r = compileAndRun(wrapMain(R"(
    out(StringInt(42), StringFloat(3.14), StringBool(True));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "423.14True\n");
}

// ============================================================================
// Functions
// ============================================================================

TEST(CodeGen, FunctionCallReturn) {
  auto r = compileAndRun(R"(
    fn add(a: int, b: int) -> int { return a + b; }
    fn main() -> int {
      x: int = add(3, 4);
      out(StringInt(x));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "7\n");
}

TEST(CodeGen, FunctionVoid) {
  auto r = compileAndRun(R"(
    fn greet(s: const String&) { out(s); }
    fn main() -> int {
      greet("yo");
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "yo\n");
}

// ============================================================================
// Ownership — unique
// ============================================================================

TEST(CodeGen, UniqueMovTransfer) {
  auto r = compileAndRun(withFns(
    "fn take(s: String) { out(&s); }",
    "a: String = \"hello\";\n  take(mov a);"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "hello\n");
}

// ============================================================================
// Ownership — shared
// ============================================================================

TEST(CodeGen, SharedBasic) {
  auto r = compileAndRun(wrapMain(R"(
    a: shared String = "alpha";
    out(&a);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "alpha\n");
}

TEST(CodeGen, SharedReassign) {
  auto r = compileAndRun(wrapMain(R"(
    a: shared String = "alpha";
    a = "beta";
    out(&a);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "beta\n");
}

TEST(CodeGen, SharedPassToFunction) {
  auto r = compileAndRun(withFns(
    "fn show(s: shared String) { out(&s); }",
    "a: shared String = \"hi\";\n  show(a);"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "hi\n");
}

// ============================================================================
// Ownership — references
// ============================================================================

TEST(CodeGen, RefParam) {
  auto r = compileAndRun(withFns(
    "fn show(s: String&) { out(s); }",
    "a: String = \"ref test\";\n  show(&a);"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "ref test\n");
}

TEST(CodeGen, ConstRefLiteral) {
  auto r = compileAndRun(withFns(
    "fn show(s: const String&) { out(s); }",
    "show(\"const ref\");"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "const ref\n");
}

// ============================================================================
// Return code
// ============================================================================

TEST(CodeGen, ReturnCode) {
  auto r = compileAndRun("fn main() -> int { return 42; }");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 42);
}

TEST(CodeGen, ReturnZero) {
  auto r = compileAndRun("fn main() -> int { return 0; }");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
}

// ============================================================================
// if / else
// ============================================================================

TEST(CodeGen, IfTrue) {
  auto r = compileAndRun(wrapMain(R"(
    if (True) {
      out("yes");
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "yes\n");
}

TEST(CodeGen, IfFalse) {
  auto r = compileAndRun(wrapMain(R"(
    if (False) {
      out("no");
    }
    out("done");
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "done\n");
}

TEST(CodeGen, IfElseTrueBranch) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = 10;
    if (x > 5) {
      out("big");
    } else {
      out("small");
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "big\n");
}

TEST(CodeGen, IfElseFalseBranch) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = 3;
    if (x > 5) {
      out("big");
    } else {
      out("small");
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "small\n");
}

TEST(CodeGen, IfElseIfChain) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = 42;
    if (x > 100) {
      out("large");
    } else if (x > 10) {
      out("medium");
    } else {
      out("small");
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "medium\n");
}

TEST(CodeGen, IfElseIfLastBranch) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = 3;
    if (x > 100) {
      out("large");
    } else if (x > 10) {
      out("medium");
    } else {
      out("small");
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "small\n");
}

TEST(CodeGen, NestedIf) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = 5;
    if (x > 0) {
      if (x < 10) {
        out("single digit");
      } else {
        out("big");
      }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "single digit\n");
}

TEST(CodeGen, IfWithReturn) {
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
      out(StringInt(classify(42)));
      out(StringInt(classify(-5)));
      out(StringInt(classify(0)));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1\n-1\n0\n");
}

// ============================================================================
// while
// ============================================================================

TEST(CodeGen, WhileCountUp) {
  auto r = compileAndRun(wrapMain(R"(
    i: int = 0;
    while (i < 5) {
      out(StringInt(i));
      i = i + 1;
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n1\n2\n3\n4\n");
}

TEST(CodeGen, WhileNeverExecutes) {
  auto r = compileAndRun(wrapMain(R"(
    while (False) {
      out("nope");
    }
    out("done");
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "done\n");
}

TEST(CodeGen, WhileSum) {
  auto r = compileAndRun(wrapMain(R"(
    i: int = 1;
    sum: int = 0;
    while (i <= 10) {
      sum = sum + i;
      i = i + 1;
    }
    out(StringInt(sum));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "55\n");
}

TEST(CodeGen, WhileWithIf) {
  auto r = compileAndRun(wrapMain(R"(
    i: int = 0;
    while (i < 6) {
      if (i % 2 == 0) {
        out(StringInt(i));
      }
      i = i + 1;
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0\n2\n4\n");
}

TEST(CodeGen, NestedWhile) {
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
    out(StringInt(count));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "9\n");
}

// ============================================================================
// Logical operators && ||
// ============================================================================

TEST(CodeGen, LogicalAndTrueTrue) {
  auto r = compileAndRun(wrapMain(R"(
    a: bool = True && True;
    out(StringBool(a));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\n");
}

TEST(CodeGen, LogicalAndTrueFalse) {
  auto r = compileAndRun(wrapMain(R"(
    a: bool = True && False;
    out(StringBool(a));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "False\n");
}

TEST(CodeGen, LogicalAndFalseShortCircuit) {
  // False && anything should be False without evaluating RHS.
  auto r = compileAndRun(wrapMain(R"(
    a: bool = False && True;
    out(StringBool(a));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "False\n");
}

TEST(CodeGen, LogicalOrFalseFalse) {
  auto r = compileAndRun(wrapMain(R"(
    a: bool = False || False;
    out(StringBool(a));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "False\n");
}

TEST(CodeGen, LogicalOrTrueShortCircuit) {
  auto r = compileAndRun(wrapMain(R"(
    a: bool = True || False;
    out(StringBool(a));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\n");
}

TEST(CodeGen, LogicalOrFalseTrue) {
  auto r = compileAndRun(wrapMain(R"(
    a: bool = False || True;
    out(StringBool(a));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\n");
}

TEST(CodeGen, LogicalWithRelational) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = 5;
    a: bool = x > 0 && x < 10;
    out(StringBool(a));
    b: bool = x < 0 || x > 3;
    out(StringBool(b));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\nTrue\n");
}

TEST(CodeGen, LogicalInIfCondition) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = 5;
    y: int = 10;
    if (x > 0 && y > 0) {
      out("both positive");
    } else {
      out("not both positive");
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "both positive\n");
}

TEST(CodeGen, LogicalInWhileCondition) {
  auto r = compileAndRun(wrapMain(R"(
    i: int = 0;
    j: int = 10;
    while (i < 5 && j > 5) {
      i = i + 1;
      j = j - 1;
    }
    out(StringInt(i), StringInt(j));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "55\n");
}

TEST(CodeGen, LogicalChained) {
  auto r = compileAndRun(wrapMain(R"(
    a: bool = True && True || False;
    out(StringBool(a));
    b: bool = False || False && True;
    out(StringBool(b));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\nFalse\n");
}

TEST(CodeGen, LogicalNot) {
  auto r = compileAndRun(wrapMain(R"(
    a: bool = !True && False;
    out(StringBool(a));
    b: bool = !False || False;
    out(StringBool(b));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "False\nTrue\n");
}

// ============================================================================
// Ternary expressions
// ============================================================================

TEST(CodeGen, TernaryTrueBranch) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = if True then 10 else 20;
    out(StringInt(x));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "10\n");
}

TEST(CodeGen, TernaryFalseBranch) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = if False then 10 else 20;
    out(StringInt(x));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "20\n");
}

TEST(CodeGen, TernaryWithCondition) {
  auto r = compileAndRun(wrapMain(R"(
    a: int = 5;
    x: int = if a > 3 then 100 else 200;
    out(StringInt(x));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "100\n");
}

TEST(CodeGen, TernaryNested) {
  auto r = compileAndRun(wrapMain(R"(
    x: int = if False then 1 else if True then 2 else 3;
    out(StringInt(x));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "2\n");
}

TEST(CodeGen, TernaryBoolResult) {
  auto r = compileAndRun(wrapMain(R"(
    x: bool = if True then False else True;
    out(StringBool(x));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "False\n");
}
