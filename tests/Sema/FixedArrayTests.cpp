// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: fixed-size arrays, `T[n]` (docs/language/05-arrays.md,
// "Fixed-size arrays").  For now the type of a local variable, a parameter,
// a result or a field, of scalars, enums, references and other such arrays.

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

// A field is part of its object: changed only where the object can be, and
// assigned by `__init__` like any other.
TEST(FixedArray, Fields) {
  auto r = semaCheck(R"(
    class Grid { cells: int[2][3]; tags: Str[2]; }
    impl Grid {
      fn set(c: int, v: int) { self.cells[1][c] = v; }
      view fn at(c: int) -> int { return self.cells[1][c]; }
    }
    class Box<T> { xs: T[2]; }
    fn total(xs: view int[3]) -> int { return xs[0] + xs[1] + xs[2]; }
    fn main() -> int {
      g = Grid([[1, 2, 3], [4, 5, 6]], ["a", "b"]);
      g.cells[0] = [7, 8, 9];
      g.tags = [g.tags[1], "c"];
      b = Box<int>([1, 2]);
      return total(g.cells[1]) + g.at(0) + b.xs[1] + g.tags[0].len();
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  expectError("class G { c: int[2]; }\n"
              "impl G { view fn f() { self.c[0] = 1; } }\n"
              "fn main() -> int { return 0; }",
              "'self' is read-only in 'view fn f'; cannot assign to its "
              "elements");
  expectError("class G { c: int[2]; }\n"
              "fn f(g: view G) { g.c[1] = 1; }\n"
              "fn main() -> int { return 0; }",
              "'g' is a 'view' parameter; cannot assign to its elements");
  expectError("class G { c: int[2]; n: int; }\n"
              "impl G { fn __init__() { self.n = 1; } }\n"
              "fn main() -> int { return 0; }",
              "field 'c' of class 'G' is not assigned on every path through "
              "'__init__'");
}

TEST(FixedArray, OnlyALocalAParameterAResultOrAFieldForNow) {
  const std::string notYet = "a fixed-size array ('int[2]') can only be a "
                             "local variable, a parameter, a result or a "
                             "field for now";
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

// An optional value takes a presence flag padded to its value's alignment
// and the value.  An array holds at most 16384 elements, counted across its
// rows.
TEST(FixedArray, ElementsAndSize) {
  expectOk("  os: int?[2] = [None, 1];\n"
           "  bs: bool?[3] = [True, None, False];\n"
           "  os[0] = os[1];\n"
           "  b: bool = os[0] == None && bs[1] != None;");
  auto atMost =
      semaCheck("fn f(a: view int?[16384], b: view char[128][128]) {}\n"
                "fn main() -> int { return 0; }");
  EXPECT_TRUE(atMost.Ok) << atMost.Diagnostics;
  expectBodyError("  big: int?[16385] = [];", "which is too big");
  expectBodyError("  big: int[200000] = [];",
                  "variable 'big' has type 'int[200000]', which is too big: a "
                  "fixed-size array holds at most 16384 elements");
  expectBodyError("  big: char[128][129] = [];", "which is too big");
  expectBodyError("  big: char[1][9223372036854775807] = [];",
                  "which is too big");
}

// A `view T[]` or `inout T[]` parameter is a slice: it takes a fixed-size
// array of any length (a literal becomes one) or another slice, and is
// indexed, measured with `len()` and passed on, nothing else.
TEST(FixedArray, SliceParameters) {
  auto r = semaCheck(R"(
    fn sum(xs: view int[]) -> int { return xs[0] + xs.len(); }
    fn bump(xs: inout int[]) { xs[0] = sum(xs); }
    fn count<T>(xs: view T[]) -> int { return xs.len(); }
    class C { n: int; }
    impl C { fn __init__(xs: view Str[]) { self.n = xs[0].len(); } }
    fn main() -> int {
      a: int[3] = [1, 2, 3];
      g: int[2][2] = [[1, 2], [3, 4]];
      bump(a);
      c = C(["ab"]);
      return sum(a) + sum([4, 5]) + sum(g[1]) + count(a) + c.n;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  const std::string slice = "a slice ('int[]') can only be indexed, measured "
                            "with 'len()' or passed to a 'view' or 'inout' "
                            "array parameter";
  expectError("fn f(xs: view int[]) {}\n"
              "fn main() -> int { g = [1, 2]; f(g); return 0; }",
              "argument 1 of 'f' has type 'int[]', expected 'int[]' (a slice "
              "takes a fixed-size array, not a growable one)");
  expectError("fn f(xs: view float[]) {}\n"
              "fn main() -> int { a: int[1] = [1]; f(a); return 0; }",
              "(a slice takes a fixed-size array of exactly its element "
              "type)");
  expectError("fn f(xs: inout int[]) { xs.push(1); }\n"
              "fn main() -> int { return 0; }",
              "'int[]' has no method 'push' with these arguments (a slice "
              "has 'len()')");
  expectError("fn f(xs: inout int[]) { xs = [1]; }\n"
              "fn main() -> int { return 0; }",
              "'xs' is a slice; assign its elements ('xs[i] = ...')");
  expectError("fn f(xs: inout int[]) { ys = xs; }\n"
              "fn main() -> int { return 0; }",
              "variable 'ys': " + slice);
  expectError("fn f(xs: inout int[]) { t = (xs, 1); }\n"
              "fn main() -> int { return 0; }",
              "tuple element 0: " + slice);
  expectError("fn id<T>(x: T) -> int { return 0; }\n"
              "fn f(xs: view int[]) -> int { return id(xs); }\n"
              "fn main() -> int { return 0; }",
              "type argument 1 of 'id': " + slice);
  expectError("fn f(xs: view int[]) { xs[0] = 1; }\n"
              "fn main() -> int { return 0; }",
              "'xs' is a 'view' parameter; cannot assign to its elements");
  expectError("fn f(xs: inout int[]) {}\n"
              "fn main() -> int { let a: int[1] = [1]; f(a); return 0; }",
              "'a' is declared with 'let' and cannot be passed to 'inout' "
              "parameter 'xs'");
}

// A length of `_` in a variable declaration with an initializer is the
// initializer's: an array literal's element count (its first element's
// lengths for a nested `_`), or the length of the fixed-size array anything
// else is.
TEST(FixedArray, InferredLengths) {
  auto r = semaCheck(R"(
    class A {}
    class B : A {}
    fn fill<T>(v: T) -> T[3] { return [v, v, v]; }
    fn main() -> int {
      x: int[_][_] = [[1, 2, 3], [4, 5, 6]];
      y: A[_] = [B(), A(), B()];
      z: float[_] = fill<float>(1.5);
      return x[1][2] + y.len() + z.len();
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  expectOk("  x: int[_][_] = [[1, 2, 3], [4, 5, 6]];\n"
           "  a: int[2][3] = x;\n"
           "  rows: int[_][_] = [a[0], [7, 8, 9], x[1]];\n"
           "  y: int[2][_] = x;\n"
           "  let c: char[_] = ['h', 'i'];\n"
           "  f: float[_] = [1, 2.5];\n"
           "  v: view int[_] = x[1];\n"
           "  n: int = a[1][2] + y[0].len() + c.len() + v[0] + "
           "rows[2][0];");
  const std::string onlyHere = "has type 'int[_]': a length can be '_' only "
                               "in a variable declaration with an "
                               "initializer, which gives it";
  expectError("fn f(x: int[_]) {}\nfn main() -> int { return 0; }",
              "parameter 'x' " + onlyHere);
  expectError("fn f() -> int[_] { return [1]; }\n"
              "fn main() -> int { return 0; }",
              "function 'f' return type " + onlyHere);
  expectError("class C { xs: int[_]; }\nfn main() -> int { return 0; }",
              "field 'xs' in class 'C' " + onlyHere);
  expectBodyError("  x: int[_] = [];",
                  "an empty array literal gives 'int[_]' no length: a "
                  "fixed-size array has at least one element");
  expectBodyError("  x: int[_] = 3;",
                  "'int[_]' takes its lengths from a fixed-size array of its "
                  "shape, not from 'int'");
  expectBodyError("  x: int[_][_] = [1, 2];",
                  "'int[_]' takes its lengths from a fixed-size array of its "
                  "shape, not from 'int'");
  expectBodyError("  x: int[_][_] = [[1, 2], [3]];",
                  "array literal has 1 element(s), but 'int[2]' has 2");
  expectError("fn big() -> int[8192] { return big(); }\n"
              "fn main() -> int {\n  x: int[_][_] = [big(), big(), big()];\n"
              "  return 0;\n}\n",
              "variable 'x' has type 'int[3][8192]', which is too big");
}
