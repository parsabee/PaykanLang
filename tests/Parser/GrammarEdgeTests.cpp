// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Grammar corners where the Bison grammar used to disagree with
// docs/grammar.md (see its section 10).  These run on every frontend.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;
using namespace paykan::ast;

// -- Leading commas are syntax errors ----------------------------------------

TEST(GrammarEdge, LeadingCommaInArgumentListRejected) {
  EXPECT_FALSE(parse("fn main() -> int { f(, 1); return 0; }").Ok);
  EXPECT_FALSE(parse("fn main() -> int { f(,); return 0; }").Ok);
  EXPECT_FALSE(parse("fn main() -> int { x.m(, 1); return 0; }").Ok);
  EXPECT_TRUE(parse("fn main() -> int { f(1, 2); f(); return 0; }").Ok);
}

TEST(GrammarEdge, LeadingCommaInArrayLiteralRejected) {
  EXPECT_FALSE(parse("fn main() -> int { x: int[] = [, 1]; return 0; }").Ok);
  EXPECT_TRUE(parse("fn main() -> int { x: int[] = [1, 2]; y: int[] = []; "
                    "return 0; }")
                  .Ok);
}

TEST(GrammarEdge, LeadingCommaInParamListRejected) {
  EXPECT_FALSE(parse("fn f(, a: int) {} fn main() -> int { return 0; }").Ok);
  EXPECT_TRUE(
      parse("fn f(a: int, b: Str) {} fn g() {} fn main() -> int { return 0; }")
          .Ok);
}

TEST(GrammarEdge, RepeatedCommaInEnumRejected) {
  EXPECT_FALSE(parse("enum E { a,, b } fn main() -> int { return 0; }").Ok);
  EXPECT_FALSE(parse("enum E { a, , } fn main() -> int { return 0; }").Ok);
  EXPECT_FALSE(parse("enum E { , a } fn main() -> int { return 0; }").Ok);
  // Exactly one trailing comma stays permitted.
  EXPECT_TRUE(parse("enum E { a, b, } fn main() -> int { return 0; }").Ok);
  EXPECT_TRUE(parse("enum E { a } fn main() -> int { return 0; }").Ok);
}

// -- A '<' opens a generic call only when `< types > (` fits ---------------

TEST(GrammarEdge, ComparisonOfCallResultIsNotAGenericCall) {
  // `f(a < b, c) > (d)`: the ')' closes the call's own parenthesis, so the
  // '<' cannot open a type argument list.
  auto [ok, driver] =
      parse("fn main() -> int { x: bool = f(a < b, c) > (d); return 0; }");
  ASSERT_TRUE(ok);
  auto *body = driver->getRoot()->getFuncDecls()[0]->getBody();
  auto *decl = cast<DeclStmt>(body->getStatements()[0]);
  auto *init = cast<VarDecl>(decl->getDecl())->getInitExpr();
  auto *gt = dyn_cast<BinaryExpr>(init);
  ASSERT_NE(gt, nullptr);
  EXPECT_EQ(gt->getOpcode(), BinaryOpcode::Gt);
  auto *call = dyn_cast<CallExpr>(gt->getLHS());
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(call->getCalleeName(), "f");
  EXPECT_FALSE(call->hasTypeArgs());
  ASSERT_EQ(call->getNumArguments(), 2u);
  auto *lt = dyn_cast<BinaryExpr>(call->getArguments()[0]);
  ASSERT_NE(lt, nullptr);
  EXPECT_EQ(lt->getOpcode(), BinaryOpcode::Lt);
}

TEST(GrammarEdge, GenericCallInsideArgumentListStillParses) {
  // `f(a < b, c > (d))` has exactly the shape of a generic call a<b, c>(d).
  auto [ok, driver] =
      parse("fn main() -> int { x = f(a < b, c > (d)); return 0; }");
  ASSERT_TRUE(ok);
  auto *body = driver->getRoot()->getFuncDecls()[0]->getBody();
  auto *assign = cast<AssignStmt>(body->getStatements()[0]);
  auto *f = cast<CallExpr>(assign->getValue());
  ASSERT_EQ(f->getNumArguments(), 1u);
  auto *a = dyn_cast<CallExpr>(f->getArguments()[0]);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->getCalleeName(), "a");
  EXPECT_EQ(a->getTypeArgs().size(), 2u);
}

TEST(GrammarEdge, SubscriptComparisonIsNotAGenericCall) {
  EXPECT_TRUE(
      parse("fn main() -> int { x: bool = xs[a < b] > (d); return 0; }").Ok);
}
