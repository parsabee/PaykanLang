// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: pure virtual methods and abstract classes
// (docs/language/05-classes.md, "Pure virtual methods").  A class body's
// method without a body is pure virtual; a class with one it does not
// implement is abstract, cannot be constructed and cannot be a `match` arm,
// and is otherwise an ordinary type.

#include "TestUtils.h"
#include <gtest/gtest.h>

#include <fstream>

using namespace paykan::test;

namespace {

/// A hierarchy with an abstract root (fields, an `__init__`, an `impl`
/// method calling a pure one), an abstract middle class implementing one of
/// its three pure methods, and a concrete leaf.
const char *kShapes = R"(
import ::io;

class Shape {
  name: Str;
  view fn area() -> float;
  view fn perimeter() -> float;
  view fn kind() -> Str;
  view fn label() -> Str { return self.kind() + " " + self.name; }
}

impl Shape {
  fn __init__(name: Str) { self.name = name; }
  view fn report() -> Str { return self.label() + " " + Str(self.area()); }
  static fn unit() -> Shape { return Square("unit", 1.0); }
}

class Polygon : Shape {
  sides: int;
  side: float;
  view fn perimeter() -> float { return float<int>(self.sides) * self.side; }
}

impl Polygon {
  fn __init__(name: Str, sides: int, side: float) {
    __super__(name);
    self.sides = sides;
    self.side = side;
  }
}

class Square : Polygon {
  view fn area() -> float { return self.side * self.side; }
  view fn kind() -> Str { return "square"; }
}

impl Square {
  fn __init__(name: Str, side: float) { __super__(name, 4, side); }
}
)";

std::string withShapes(const std::string &rest) {
  return std::string(kShapes) + rest;
}

/// How many times @p needle occurs in @p text.
size_t count(const std::string &text, const std::string &needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos;
       at = text.find(needle, at + needle.size()))
    ++n;
  return n;
}

const char *kShapeMissing = "cannot instantiate abstract class 'Shape': "
                            "'Shape.area', 'Shape.perimeter' and 'Shape.kind' "
                            "have no implementation";
const char *kPolygonMissing = "cannot instantiate abstract class 'Polygon': "
                              "'Shape.area' and 'Shape.kind' have no "
                              "implementation";

} // namespace

// -- Declaring and implementing

