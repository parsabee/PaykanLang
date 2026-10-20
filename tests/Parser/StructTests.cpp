// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: struct declarations (docs/language/11-structs.md): fields,
// generic structs, the `impl` blocks that give them methods, and the forms
// the grammar rejects (methods in the body, a superclass, no fields).

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

TEST(Struct, DeclarationWithFields) {
  auto dump = dumpAST(R"(
struct Point {
  x: int;
  y: int;
}
fn main() -> int { return 0; }
)");
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("StructDecl <2:1-5:2> 'Point'"), std::string::npos)
      << dump;
  EXPECT_NE(dump.find("VarDecl <3:3-3:9> 'x' type"), std::string::npos) << dump;
  EXPECT_NE(dump.find("VarDecl <4:3-4:9> 'y' type"), std::string::npos) << dump;
  // A struct body holds no methods.
  EXPECT_EQ(dump.find("FuncDecl 'x'"), std::string::npos);
}

TEST(Struct, FieldTypes) {
  // Fields of every kind of type a tuple element may have, and of another
  // struct, a class and an enum (names Sema resolves).
  auto [ok, _] = parse(R"(
enum Color { Red, Green }
class Node { n: int; }
struct Inner { a: int; }
struct Mixed {
  i: int;
  f: float;
  b: bool;
  c: char;
  s: Str;
  o: Obj;
  maybe: Str?;
  oi: int?;
  t: (int, Str);
  in: Inner;
  node: Node;
  color: Color;
  box: Box<int>;
}
class Box<T> { v: T; }
fn main() -> int { return 0; }
)");
  EXPECT_TRUE(ok);
}

TEST(Struct, OneFieldOnOneLine) {
  auto dump =
      dumpAST("struct One { only: Str; }\nfn main() -> int { return 0; }\n");
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("StructDecl <1:1-1:26> 'One'"), std::string::npos)
      << dump;
}

TEST(Struct, GenericStruct) {
  auto dump = dumpAST(R"(
struct Pair<A, B: Show> {
  first: A;
  second: B;
}
fn main() -> int { return 0; }
)");
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("StructDecl <2:1-5:2> 'Pair' <A, B: Show>"),
            std::string::npos)
      << dump;
}

TEST(Struct, UsedAsATypeAndConstructed) {
  // `Point` in a type position is a ClassType stub, as any unknown name is;
  // `Point(1, 2)` a call; `p.x` a member access; `p.x = 3` a member
  // assignment.  Sema tells a struct apart from a class.
  auto dump = dumpAST(R"(
struct Point { x: int; y: int; }
fn main() -> int {
  p: Point = Point(1, 2);
  p.x = 3;
  return p.x + p.y;
}
)");
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("ClassType <4:6-4:11> 'Point'"), std::string::npos)
      << dump;
  EXPECT_NE(dump.find("CallExpr <4:14-4:25> 'Point'"), std::string::npos)
      << dump;
  EXPECT_NE(dump.find("MemberAssignStmt 'x'"), std::string::npos) << dump;
  EXPECT_NE(dump.find("MemberAccessExpr 'x'"), std::string::npos) << dump;
}

TEST(Struct, ImplBlockForAStruct) {
  // An `impl` block is parsed the same for a struct and a class.
  auto dump = dumpAST(R"(
struct Point { x: int; y: int; }
impl Point {
  fn __init__(x: int, y: int) { self.x = x; self.y = y; }
  static fn origin() -> Point { return Point(0, 0); }
  view fn sum() -> int { return self.x + self.y; }
  fn shift(d: int) { self.x = self.x + d; }
}
impl Hashable for Point {
  view fn hash() -> int { return self.x; }
}
fn main() -> int { return 0; }
)");
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("ImplDecl <3:1-8:2> 'Point'"), std::string::npos) << dump;
  EXPECT_NE(dump.find("FuncDecl <5:10-5:54> static 'origin'"),
            std::string::npos)
      << dump;
  EXPECT_NE(dump.find("ImplDecl <9:1-11:2> 'Hashable' for 'Point'"),
            std::string::npos)
      << dump;
}

