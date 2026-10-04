// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: optional types `T?` in every type position, the
// `None` match-arm pattern, optional primitives (`int?`, ...), and the
// parse-time rejections (`void?`, `T??`).

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// ─── T? in every type position ───────────────────────────────────────────────

TEST(Optional, VarDeclOptionalClass) {
  auto [ok, _] = parse(R"(
    class Node { v: int; }
    fn main() -> int {
      n: Node? = None;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Optional, VarDeclOptionalStr) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      s: Str? = "hi";
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Optional, OptionalArrayAndArrayOfOptional) {
  // `int[]?` (optional array) and `Str?[]` (array of optionals) both parse.
  auto [ok, _] = parse(R"(
    fn main() -> int {
      xs: int[]? = None;
      ys: Str?[] = [];
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Optional, ParamAndReturnType) {
  auto [ok, _] = parse(R"(
    class Node { v: int; }
    fn find(n: Node?, key: Str?) -> Node? { return None; }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Optional, ClassFieldAndMethodSignature) {
  auto [ok, _] = parse(R"(
    class Node {
      next: Node?;
      fn __init__() {}
      fn tail() -> Node? { return self.next; }
      fn link(n: Node?) { self.next = n; }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Optional, QualifiedOptionalType) {
  // module::Type? in a declaration (resolution is Sema's job).
  auto [ok, _] = parse(R"(
    fn main() -> int {
      p: geometry::Point? = None;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Optional, MatchArmsOverOptional) {
  // Type arm with binding, a `None` literal arm, and the wildcard all parse.
  auto [ok, _] = parse(R"(
    class Node { v: int; }
    fn main() -> int {
      n: Node? = None;
      match n {
        x: Node { println("some"); }
        None     { println("none"); }
      }
      match n {
        Node { }
        _    { }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Optional, ComparisonAgainstNone) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      s: Str? = None;
      if (s == None) { return 1; }
      if (s != None) { return 2; }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// ─── parse-time rejections ───────────────────────────────────────────────────

TEST(Optional, OptionalPrimitivesParse) {
  // Optional primitives (#66) parse in every type position, including a
  // primitive match arm over an `int?` subject.
  auto [ok, _] = parse(R"(
    class H { n: int?; fn __init__() {} }
    fn f(x: float?, b: bool?) -> char? { return None; }
    fn main() -> int {
      x: int? = None;
      xs: int?[] = [1, 2];
      t: (int?, Str) = (1, "a");
      match x {
        n: int { println("some"); }
        None   { println("none"); }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Optional, OptionalPrimitiveAstShowsOptionalType) {
  auto r = parse("fn main() -> int { x: int? = 5; return 0; }");
  ASSERT_TRUE(r.Ok);
  std::string ast = dumpAST(*r.Driver);
  EXPECT_NE(ast.find("OptionalType"), std::string::npos) << ast;
}

TEST(Optional, OptionalVoidRejected) {
  EXPECT_FALSE(parse("fn f() -> void? {}  fn main() -> int { return 0; }").Ok);
  EXPECT_FALSE(parse("fn f(x: void?) {}   fn main() -> int { return 0; }").Ok);
}

TEST(Optional, NestedOptionalRejected) {
  auto [ok, _] = parse(R"(
    class Node { v: int; }
    fn main() -> int {
      n: Node?? = None;
      return 0;
    }
  )");
  EXPECT_FALSE(ok);
}

TEST(Optional, NestedOptionalOfStrRejected) {
  auto [ok, _] = parse(R"(
    fn f(s: Str??) {}
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(ok);
}

TEST(Optional, QuestionMarkOutsideTypeIsSyntaxError) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = 1 ? 2;
      return 0;
    }
  )");
  EXPECT_FALSE(ok);
}
