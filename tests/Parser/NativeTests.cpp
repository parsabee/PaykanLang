// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Parser tests: `native fn` declarations, whose body is a C symbol (#198).

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Native, DeclarationParses) {
  auto [ok, driver] = parse(R"(
    native fn put(fd: int, s: view Str) -> int = "pk_test_put";
    native fn flush() = "pk_test_flush";
    fn main() -> int { return put(1, "x"); }
  )");
  ASSERT_TRUE(ok);
  auto *fn = driver->getRoot()->getFuncDecls()[0];
  EXPECT_TRUE(fn->isNative());
  EXPECT_EQ(fn->getNativeSymbol(), "pk_test_put");
  EXPECT_EQ(fn->getBody(), nullptr);
  EXPECT_EQ(fn->getParams().size(), 2u);
  EXPECT_FALSE(driver->getRoot()->getFuncDecls()[2]->isNative());
}

TEST(Native, MalformedDeclarationsAreRejected) {
  for (const char *src : {
           "native fn f() -> int;",              // no symbol
           "native fn f() -> int = pk_f;",       // symbol not a string
           "native fn f() -> int = \"pk_f\"",    // no ';'
           "native fn f() -> int { return 1; }", // a body
           "native f() = \"pk_f\";",             // no 'fn'
           "class C { native fn f() = \"pk_f\"; }",
       }) {
    auto [ok, _] = parse(std::string(src) + "\nfn main() -> int { return 0; }");
    EXPECT_FALSE(ok) << src;
  }
}

// A generic native function is a template, like any generic function.
TEST(Native, GenericDeclarationParses) {
  auto [ok, driver] = parse(R"(
    native fn pick<T>(a: view T, b: view T) -> view T = "pk_test_pick";
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(ok);
  ASSERT_EQ(driver->getRoot()->getGenericFuncDecls().size(), 1u);
  auto *fn = driver->getRoot()->getGenericFuncDecls()[0];
  EXPECT_TRUE(fn->isNative());
  EXPECT_EQ(fn->getNativeSymbol(), "pk_test_pick");
  EXPECT_EQ(fn->getTypeParams().size(), 1u);
  EXPECT_EQ(fn->getResultMode(), paykan::ast::ParamMode::View);
}