TEST(Struct, StructInEveryTypePosition) {
  auto [ok, _] = parse(R"(
struct P { x: int; }
class Holder { p: P; ps: P[2]; }
fn f(a: P, b: view P, c: inout P) -> P { return a; }
fn g() -> (P, int) { return (P(1), 2); }
fn main() -> int {
  xs: P[3] = [P(1), P(2), P(3)];
  t: (P, P) = (P(1), P(2));
  b = Box<P>(P(1));
  return 0;
}
class Box<T> { v: T; }
)");
  EXPECT_TRUE(ok);
}

// -- Rejected forms

TEST(Struct, MethodInBodyRejected) {
  auto r = parseWithDiags(R"(
struct Point {
  x: int;
  fn sum() -> int { return self.x; }
}
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Text.find("a struct body holds fields only ('name: Type;'); a "
                        "method goes in an 'impl' block: 'impl Point { fn "
                        "... }'"),
            std::string::npos)
      << r.Text;
}

TEST(Struct, ViewFnAndStaticFnInBodyRejected) {
  auto r = parseWithDiags(R"(
struct Point {
  x: int;
  view fn sum() -> int { return self.x; }
}
)");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Text.find("a struct body holds fields only"), std::string::npos)
      << r.Text;
  auto s = parseWithDiags(R"(
struct Point {
  x: int;
  static fn origin() -> Point { return Point(0); }
}
)");
  EXPECT_FALSE(s.Ok);
  EXPECT_NE(s.Text.find("a struct body holds fields only"), std::string::npos)
      << s.Text;
}

TEST(Struct, SuperclassRejected) {
  auto r = parseWithDiags(R"(
class Base { n: int; }
struct Point : Base {
  x: int;
}
)");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Text.find("a struct has no superclass: 'struct Point { ... }' "
                        "(a struct is a value, not an object; use a class to "
                        "inherit)"),
            std::string::npos)
      << r.Text;
}

TEST(Struct, EmptyBodyRejected) {
  auto r = parseWithDiags(R"(
struct Empty {}
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Text.find("struct 'Empty' has no fields: a struct has at least "
                        "one"),
            std::string::npos)
      << r.Text;
}

TEST(Struct, MissingNameAndBrace) {
  auto r = parseWithDiags("struct { x: int; }\n");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Text.find("expected a struct name after 'struct'"),
            std::string::npos)
      << r.Text;
  auto s = parseWithDiags("struct P\n  x: int;\n}\n");
  EXPECT_FALSE(s.Ok);
  EXPECT_NE(s.Text.find("to open the struct body"), std::string::npos)
      << s.Text;
}

TEST(Struct, FieldWithoutTypeRejected) {
  auto r = parseWithDiags(R"(
struct Point {
  x;
}
)");
  EXPECT_FALSE(r.Ok);
}

TEST(Struct, KeywordIsReserved) {
  // `struct` can no longer name a variable, a function or a type.
  auto r = parseWithDiags("fn main() -> int { struct = 1; return 0; }\n");
  EXPECT_FALSE(r.Ok);
  auto s = parseWithDiags("fn struct() -> int { return 0; }\n");
  EXPECT_FALSE(s.Ok);
}

TEST(Struct, RecoversAtTheNextDeclaration) {
  // One error inside a struct does not hide an error in the next
  // declaration, and a later well-formed one still parses.
  auto r = parseWithDiags(R"(
struct A {
  fn bad() {}
}
struct B { x: int; }
fn main() -> int { return 1 +; }
)");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Text.find("a struct body holds fields only"), std::string::npos)
      << r.Text;
  EXPECT_EQ(r.Diags.size(), 2u) << r.Text;
}

TEST(Struct, TopLevelErrorMentionsStruct) {
  auto r = parseWithDiags("point { x: int; }\n");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Text.find("'struct'"), std::string::npos) << r.Text;
}
