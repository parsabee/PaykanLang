// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: tuple types, literals, element access, destructuring, and the
// `(x)` vs `(x, y)` / `t.0` vs float-literal disambiguation.

#include "ASTPrinter.h"
#include "TestUtils.h"
#include <gtest/gtest.h>
#include <sstream>

using namespace paykan::test;

// Parse and return the printed AST (empty string on parse failure).
static std::string dumpAST(const std::string &src) {
  auto [ok, driver] = parse(src);
  if (!ok)
    return "";
  std::ostringstream os;
  paykan::ast::ASTPrinter printer(os);
  printer.visit(driver->getRoot());
  return os.str();
}

// ─── type annotations ───────────────────────────────────────────────────────

TEST(Tuple, PairType) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      t: (int, Str) = (1, "a");
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Tuple, TripleTypeWithClass) {
  auto [ok, _] = parse(R"(
    class Point { x: int; y: int; }
    fn main() -> int {
      t: (int, Str, Point) = (1, "a", Point());
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Tuple, NestedType) {
  auto dump = dumpAST(R"(
    fn main() -> int {
      t: (int, (Str, bool)) = (1, ("a", True));
      return 0;
    }
  )");
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("TupleType"), std::string::npos);
  // Outer arity 2 with a nested 2-element tuple type.
  EXPECT_NE(dump.find("TupleType <3:10-3:28> 2 elements"), std::string::npos);
  EXPECT_NE(dump.find("TupleType <3:16-3:27> 2 elements"), std::string::npos);
}

TEST(Tuple, ArrayOfTuplesAndTupleOfArrays) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: (int, Str)[] = [];
      b: (int[], Str) = ([1], "x");
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Tuple, TypeAsParamAndReturn) {
  auto [ok, _] = parse(R"(
    fn divmod(a: int, b: int) -> (int, int) { return (a / b, a % b); }
    fn swap(p: (int, Str)) -> (Str, int) { return (p.1, p.0); }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Tuple, OneTupleTypeIsSyntaxError) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      t: (int) = 1;
      return 0;
    }
  )");
  EXPECT_FALSE(ok);
}

TEST(Tuple, EmptyTupleTypeIsSyntaxError) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      t: () = 1;
      return 0;
    }
  )");
  EXPECT_FALSE(ok);
}

// ─── literals ───────────────────────────────────────────────────────────────

TEST(Tuple, LiteralWithExpressions) {
  auto dump = dumpAST(R"(
    fn main() -> int {
      x: int = 1;
      t = (x, x + 1, "s");
      return 0;
    }
  )");
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("TupleLiteralExpr <4:11-4:26> 3 elements"),
            std::string::npos);
}

TEST(Tuple, ParenthesisedSingleExprIsNotATuple) {
  auto dump = dumpAST(R"(
    fn main() -> int {
      x: int = (1 + 2) * 3;
      return (x);
    }
  )");
  ASSERT_FALSE(dump.empty());
  EXPECT_EQ(dump.find("TupleLiteralExpr"), std::string::npos);
  EXPECT_NE(dump.find("BinaryExpr"), std::string::npos);
}

TEST(Tuple, EmptyParensIsSyntaxError) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      t = ();
      return 0;
    }
  )");
  EXPECT_FALSE(ok);
}

TEST(Tuple, NestedLiteral) {
  auto dump = dumpAST(R"(
    fn main() -> int {
      t = ((1, 2), (3, (4, 5)));
      return 0;
    }
  )");
  ASSERT_FALSE(dump.empty());
  // Outer + 3 inner tuple literals.
  size_t count = 0;
  for (size_t pos = dump.find("TupleLiteralExpr"); pos != std::string::npos;
       pos = dump.find("TupleLiteralExpr", pos + 1))
    ++count;
  EXPECT_EQ(count, 4u);
}

TEST(Tuple, LiteralAsCallArgumentAndInArray) {
  auto [ok, _] = parse(R"(
    fn f(p: (int, int)) -> int { return p.0; }
    fn main() -> int {
      arr: (int, int)[] = [(1, 2), (3, 4)];
      return f((1, 2)) + f(arr[0]);
    }
  )");
  EXPECT_TRUE(ok);
}

// ─── element access ─────────────────────────────────────────────────────────

TEST(Tuple, IndexAccess) {
  auto dump = dumpAST(R"(
    fn main() -> int {
      t = (1, "a");
      x = t.0;
      s = t.1;
      return 0;
    }
  )");
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("TupleIndexExpr <4:11-4:14> .0"), std::string::npos);
  EXPECT_NE(dump.find("TupleIndexExpr <5:11-5:14> .1"), std::string::npos);
}

