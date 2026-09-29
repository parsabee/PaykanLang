// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: array type declarations, subscript access, and type checking.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// ============================================================================
// Array declarations
// ============================================================================

TEST(Array, IntArrayLiteral) {
  auto r = semaCheck(wrapMain("a: int[] = [1, 2, 3];"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, FloatArrayLiteral) {
  auto r = semaCheck(wrapMain("a: float[] = [1.0, 2.0];"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, BoolArrayLiteral) {
  auto r = semaCheck(wrapMain("a: bool[] = [True, False];"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, StrArrayLiteral) {
  auto r = semaCheck(wrapMain(R"(a: Str[] = ["x", "y"];)"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, EmptyArrayWithAnnotation) {
  auto r = semaCheck(wrapMain("a: int[] = [];"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, ObjArrayEmpty) {
  auto r = semaCheck(wrapMain("a: Obj[] = [];"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// Type mismatches
// ============================================================================

TEST(Array, WrongElementType) {
  // Mixing int and Str in a literal should fail type checking.
  auto r = semaCheck(wrapMain(R"(a: int[] = [1, "two", 3];)"));
  EXPECT_FALSE(r.Ok);
}

TEST(Array, AssignWrongArrayType) {
  // Assigning a Str[] to an int[] variable.
  auto r = semaCheck(wrapMain(R"(
    a: int[] = [1];
    b: Str[] = ["x"];
    a = b;
  )"));
  EXPECT_FALSE(r.Ok);
}

TEST(Array, DeclTypeMismatch) {
  // Annotated as int[] but initialised with Str literal elements.
  auto r = semaCheck(wrapMain(R"(a: int[] = ["hello"];)"));
  EXPECT_FALSE(r.Ok);
}

// ============================================================================
// Subscript access
// ============================================================================

TEST(Array, IntSubscriptRead) {
  auto r = semaCheck(wrapMain(R"(
    a: int[] = [10, 20];
    x: int = a[0];
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, StrSubscriptRead) {
  auto r = semaCheck(wrapMain(R"(
    a: Str[] = ["hi"];
    s: Str = a[0];
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, StringCharSubscript) {
  // s[i] on a Str yields char, not Str.
  auto r = semaCheck(wrapMain(R"(
    s: Str = "hello";
    c: char = s[0];
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, SubscriptAssignInt) {
  auto r = semaCheck(wrapMain(R"(
    a: int[] = [1, 2, 3];
    a[1] = 99;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, SubscriptAssignWrongType) {
  auto r = semaCheck(wrapMain(R"(
    a: int[] = [1, 2];
    a[0] = "bad";
  )"));
  EXPECT_FALSE(r.Ok);
}

// ============================================================================
// Array in function parameters and return types
// ============================================================================

TEST(Array, ArrayParam) {
  auto r = semaCheck(R"(
    fn sum(vals: int[]) -> int {
      return 0;
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, ArrayReturnType) {
  auto r = semaCheck(R"(
    fn makeArr() -> int[] {
      a: int[] = [1, 2];
      return a;
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, PassArrayToFunction) {
  auto r = semaCheck(R"(
    fn first(a: int[]) -> int { return a[0]; }
    fn main() -> int {
      arr: int[] = [5, 6];
      x: int = first(arr);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, WrongArrayParamType) {
  auto r = semaCheck(R"(
    fn takesIntArr(a: int[]) -> int { return 0; }
    fn main() -> int {
      s: Str[] = ["x"];
      takesIntArr(s);
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
}

// ============================================================================
// push / len / pop method type-checking
// ============================================================================

TEST(Array, PushCorrectType) {
  auto r = semaCheck(wrapMain(R"(
    a: int[] = [];
    a.push(42);
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, LenReturnsInt) {
  auto r = semaCheck(wrapMain(R"(
    a: int[] = [1, 2, 3];
    n: int = a.len();
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// Str indexing type
// ============================================================================

TEST(Array, StrIndexIsChar) {
  // Assigning s[i] to Str should fail — the result is char.
  auto r = semaCheck(wrapMain(R"(
    s: Str = "hi";
    bad: Str = s[0];
  )"));
  EXPECT_FALSE(r.Ok);
}

// ============================================================================
// Array equality (== / != lower to the virtual `equals` — identity semantics)
// ============================================================================

TEST(Array, IntArrayEqualityOk) {
  auto r = semaCheck(wrapMain(R"(
    a: int[] = [1, 2];
    b: int[] = [1, 2];
    same: bool = a == a;
    eq: bool = a == b;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, IntArrayInequalityOk) {
  auto r = semaCheck(wrapMain(R"(
    a: int[] = [1, 2];
    b: int[] = [3];
    ne: bool = a != b;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, StrArrayEqualityOk) {
  auto r = semaCheck(wrapMain(R"(
    a: Str[] = ["x", "y"];
    b: Str[] = ["x", "y"];
    eq: bool = a == b;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, ClassElementArrayEqualityOk) {
  auto r = semaCheck(R"(
    class P {
      v: int;
      fn __init__(v: int) { self.v = v; }
    }
    fn main() -> int {
      a: P[] = [P(1)];
      b: P[] = [P(1)];
      if (a == b) { println("eq"); }
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, ArrayLiteralEqualityOk) {
  // Both literals resolve to the canonical int[] type.
  auto r = semaCheck(wrapMain(R"(
    eq: bool = [1, 2] == [1, 2];
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Array, MismatchedElementArrayEqualityRejected) {
  auto r = semaCheck(wrapMain(R"(
    a: int[] = [1];
    b: Str[] = ["x"];
    eq: bool = a == b;
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("mismatched types"), std::string::npos);
}

TEST(Array, ArrayVsScalarEqualityRejected) {
  auto r = semaCheck(wrapMain(R"(
    a: int[] = [1];
    eq: bool = a == 1;
  )"));
  EXPECT_FALSE(r.Ok);
}

// ============================================================================
// Canonical (interned) array types
// ============================================================================

namespace {

/// Like semaCheck, but keeps the parser driver (and with it the ASTContext)
/// alive so the caller can inspect the resolved types after analysis.
struct SemaRun {
  bool Ok;
  std::string Diagnostics;
  std::unique_ptr<paykan::parser::ParserDriver> Driver;
};

SemaRun semaRun(const std::string &source) {
  auto [parseOk, driver] = parse(source);
  if (!parseOk)
    return {false, "parse error", std::move(driver)};
  std::string diagStr;
  llvm::raw_string_ostream diagOS(diagStr);
  paykan::sema::DiagEngine diag(diagOS);
  diag.setSourceInfo(driver->getCurrentFile(), &driver->getSourceLines());
  paykan::sema::Sema sema(driver->getASTContext(), diag, "");
  auto semaCtx = sema.run(driver->getRoot());
  return {semaCtx.Ok, diagStr, std::move(driver)};
}

/// Number of specialized Array<...> ClassTypes registered under @p name.
size_t countSpecializedArrays(const paykan::ast::ASTContext &ctx,
                              const std::string &name) {
  size_t n = 0;
  for (auto &entry : ctx.getSpecializedArrayTypes())
    if (entry.second->getName() == name)
      ++n;
  return n;
}

} // namespace

TEST(ArrayCanonical, ContextInternsPerElementType) {
  paykan::ast::ASTContext ctx;
  auto *intArr = ctx.getArrayType(ctx.getIntTy());
  ASSERT_NE(intArr, nullptr);
  EXPECT_EQ(intArr, ctx.getArrayType(ctx.getIntTy()));
  EXPECT_EQ(intArr->getElementType(), ctx.getIntTy());

  // Nesting: the element of int[][] is the canonical int[] node, so int[][]
  // is itself canonical at any depth.
  auto *intArr2 = ctx.getArrayType(intArr);
  EXPECT_EQ(intArr2, ctx.getArrayType(ctx.getArrayType(ctx.getIntTy())));
  EXPECT_EQ(intArr2->getElementType(), intArr);
  EXPECT_NE(intArr2, intArr);
  EXPECT_NE(ctx.getArrayType(ctx.getStrTy()), intArr);

  // The specialized class cache keys on the canonical element type, so
  // Array<int[]> is built exactly once no matter how often it is requested.
  auto *spec = ctx.getOrCreateSpecializedArrayType(intArr);
  EXPECT_EQ(spec, ctx.getOrCreateSpecializedArrayType(
                      ctx.getArrayType(ctx.getIntTy())));
  EXPECT_EQ(spec->getName(), "Array<int[]>");
  EXPECT_EQ(countSpecializedArrays(ctx, "Array<int[]>"), 1u);
  EXPECT_EQ(ctx.lookupClassType("Array<int[]>"), spec);
}

TEST(ArrayCanonical, NestedAnnotationsResolveToSameType) {
  // Two field annotations, a method return type, and locals all spell
  // int[][]; each is a distinct parser node but Sema must resolve every one
  // to the same canonical Type*.
  auto r = semaRun(R"(
    class Grid {
      a: int[][];
      b: int[][];
      fn __init__() {
        self.a = [];
        self.b = [[1, 2]];
        self.a.push([3]);
        self.b.push([4, 5]);
      }
      fn rows() -> int[][] { return self.a; }
    }
    fn main() -> int {
      g: Grid = Grid();
      x: int[][] = g.rows();
      y: int[][] = [[6]];
      x.push([7]);
      y.push(x[0]);
      x[0].push(8);
      y[0].push(9);
      return 0;
    }
  )");
  ASSERT_TRUE(r.Ok) << r.Diagnostics;

  auto &ctx = r.Driver->getASTContext();
  auto *grid = ctx.lookupClassType("Grid");
  ASSERT_NE(grid, nullptr);
  ASSERT_EQ(grid->getNumFields(), 2u);
  auto *aTy = grid->getFields()[0].second;
  auto *bTy = grid->getFields()[1].second;
  ASSERT_TRUE(paykan::ast::isa<paykan::ast::ArrayType>(aTy));
  EXPECT_EQ(aTy, bTy);

  auto *rows = grid->findMethod("rows");
  ASSERT_NE(rows, nullptr);
  EXPECT_EQ(rows->getReturnType(), aTy);

  // Structure matches the canonical chain int -> int[] -> int[][].
  auto *intArr = ctx.getArrayType(ctx.getIntTy());
  EXPECT_EQ(aTy, ctx.getArrayType(intArr));
  EXPECT_EQ(paykan::ast::cast<paykan::ast::ArrayType>(aTy)->getElementType(),
            intArr);

  // Every push() on an int[][] receiver went through one Array<int[]> class
  // (and every push() on an int[] receiver through one Array<int>); the
  // registry holds exactly one entry per name.
  EXPECT_EQ(countSpecializedArrays(ctx, "Array<int[]>"), 1u);
  EXPECT_EQ(countSpecializedArrays(ctx, "Array<int>"), 1u);
  auto *spec = ctx.lookupClassType("Array<int[]>");
  ASSERT_NE(spec, nullptr);
  EXPECT_EQ(ctx.getSpecializedArrayElemType(spec), intArr);
  EXPECT_EQ(ctx.getOrCreateSpecializedArrayType(intArr), spec);
}

TEST(ArrayCanonical, MultiDimTypeCheckAssignSubscript) {
  auto r = semaCheck(wrapMain(R"(
    a: int[][] = [[1, 2], [3]];
    b: int[][][] = [[[1]], [[2, 3]]];
    empty2: int[][] = [];
    empty3: Str[][][] = [];
    row: int[] = a[0];
    v: int = a[1][0];
    w: int = b[1][0][1];
    plane: int[][] = b[0];
    a[0] = [9];
    a[1][0] = 5;
    b[0] = a;
    b[0][0] = [7, 8];
    b[1][0][0] = 4;
    a = b[1];
    a = plane;
    empty2 = a;
    a.push([10]);
    b.push(a);
    b.push([]);
    row = a.pop();
    plane = b.pop();
    n: int = b[0].len();
    same: bool = a == b[0];
    diff: bool = a != plane;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ArrayCanonical, MultiDimArrayInSignatures) {
  auto r = semaCheck(R"(
    fn transpose(m: int[][]) -> int[][] { return m; }
    fn depth3(c: int[][][]) -> int[] { return c[0][0]; }
    fn main() -> int {
      m: int[][] = [[1, 2], [3, 4]];
      t: int[][] = transpose(m);
      t = transpose([[5]]);
      c: int[][][] = [m, t];
      first: int[] = depth3(c);
      first = depth3([[[6]]]);
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(ArrayCanonical, DimensionMismatchRejected) {
  {
    auto r = semaCheck(wrapMain(R"(
      a: int[][] = [[1]];
      b: int[][][] = [[[1]]];
      a = b;
    )"));
    EXPECT_FALSE(r.Ok);
  }
  {
    auto r = semaCheck(wrapMain(R"(
      a: int[][] = [[1]];
      b: int[][][] = [[[1]]];
      eq: bool = a == b;
    )"));
    EXPECT_FALSE(r.Ok);
    EXPECT_NE(r.Diagnostics.find("mismatched types"), std::string::npos);
  }
  {
    auto r = semaCheck(wrapMain(R"(
      a: int[][] = [[1]];
      a.push(1);
    )"));
    EXPECT_FALSE(r.Ok);
  }
  {
    auto r = semaCheck(wrapMain(R"(
      a: int[][] = [[1]];
      s: Str[][] = [["x"]];
      a[0] = s[0];
    )"));
    EXPECT_FALSE(r.Ok);
  }
  {
    auto r = semaCheck(wrapMain(R"(
      a: int[][] = [[1], ["x"]];
    )"));
    EXPECT_FALSE(r.Ok);
  }
}
