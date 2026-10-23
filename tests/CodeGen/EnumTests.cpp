// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// CodeGen / E2E tests for the `enum` type.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "import ::io; fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}
static std::string withEnums(const std::string &enums,
                             const std::string &body) {
  return enums + "\n" + wrapMain(body);
}

TEST(EnumCodeGen, EqualityTrueBranch) {
  auto r = compileAndRun(withEnums("enum Color { Red, Green, Blue }", R"(
    c: Color = Color::Green;
    if c == Color::Green { io::println("yes"); }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "yes\n");
}

TEST(EnumCodeGen, EqualityFalseBranch) {
  auto r = compileAndRun(withEnums("enum Color { Red, Green, Blue }", R"(
    c: Color = Color::Green;
    if c == Color::Red { io::println("a"); } else { io::println("b"); }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "b\n");
}

TEST(EnumCodeGen, Inequality) {
  auto r = compileAndRun(withEnums("enum Color { Red, Green, Blue }", R"(
    c: Color = Color::Blue;
    if c != Color::Red { io::println("ne"); }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "ne\n");
}

TEST(EnumCodeGen, MatchVariant) {
  auto r = compileAndRun(withEnums("enum Color { Red, Green, Blue }", R"(
    c: Color = Color::Green;
    match c {
      Red   { io::println("r"); }
      Green { io::println("g"); }
      Blue  { io::println("b"); }
      _     { io::println("?"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "g\n");
}

TEST(EnumCodeGen, MatchWildcardFallthrough) {
  auto r = compileAndRun(withEnums("enum Color { Red, Green, Blue }", R"(
    c: Color = Color::Blue;
    match c {
      Red   { io::println("r"); }
      Green { io::println("g"); }
      _     { io::println("other"); }
    }
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "other\n");
}

TEST(EnumCodeGen, EnumThroughFunctionParam) {
  auto src =
      "import ::io; enum Dir { North, East, South, West }\n"
      "fn step(d: Dir) -> int {\n"
      "  match d {\n"
      "    North { io::println(\"N\"); }\n"
      "    East  { io::println(\"E\"); }\n"
      "    _     { io::println(\"?\"); }\n"
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
    if e == E::A { io::println("0"); }
    if e == E::B { io::println("1"); }
    if e == E::C { io::println("2"); }
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
             wrapMain("io::println(name(Dir::South));");
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
                      "  if x == Dir::South { io::println(\"south\"); }");
  auto r = compileAndRun(src);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "south\n");
}

// An array of enums holds raw (i64) values, not references: literal
// construction, subscript get/set, and len must all treat the element as a raw
// value rather than a boxed PaykanShared*.
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
      wrapMain("xs: Dir[3] = [Dir::North, Dir::East, Dir::North];\n"
               "  xs[2] = Dir::South;\n" // subscript set
               "  xs[0] = Dir::West;\n"
               "  i: int = 0;\n"
               "  while i < xs.len() { io::println(nm(xs[i])); i = i + 1; }");
  auto r = compileAndRun(src);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "W\nE\nS\n");
}

// -- Enums shown by name

TEST(EnumCodeGen, ShownByName) {
  LeakGuard g;
  auto r = compileAndRun(withEnums("enum Dir { North, East }", R"(
    d = Dir::East;
    o: Dir? = Dir::North;
    io::println(d.toString() + " " + o.toString());
    io::println(d);
    io::println((d, 1));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "East North\nEast\n(East, 1)\n");
  g.expectNoLeaks("ShownByName");
}

// -- Variants with fields (a tagged union)

static const char *kShape = "import ::io;\n"
                            "class Tag {\n"
                            "  n: int;\n"
                            "  view fn toString() -> Str {\n"
                            "    return \"Tag\" + Str(self.n);\n"
                            "  }\n"
                            "}\n"
                            "enum Shape {\n"
                            "  Empty,\n"
                            "  Circle(r: float),\n"
                            "  Rect(w: int, h: int),\n"
                            "  Named(name: Str, tag: Tag?)\n"
                            "}\n";

TEST(EnumCodeGen, VariantsMadeMatchedAndShown) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kShape) + R"(
fn area(s: Shape) -> int {
  match s {
    Empty { return 0; }
    c: Circle { return int(c.r * c.r); }
    r: Rect { return r.w * r.h; }
    n: Named { return n.name.len(); }
  }
}
fn main() -> int {
  xs = [Shape::Empty, Shape::Circle(2.0), Shape::Rect(2, 3),
        Shape::Named("four", Tag(1))];
  total = 0;
  for i in 0..4 {
    io::println(xs[i]);
    total += area(xs[i]);
  }
  return total;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Empty\nCircle(r: 2)\nRect(w: 2, h: 3)\n"
                      "Named(name: four, tag: Tag1)\n");
  EXPECT_EQ(r.ExitCode, 14);
  g.expectNoLeaks("VariantsMadeMatchedAndShown");
}

// Assigning a variant releases the references of the one it replaces;
// copies share them; an `inout` binding changes the subject's fields.
TEST(EnumCodeGen, VariantOwnership) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kShape) + R"(
fn grow(s: inout Shape) {
  match s {
    r: inout Rect { r.w *= 10; }
    n: inout Named { n.name = n.name + "!"; }
    _ { }
  }
}
fn main() -> int {
  s = Shape::Named("a", Tag(1));
  copy = s;
  s = Shape::Rect(1, 2);
  grow(s);
  grow(copy);
  io::println(s.toString() + " " + copy.toString());
  s = copy;
  s = Shape::Empty;
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Rect(w: 10, h: 2) Named(name: a!, tag: Tag1)\n");
  g.expectNoLeaks("VariantOwnership");
}

TEST(EnumCodeGen, VariantEquality) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kShape) + R"(
fn main() -> int {
  a = Shape::Rect(1, 2);
  io::println(Str(a == Shape::Rect(1, 2)) + " " + Str(a == Shape::Rect(2, 1))
      + " " + Str(a != Shape::Empty) + " " + Str(Shape::Empty == Shape::Empty)
      + " " + Str(Shape::Named("x", None) == Shape::Named("x", None)));
  o: Shape? = None;
  p: Shape? = a;
  io::println(Str(o == None) + " " + Str(o == p));
  o = Shape::Rect(1, 2);
  io::println(Str(o == p));
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True False True True True\nTrue False\nTrue\n");
  g.expectNoLeaks("VariantEquality");
}

// A tagged union in a struct, a tuple, an optional, a List and an object's
// field; passed, returned, captured; one variant's fields as a parameter.
TEST(EnumCodeGen, TaggedUnionHeldEverywhere) {
  LeakGuard g;
  auto r =
      compileAndRun(std::string(kShape) + "import ::collections as col;\n" + R"(
struct Slot { s: Shape; n: int; }
class Holder { s: Shape; }
fn rectArea(r: Shape::Rect) -> int { return r.w * r.h; }
fn make(k: int) -> Shape {
  return if k > 0 then Shape::Named(Str(k), None) else Shape::Empty;
}
fn main() -> int {
  slot = Slot(Shape::Circle(1.5), 1);
  t = (make(2), 3);
  o: Shape? = make(0);
  l = col::List<Shape>();
  l.push(make(5));
  l.push(Shape::Rect(3, 4));
  h = Holder(Shape::Empty);
  h.s = l.get(0);
  f = fn() => slot.s.toString();
  io::println(slot.toString() + " " + t.toString() + " " + o.toString());
  io::println(h.s.toString() + " " + f() + " " + Str(l.len()));
  match l.get(1) {
    r: Rect { io::println(rectArea(r)); }
    _ { }
  }
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "Slot(s: Circle(r: 1.5), n: 1) (Named(name: 2, tag: None), 3) "
            "Empty\nNamed(name: 5, tag: None) Circle(r: 1.5) 2\n12\n");
  g.expectNoLeaks("TaggedUnionHeldEverywhere");
}

// A generic enum: each instantiation is an enum of its own, made, matched,
// shown, compared and passed through generic code.
TEST(EnumCodeGen, GenericEnums) {
  LeakGuard g;
  auto r = compileAndRun(R"(
import ::io;
enum Maybe<T> { Just(value: T), Nothing }
enum Result<T, E> { Ok(value: T), Err(error: E) }
struct Cell<T> { m: Maybe<T>; n: int; }
class Holder { m: Maybe<Str>; }
fn unwrapOr<T>(m: Maybe<T>, d: T) -> T {
  match m {
    j: Just { return j.value; }
    Nothing { return d; }
  }
}
fn parse(s: view Str) -> Result<int, Str> {
  if (s == "42") {
    return Result::Ok(42);
  }
  return Result::Err("not 42: " + s);
}
fn main() -> int {
  a = Maybe::Just("a");
  b: Maybe<Str> = Maybe::Nothing;
  io::println(a.toString() + " " + b.toString());
  io::println(unwrapOr(a, "x") + unwrapOr(b, "y"));
  io::println(Str(a == Maybe::Just("a")) + " " + Str(b == Maybe::Nothing));
  c = Cell(Maybe::Just(2), 3);
  h = Holder(b);
  h.m = a;
  io::println(c.toString() + " " + h.m.toString());
  xs: Maybe<int>[3] = [Maybe::Just(1), Maybe::Nothing, Maybe::Just(3)];
  total = 0;
  for i in 0..3 {
    total += unwrapOr(xs[i], 10);
  }
  match parse("42") {
    o: Ok { total += o.value; }
    e: Err { io::println(e.error); }
  }
  match parse("7") {
    o: Ok { total += o.value; }
    e: Err { io::println(e.error); }
  }
  return total;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Just(value: a) Nothing\nay\nTrue True\n"
                      "Cell<int>(m: Just(value: 2), n: 3) Just(value: a)\n"
                      "not 42: 7\n");
  EXPECT_EQ(r.ExitCode, 56);
  g.expectNoLeaks("GenericEnums");
}
