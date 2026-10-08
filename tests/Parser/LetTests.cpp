// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: `let` local declarations.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Let, DeclaresALocal) {
  auto [ok, driver] = parse(R"(fn main() -> int {
  let n = 3;
  let s: Str = "hi";
  return n;
})");
  ASSERT_TRUE(ok);
  std::string ast = dumpAST(*driver);
  // The declaration starts at `let` and ends with its initializer, like a
  // typed declaration's.
  EXPECT_NE(ast.find("DeclStmt <2:3-2:12>"), std::string::npos) << ast;
  EXPECT_NE(ast.find("VarDecl <2:3-2:12> 'n' let\n"), std::string::npos)
      << ast;
  EXPECT_NE(ast.find("VarDecl <3:3-3:20> 's' let type\n"), std::string::npos)
      << ast;
}

// `let` is reserved and only starts a local declaration, which has one name
// and an initializer.
TEST(Let, OnlyDeclaresOneInitializedLocal) {
  const char *const cases[] = {
      "fn main() -> int { let n; return 0; }",
      "fn main() -> int { let n: int; return 0; }",
      "fn main() -> int { let a, b = (1, 2); return 0; }",
      "fn main() -> int { let = 1; return 0; }",
      "fn main() -> int { let: int = 1; return 0; }",
      "fn f(let n: int) { }\nfn main() -> int { return 0; }",
      "fn f() -> let int { return 1; }\nfn main() -> int { return 0; }",
      "class C { let n: int; }\nfn main() -> int { return 0; }",
      "let n = 1;\nfn main() -> int { return 0; }",
      "fn main() -> int { x = let; return 0; }",
  };
  for (const char *src : cases) {
    auto [ok, _] = parse(src);
    EXPECT_FALSE(ok) << src;
  }
}