TEST(Tuple, ChainedIndexLexesAsTwoIndices) {
  // `t.0.1` must be IDENT TUPLE_INDEX(0) TUPLE_INDEX(1), never IDENT DOT
  // FLOAT(0.1).
  auto dump = dumpAST(R"(
    fn main() -> int {
      t = ((1, 2), 3);
      x = t.0.1;
      return 0;
    }
  )");
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("TupleIndexExpr <4:11-4:16> .1"), std::string::npos);
  EXPECT_NE(dump.find("TupleIndexExpr <4:11-4:14> .0"), std::string::npos);
  EXPECT_EQ(dump.find("FloatLiteral"), std::string::npos);
}

TEST(Tuple, FloatLiteralsStillLex) {
  // A digit before the dot always wins as a float literal.
  auto dump = dumpAST(R"(
    fn main() -> int {
      a: float = 1.5;
      b: float = 2.;
      c: float = 1.5e3;
      t = (1.0, 2.0);
      return 0;
    }
  )");
  ASSERT_FALSE(dump.empty());
  EXPECT_EQ(dump.find("TupleIndexExpr"), std::string::npos);
  EXPECT_NE(dump.find("FloatLiteral <3:18-3:21>"), std::string::npos);
  EXPECT_NE(dump.find("FloatLiteral <4:18-4:20>"), std::string::npos);
  EXPECT_NE(dump.find("FloatLiteral <5:18-5:23>"), std::string::npos);
  size_t count = 0;
  for (size_t pos = dump.find("FloatLiteral"); pos != std::string::npos;
       pos = dump.find("FloatLiteral", pos + 1))
    ++count;
  EXPECT_EQ(count, 5u); // a, b, c and the two tuple elements
}

TEST(Tuple, IndexOnCallResultAndLiteral) {
  auto [ok, _] = parse(R"(
    fn mk() -> (int, int) { return (1, 2); }
    fn main() -> int {
      return mk().0 + (3, 4).1;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Tuple, IndexThenMethodCall) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      t = (1, "abc");
      return t.1.len() + t.1.toString().len();
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Tuple, IndexWithLeadingZeroIsSyntaxError) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      t = (1, 2);
      return t.01;
    }
  )");
  EXPECT_FALSE(ok);
}

TEST(Tuple, IndexAssignmentParsesAsMemberAssign) {
  // Syntactically accepted; Sema rejects it (tuples are immutable).
  auto dump = dumpAST(R"(
    fn main() -> int {
      t = (1, 2);
      t.0 = 5;
      return 0;
    }
  )");
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("MemberAssignStmt '0'"), std::string::npos);
}

// ─── destructuring ──────────────────────────────────────────────────────────

TEST(Tuple, DestructureTwoNames) {
  auto dump = dumpAST(R"(
    fn divmod(a: int, b: int) -> (int, int) { return (a / b, a % b); }
    fn main() -> int {
      q, r = divmod(7, 2);
      return q + r;
    }
  )");
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("DestructureStmt <4:7-4:27> targets={q, r}"),
            std::string::npos);
}

TEST(Tuple, DestructureWithSkipAndThree) {
  auto dump = dumpAST(R"(
    fn main() -> int {
      a, _, c = (1, 2, 3);
      return a + c;
    }
  )");
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("targets={a, _, c}"), std::string::npos);
}

TEST(Tuple, DestructureTyped) {
  auto dump = dumpAST(R"(
    fn main() -> int {
      q: int, r: int = (7, 2);
      s: Str, _ = ("x", 1);
      return 0;
    }
  )");
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("targets={q: type, r: type}"), std::string::npos);
  EXPECT_NE(dump.find("targets={s: type, _}"), std::string::npos);
}

TEST(Tuple, DestructureDoesNotStealPlainAssignment) {
  // `a = ...` and `a: int = ...` must still parse as before.
  auto dump = dumpAST(R"(
    fn main() -> int {
      a = 1;
      b: int = 2;
      a = b;
      return 0;
    }
  )");
  ASSERT_FALSE(dump.empty());
  EXPECT_EQ(dump.find("DestructureStmt"), std::string::npos);
  EXPECT_NE(dump.find("AssignStmt"), std::string::npos);
  EXPECT_NE(dump.find("VarDecl"), std::string::npos);
}

TEST(Tuple, DestructureSingleTargetIsSyntaxError) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      _ = (1, 2);
      return 0;
    }
  )");
  EXPECT_FALSE(ok);
}

TEST(Tuple, DestructureNestedPatternIsSyntaxError) {
  // Nested destructuring `(a, (b, c)) = ...` is out of scope.
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a, (b, c) = (1, (2, 3));
      return 0;
    }
  )");
  EXPECT_FALSE(ok);
}
