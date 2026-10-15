// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: traits, `impl Trait for Type` and type parameter bounds
// (#216).

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;
using namespace paykan::ast;

TEST(Trait, DeclarationParses) {
  auto [ok, driver] = parse(R"(
    trait Show { view fn toString() -> Str; }
    trait Into<T> {
      view fn into() -> T;
      fn reset(n: inout int);
      view fn name() -> view Str;
    }
  )");
  ASSERT_TRUE(ok);
  const auto &traits = driver->getRoot()->getTraitDecls();
  ASSERT_EQ(traits.size(), 2u);
  EXPECT_EQ(traits[0]->getName(), "Show");
  ASSERT_EQ(traits[0]->getMethods().size(), 1u);
  EXPECT_TRUE(traits[0]->getMethods()[0]->isView());
  EXPECT_EQ(traits[0]->getMethods()[0]->getBody(), nullptr);
  ASSERT_EQ(traits[1]->getTypeParams().size(), 1u);
  EXPECT_EQ(*traits[1]->getTypeParams()[0], "T");
  EXPECT_EQ(traits[1]->getMethods()[2]->getResultMode(), ParamMode::View);
}

TEST(Trait, ImplForParses) {
  auto [ok, driver] = parse(R"(
    impl Show for A { view fn toString() -> Str { return "A"; } }
    impl Into<int> for geo::Point {}
    impl A {}
  )");
  ASSERT_TRUE(ok);
  const auto &impls = driver->getRoot()->getImplDecls();
  ASSERT_EQ(impls.size(), 3u);
  EXPECT_EQ(impls[0]->getTypeName(), "A");
  EXPECT_EQ(typeName(impls[0]->getTrait()), "Show");
  EXPECT_EQ(impls[1]->getTypeName(), "geo::Point");
  EXPECT_EQ(typeName(impls[1]->getTrait()), "Into<int>");
  EXPECT_EQ(impls[2]->getTrait(), nullptr);
}

TEST(Trait, BoundsParse) {
  auto [ok, driver] = parse(R"(
    fn show<T: Show + Into<int>, U>(x: view T) -> Str { return ""; }
    class Box<T: Show> { v: T; }
    fn plain<T>(x: T) -> T { return x; }
  )");
  ASSERT_TRUE(ok);
  const auto &fns = driver->getRoot()->getGenericFuncDecls();
  ASSERT_EQ(fns.size(), 2u);
  const auto &bounds = fns[0]->getTypeParamBounds();
  ASSERT_EQ(bounds.size(), 2u);
  ASSERT_EQ(bounds[0].size(), 2u);
  EXPECT_EQ(typeName(bounds[0][0]), "Show");
  EXPECT_EQ(typeName(bounds[0][1]), "Into<int>");
  EXPECT_TRUE(bounds[1].empty());
  EXPECT_TRUE(fns[1]->getTypeParamBounds().empty());
  const auto &box = driver->getRoot()->getGenericClassDecls()[0];
  ASSERT_EQ(box->getTypeParamBounds().size(), 1u);
  EXPECT_EQ(typeName(box->getTypeParamBounds()[0][0]), "Show");
}

TEST(Trait, MalformedDeclarationsAreRejected) {
  for (const char *src : {
           "trait { fn f(); }",            // no name
           "trait S { fn f() {} }",        // a body
           "trait S { fn f() }",           // no ';'
           "trait S { x: int; }",          // a field
           "trait S { fn f<T>(); }",       // a generic method
           "trait S<T: Show> { fn f(); }", // a bound on its own
           "trait S { fn f(); ",           // unclosed
           "impl Show for { }",            // no type after for
           "impl Show for Box<T> { }",     // generic, not yet
           "fn f<T: >(x: T) {}",           // an empty bound
           "fn f<T: Show +>(x: T) {}",     // a dangling '+'
       }) {
    auto [ok, _] = parse(std::string(src) + "\nfn main() -> int { return 0; }");
    EXPECT_FALSE(ok) << src;
  }
}
