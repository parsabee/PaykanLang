// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: `weak` declarations (docs/grammar.md): a local `weak y:
// Point? = x;` and a class field `weak parent: Node?;`.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Weak, DeclaresLocalsAndFields) {
  auto [ok, driver] = parse(R"(class Node {
  value: int;
  weak parent: Node?;
}
fn main() -> int {
  x = Node(1, None);
  weak w: Node? = x;
  weak u = x;
  weak e: Node?;
  return 0;
})");
  ASSERT_TRUE(ok);
  std::string ast = dumpAST(*driver);
  EXPECT_NE(ast.find("VarDecl <3:8-3:21> 'parent' weak type\n"),
            std::string::npos)
      << ast;
  // A local declaration starts at `weak` and ends with its initializer (or
  // its type).
  EXPECT_NE(ast.find("DeclStmt <7:3-7:20>"), std::string::npos) << ast;
  EXPECT_NE(ast.find("VarDecl <7:3-7:20> 'w' weak type\n"), std::string::npos)
      << ast;
  EXPECT_NE(ast.find("VarDecl <8:3-8:13> 'u' weak\n"), std::string::npos)
      << ast;
  EXPECT_NE(ast.find("VarDecl <9:3-9:16> 'e' weak type\n"), std::string::npos)
      << ast;
}

// `weak` is reserved: it starts a local declaration (one name, with a type
// or an initializer, never a borrow) or a class field, and nothing else.
TEST(Weak, OnlyStartsALocalOrAField) {
  const char *const cases[] = {
      "fn main() -> int { weak; return 0; }",
      "fn main() -> int { weak n; return 0; }",
      "fn main() -> int { weak a, b = (1, 2); return 0; }",
      "fn main() -> int { weak n: view Point? = x; return 0; }",
      "fn main() -> int { weak n: inout Point? = x; return 0; }",
      "fn main() -> int { let weak n = x; return 0; }",
      "fn main() -> int { weak let n = x; return 0; }",
      "fn main() -> int { x = weak; return 0; }",
      "fn f(weak n: Point?) { }\nfn main() -> int { return 0; }",
      "fn f() -> weak Point? { return None; }\nfn main() -> int { return 0; }",
      "class C { weak fn f() { } }\nfn main() -> int { return 0; }",
      "class C { weak; }\nfn main() -> int { return 0; }",
      "struct S { weak p: Point?; }\nfn main() -> int { return 0; }",
      "weak n = x;\nfn main() -> int { return 0; }",
  };
  for (const char *src : cases) {
    auto [ok, _] = parse(src);
    EXPECT_FALSE(ok) << src;
  }
}
