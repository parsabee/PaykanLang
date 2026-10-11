// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: array type declarations, subscript access, and type checking.

#include "TestUtils.h"
#include <gtest/gtest.h>
#include <sstream>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// -- Array declarations

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

// -- Type mismatches

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

// -- Subscript access

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

// -- Array in function parameters and return types

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

// -- push / len / pop method type-checking

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

// -- Str indexing type

TEST(Array, StrIndexIsChar) {
  // Assigning s[i] to Str should fail — the result is char.
  auto r = semaCheck(wrapMain(R"(
    s: Str = "hi";
    bad: Str = s[0];
  )"));
  EXPECT_FALSE(r.Ok);
}

// -- Array equality: == / != are the virtual `equals`, identity semantics

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

// -- Canonical (interned) array types

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
  std::ostringstream diagOS;
  paykan::sema::DiagEngine diag(diagOS);
  diag.setSourceInfo(driver->getCurrentFile(), &driver->getSourceLines());
  paykan::sema::Sema sema(driver->getASTContext(), diag, "");
  auto semaCtx = sema.run(driver->getRoot());
  return {semaCtx.Ok, diagOS.str(), std::move(driver)};
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

// -- Buffer<T>: growable storage reached only through its methods

namespace {
bool hasDiag(const SemaResult &r, const std::string &text) {
  return r.Diagnostics.find(text) != std::string::npos;
}
} // namespace

TEST(Buffer, MethodsOnEveryElementKind) {
  auto r = semaCheck(R"(
    class P { x: int; fn __init__(x: int) { self.x = x; } }
    fn total(b: view Buffer<int>) -> int {
      s: int = 0; i: int = 0;
      while (i < b.len()) { s = s + b.get(i); i = i + 1; }
      return s;
    }
    fn main() -> int {
      a = Buffer<int>(); a.push(1); a.set(0, 2); n: int = a.pop();
      f: Buffer<float> = Buffer<float>(); f.push(1); f.set(0, 2);
      s = Buffer<Str>(); s.push("x"); t: Str = s.get(0);
      p = Buffer<P>(); p.push(P(1)); q: P = p.get(0);
      o = Buffer<int?>(); o.push(None); o.push(3);
      nested = Buffer<Buffer<int>>(); nested.push(a);
      tup = Buffer<(int, Str)>(); tup.push((1, "a"));
      asObj: Obj = a;
      return total(a) + nested.get(0).len();
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics, "");
}

TEST(Buffer, InGenericsAndInference) {
  auto r = semaCheck(R"(
    class Stack<T> {
      items: Buffer<T>;
      fn __init__() { self.items = Buffer<T>(); }
      fn push(v: T) { self.items.push(v); }
      view fn top() -> T { return self.items.get(self.items.len() - 1); }
    }
    fn first<T>(b: view Buffer<T>) -> T { return b.get(0); }
    fn main() -> int {
      s = Stack<Str>(); s.push("a"); t: Str = s.top();
      b = Buffer<int>(); b.push(4);
      n: int = first(b);
      return n;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;

  // A `T[]` does not bind a `Buffer<T>` parameter, nor a Buffer a `T[]` one.
  auto r2 = semaCheck(R"(
    fn g<T>(b: Buffer<T>) -> int { return b.len(); }
    fn h<T>(xs: T[]) -> int { return xs.len(); }
    fn main() -> int { a: int[] = [1]; b = Buffer<int>(); return g(a) + h(b); }
  )");
  EXPECT_FALSE(r2.Ok);
  EXPECT_EQ(r2.ErrorCount, 2u) << r2.Diagnostics;
  EXPECT_TRUE(hasDiag(r2, "cannot infer type parameter 'T' of 'g'"))
      << r2.Diagnostics;
  EXPECT_TRUE(hasDiag(r2, "cannot infer type parameter 'T' of 'h'"))
      << r2.Diagnostics;
}

TEST(Buffer, NoSubscriptsNoLiteralsNotAnArray) {
  struct Case {
    const char *Body;
    const char *Diag;
  } cases[] = {
      {"b = Buffer<int>(); x = b[0];",
       "a 'Buffer<int>' has no '[]': use its get(i) and set(i, v) methods"},
      {"b = Buffer<int>(); b[0] = 1;",
       "a 'Buffer<int>' has no '[]': use its get(i) and set(i, v) methods"},
      {"b: Buffer<int> = [1, 2];",
       "initializer of type 'int[]' does not match declared type "
       "'Buffer<int>'"},
      {"b: Buffer<int> = [];",
       "an array literal is not a 'Buffer<int>': make one with "
       "'Buffer<int>()'"},
      {"b = Buffer<int>(); a: int[] = b;",
       "initializer of type 'Buffer<int>' does not match declared type "
       "'int[]'"},
      {"o: Obj = Buffer<int>(); match o { b: Buffer<int> { } _ { } }",
       "a match arm cannot test for 'Buffer<int>'"},
  };
  for (const Case &c : cases) {
    auto r = semaCheck(wrapMain(c.Body));
    EXPECT_FALSE(r.Ok) << c.Body;
    EXPECT_EQ(r.ErrorCount, 1u) << c.Body << "\n" << r.Diagnostics;
    EXPECT_TRUE(hasDiag(r, c.Diag)) << c.Body << "\n" << r.Diagnostics;
  }
}

TEST(Buffer, ConstructorAndTypeArguments) {
  struct Case {
    const char *Body;
    const char *Diag;
  } cases[] = {
      {"b = Buffer();", "'Buffer' needs its element type: 'Buffer<int>()'"},
      {"b = Buffer<int, int>();", "'Buffer' expects 1 type argument, got 2"},
      {"b = Buffer<int>(3);",
       "function 'Buffer<int>' expects 0 argument(s), got 1"},
      {"b: Buffer<void> = None;",
       "variable 'b' has type 'Buffer<void>': a buffer holds values"},
      {"b = Buffer<int>(); b.get(0, 1);",
       "method 'get' expects 1 argument(s), got 2"},
      {"b = Buffer<int>(); b.set(0, \"x\");",
       "argument 2 of 'set' has type 'Str', expected 'int'"},
  };
  for (const Case &c : cases) {
    auto r = semaCheck(wrapMain(c.Body));
    EXPECT_FALSE(r.Ok) << c.Body;
    EXPECT_TRUE(hasDiag(r, c.Diag)) << c.Body << "\n" << r.Diagnostics;
  }
}

TEST(Buffer, NameIsReserved) {
  for (const char *decl :
       {"class Buffer { x: int; }", "class Buffer<T> { x: T; }",
        "fn Buffer() -> int { return 0; }"}) {
    auto r = semaCheck(std::string(decl) + "\nfn main() -> int { return 0; }");
    EXPECT_FALSE(r.Ok) << decl;
    EXPECT_TRUE(hasDiag(r, "'Buffer' is a builtin class and cannot be "
                           "redeclared"))
        << decl << "\n"
        << r.Diagnostics;
  }
}

// Only `len` and `get` read: a `view` of a buffer reaches no other method,
// and `push` and `set` keep their value, which a view cannot give.
TEST(Buffer, ViewRules) {
  auto r = semaCheck(R"(
    fn f(b: view Buffer<int>) { b.set(0, 1); b.push(2); n: int = b.pop(); }
    fn g(s: view Str, b: Buffer<Str>) { b.push(s); b.set(0, s); }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 5u) << r.Diagnostics;
  EXPECT_TRUE(hasDiag(r, "'b' is a 'view' parameter; 'set' is not a 'view fn'"))
      << r.Diagnostics;
  EXPECT_TRUE(hasDiag(r, "'b' is a 'view' parameter; 'pop' is not a 'view fn'"))
      << r.Diagnostics;
  EXPECT_TRUE(hasDiag(r, "'s' is a 'view' parameter; it cannot be stored"))
      << r.Diagnostics;
}

// Nothing borrows an element in place: `get` returns a new value, so a
// container cannot hand out an `inout` or `view` into its buffer (the
// standard library's List<T> is reached only through values).
TEST(Buffer, NoBorrowIntoElements) {
  auto r = semaCheck(R"(
    class Bag<T> {
      items: Buffer<T>;
      fn __init__() { self.items = Buffer<T>(); }
      fn ref(i: int) -> inout T { return self.items.get(i); }
      view fn peek(i: int) -> view T { return self.items.get(i); }
    }
    fn main() -> int {
      b = Bag<int>();
      b.items.push(1);
      e: inout = b.items.get(0);
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
  for (const char *diag :
       {"an 'inout' result must be part of 'self' or of an 'inout' "
        "parameter, not a new value",
        "a 'view' result must be part of 'self' or of a 'view' or 'inout' "
        "parameter, not a new value",
        "'inout' local 'e' must name a variable or a field"})
    EXPECT_TRUE(hasDiag(r, diag)) << diag << "\n" << r.Diagnostics;
}
