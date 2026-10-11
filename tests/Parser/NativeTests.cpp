// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: `native fn` declarations, whose body is a C symbol (#198).

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Native, DeclarationParses) {
  auto [ok, driver] = parse(R"(
    native fn put(fd: int, s: Str) -> int = "pk_test_put";
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
           "native fn f<T>(x: T) = \"pk_f\";",   // generic
           "native f() = \"pk_f\";",             // no 'fn'
           "class C { native fn f() = \"pk_f\"; }",
       }) {
    auto [ok, _] = parse(std::string(src) + "\nfn main() -> int { return 0; }");
    EXPECT_FALSE(ok) << src;
  }
}
