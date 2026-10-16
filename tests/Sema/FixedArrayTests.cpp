// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: fixed-size arrays, `T[n]` (docs/language/05-arrays.md,
// "Fixed-size arrays").  For now a local variable's own type only, of
// scalars, enums and other such arrays.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

std::string wrapMain(const std::string &body) {
  return "enum Color { Red, Green }\nfn main() -> int {\n" + body +
         "\n  return 0;\n}\n";
}

void expectOk(const std::string &body) {
  auto r = semaCheck(wrapMain(body));
  EXPECT_TRUE(r.Ok) << body << "\n" << r.Diagnostics;
}

void expectError(const std::string &source, const std::string &message) {
  auto r = semaCheck(source);
  EXPECT_FALSE(r.Ok) << source;
  EXPECT_NE(r.Diagnostics.find(message), std::string::npos)
      << source << "\n"
      << r.Diagnostics;
}

void expectBodyError(const std::string &body, const std::string &message) {
  expectError(wrapMain(body), message);
}

} // namespace

TEST(FixedArray, LocalsOfScalarsAndEnums) {
  expectOk("  xs: int[3] = [1, 2, 3];\n"
           "  fs: float[2] = [1, 2.5];\n" // an int element is promoted
           "  bs: bool[1] = [True];\n"
           "  cs: char[2] = ['a', 'b'];\n"
           "  es: Color[2] = [Color::Red, Color::Green];\n"
           "  grid: int[2][3] = [[1, 2, 3], [4, 5, 6]];\n"
           "  row = grid[1];\n"
           "  n: int = row[2] + xs[0] + grid[0][1] + xs.len() + grid[1].len();\n"
           "  ys = xs;\n"
           "  ys[0] = n;\n"
           "  xs = ys;\n"
           "  grid[0] = row;\n"
           "  v: view = xs;\n"
           "  first = v[0];\n"
           "  r: inout = xs;\n"
           "  r[1] = first;");
}

TEST(FixedArray, LiteralsHaveTheArraysLengthAndElementType) {
  expectBodyError("  xs: int[3] = [1, 2];",
                  "array literal has 2 element(s), but 'int[3]' has 3");
  expectBodyError("  xs: int[2] = [1, \"a\"];",
                  "array literal element of type 'Str' does not match the "
                  "element type 'int' of 'int[2]'");
  expectBodyError("  g: int[2][2] = [[1, 2], [3]];",
                  "array literal has 1 element(s), but 'int[2]' has 2");
  // Two different lengths are two types.
  expectBodyError("  a: int[2] = [1, 2];\n  b: int[3] = a;",
                  "initializer of type 'int[2]' does not match declared type "
                  "'int[3]'");
  // A literal without a fixed-size array slot is still a growable array.
  expectBodyError("  a = [1, 2];\n  b: int[2] = a;",
                  "initializer of type 'int[]' does not match declared type "
                  "'int[2]'");
}

TEST(FixedArray, ConstantIndexesAreChecked) {
  expectBodyError("  xs: int[3] = [1, 2, 3];\n  y = xs[3];",
                  "index 3 is out of bounds for 'int[3]' (its indexes are 0 "
                  "to 2)");
  expectBodyError("  xs: int[3] = [1, 2, 3];\n  xs[-1] = 0;",
                  "index -1 is out of bounds for 'int[3]'");
  expectBodyError("  xs: int[3] = [1, 2, 3];\n  y = xs[True];",
                  "array index must be 'int', got 'bool'");
}

TEST(FixedArray, ElementsArePartOfTheValue) {
  expectBodyError("  let xs: int[2] = [1, 2];\n  xs[0] = 5;",
                  "'xs' is declared with 'let'; its elements are part of its "
                  "value and cannot be assigned");
  expectBodyError("  let g: int[2][2] = [[1, 2], [3, 4]];\n  g[1][0] = 5;",
                  "'g' is declared with 'let'");
  expectBodyError("  xs: int[2] = [1, 2];\n  v: view = xs;\n  v[0] = 5;",
                  "'v' is a 'view' local; cannot assign to its elements");
  expectBodyError("  xs: int[2] = [1, 2];\n  xs[0] = 1.5;",
                  "cannot assign value of type 'float' to array of 'int'");
}

TEST(FixedArray, HasOnlyLen) {
  expectBodyError("  xs: int[2] = [1, 2];\n  xs.push(3);",
                  "'int[2]' has no method 'push' with these arguments (a "
                  "fixed-size array has 'len()')");
  expectBodyError("  xs: int[2] = [1, 2];\n  ys: int[2] = [1, 2];\n"
                  "  b = xs == ys;",
                  "int[2]");
}

TEST(FixedArray, OnlyALocalsOwnTypeForNow) {
  const std::string notYet = "a fixed-size array ('int[2]') is only supported "
                             "as a local variable's own type for now";
  expectError("fn f(a: int[2]) {}\nfn main() -> int { return 0; }", notYet);
  expectError("fn f() -> int[2] { return [1, 2]; }\n"
              "fn main() -> int { return 0; }",
              notYet);
  expectError("class C { a: int[2]; }\nfn main() -> int { return 0; }",
              notYet);
  expectBodyError("  o: int[2]? = None;", notYet);
  expectBodyError("  a: int[2][] = [];", notYet);
  expectBodyError("  xs: int[2] = [1, 2];\n  t = (xs, 1);",
                  "tuple element 0: " + notYet);
  expectBodyError("  xs: int[2] = [1, 2];\n  zs = [xs, xs];",
                  "an element of a growable array: " + notYet);
  expectBodyError("  xs: int[2] = [1, 2];\n  c = if True then xs else xs;",
                  "a conditional expression: " + notYet);
  expectError("fn id<T>(x: T) -> T { return x; }\n"
              "fn main() -> int {\n  xs: int[2] = [1, 2];\n  ys = id(xs);\n"
              "  return 0;\n}\n",
              "type argument 1 of 'id': " + notYet);
}

TEST(FixedArray, ElementsAndSize) {
  expectBodyError("  ss: Str[2] = [\"a\", \"b\"];",
                  "variable 'ss' has type 'Str[2]': a fixed-size array of "
                  "'Str' is not supported yet");
  expectBodyError("  g: Str[2][3] = [];", "has type 'Str[2][3]'");
  expectBodyError("  big: int[200000] = [];",
                  "variable 'big' has type 'int[200000]', which is too big: a "
                  "fixed-size array holds at most 1048576 bytes");
  expectBodyError("  big: char[1024][1025] = [];", "which is too big");
}
