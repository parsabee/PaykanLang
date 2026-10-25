// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Parser tests: `impl` blocks and `static fn` (#216).

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Impl, BlockParses) {
  auto [ok, driver] = parse(R"(
    class A { n: int; }
    impl A {
      view fn read() -> int { return self.n; }
      fn bump() { self.n = self.n + 1; }
      static fn make() -> A { return A(); }
    }
    impl geo::Point { view fn norm() -> float { return 0.0; } }
  )");
  ASSERT_TRUE(ok);
  const auto &impls = driver->getRoot()->getImplDecls();
  ASSERT_EQ(impls.size(), 2u);
  EXPECT_EQ(impls[0]->getTypeName(), "A");
  ASSERT_EQ(impls[0]->getMethods().size(), 3u);
  EXPECT_TRUE(impls[0]->getMethods()[0]->isView());
  EXPECT_FALSE(impls[0]->getMethods()[1]->isStatic());
  EXPECT_TRUE(impls[0]->getMethods()[2]->isStatic());
  EXPECT_EQ(impls[1]->getTypeName(), "geo::Point");
}

TEST(Impl, MalformedBlocksAreRejected) {
  for (const char *src : {
           "impl { fn f() {} }",               // no type
           "impl A { x: int; }",               // a field
           "impl A { static view fn f() {} }", // static and view
           "impl A { fn f() {} ",              // unclosed
           "impl Box<int> { fn f() {} }",      // a type, not a parameter
           "impl Box<T[2]> { fn f() {} }",
           "impl Box<Str> { fn f() {} }",
           "impl Box<a::T> { fn f() {} }",
           "impl Box? { fn f() {} }",
           "static fn f() {}",             // outside an impl
           "class A { static fn f() {} }", // in a class body
       }) {
    auto [ok, _] = parse(std::string(src) + "\nfn main() -> int { return 0; }");
    EXPECT_FALSE(ok) << src;
  }
}

// A generic type's `impl` block names its type parameters, in either form.
TEST(Impl, GenericBlockNamesItsTypeParameters) {
  auto [ok, driver] = parse(R"(
    impl Box<T> { view fn get() -> T { return self.v; } }
    impl Show for Pair<K, V> { view fn toString() -> Str { return ""; } }
    impl Into<T> for gen::Box<T> { view fn into() -> T { return self.v; } }
  )");
  ASSERT_TRUE(ok);
  const auto &impls = driver->getRoot()->getImplDecls();
  ASSERT_EQ(impls.size(), 3u);
  EXPECT_EQ(impls[0]->getTypeName(), "Box");
  ASSERT_EQ(impls[0]->getTypeParams().size(), 1u);
  EXPECT_EQ(*impls[0]->getTypeParams()[0], "T");
  EXPECT_EQ(impls[1]->getTypeName(), "Pair");
  ASSERT_EQ(impls[1]->getTypeParams().size(), 2u);
  EXPECT_EQ(*impls[1]->getTypeParams()[1], "V");
  EXPECT_NE(impls[1]->getTrait(), nullptr);
  EXPECT_EQ(impls[2]->getTypeName(), "gen::Box");
  EXPECT_EQ(impls[2]->getTypeParams().size(), 1u);
}

// A static function is called like a module function: `A::make()`.
TEST(Impl, StaticCallParsesAsAQualifiedCall) {
  auto [ok, driver] = parse("fn main() -> int { a = A::make(); return 0; }");
  ASSERT_TRUE(ok);
}

// An `impl` block's method or `static fn` cannot have type parameters of
// its own: they were parsed and ignored (`static fn id<T>(y: T) -> T` was
// `unknown class type 'T'`).  Only a free function is generic; the methods
// of a generic type's `impl` block use the block's.
TEST(Impl, MethodsAndStaticFnsAreNotGeneric) {
  auto r = parseWithDiags("impl A {\n"
                          "  fn f<T>(x: T) {}\n"
                          "  static fn id<T>(y: T) -> T { return y; }\n"
                          "  view fn g() -> int => 1;\n"
                          "}\n"
                          "fn main() -> int { return 0; }\n");
  EXPECT_FALSE(r.Ok);
  ASSERT_EQ(r.Diags.size(), 2u) << r.Text;
  EXPECT_EQ(r.Diags[0].Message, paykan::frontend::kGenericMethod);
  EXPECT_EQ(r.Diags[0].Loc.getLineStart(), 2u);
  EXPECT_EQ(r.Diags[0].Loc.getColumnStart(), 7u);
  EXPECT_EQ(r.Diags[1].Message, paykan::frontend::kGenericStaticFn);
  EXPECT_EQ(r.Diags[1].Loc.getLineStart(), 3u);
  EXPECT_EQ(r.Diags[1].Loc.getColumnStart(), 15u);
  auto [ok, _] = parse("impl Box<T> { static fn of(v: T) -> Box<T> { return "
                       "Box<T>(v); } view fn get() -> T { return self.v; } }\n"
                       "fn main() -> int { return 0; }\n");
  EXPECT_TRUE(ok);
}