TEST(PureVirtual, ConcreteLeafOfAnAbstractChain) {
  auto r = semaCheck(withShapes(R"(
fn main() -> int {
  s = Square("a", 2.0);
  io::println(s.report());
  io::println(Shape::unit().label());
  return 0;
}
)"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// A pure virtual method may be declared pure again further down (it stays
// unimplemented), and is implemented below that.
TEST(PureVirtual, RedeclaredPureStaysPureUntilImplemented) {
  auto r = semaCheck(R"(
class A { view fn f() -> int; }
class B : A { view fn f() -> int; }
class C : B { view fn f() -> int { return 1; } }
fn main() -> int {
  b = B();
  c = C();
  return c.f();
}
)");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("cannot instantiate abstract class 'B': 'A.f' "
                               "has no implementation"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
}

// -- Constructing an abstract class

TEST(PureVirtual, EveryMissingImplementationIsListed) {
  auto r = semaCheck(withShapes(R"(
fn main() -> int {
  s = Shape("s");
  p = Polygon("p", 3, 1.0);
  return 0;
}
)"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find(kShapeMissing), std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(kPolygonMissing), std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
}

// Every construction is a call by the class's name, wherever it is written:
// each position is reported once, and nothing else.
TEST(PureVirtual, ConstructionIsRejectedInEveryPosition) {
  auto r = semaCheck(withShapes(R"(
struct Pair { a: Shape; n: int; }
class Holder {
  s: Shape;
  maybe: Shape?;
}
impl Holder {
  fn __init__() {
    self.s = Polygon("field", 3, 1.0);
    self.maybe = None;
  }
}
fn first<T>(x: T) -> T { return x; }
fn take(s: view Shape) -> float { return s.area(); }
fn make() -> Shape { return Polygon("ret", 3, 1.0); }
fn main() -> int {
  a = Polygon("local", 3, 1.0);
  b: Shape = Square("b", 1.0);
  b = Polygon("assign", 3, 1.0);
  xs: Shape[2] = [Square("x", 1.0), Polygon("array", 3, 1.0)];
  f = take(Polygon("arg", 3, 1.0));
  g = fn() -> Shape { return Polygon("lambda", 3, 1.0); };
  h = first(Polygon("generic", 3, 1.0));
  t = (Polygon("tuple", 3, 1.0), 1);
  o: Shape? = Polygon("optional", 3, 1.0);
  p = Pair(Polygon("struct", 3, 1.0), 2);
  c = if f > 1.0 then Polygon("cond", 3, 1.0) else b;
  n = Polygon("receiver", 3, 1.0).perimeter();
  io::println(Polygon("print", 3, 1.0));
  Polygon("statement", 3, 1.0);
  return 0;
}
)"));
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(count(r.Diagnostics, kPolygonMissing), 15u) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 15u) << r.Diagnostics;
}

// The construction's arguments are still checked.
TEST(PureVirtual, ArgumentsOfARejectedConstructionAreChecked) {
  auto r = semaCheck(withShapes(R"(
fn main() -> int {
  s = Shape(1);
  return 0;
}
)"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find(kShapeMissing), std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("argument 1 of 'Shape' has type 'int', "
                               "expected 'Str'"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
}

// A class with no `__init__` of its own gets the default one, and its
// subclasses call it; it is still abstract.
TEST(PureVirtual, AbstractClassWithTheDefaultInit) {
  auto r = semaCheck(R"(
class Named {
  name: Str;
  view fn greet() -> Str;
}
class Hello : Named {
  view fn greet() -> Str { return "hello " + self.name; }
}
fn main() -> int {
  h = Hello("x");
  n = Named("y");
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("cannot instantiate abstract class 'Named': "
                               "'Named.greet' has no implementation"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
}

// -- An abstract class is an ordinary type

TEST(PureVirtual, AbstractTypesEverywhereATypeGoes) {
  auto r = semaCheck(withShapes(R"(
class Box<T> { item: T; }
class Scene {
  main: Shape;
  extra: Shape?;
  all: Shape[2];
}
fn total(shapes: view Shape[]) -> float {
  sum = 0.0;
  i = 0;
  while i < shapes.len() {
    sum += shapes[i].area();
    i += 1;
  }
  return sum;
}
fn pick(a: Shape, b: Shape) -> Shape { return if a.area() > b.area() then a else b; }
fn describe(o: Obj) -> Str {
  match o {
    s: Square { return s.report(); }
    _ { return "other"; }
  }
}
fn main() -> int {
  a: Shape = Square("a", 1.0);
  b: Polygon = Square("b", 2.0);
  xs: Shape[2] = [a, b];
  scene = Scene(a, None, xs);
  box = Box<Shape>(pick(a, b));
  o: Obj = box.item;
  t: (Shape, int) = (b, 1);
  m: Shape? = scene.extra;
  match m {
    s: Shape { io::println(s.report()); }
    None { io::println(describe(o)); }
  }
  match a {
    sq: Square { io::println(sq.kind()); }
    _ { }
  }
  io::println(Str(total(scene.all)) + " " + t.0.label());
  return 0;
}
)"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- `match` arms

// A type arm is an exact-class test: one naming an abstract class can never
// run, with or without a binding, whatever the subject's type.
TEST(PureVirtual, TypeArmNamingAnAbstractClassIsUnreachable) {
  auto r = semaCheck(withShapes(R"(
fn f(o: Obj, s: Shape) -> int {
  match o {
    Shape { return 1; }
    Square { return 2; }
    _ { }
  }
  match s {
    p: Polygon { return p.sides; }
    _ { }
  }
  return 0;
}
fn main() -> int { return 0; }
)"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("unreachable arm: no object has the exact type "
                               "of abstract class 'Shape'"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("unreachable arm: no object has the exact type "
                               "of abstract class 'Polygon'"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
}

// On an optional, the arm naming the wrapped type unwraps any present value,
// so it may be abstract; a strict subclass's arm is an exact test again.
TEST(PureVirtual, OptionalArmsUnwrapAnAbstractTypeButTestNoAbstractSubclass) {
  auto ok = semaCheck(withShapes(R"(
fn f(s: Shape?, p: Polygon?) -> Str {
  match p {
    q: Polygon { return q.label(); }
    None { }
  }
  match s {
    sq: Square { return sq.kind(); }
    x: Shape { return x.label(); }
    None { return "none"; }
  }
}
fn main() -> int { return 0; }
)"));
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;

  auto bad = semaCheck(withShapes(R"(
fn f(s: Shape?) -> Str {
  match s {
    p: Polygon { return p.label(); }
    x: Shape { return x.label(); }
    None { return "none"; }
  }
}
fn main() -> int { return 0; }
)"));
  EXPECT_FALSE(bad.Ok);
  EXPECT_NE(bad.Diagnostics.find("unreachable arm: no object has the exact "
                                 "type of abstract class 'Polygon'"),
            std::string::npos)
      << bad.Diagnostics;
  EXPECT_EQ(bad.ErrorCount, 1u) << bad.Diagnostics;
}

// -- Overrides

// An implemented method (the superclass's own, inherited, or the runtime's
// for `Obj`'s) cannot become pure virtual again.
TEST(PureVirtual, ImplementedMethodCannotBecomePureAgain) {
  auto r = semaCheck(withShapes(R"(
class Tile : Square {
  view fn area() -> float;
}
class Thing {
  view fn toString() -> Str;
}
fn main() -> int { return 0; }
)"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("method 'area' of class 'Tile' has no body, but "
                               "its superclass 'Square' implements it: an "
                               "implemented method cannot become pure virtual "
                               "again"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("method 'toString' of class 'Thing' has no "
                               "body, but its superclass 'Obj' implements it"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
}

// An implementation is an override: the same signature and markers.
TEST(PureVirtual, ImplementationKeepsTheSignature) {
  auto r = semaCheck(R"(
class A { view fn f(x: int) -> int; }
class B : A { view fn f(x: float) -> int { return 1; } }
class C : A { fn f(x: int) -> int { return x; } }
class D : A { view fn f(x: inout int) -> int { return x; } }
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("override of 'A.f(int) -> int' has "
                               "incompatible signature 'B.f(float) -> int'"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("override of 'f' must be a 'view fn', like the "
                               "method it overrides"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
}

// -- What cannot be pure virtual

// `__init__` is the constructor, not a virtual method, and `destroy` is the
// compiler's: written without a body in the class body, each gets its usual
// error, and nothing more.
TEST(PureVirtual, InitAndDestroyCannotBePure) {
  auto r = semaCheck(R"(
class A { fn __init__(); }
class B { fn destroy(); }
fn main() -> int {
  a = A();
  b = B();
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'__init__' goes in an 'impl' block, not the "
                               "class body"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("'destroy' is the compiler-generated destructor "
                               "and cannot be overridden"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("abstract"), std::string::npos) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
}

// -- Calls

// A pure virtual method is called like any method: its signature types the
// call.
TEST(PureVirtual, CallsOfPureMethodsAreChecked) {
  auto r = semaCheck(withShapes(R"(
fn main() -> int {
  s: Shape = Square("a", 1.0);
  x: int = s.area();
  y = s.volume();
  return 0;
}
)"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("initializer of type 'float' does not match "
                               "declared type 'int' for variable 'x'"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("no method 'volume' on type 'Shape'"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
}

// A pure virtual method has no body to judge: its family gets no "never
// changes 'self'" warning, and neither do its implementations.
TEST(PureVirtual, NoViewFnWarningForAPureMethod) {
  auto r = semaCheck(R"(
class A { fn f() -> int; }
class B : A { fn f() -> int { return 1; } }
fn main() -> int { return B().f(); }
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("never changes"), std::string::npos)
      << r.Diagnostics;
}

// -- Generic classes

// A generic class's pure virtual methods make each instantiation abstract; a
// generic subclass of an abstract class is concrete once it implements them.
TEST(PureVirtual, GenericClassesFollowTheSameRules) {
  auto r = semaCheck(R"(
class Shape { view fn area() -> float; }
class Source<T> {
  view fn next() -> T;
  view fn peek() -> T;
}
class Tagged<T> : Shape {
  tag: T;
  view fn area() -> float { return 1.0; }
}
class Labelled<T> : Shape { label: T; }
fn wrap<T>(x: T) -> Shape { return Labelled<T>(x); }
fn main() -> int {
  t: Shape = Tagged<int>(1);
  s = Source<int>();
  l = Labelled("x");
  w = wrap(1.5);
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("cannot instantiate abstract class "
                               "'Source<int>': 'Source<int>.next' and "
                               "'Source<int>.peek' have no implementation"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("cannot instantiate abstract class "
                               "'Labelled<Str>': 'Shape.area' has no "
                               "implementation"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("cannot instantiate abstract class "
                               "'Labelled<float>': 'Shape.area' has no "
                               "implementation"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
}

// -- Across modules

namespace {

std::string writeModuleFile(const std::filesystem::path &dir,
                            const std::string &rel,
                            const std::string &content) {
  auto full = dir / rel;
  std::filesystem::create_directories(full.parent_path());
  std::ofstream(full) << content;
  return full.string();
}

SemaResult semaCheckMain(const std::string &mainPath, const std::string &root) {
  paykan::sema::Sema::ModuleCache.clear();
  paykan::parser::ParserDriver drv(testFrontend());
  if (drv.parseFile(mainPath) != 0)
    return {false, "parse error", 1};
  std::ostringstream os;
  paykan::sema::DiagEngine diag(os);
  diag.setSourceInfo(drv.getCurrentFile(), &drv.getSourceLines());
  paykan::sema::Sema sema(drv.getASTContext(), diag, root,
                          drv.getFrontendName());
  auto ctx = sema.run(drv.getRoot());
  return {ctx.Ok, os.str(), ctx.ErrorCount};
}

} // namespace

// An imported class's pure virtual methods come with it: an importer's
// subclass implements them, and the importer works out abstractness itself.
TEST(PureVirtual, AcrossModules) {
  auto dir = tempDir() / "pv_modules";
  std::filesystem::remove_all(dir);
  writeModuleFile(dir, "geo/shapes.pkn", R"(
class Shape {
  name: Str;
  view fn area() -> float;
  view fn kind() -> Str;
}
impl Shape {
  fn __init__(name: Str) { self.name = name; }
  view fn report() -> Str { return self.kind() + " " + Str(self.area()); }
}
class Polygon : Shape {
  view fn kind() -> Str { return "polygon"; }
}
)");
  auto good = writeModuleFile(dir, "good.pkn", R"(
import ::io;
import geo::shapes;
class Square : shapes::Polygon {
  side: float;
  view fn area() -> float { return self.side * self.side; }
}
fn main() -> int {
  s: shapes::Shape = Square("s", 2.0);
  io::println(s.report());
  return 0;
}
)");
  auto r = semaCheckMain(good, dir.string());
  EXPECT_TRUE(r.Ok) << r.Diagnostics;

  auto bad = writeModuleFile(dir, "bad.pkn", R"(
import geo::shapes;
class Half : shapes::Polygon { }
class Wrong : shapes::Polygon { view fn area() -> int { return 1; } }
fn f(o: Obj) -> int {
  match o {
    shapes::Polygon { return 1; }
    _ { return 0; }
  }
}
fn main() -> int {
  a = shapes::Shape("a");
  b = shapes::Polygon("b");
  c = Half("c");
  d = geo::shapes::Shape("d");
  return 0;
}
)");
  r = semaCheckMain(bad, dir.string());
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("override of 'Polygon.area() -> float' has "
                               "incompatible signature 'Wrong.area() -> int'"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("unreachable arm: no object has the exact type "
                               "of abstract class 'Polygon'"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(count(r.Diagnostics, "cannot instantiate abstract class 'Shape': "
                                 "'Shape.area' and 'Shape.kind' have no "
                                 "implementation"),
            2u)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("cannot instantiate abstract class 'Polygon': "
                               "'Shape.area' has no implementation"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("cannot instantiate abstract class 'Half': "
                               "'Shape.area' has no implementation"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 6u) << r.Diagnostics;
  std::filesystem::remove_all(dir);
}
