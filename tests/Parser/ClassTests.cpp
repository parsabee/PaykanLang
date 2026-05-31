// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: class declarations, inheritance, member access, match

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// ─── class declarations ─────────────────────────────────────────────────────

TEST(Class, EmptyClass) {
  auto [ok, _] = parse(R"(
    class A {}
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, ClassWithFields) {
  auto [ok, _] = parse(R"(
    class Point {
      x: int;
      y: int;
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, ClassWithMethod) {
  auto [ok, _] = parse(R"(
    class Counter {
      count: int;
      fn getCount() -> int {
        return 0;
      }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, ClassWithInitAndMethod) {
  auto [ok, _] = parse(R"(
    class A {
      a: int;
      fn __init__(a: int) {
        self.a = a;
      }
      fn getA() -> int {
        return 0;
      }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, ClassWithSuperclass) {
  auto [ok, _] = parse(R"(
    class Animal {}
    class Dog : Animal {}
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, ClassMemberAccess) {
  auto [ok, _] = parse(R"(
    class Foo {
      x: int;
      fn getX() -> int {
        return self.x;
      }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, ClassMemberAssign) {
  auto [ok, _] = parse(R"(
    class Foo {
      x: int;
      fn setX(v: int) {
        self.x = v;
      }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, MultipleClasses) {
  auto [ok, _] = parse(R"(
    class A {
      a: int;
    }
    class B {
      b: float;
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, MissingCloseBrace) {
  auto [ok, _] = parse(R"(
    class A {
      x: int;
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(ok);
}

TEST(Class, FieldMissingSemicolon) {
  auto [ok, _] = parse(R"(
    class A {
      x: int
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(ok);
}

// ─── match ──────────────────────────────────────────────────────────────────

TEST(Class, MatchBasicTypeArms) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: Obj = 0;
      match x {
        Dog { }
        Cat { }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, MatchWithBinding) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: Obj = 0;
      match x {
        d: Dog { }
        c: Cat { }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, MatchWithWildcard) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: Obj = 0;
      match x {
        Dog { }
        _ { }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, MatchAllArmForms) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: Obj = 0;
      match x {
        Dog { }
        c: Cat { }
        _ { }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, MatchEmptyBody) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: Obj = 0;
      match x { }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, MatchWithStmtsInArm) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: Obj = 0;
      match x {
        d: Dog {
          y: int = 1;
        }
        _ {
          y: int = 2;
        }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, MatchMissingCloseBrace) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: Obj = 0;
      match x {
        Dog { }
    }
  )");
  EXPECT_FALSE(ok);
}

TEST(Class, MatchArmMissingBodyBrace) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: Obj = 0;
      match x {
        Dog
      }
      return 0;
    }
  )");
  EXPECT_FALSE(ok);
}
