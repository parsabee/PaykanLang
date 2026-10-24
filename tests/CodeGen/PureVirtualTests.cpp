// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// CodeGen tests: pure virtual methods and abstract classes
// (docs/language/05-classes.md, "Pure virtual methods").  A pure virtual
// method is a vtable slot that the object's class fills: every call goes
// through the vtable, from a variable of the abstract type, an array of
// mixed subclasses and an `impl` method of the abstract class.  While a
// class's `__init__` runs the object is one of that class, so the abstract
// class's `__init__` reaches the slot's own target, the trap.  Each program
// runs with every frontend, prints what it should and leaves zero live heap
// blocks.

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

// While a class's `__init__` runs, the object is one of that class, as in
// C++: each level of the chain, its fields assigned, calls its own class's
// implementation, never a subclass's that would read a field the subclass
// has not assigned yet; the constructed object is one of its own class.
TEST(PureVirtual, EachInitSeesItsOwnClass) {
  expectRunsOnEveryFrontend("Init", R"(import ::io;
class Shape {
  tag: Str;
  view fn kind() -> Str { return "shape"; }
  view fn area() -> float;
}
impl Shape {
  fn __init__() {
    self.tag = "";
    self.tag = "made as a " + self.kind();
  }
}
class Square : Shape {
  side: float;
  name: Str;
  view fn kind() -> Str { return self.name + ""; }
  view fn area() -> float { return self.side * self.side; }
}
impl Square {
  fn __init__(side: float) {
    __super__();
    self.side = side;
    self.name = "square";
    self.tag = self.tag + ", then a " + self.kind();
  }
}
class Tile : Square {
  view fn kind() -> Str { return "tile of " + Str(self.area()); }
}
fn main() -> int {
  s: Shape = Square(2.0);
  t: Shape = Tile(1.0);
  io::println(s.tag + "; " + s.kind());
  io::println(t.tag + "; " + t.kind());
  return 0;
}
)",
                            "made as a shape, then a square; square\n"
                            "made as a shape, then a square; tile of 1\n");
}

// A superclass's `__init__` may give `self` away once its own fields are
// assigned.  Until the subclass's `__init__` has assigned every field of its
// own (on every path: in both branches here), the object stays one of the
// superclass, so a virtual call or a type test made through that reference
// meanwhile sees the superclass, never a subclass's field without a value.
TEST(PureVirtual, AGivenAwaySelfIsTheSuperclassUntilTheSubclassFieldsAreSet) {
  expectRunsOnEveryFrontend("GivenAway", R"(import ::io;
class Panel { kid: Widget?; }
impl Panel {
  fn add(w: Widget) { self.kid = w; }
  view fn show() {
    match self.kid {
      l: Label { io::println("label " + l.text + " " + Str(l.width())); }
      w: Widget { io::println("widget " + Str(w.width())); }
      None { }
    }
  }
}
class Widget {
  id: int;
  view fn width() -> int { return 0; }
}
impl Widget {
  fn __init__(p: Panel) { self.id = 1; p.add(self); p.show(); }
}
class Label : Widget {
  text: Str;
  view fn width() -> int { return self.text.len(); }
}
impl Label {
  fn __init__(p: Panel, t: Str) {
    __super__(p);
    p.show();
    if t.len() > 3 { self.text = t; } else { self.text = t + "!"; }
    p.show();
  }
}
fn main() -> int {
  p = Panel(None);
  a = Label(p, "hi");
  b = Label(p, "hello");
  p.show();
  return 0;
}
)",
                            "widget 0\nwidget 0\nlabel hi! 3\n"
                            "widget 0\nwidget 0\nlabel hello 5\n"
                            "label hello 5\n");
}

// An abstract class's `__init__` that calls a pure virtual method, its
// fields assigned, reaches the slot's trap: the object is one of the
// abstract class while that runs, so the program stops with the runtime's
// message (tests/Runtime/ObjectTests.cpp) instead of reading a field the
// subclass has not assigned yet.  So does a call through a reference that
// `__init__` gave away, made by the subclass's `__init__` before its own
// field has a value: the object is still one of the abstract class.
TEST(PureVirtual, AbstractInitCallingAPureMethodAborts) {
  for (const char *source : {R"(
class Shape {
  n: int;
  view fn sides() -> int;
}
impl Shape {
  fn __init__() { self.n = 0; self.n = self.sides(); }
}
class Square : Shape {
  label: Str;
  view fn sides() -> int { return self.label.len(); }
}
impl Square {
  fn __init__() { __super__(); self.label = "four"; }
}
fn main() -> int { s = Square(); return s.n; }
)",
                             R"(
class Board { s: Shape?; }
impl Board {
  view fn count() -> int {
    match self.s { s: Shape { return s.sides(); } None { return 0; } }
  }
}
class Shape {
  n: int;
  view fn sides() -> int;
}
impl Shape {
  fn __init__(b: Board) { self.n = 0; b.s = self; }
}
class Square : Shape {
  label: Str;
  view fn sides() -> int { return self.label.len(); }
}
impl Square {
  fn __init__(b: Board) { __super__(b); self.n = b.count(); self.label = "four"; }
}
fn main() -> int { b = Board(None); s = Square(b); return s.n; }
)"}) {
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
      auto r = compileAndRun(source);
      if (!r.CompileOk)
        fprintf(stderr, "child compile failed: %s\n", r.StdErr.c_str());
      _exit(r.CompileOk ? 0 : 1); // only when the call did not abort
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status)) << source;
    EXPECT_EQ(WEXITSTATUS(status), 128 + SIGABRT) << source;
  }
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
// No compiled program has an object of an abstract class (one is of it only
// while its `__init__` runs, above); a hand-written PIR one that allocates
// one and calls the slot aborts (in a child: the abort ends the process;
// tests/Runtime/ObjectTests.cpp checks the message).
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
