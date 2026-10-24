// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// CodeGen tests: pure virtual methods and abstract classes
// (docs/language/05-classes.md, "Pure virtual methods").  A pure virtual
// method is a vtable slot that the object's class fills: every call goes
// through the vtable, from a variable of the abstract type, an array of
// mixed subclasses, an `impl` method of the abstract class and its
// `__init__` (which sees the object's own class).  Each program runs with
// every frontend, prints what it should and leaves zero live heap blocks.
// The slot's own target, the trap, is reached only by a broken program: a
// hand-written PIR one here.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

#include <csignal>
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>

using namespace paykan;
using namespace paykan::test;

namespace {

void expectRunsOnEveryFrontend(const char *label, const std::string &source,
                               const std::string &expected) {
  for (const std::string &fe : frontend::Registry::get().names()) {
    LeakGuard g;
    auto r = compileAndRunWithFrontend(source, fe);
    ASSERT_TRUE(r.CompileOk) << label << " [" << fe << "]: " << r.StdErr;
    EXPECT_EQ(r.ExitCode, 0) << label << " [" << fe << "]: " << r.StdErr;
    EXPECT_EQ(r.StdOut, expected) << label << " [" << fe << "]";
    g.expectNoLeaks((std::string(label) + " [" + fe + "]").c_str());
  }
}

/// An abstract root with a field, an `__init__` and three pure methods; an
/// abstract middle class implementing one; two concrete leaves.
const char *kShapes = R"(import ::io;

class Shape {
  name: Str;
  view fn area() -> float;
  view fn perimeter() -> float;
  view fn kind() -> Str;
  view fn label() -> Str { return self.kind() + " " + self.name; }
}

impl Shape {
  fn __init__(name: Str) { self.name = name; }
  view fn report() -> Str {
    return self.label() + ": " + Str(self.area()) + " / " +
           Str(self.perimeter());
  }
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

class Rect : Shape {
  w: float;
  h: float;
  view fn area() -> float { return self.w * self.h; }
  view fn perimeter() -> float { return 2.0 * (self.w + self.h); }
  view fn kind() -> Str { return "rect"; }
}

impl Rect {
  fn __init__(name: Str, w: float, h: float) {
    __super__(name);
    self.w = w;
    self.h = h;
  }
}
)";

std::string withShapes(const std::string &rest) {
  return std::string(kShapes) + rest;
}

} // namespace

TEST(PureVirtual, CallsThroughAnAbstractTypedVariable) {
  expectRunsOnEveryFrontend("Variable", withShapes(R"(
fn main() -> int {
  s: Shape = Square("a", 2.0);
  io::println(s.report());
  s = Rect("b", 1.5, 2.0);
  io::println(s.report());
  p: Polygon = Square("c", 0.5);
  io::println(Str(p.area()) + " " + p.kind());
  return 0;
}
)"),
                            "square a: 4 / 8\n"
                            "rect b: 3 / 7\n"
                            "0.25 square\n");
}

TEST(PureVirtual, ArrayOfDifferentSubclasses) {
  expectRunsOnEveryFrontend("Array", withShapes(R"(
fn total(shapes: view Shape[]) -> float {
  sum = 0.0;
  i = 0;
  while i < shapes.len() {
    sum += shapes[i].area();
    i += 1;
  }
  return sum;
}
fn main() -> int {
  shapes: Shape[3] = [Square("a", 1.0), Rect("b", 2.0, 3.0), Square("c", 2.0)];
  i = 0;
  while i < shapes.len() {
    io::println(shapes[i].label());
    i += 1;
  }
  io::println("total " + Str(total(shapes)));
  return 0;
}
)"),
                            "square a\nrect b\nsquare c\ntotal 11\n");
}

// `report`, an `impl` method of the abstract class, and `label`, one of its
// own virtual methods, call the pure ones through the vtable; so does a
// static function of the abstract class.
TEST(PureVirtual, ImplMethodOfTheAbstractClassCallsPureOnes) {
  expectRunsOnEveryFrontend("Impl", R"(import ::io;
class Counter {
  n: int;
  fn step() -> int;
}
impl Counter {
  fn __init__() { self.n = 0; }
  fn run(times: int) -> int {
    i = 0;
    while i < times {
      self.n += self.step();
      i += 1;
    }
    return self.n;
  }
  static fn twice(c: Counter) -> int { return c.run(2); }
}
class ByTwo : Counter {
  fn step() -> int { return 2; }
}
class ByTen : Counter {
  fn step() -> int { return 10; }
}
fn main() -> int {
  a: Counter = ByTwo();
  b: Counter = ByTen();
  io::println(Str(a.run(3)) + " " + Str(Counter::twice(b)));
  return 0;
}
)",
                            "6 20\n");
}

