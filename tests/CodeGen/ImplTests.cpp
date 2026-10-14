// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen tests: `impl` blocks.  Their methods are called directly (no
// vtable slot), subclasses inherit them, and their `static fn`s are called
// as `Type::name()`, also from another module.

#include "CodeGenTestUtils.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Impl, MethodsStaticFnsAndSubclasses) {
  LeakGuard guard;
  auto r = compileAndRun(R"pkn(
    class Point {
      x: int; y: int;
      fn __init__(x: int, y: int) { self.x = x; self.y = y; }
      view fn describe() -> Str {
        return "Point(" + Str(self.x) + ", " + Str(self.y) + ")";
      }
    }
    impl Point {
      static fn origin() -> Point { return Point(0, 0); }
      view fn sum() -> int { return self.x + self.y; }
      fn shift(d: int) { self.x = self.x + d; self.y = self.y + d; }
      view fn label(prefix: Str) -> Str { return prefix + self.describe(); }
    }
    class Point3 : Point {
      z: int;
      fn __init__(x: int, y: int, z: int) { __super__(x, y); self.z = z; }
      view fn describe() -> Str {
        return "Point3(" + Str(self.sum()) + ", " + Str(self.z) + ")";
      }
    }
    impl Point3 {
      static fn unit() -> Point3 { return Point3(1, 1, 1); }
      view fn volume() -> int { return self.x * self.y * self.z; }
    }
    fn main() -> int {
      p = Point::origin();
      p.shift(2);
      println(p.label("p=") + " " + Str(p.sum()));
      q = Point3::unit();
      q.shift(1);
      base: Point = q;
      println(base.label("q=") + " " + Str(q.volume()) + " " +
              Str(base.sum()));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "p=Point(2, 2) 4\nq=Point3(4, 1) 4 4\n");
  guard.expectNoLeaks("impl methods");
}

// An importer calls the `impl` methods of a class of another module, also
// on its own subclass of it, and its `static fn`s as `mod::Type::name()`.
TEST(Impl, AcrossModules) {
  LeakGuard guard;
  auto dir = tempDir() / "pkn_impl_modules";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "shapes.pkn") << R"(
class Bag {
  items: Str[];
  fn __init__() { self.items = []; }
  view fn size() -> int { return self.items.len(); }
}
impl Bag {
  static fn of(a: Str, b: Str) -> Bag {
    bag = Bag();
    bag.add(a);
    bag.add(b);
    return bag;
  }
  fn add(s: Str) { self.items.push(s); }
  view fn first() -> view Str { return self.items[0]; }
}
)";
  std::ofstream(dir / "main.pkn") << R"(
import shapes;
class Tagged : shapes::Bag { fn __init__() { __super__(); } }
fn main() -> int {
  b = shapes::Bag::of("a", "b");
  b.add("c");
  t = Tagged();
  t.add("x");
  println(b.first() + " " + Str(b.size()) + " " + t.first());
  return 0;
}
)";
  auto r = compileAndRunFile((dir / "main.pkn").string());
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "a 3 x\n");
  guard.expectNoLeaks("impl across modules");
  std::filesystem::remove_all(dir);
}
