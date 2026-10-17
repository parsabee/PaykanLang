// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: array types, literals (`[a, b]` and `[v; n]`), subscript

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;
using namespace paykan::ast;

// ─── type annotations ───────────────────────────────────────────────────────

TEST(Array, IntArrayType) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[3] = [1, 2, 3];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, FloatArrayType) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: float[2] = [0.5, 1.5];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, StrArrayType) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: Str[2] = ["a", "b"];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, ClassArrayType) {
  auto [ok, _] = parse(R"(
    class Point { x: int; y: int; }
    fn main() -> int {
      pts: Point[2] = [Point(1, 2), Point(3, 4)];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// `T[n]`: a fixed-size array type, n an integer literal.  A run of lengths
// is one array of arrays whose first length is its first index's, so
// `int[2][3]` is two `int[3]`s; a slice parameter's `int[3][]` takes any
// number of `int[3]`s.
TEST(Array, FixedSizeArrayTypes) {
  auto [ok, driver] = parse(R"(
    fn f(a: float[4], b: Str?[2]) -> bool[1] { return [True]; }
    fn main() -> int {
      xs: int[3] = [1, 2, 3];
      grid: int[2][3] = [[1, 2, 3], [4, 5, 6]];
      return 0;
    }
    fn rows(g: view int[3][]) -> int { return g.len(); }
  )");
  ASSERT_TRUE(ok);
  std::string d = dumpAST(*driver);
  // (The dump shows a function's result type, not its parameters'.)
  for (const char *e :
       {"FixedArrayType <2:38-2:45> 1", "FixedArrayType <4:11-4:17> 3",
        "FixedArrayType <5:13-5:22> 2\n|   |   | `-FixedArrayType <5:13-5:22> "
        "3\n|   |   |   `-BuiltinType 'int'"})
    EXPECT_NE(d.find(e), std::string::npos) << e << "\n" << d;
  const FuncDecl *rows = driver->getRoot()->getFuncDecls().back();
  ASSERT_EQ(rows->getName(), "rows");
  auto *slice = dyn_cast<ArrayType>(rows->getParams()[0].ParamType);
  ASSERT_NE(slice, nullptr);
  auto *row = dyn_cast<FixedArrayType>(slice->getElementType());
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->getLength(), 3);
}

// A length can be `_`, for the declaration to infer (Sema checks where).
TEST(Array, InferredLengths) {
  auto [ok, driver] = parse(R"(
    fn main() -> int {
      x: int[_][_] = [[1, 2, 3], [4, 5, 6]];
      y: int[2][_] = [[1], [2]];
      return 0;
    }
  )");
  ASSERT_TRUE(ok);
  std::string d = dumpAST(*driver);
  for (const char *e :
       {"FixedArrayType <3:10-3:19> _\n    |   | `-FixedArrayType <3:10-3:19> "
        "_\n    |   |   `-BuiltinType 'int'",
        "FixedArrayType <4:10-4:19> 2\n    |   | `-FixedArrayType <4:10-4:19> "
        "_\n"})
    EXPECT_NE(d.find(e), std::string::npos) << e << "\n" << d;
}

TEST(Array, FixedSizeArrayLength) {
  auto zero = parseWithDiags("fn main() -> int {\n"
                             "  xs: int[0] = [];\n"
                             "  return 0;\n"
                             "}\n");
  EXPECT_FALSE(zero.Ok);
  EXPECT_NE(zero.Text.find("2:11: error: a fixed-size array has at least one "
                           "element: its length must be 1 or more"),
            std::string::npos)
      << zero.Text;
  // The length is an integer literal.
  auto name = parseWithDiags("fn f(n: int, xs: int[n]) {}\n");
  EXPECT_FALSE(name.Ok);
  EXPECT_NE(name.Text.find("to close the array type"), std::string::npos)
      << name.Text;
}

TEST(Array, ArrayTypeAsParam) {
  auto [ok, _] = parse(R"(
    fn first(nums: int[3]) -> int { return nums[0]; }
    fn sum(nums: view int[]) -> int { return 0; }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, ArrayTypeAsReturnType) {
  auto [ok, _] = parse(R"(
    fn makeArr() -> int[3] { return [1, 2, 3]; }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

// ─── array literals ─────────────────────────────────────────────────────────

// `[]` parses; Sema rejects it (an array has at least one element).
TEST(Array, EmptyLiteralParses) {
  auto [ok, driver] = parse(R"(
    fn main() -> int {
      a = [];
      return 0;
    }
  )");
  ASSERT_TRUE(ok);
  std::string d = dumpAST(*driver);
  EXPECT_NE(d.find("ArrayLiteralExpr <3:11-3:13> 0 elements\n"),
            std::string::npos)
      << d;
}

TEST(Array, IntLiteral) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[3] = [1, 2, 3];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, FloatLiteral) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: float[3] = [1.0, 2.5, 3.14];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, StringLiteral) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: Str[2] = ["hello", "world"];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, SingleElementLiteral) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[1] = [42];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, LiteralInitialisesAnUntypedDecl) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a = ["hello", "world"];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, LiteralWithExpressions) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = 1;
      a: int[3] = [x, x + 1, x * 2];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// `[v; n]` is n copies of v: one element, v (any expression, an array
