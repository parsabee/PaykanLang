// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
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
           "impl Box<T> { fn f() {} }",        // generic, not yet
           "static fn f() {}",                 // outside an impl
           "class A { static fn f() {} }",     // in a class body
       }) {
    auto [ok, _] = parse(std::string(src) + "\nfn main() -> int { return 0; }");
    EXPECT_FALSE(ok) << src;
  }
}

// A static function is called like a module function: `A::make()`.
TEST(Impl, StaticCallParsesAsAQualifiedCall) {
  auto [ok, driver] = parse("fn main() -> int { a = A::make(); return 0; }");
  ASSERT_TRUE(ok);
}
