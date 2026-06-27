// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// CodeGen / E2E tests for the `enum` type.

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

TEST(EnumCodeGen, EqualityTrueBranch) {
  auto r = compileAndRun(withEnums("enum Color { Red, Green, Blue }", R"(
    c: Color = Color::Green;
    if (c == Color::Green) { println("yes"); }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "yes\n");
}

TEST(EnumCodeGen, EqualityFalseBranch) {
  auto r = compileAndRun(withEnums("enum Color { Red, Green, Blue }", R"(
    c: Color = Color::Green;
    if (c == Color::Red) { println("a"); } else { println("b"); }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "b\n");
}

TEST(EnumCodeGen, Inequality) {
  auto r = compileAndRun(withEnums("enum Color { Red, Green, Blue }", R"(
    c: Color = Color::Blue;
    if (c != Color::Red) { println("ne"); }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "ne\n");
}

TEST(EnumCodeGen, MatchVariant) {
  auto r = compileAndRun(withEnums("enum Color { Red, Green, Blue }", R"(
    c: Color = Color::Green;
    match c {
      Red   { println("r"); }
      Green { println("g"); }
      Blue  { println("b"); }
      _     { println("?"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "g\n");
}

TEST(EnumCodeGen, MatchWildcardFallthrough) {
  auto r = compileAndRun(withEnums("enum Color { Red, Green, Blue }", R"(
    c: Color = Color::Blue;
    match c {
      Red   { println("r"); }
      Green { println("g"); }
      _     { println("other"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "other\n");
}

TEST(EnumCodeGen, EnumThroughFunctionParam) {
  auto src =
      "enum Dir { North, East, South, West }\n"
      "fn step(d: Dir) -> int {\n"
      "  match d {\n"
      "    North { println(\"N\"); }\n"
      "    East  { println(\"E\"); }\n"
      "    _     { println(\"?\"); }\n"
      "  }\n"
      "  return 0;\n"
      "}\n" +
      wrapMain("step(Dir::East);\n  step(Dir::North);\n  step(Dir::South);");
  auto r = compileAndRun(src);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "E\nN\n?\n");
}

TEST(EnumCodeGen, VariantOrderingValues) {
  // The third variant (index 2) is reached only when the subject equals it,
  // proving variants get implicit increasing values 0,1,2.
  auto r = compileAndRun(withEnums("enum E { A, B, C }", R"(
    e: E = E::C;
    if (e == E::A) { println("0"); }
    if (e == E::B) { println("1"); }
    if (e == E::C) { println("2"); }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "2\n");
}

// A wildcard-less enum match that covers every variant (and whose arms all
// return) is exhaustive, so it satisfies the "always returns" requirement of a
// non-void function — no trailing return or `_` arm needed.
TEST(EnumCodeGen, ExhaustiveMatchSatisfiesReturn) {
  auto src = "enum Dir { North, East, South, West }\n"
             "fn name(d: Dir) -> Str {\n"
             "  match d {\n"
             "    North { return \"N\"; }\n"
             "    East  { return \"E\"; }\n"
             "    South { return \"S\"; }\n"
             "    West  { return \"W\"; }\n"
             "  }\n"
             "}\n" +
             wrapMain("println(name(Dir::South));");
  auto r = compileAndRun(src);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "S\n");
}

// Returning an enum from a function: the i64 value must flow back unboxed (an
// enum is not a ref type, so the return path must not wrap it in a
// PaykanShared).
TEST(EnumCodeGen, ReturnEnumFromFunction) {
  auto src = "enum Dir { North, East, South, West }\n"
             "fn opp(d: Dir) -> Dir {\n"
             "  match d {\n"
             "    North { return Dir::South; }\n"
             "    South { return Dir::North; }\n"
             "    East  { return Dir::West; }\n"
             "    West  { return Dir::East; }\n"
             "  }\n"
             "}\n" +
             wrapMain("x: Dir = opp(Dir::North);\n"
                      "  if (x == Dir::South) { println(\"south\"); }");
  auto r = compileAndRun(src);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "south\n");
}

// Arrays of enums are primitive (i64) arrays, not object arrays: literal
// construction, push, subscript get/set, and len must all treat the element as
// a raw value rather than a boxed PaykanShared*.
TEST(EnumCodeGen, ArrayOfEnums) {
  auto src =
      "enum Dir { North, East, South, West }\n"
      "fn nm(d: Dir) -> Str {\n"
      "  match d {\n"
      "    North { return \"N\"; }\n"
      "    East  { return \"E\"; }\n"
      "    South { return \"S\"; }\n"
      "    West  { return \"W\"; }\n"
      "  }\n"
      "}\n" +
      wrapMain("xs: Dir[] = [Dir::North, Dir::East];\n"
               "  xs.push(Dir::South);\n" // grow
               "  xs[0] = Dir::West;\n"   // subscript set
               "  i: int = 0;\n"
               "  while (i < xs.len()) { println(nm(xs[i])); i = i + 1; }");
  auto r = compileAndRun(src);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "W\nE\nS\n");
}