// literal too), and its count, an integer literal.
TEST(Array, RepeatLiteral) {
  auto [ok, driver] = parse(R"(
    fn main() -> int {
      a: int[3] = [0; 3];
      g = [[0; 2]; 3];
      x = 1;
      b = [x + 1; 4];
      return 0;
    }
  )");
  ASSERT_TRUE(ok);
  std::string d = dumpAST(*driver);
  for (const char *e :
       {"ArrayLiteralExpr <3:19-3:25> 3 copies\n    |     `-IntegerLiteral "
        "<3:20-3:21> 0\n",
        "ArrayLiteralExpr <4:11-4:22> 3 copies\n    |   `-ArrayLiteralExpr "
        "<4:12-4:18> 2 copies\n    |     `-IntegerLiteral <4:13-4:14> 0\n",
        "ArrayLiteralExpr <6:11-6:21> 4 copies\n    |   `-BinaryExpr "
        "<6:12-6:17> '+'\n"})
    EXPECT_NE(d.find(e), std::string::npos) << e << "\n" << d;
  auto *body = driver->getRoot()->getFuncDecls()[0]->getBody();
  auto *g = cast<ArrayLiteralExpr>(
      cast<AssignStmt>(body->getStatements()[1])->getValue());
  EXPECT_TRUE(g->isRepeat());
  EXPECT_EQ(g->getRepeatCount(), 3);
  EXPECT_EQ(g->getLength(), 3);
  ASSERT_EQ(g->getNumElements(), 1u);
  auto *row = cast<ArrayLiteralExpr>(g->getElements()[0]);
  EXPECT_EQ(row->getLength(), 2);
  // A list literal is not a repeat.
  auto [ok2, list] = parse("fn main() -> int { a = [7]; return 0; }");
  ASSERT_TRUE(ok2);
  auto *one = cast<ArrayLiteralExpr>(
      cast<AssignStmt>(
          list->getRoot()->getFuncDecls()[0]->getBody()->getStatements()[0])
          ->getValue());
  EXPECT_FALSE(one->isRepeat());
  EXPECT_EQ(one->getLength(), 1);
}

// The count is an integer literal of at least 1, after exactly one value.
TEST(Array, RepeatLiteralErrors) {
  struct Case {
    const char *Literal;
    const char *Error;
  };
  for (const Case &c :
       {Case{"[0; 0]", "2:12: error: an array has at least one element: the "
                       "count must be 1 or more"},
        Case{"[0; n]", "2:12: error: unexpected identifier 'n'; expected the "
                       "number of copies, an integer literal"},
        Case{"[0; -1]", "2:12: error: unexpected '-'; expected the number of "
                        "copies, an integer literal"},
        Case{"[0; 3.0]", "2:12: error: unexpected float literal; expected the "
                         "number of copies, an integer literal"},
        Case{"[1, 2; 3]", "2:13: error: unexpected ';'; expected ']' to close "
                          "the array literal"},
        Case{"[0; 3", "2:13: error: unexpected ';'; expected ']' to close the "
                      "array literal"}}) {
    auto r = parseWithDiags(std::string("fn main() -> int {\n  xs = ") +
                            c.Literal + ";\n  return 0;\n}\n");
    EXPECT_FALSE(r.Ok) << c.Literal;
    EXPECT_NE(r.Text.find(c.Error), std::string::npos) << c.Literal << "\n"
                                                       << r.Text;
  }
}

// ─── subscript ──────────────────────────────────────────────────────────────

TEST(Array, SubscriptRead) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[3] = [10, 20, 30];
      x: int = a[0];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, SubscriptWithExprIndex) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[3] = [1, 2, 3];
      i: int = 1;
      x: int = a[i + 1];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, SubscriptInExpression) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[2] = [3, 7];
      return a[0] + a[1];
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, SubscriptPassedToCall) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: Str[1] = ["hi"];
      println(a[0]);
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Array, SubscriptChained) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[1][2] = [[1, 2]];
      x: int = a[0][1];
      return x;
    }
  )");
  EXPECT_TRUE(ok);
}

// ─── len() ──────────────────────────────────────────────────────────────────

TEST(Array, LenCall) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[3] = [1, 2, 3];
      n: int = a.len();
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// ─── parse errors ────────────────────────────────────────────────────────────

TEST(Array, MissingCloseBracketInLiteral) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[2] = [1, 2;
      return 0;
    }
  )");
  EXPECT_FALSE(ok);
}

TEST(Array, MissingCloseBracketInSubscript) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: int[2] = [1, 2];
      x: int = a[0;
      return 0;
    }
  )");
  EXPECT_FALSE(ok);
}
