// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: if/else, while, for, logical ops, ternary, break/continue

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// ─── if / else / else-if ────────────────────────────────────────────────────

TEST(ControlFlow, IfStmt) {
  auto [ok, _] = parse(R"(import ::io;
    fn main() -> int {
      if True {
        io::println("yes");
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(ControlFlow, IfElseStmt) {
  auto [ok, _] = parse(R"(import ::io;
    fn main() -> int {
      x: int = 10;
      if x > 5 {
        io::println("big");
      } else {
        io::println("small");
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(ControlFlow, IfElifElse) {
  auto [ok, _] = parse(R"(import ::io;
    fn main() -> int {
      x: int = 42;
      if x > 100 {
        io::println("large");
      } elif x > 10 {
        io::println("medium");
      } else {
        io::println("small");
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(ControlFlow, NestedIf) {
  auto [ok, _] = parse(R"(import ::io;
    fn main() -> int {
      x: int = 5;
      if x > 0 {
        if x < 10 {
          io::println("single digit positive");
        }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// ─── while ──────────────────────────────────────────────────────────────────

TEST(ControlFlow, WhileStmt) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      i: int = 0;
      while i < 5 {
        i = i + 1;
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(ControlFlow, WhileTrue) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      while True {
        return 0;
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// ─── for ─────────────────────────────────────────────────────────────────────

TEST(ControlFlow, ForStmt) {
  auto [ok, driver] = parse(R"(fn main() -> int {
  for i in 0..n { }
  for b: u8 in 9..=0 step -3 { break; }
  return 0;
}
)");
  ASSERT_TRUE(ok);
  std::string d = dumpAST(*driver);
  for (const char *e :
       {"ForStmt <2:3-2:20> ..\n    | |-VarDecl <2:7-2:8> 'i'\n"
        "    | |-IntegerLiteral <2:12-2:13> 0\n"
        "    | |-Identifier <2:15-2:16> 'n'\n"
        "    | `-CompoundStmt <2:18-2:18>\n",
        "ForStmt <3:3-3:40> ..= step\n    | |-VarDecl <3:7-3:12> 'b' type\n"
        "    | | `-BuiltinType 'u8'\n"
        "    | |-IntegerLiteral <3:16-3:17> 9\n"
        "    | |-IntegerLiteral <3:20-3:21> 0\n"
        "    | |-UnaryExpr <3:27-3:29> '-'\n"})
    EXPECT_NE(d.find(e), std::string::npos) << e << "\n" << d;
}

// `in` and `step` are words only in a `for` header: a variable or field may
// be named so, the step may be a variable named `step`, and the loop's
// variable may be named `in`.  The ends are any expressions, `..` taking
// the lowest precedence.
TEST(ControlFlow, ForInAndStepAreNotKeywords) {
  auto [ok, driver] = parse(R"(fn main() -> int {
  step = 2;
  for i in step - 1..step * 4 + 1 step step { }
  for in in 0..3 { x = in; }
  return 0;
}
)");
  ASSERT_TRUE(ok);
  std::string d = dumpAST(*driver);
  EXPECT_NE(d.find("ForStmt <3:3-3:48> .. step\n"), std::string::npos) << d;
  EXPECT_NE(d.find("| |-BinaryExpr <3:12-3:20> '-'"), std::string::npos) << d;
  EXPECT_NE(d.find("| |-BinaryExpr <3:22-3:34> '+'"), std::string::npos) << d;
  EXPECT_NE(d.find("| |-VarDecl <4:7-4:9> 'in'"), std::string::npos) << d;
}

TEST(ControlFlow, ForSyntaxErrors) {
  struct Case {
    const char *Loop;
    const char *Error;
  };
  for (const Case &c :
       {Case{"for x in xs { }", "expected '..' or '..=' in the 'for' loop's "
                                "range ('start..end')"},
        Case{"for (i in 0..3) { }",
             "expected the loop variable's name after 'for'"},
        Case{"for i 0..3 { }", "expected 'in' after the 'for' loop's variable"},
        Case{"for i in 0..3 2 { }",
             "expected 'step' or '{' after the 'for' loop's range"},
        Case{"for i in 0..3 step 2 3 { }",
             "expected '{' after the 'for' loop's step"}}) {
    auto r = parseWithDiags(std::string("fn main() -> int {\n  xs = 1;\n  ") +
                            c.Loop + "\n  return 0;\n}\n");
    EXPECT_FALSE(r.Ok) << c.Loop;
    ASSERT_FALSE(r.Diags.empty()) << c.Loop;
    EXPECT_NE(r.Diags[0].Message.find(c.Error), std::string::npos)
        << c.Loop << ": " << r.Diags[0].Message;
  }
}

// ─── logical operators ──────────────────────────────────────────────────────

TEST(ControlFlow, LogicalAnd) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: bool = True && False;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(ControlFlow, LogicalOr) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: bool = True || False;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(ControlFlow, LogicalPrecedence) {
  // && binds tighter than ||
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: bool = True || False && True;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(ControlFlow, LogicalWithRelational) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = 5;
      y: int = 10;
      a: bool = x > 0 && y < 20;
      b: bool = x == 5 || y != 10;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// ─── ternary ────────────────────────────────────────────────────────────────

TEST(ControlFlow, TernaryBasic) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = if True then 1 else 2;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(ControlFlow, TernaryNested) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = if True then 1 else if False then 2 else 3;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(ControlFlow, TernaryWithRelational) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int = 5;
      x: int = if a > 3 then 10 else 20;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// ─── break / continue ───────────────────────────────────────────────────────

TEST(ControlFlow, BreakInWhile) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      while True {
        break;
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(ControlFlow, ContinueInWhile) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      i: int = 0;
      while i < 10 {
        i = i + 1;
        continue;
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(ControlFlow, BreakContinueNested) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      while True {
        while True {
          break;
        }
        continue;
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// ─── conditions without parentheses ─────────────────────────────────────────

namespace {
/// How many times @p what occurs in @p s.
size_t count(const std::string &s, const std::string &what) {
  size_t n = 0;
  for (size_t at = s.find(what); at != std::string::npos;
       at = s.find(what, at + 1))
    ++n;
  return n;
}
} // namespace

TEST(ControlFlow, ParenthesisedConditionIsAParenthesisedExpression) {
  // The old spelling still parses: `(x > 1)` is an expression.
  auto [ok, driver] = parse(R"(
    fn main() -> int {
      x = 2;
      if x > 1 { x = 0; } elif (x == 1) && (x < 3) { x = 1; }
      while x < 3 { x = x + 1; }
      return x;
    }
  )");
  ASSERT_TRUE(ok);
  std::string ast = dumpAST(*driver);
  EXPECT_EQ(count(ast, "IfStmt"), 2u) << ast;
  EXPECT_EQ(count(ast, "WhileStmt"), 1u) << ast;
}

TEST(ControlFlow, ThenAfterTheConditionMakesATernaryStatement) {
  // `if c then a else b;` is an expression statement; `if c {` is an
  // if-statement even when c holds a ternary, brackets or a lambda.
  auto [ok, driver] = parse(R"(
    fn main() -> int {
      c = True;
      if c then 1 else 2;
      if if c then False else True { c = False; }
      if if c then True else False { c = True; }
      if [1, 2][0] > 0 { c = True; }
      if fn(x: int) -> bool { return x > 0; }(1) { c = False; }
      while if c then False else c { c = False; }
      return 0;
    }
  )");
  ASSERT_TRUE(ok);
  std::string ast = dumpAST(*driver);
  EXPECT_EQ(count(ast, "IfStmt"), 4u) << ast;
  EXPECT_EQ(count(ast, "WhileStmt"), 1u) << ast;
  EXPECT_EQ(count(ast, "TernaryExpr"), 4u) << ast;
}

TEST(ControlFlow, ConditionNeedsABlock) {
  EXPECT_FALSE(parse("fn main() -> int { if 1 > 0 return 0; }").Ok);
  EXPECT_FALSE(parse("fn main() -> int { while True return 0; }").Ok);
  EXPECT_FALSE(parse("fn main() -> int { if { } return 0; }").Ok);
}

// ─── elif ───────────────────────────────────────────────────────────────────

TEST(ControlFlow, ElifChainsNestAsElseBranches) {
  // Each `elif` is an IfStmt in the else branch of the one before; it may
  // start its own line.
  auto [ok, driver] = parse(R"(
    fn main() -> int {
      x = 1;
      if x > 0 { x = 2; }
      elif x == 0 { x = 3; }
      elif x == -1 { x = 4; }
      else { x = 5; }
      if x > 9 { x = 6; } elif x > 8 { x = 7; }
      return if x > 0 then 1 else if x == 0 then 0 else -1;
    }
  )");
  ASSERT_TRUE(ok);
  std::string ast = dumpAST(*driver);
  EXPECT_EQ(count(ast, "IfStmt"), 5u) << ast;
  EXPECT_EQ(count(ast, "TernaryExpr"), 2u) << ast;
}

TEST(ControlFlow, EachFormRejectsTheOthersElseIf) {
  EXPECT_FALSE(
      parse("fn main() -> int { if True { } else if False { } return 0; }").Ok);
  EXPECT_FALSE(
      parse("fn main() -> int { return if True then 1 elif False then 2 "
            "else 3; }")
          .Ok);
  // `elif` is reserved, and only continues an if-statement.
  EXPECT_FALSE(parse("fn main() -> int { elif = 1; return 0; }").Ok);
  EXPECT_FALSE(parse("fn main() -> int { elif True { } return 0; }").Ok);
}
