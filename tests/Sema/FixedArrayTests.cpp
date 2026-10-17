// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: fixed-size arrays, `T[n]` (docs/language/05-arrays.md,
// "Fixed-size arrays").  For now the type of a local variable, a parameter
// or a result, of scalars, enums, references and other such arrays.

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
  EXPECT_NE(r.Diagnostics.find(message), std::string::npos) << source << "\n"
                                                            << r.Diagnostics;
}

void expectBodyError(const std::string &body, const std::string &message) {
  expectError(wrapMain(body), message);
}

} // namespace

TEST(FixedArray, LocalsOfScalarsAndEnums) {
  expectOk(
      "  xs: int[3] = [1, 2, 3];\n"
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

// A parameter in every mode and a result can be a fixed-size array; a
// literal argument or result takes the slot's type.
TEST(FixedArray, ParametersAndResults) {
  auto r = semaCheck(R"(
    fn total(xs: view int[3]) -> int { return xs[0] + xs[1] + xs[2]; }
    fn twice(xs: int[3]) -> int[3] {
      xs[0] = xs[0] * 2;
      return xs;
    }
    fn zero(xs: inout int[3]) { xs = [0, 0, 0]; }
    fn unit() -> float[2][2] { return [[1, 0], [0, 1]]; }
    class C { n: int; }
    impl C {
      fn __init__(xs: view int[3]) { self.n = total(xs); }
      view fn row(i: int) -> int[3] { return [i, self.n, i]; }
    }
    fn main() -> int {
      xs: int[3] = [1, 2, 3];
      ys = twice(xs);
      zero(ys);
      c = C([4, 5, 6]);
      zs: int[3] = c.row(total(xs));
      return total(twice([1, 1, 1])) + int(unit()[1][1]) + c.row(1)[0];
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  expectError("fn f(xs: int[3]) {}\n"
              "fn main() -> int { f([1, 2]); return 0; }",
              "array literal has 2 element(s), but 'int[3]' has 3");
  expectError("fn f(xs: view int[3]) { xs[0] = 1; }\n"
              "fn main() -> int { return 0; }",
              "'xs' is a 'view' parameter; cannot assign to its elements");
  expectError("fn f() -> view int[2] { return [1, 2]; }\n"
              "fn main() -> int { return 0; }",
              "a 'view' result of a fixed-size array ('int[2]') is not "
              "supported yet; return a copy ('-> int[2]')");
  expectError("native fn f(xs: int[2]) = \"pk_f\";\n"
              "fn main() -> int { return 0; }",
              "'int[2]' cannot cross into C");
}

TEST(FixedArray, OnlyALocalAParameterOrAResultForNow) {
  const std::string notYet = "a fixed-size array ('int[2]') can only be a "
                             "local variable, a parameter or a result for now";
  expectError("class C { a: int[2]; }\nfn main() -> int { return 0; }", notYet);
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

// An element is a reference like any other: a view of the array is a view
// of its elements, and an element is not an `inout` place yet.
TEST(FixedArray, ElementsOfReferenceTypes) {
  auto r = semaCheck(R"(
    class P { x: int; }
    fn names() -> Str[2] { return ["a", "b"]; }
    fn main() -> int {
      ss: Str[3] = ["a", names()[1], "c" + "d"];
      ps: P[2] = [P(1), P(2)];
      ps[1].x = ps[0].x;
      let fixed: P[1] = [P(3)];
      fixed[0].x = 4;
      os: Str?[2] = [None, ss[0]];
      ts: (int, Str)[1] = [(1, "one")];
      arrs: int[][2] = [[1], []];
      arrs[1].push(ts[0].0);
      grid: Str[2][2] = [[ss[0], ss[1]], names()];
      return ss[2].len() + grid[1][0].len();
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  expectError("class P { x: int; }\n"
              "fn f(ps: view P[2]) { ps[0].x = 1; }\n"
              "fn main() -> int { return 0; }",
              "'ps' is a 'view' parameter");
  expectError("fn f(ss: view Str[2]) -> Str { return ss[0]; }\n"
              "fn main() -> int { return 0; }",
              "'ss' is a 'view' parameter; it cannot be returned");
  expectBodyError("  xs: int[2] = [1, 2];\n  r: inout = xs[1];",
                  "an array element cannot be named by 'inout' local 'r' "
                  "yet");
}

TEST(FixedArray, ElementsAndSize) {
  expectBodyError("  os: int?[2] = [None, 1];",
                  "variable 'os' has type 'int?[2]': a fixed-size array of "
                  "'int?' is not supported yet");
  expectBodyError("  g: int?[2][3] = [];", "has type 'int?[2][3]'");
  expectBodyError("  big: int[200000] = [];",
                  "variable 'big' has type 'int[200000]', which is too big: a "
                  "fixed-size array holds at most 1048576 bytes");
  expectBodyError("  big: char[1024][1025] = [];", "which is too big");
}