// The constructor installs the object's own vtable before any `__init__`
// runs: the abstract base's `__init__` calling a pure virtual method reaches
// the subclass's implementation, on every level of the chain.
TEST(PureVirtual, BaseInitCallsThePureVirtualImplementation) {
  expectRunsOnEveryFrontend("Init", R"(import ::io;
class Shape {
  tag: Str;
  view fn kind() -> Str;
}
impl Shape {
  fn __init__() { self.tag = "made a " + self.kind(); }
}
class Square : Shape {
  side: float;
  view fn kind() -> Str { return "square"; }
}
impl Square {
  fn __init__(side: float) {
    __super__();
    self.side = side;
  }
}
class Tile : Square {
  view fn kind() -> Str { return "tile"; }
}
fn main() -> int {
  s: Shape = Square(2.0);
  t: Shape = Tile(1.0);
  io::println(s.tag);
  io::println(t.tag);
  return 0;
}
)",
                            "made a square\nmade a tile\n");
}

// Abstract types as fields, optionals, tuple elements and type arguments,
// matched and unwrapped: every reference is released.
TEST(PureVirtual, AbstractTypesHoldAndReleaseTheirObjects) {
  expectRunsOnEveryFrontend("Types", withShapes(R"(
class Box<T> { item: T; }
class Scene {
  main: Shape;
  extra: Shape?;
}
fn describe(o: Obj) -> Str {
  match o {
    s: Square { return "square " + s.name; }
    r: Rect { return "rect " + r.name; }
    _ { return "other"; }
  }
}
fn main() -> int {
  scene = Scene(Rect("r", 1.0, 2.0), None);
  scene.extra = Square("s", 3.0);
  match scene.extra {
    e: Shape { io::println(e.report()); }
    None { io::println("none"); }
  }
  box = Box<Shape>(scene.main);
  pair: (Shape, int) = (box.item, 2);
  o: Obj = Square("t", 1.0);
  io::println(describe(pair.0) + ", " + describe(o) + ", " + describe(box));
  return 0;
}
)"),
                            "square s: 9 / 12\nrect r, square t, other\n");
}

// A generic class implementing an abstract class's methods is concrete.
TEST(PureVirtual, GenericSubclassImplementsThem) {
  expectRunsOnEveryFrontend("Generic", R"(import ::io;
class Named { view fn name() -> Str; }
class Tagged<T> : Named {
  tag: T;
  view fn name() -> Str { return "tag " + self.tag.toString(); }
}
fn main() -> int {
  xs: Named[2] = [Tagged<int>(1), Tagged<Str>("two")];
  io::println(xs[0].name() + ", " + xs[1].name());
  return 0;
}
)",
                            "tag 1, tag two\n");
}

// The trap: a pure virtual slot's target calls Paykan_panic_pure_virtual.
// No compiled program has an object of an abstract class; a hand-written
// PIR one that allocates one and calls the slot aborts (in a child: the
// abort ends the process; tests/Runtime/ObjectTests.cpp checks the
// message).
TEST(PureVirtual, PureSlotTargetAborts) {
  const char *pir = R"(module "t"
cstr @.str0 = "Shape" len 5
cstr @.str1 = "area" len 4

class Shape {
  vtable {
    destroy = @Shape.destroy : (obj) -> void
    toString = @$rt.PaykanObject_toString : (obj) -> box
    equals = @$rt.PaykanObject_equals : (obj, box) -> i64
    area = @Shape.area : (obj) -> f64
  }
}

fn @Shape.area(%self.1: obj) -> f64 {
  call @$rt.Paykan_panic_pure_virtual(@.str0, 5, @.str1, 4)
  unreachable
}

fn @Shape.destroy(%self.1: obj) -> void {
  ret
}

fn @main() -> i64 {
  %o = new Shape
  %b = box %o
  %a = vcall %o : Shape [3] ()
  release %b
  ret 0
}
extern fn @$rt.Paykan_panic_pure_virtual(ptr, i64, ptr, i64) -> void
extern fn @$rt.PaykanObject_toString(obj) -> box
extern fn @$rt.PaykanObject_equals(obj, box) -> i64
)";
  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid == 0) {
    auto r = compileAndRunPIR(pir);
    if (!r.CompileOk)
      fprintf(stderr, "child compile failed: %s\n", r.StdErr.c_str());
    _exit(r.CompileOk ? 0 : 1); // only when the slot did not abort
  }
  // The JIT turns the runtime's abort into the exit status a built program
  // would have, 128 + SIGABRT (src/Backends/LLVM/JIT/JIT.cpp).
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 128 + SIGABRT);
}
