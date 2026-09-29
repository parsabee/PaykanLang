// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests for the `enum` type: declaration, variant access, equality rules,
// non-convertibility, and enum-subject match.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}
static std::string withEnums(const std::string &enums,
                             const std::string &body) {
  return enums + "\n" + wrapMain(body);
}

// --- Declaration & basic use ------------------------------------------------

TEST(Enum, DeclareAndUse) {
  auto r = semaCheck(
      withEnums("enum Color { Red, Green, Blue }", "c: Color = Color::Green;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Enum, TrailingCommaAllowed) {
  auto r = semaCheck(
      withEnums("enum Color { Red, Green, Blue, }", "c: Color = Color::Blue;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Enum, DuplicateVariantRejected) {
  auto r = semaCheck(withEnums("enum Color { Red, Red }", "return 0;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Enum, DuplicateEnumNameRejected) {
  auto r = semaCheck(
      withEnums("enum Color { Red }\nenum Color { Blue }", "return 0;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Enum, NameClashesWithClassRejected) {
  auto r =
      semaCheck("class Color {}\nenum Color { Red }\n" + wrapMain("return 0;"));
  EXPECT_FALSE(r.Ok);
}

// Enums share the top-level namespace with the builtins.
TEST(Enum, NameShadowsBuiltinClassRejected) {
  auto r = semaCheck(withEnums("enum Error { NotFound }", "return 0;"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(
      r.Diagnostics.find("'Error' is a builtin class and cannot be redeclared"),
      std::string::npos)
      << r.Diagnostics;
}

TEST(Enum, NameShadowsBuiltinFunctionRejected) {
  auto r = semaCheck(withEnums("enum open { Read, Write }", "return 0;"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find(
                "'open' is a builtin function and cannot be redeclared"),
            std::string::npos)
      << r.Diagnostics;
}

// --- Variant access ---------------------------------------------------------

TEST(Enum, UnknownVariantRejected) {
  auto r = semaCheck(
      withEnums("enum Color { Red, Green }", "c: Color = Color::Nope;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Enum, UnknownEnumNameRejected) {
  auto r = semaCheck(wrapMain("c: Nope = Nope::X;"));
  EXPECT_FALSE(r.Ok);
}

// --- Equality / non-convertibility ------------------------------------------

TEST(Enum, EqualitySameEnumOk) {
  auto r = semaCheck(withEnums("enum Color { Red, Green }",
                               "if (Color::Red == Color::Green) { }"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Enum, InequalitySameEnumOk) {
  auto r = semaCheck(withEnums("enum Color { Red, Green }",
                               "if (Color::Red != Color::Green) { }"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Enum, CompareDifferentEnumsRejected) {
  auto r = semaCheck("enum A { X, Y }\nenum B { P, Q }\n" +
                     wrapMain("if (A::X == B::P) { }"));
  EXPECT_FALSE(r.Ok);
}

TEST(Enum, CompareEnumWithIntRejected) {
  auto r =
      semaCheck(withEnums("enum Color { Red }", "if (Color::Red == 0) { }"));
  EXPECT_FALSE(r.Ok);
}

TEST(Enum, AssignEnumToIntRejected) {
  auto r = semaCheck(withEnums("enum Color { Red }", "x: int = Color::Red;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Enum, AssignIntToEnumRejected) {
  auto r = semaCheck(withEnums("enum Color { Red }", "c: Color = 0;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Enum, OrderingOperatorRejected) {
  auto r = semaCheck(withEnums("enum Color { Red, Green }",
                               "if (Color::Red < Color::Green) { }"));
  EXPECT_FALSE(r.Ok);
}

// --- Enum match -------------------------------------------------------------

TEST(Enum, MatchBareVariantsOk) {
  auto r = semaCheck(withEnums("enum Color { Red, Green, Blue }", R"(
    c: Color = Color::Red;
    match c {
      Red { }
      Green { }
      Blue { }
      _ { }
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Enum, MatchUnknownVariantRejected) {
  auto r = semaCheck(withEnums("enum Color { Red, Green }", R"(
    c: Color = Color::Red;
    match c {
      Red { }
      Nope { }
      _ { }
    }
  )"));
  EXPECT_FALSE(r.Ok);
}

TEST(Enum, MatchWildcardNotLastRejected) {
  auto r = semaCheck(withEnums("enum Color { Red, Green }", R"(
    c: Color = Color::Red;
    match c {
      _ { }
      Red { }
    }
  )"));
  EXPECT_FALSE(r.Ok);
}

// A wildcard-less enum match covering every variant (all arms returning)
// satisfies the non-void return requirement — no trailing return needed.
TEST(Enum, ExhaustiveMatchSatisfiesReturn) {
  auto r = semaCheck("enum Color { Red, Green, Blue }\n"
                     "fn pick(c: Color) -> Str {\n"
                     "  match c {\n"
                     "    Red   { return \"r\"; }\n"
                     "    Green { return \"g\"; }\n"
                     "    Blue  { return \"b\"; }\n"
                     "  }\n"
                     "}\n"
                     "fn main() -> int { return 0; }\n");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// A wildcard-less enum match that misses a variant is NOT exhaustive, so the
// non-void function can fall through without returning -> error.
TEST(Enum, NonExhaustiveMatchDoesNotSatisfyReturn) {
  auto r = semaCheck("enum Color { Red, Green, Blue }\n"
                     "fn pick(c: Color) -> Str {\n"
                     "  match c {\n"
                     "    Red   { return \"r\"; }\n"
                     "    Green { return \"g\"; }\n"
                     "  }\n"
                     "}\n"
                     "fn main() -> int { return 0; }\n");
  EXPECT_FALSE(r.Ok);
}
