// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: tuple types — element types, indexing, immutability,
// destructuring, assignability, tuples in signatures/fields/arrays, and the
// module round-trip of tuple-typed signatures.

#include "TestUtils.h"
#include <gtest/gtest.h>
#include <sstream>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// -- Literals and element types

TEST(Tuple, LiteralInferredAndIndexed) {
  auto r = semaCheck(wrapMain(R"(
    t = (1, "a", True);
    x: int = t.0;
    s: Str = t.1;
    b: bool = t.2;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Tuple, AnnotatedDeclMatchesLiteral) {
  auto r = semaCheck(wrapMain(R"(
    t: (int, Str) = (1, "a");
    u: (float, (int, bool)) = (1.5, (2, False));
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Tuple, ElementTypeMismatchOnIndexRead) {
  auto r = semaCheck(wrapMain(R"(
    t = (1, "a");
    s: Str = t.0;
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("does not match declared type 'Str'"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Tuple, IndexOutOfRange) {
  auto r = semaCheck(wrapMain(R"(
    t = (1, "a");
    x = t.2;
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("tuple index '.2' is out of range for type "
                               "'(int, Str)' (valid indices are .0 to .1)"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Tuple, IndexOnNonTuple) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 3;
    y = x.0;
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("applied to non-tuple type 'int'"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Tuple, NestedIndexChain) {
  auto r = semaCheck(wrapMain(R"(
    t = ((1, "x"), 2.5);
    s: Str = t.0.1;
    f: float = t.1;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Tuple, VoidElementRejected) {
  auto r = semaCheck(R"(
    fn nothing() {}
    fn main() -> int {
      t = (nothing(), 1);
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("has type 'void'"), std::string::npos)
      << r.Diagnostics;
}

TEST(Tuple, EmptyArrayLiteralElementRejected) {
  auto r = semaCheck(wrapMain("t = ([], 1);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("inside a tuple literal"), std::string::npos)
      << r.Diagnostics;
}

// -- Immutability

TEST(Tuple, IndexAssignmentRejected) {
  auto r = semaCheck(wrapMain(R"(
    t = (1, 2);
    t.0 = 5;
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("tuples are immutable"), std::string::npos)
      << r.Diagnostics;
}

// -- Destructuring

TEST(Tuple, DestructureDeclaresElementTypes) {
  auto r = semaCheck(R"(
    fn divmod(a: int, b: int) -> (int, int) { return (a / b, a % b); }
    fn main() -> int {
      q, r = divmod(7, 2);
      x: int = q + r;
      return x;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Tuple, DestructureElementTypeIsPrecise) {
  auto r = semaCheck(wrapMain(R"(
    n, s = (1, "a");
    bad: int = s;
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("initializer of type 'Str'"), std::string::npos)
      << r.Diagnostics;
}

TEST(Tuple, DestructureArityMismatch) {
  auto r = semaCheck(wrapMain(R"(
    a, b, c = (1, 2);
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("into 3 targets (it has 2 elements)"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Tuple, DestructureNonTuple) {
  auto r = semaCheck(wrapMain(R"(
    a, b = 42;
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("only tuples can be destructured"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Tuple, DestructureSkipAndTyped) {
  auto r = semaCheck(wrapMain(R"(
    a: int, _, c: Str = (1, 2.5, "z");
    _, f = (1, 2.5);
    g: float = f;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Tuple, DestructureTypedMismatch) {
  auto r = semaCheck(wrapMain(R"(
    a: Str, b: int = (1, 2);
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("does not match declared type 'Str' for "
                               "target 'a'"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Tuple, DestructureTypedRedeclaration) {
  auto r = semaCheck(wrapMain(R"(
    a: int = 0;
    a: int, b: int = (1, 2);
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("redeclaration of variable 'a'"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Tuple, DestructureReassignsExisting) {
  auto r = semaCheck(wrapMain(R"(
    a: int = 0;
    s: Str = "";
    a, s = (1, "x");
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Tuple, DestructureReassignTypeMismatch) {
  auto r = semaCheck(wrapMain(R"(
    a: int = 0;
    a, b = ("x", 1);
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("cannot assign element 0 of type 'Str' to "
                               "variable 'a' of type 'int'"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Tuple, DestructureDuplicateTarget) {
  auto r = semaCheck(wrapMain("a, a = (1, 2);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("duplicate target 'a'"), std::string::npos)
      << r.Diagnostics;
}

// -- Assignability, equality, and tuples in other positions

TEST(Tuple, AssignableToObj) {
  auto r = semaCheck(wrapMain(R"(
    o: Obj = (1, "a");
    println((1, 2));
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Tuple, RefElementCovariance) {
  auto r = semaCheck(R"(
    class A { fn __init__() {} }
    class B : A { fn __init__() { __super__(); } }
    fn main() -> int {
      t: (int, A) = (1, B());
      u: (int, Obj) = (1, "s");
      n: (int, (Str, Obj)) = (1, ("a", "b"));
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Tuple, NoPrimitivePromotionAcrossElements) {
  auto r = semaCheck(wrapMain("t: (float, int) = (1, 2);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("initializer of type '(int, int)' does not "
                               "match declared type '(float, int)'"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Tuple, ArityMismatchInAssignment) {
  auto r = semaCheck(wrapMain(R"(
    t: (int, int) = (1, 2);
    t = (1, 2, 3);
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("cannot assign value of type '(int, int, int)'"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Tuple, NestedAssignabilityIsStructural) {
  auto r = semaCheck(wrapMain(R"(
    a: (int, (Str, bool)) = (1, ("x", True));
    b: (int, (Str, bool)) = a;
    c: (int, (Str, int)) = a;
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'(int, (Str, bool))' does not match declared "
                               "type '(int, (Str, int))'"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Tuple, EqualitySameTypeOk) {
  auto r = semaCheck(wrapMain(R"(
    a = (1, "x");
    b = (1, "x");
    eq: bool = a == b;
    ne: bool = a != b;
    eq2: bool = a.equals(b);
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Tuple, EqualityMismatchedTuplesRejected) {
  auto r = semaCheck(wrapMain(R"(
    a = (1, "x");
    b = (1, 2);
    eq: bool = a == b;
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("mismatched types '(int, Str)' and "
                               "'(int, int)'"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Tuple, OrderingOperatorsRejected) {
  auto r = semaCheck(wrapMain(R"(
    a = (1, 2);
    b = (1, 3);
    lt: bool = a < b;
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("operator '<' is not defined"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Tuple, ToStringMethodAndNoOtherMethods) {
  auto ok = semaCheck(wrapMain(R"(
    t = (1, "a");
    s: Str = t.toString();
  )"));
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;
  auto bad = semaCheck(wrapMain(R"(
    t = (1, "a");
    n: int = t.len();
  )"));
  EXPECT_FALSE(bad.Ok);
  EXPECT_NE(bad.Diagnostics.find("no method 'len' on type 'Tuple<int, Str>'"),
            std::string::npos)
      << bad.Diagnostics;
}

TEST(Tuple, AsFieldParamReturnAndArrayElement) {
  auto r = semaCheck(R"(
    class Holder {
      pair: (int, Str);
      fn __init__(p: (int, Str)) { self.pair = p; }
      fn first() -> int { return self.pair.0; }
      fn get() -> (int, Str) { return self.pair; }
    }
    fn swap(p: (int, Str)) -> (Str, int) { return (p.1, p.0); }
    fn main() -> int {
      h: Holder = Holder((1, "a"));
      q, r = h.get();
      pairs: (int, Str)[] = [(1, "a"), (2, "b")];
      pairs.push(h.pair);
      s, n = swap(pairs[0]);
      inner: Str = pairs[1].1;
      arrs: (int[], Str) = ([1, 2], "x");
      first: int = arrs.0[0];
      return h.first() + q + n;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Tuple, ReturnTypeMismatch) {
  auto r = semaCheck(R"(
    fn f() -> (int, Str) { return (1, 2); }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("return value of type '(int, int)' does not "
                               "match function return type '(int, Str)'"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Tuple, ArgumentTypeMismatch) {
  auto r = semaCheck(R"(
    fn f(p: (int, Str)) -> int { return p.0; }
    fn main() -> int { return f((1, 2, 3)); }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("has type '(int, int, int)', expected "
                               "'(int, Str)'"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Tuple, UnknownElementClassInAnnotation) {
  auto r = semaCheck(wrapMain("t: (int, Nope) = (1, 2);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("element 1 has unknown class type 'Nope'"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Tuple, MatchArmOnTupleRejected) {
  auto r = semaCheck(wrapMain(R"(
    o: Obj = (1, 2);
    match o {
      (int, int) { println("pair"); }
      _ { }
    }
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("match arm type must be a class or array type"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Tuple, TupleIsReservedBuiltinName) {
  auto r = semaCheck("class Tuple {}\nfn main() -> int { return 0; }\n");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'Tuple' is a builtin class"), std::string::npos)
      << r.Diagnostics;
}

// -- Module round-trip of tuple-typed signatures

static std::string writeFile(const std::string &dir, const std::string &relPath,
                             const std::string &content) {
  auto full = std::filesystem::path(dir) / relPath;
  std::filesystem::create_directories(full.parent_path());
  std::ofstream ofs(full);
  ofs << content;
  return full.string();
}

struct SemaFileResult {
  bool Ok;
  std::string Diagnostics;
};

static SemaFileResult semaCheckFile(const std::string &filePath,
                                    const std::string &projectRoot) {
  paykan::parser::ParserDriver drv(paykan::test::testFrontend());
  if (drv.parseFile(filePath) != 0)
    return {false, "parse error"};
  std::ostringstream os;
  paykan::sema::DiagEngine diagEngine(os);
  diagEngine.setSourceInfo(drv.getCurrentFile(), &drv.getSourceLines());
  paykan::sema::Sema sema(drv.getASTContext(), diagEngine, projectRoot,
                          drv.getFrontendName());
  bool ok = sema.run(drv.getRoot()).Ok;
  return {ok, os.str()};
}

TEST(Tuple, ModuleRoundTripOfTupleSignatures) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_tuple").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "geo.pkn", R"(
class Pt { x: int; y: int; fn __init__(x: int, y: int) { self.x = x; self.y = y; } }
class Bag {
  pair: (int, Str);
  fn __init__() { self.pair = (1, "one"); }
  fn get() -> (int, Str) { return self.pair; }
  fn nested() -> (Pt, (int, Str)[]) { return (Pt(1, 2), [self.pair]); }
}
fn divmod(a: int, b: int) -> (int, int) { return (a / b, a % b); }
fn pairs() -> (int, Str)[] { return [(1, "a")]; }
fn take(p: (Pt, (int, Str)[])) -> int { return p.0.x; }
)");
  auto main = writeFile(tmp, "main.pkn", R"(
import geo;
fn main() -> int {
  q, r = geo::divmod(7, 2);
  b: geo::Bag = geo::Bag();
  n, s = b.get();
  ps: (int, Str)[] = geo::pairs();
  t: (geo::Pt, (int, Str)[]) = b.nested();
  x: int = geo::take(t) + geo::take(b.nested());
  return q + r + n + x + ps[0].0;
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Tuple, ModuleRoundTripArityIsChecked) {
  // The imported signature must come back as the precise tuple type, so a
  // wrong-arity destructuring at the import site is still rejected.
  auto tmp = (paykan::test::tempDir() / "pkn_ms_tuple_err").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "geo.pkn",
            "fn divmod(a: int, b: int) -> (int, int) { return (a / b, a % b); "
            "}\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import geo;
fn main() -> int {
  q, r, s = geo::divmod(7, 2);
  return 0;
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("into 3 targets (it has 2 elements)"),
            std::string::npos)
      << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}
