// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Parser tests: `weak` declarations (docs/grammar.md): a local `y: weak
// Point? = x;` and a class field `parent: weak Node?;`, `weak mut` for one
// that changes a `shared mut` object, and the old `weak y: ...` spelling.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Weak, DeclaresLocalsAndFields) {
  auto [ok, driver] = parse(R"(class Node {
  value: int;
  parent: weak Node?;
}
fn main() -> int {
  x = Node(1, None);
  w: weak Node? = x;
  u: weak = x;
  e: weak Node?;
  return 0;
})");
  ASSERT_TRUE(ok);
  std::string ast = dumpAST(*driver);
  EXPECT_NE(ast.find("VarDecl <3:3-3:21> 'parent' weak type\n"),
            std::string::npos)
      << ast;
  // A local declaration starts at its name and ends with its initializer (or
  // its type).
  EXPECT_NE(ast.find("DeclStmt <7:3-7:20>"), std::string::npos) << ast;
  EXPECT_NE(ast.find("VarDecl <7:3-7:20> 'w' weak type\n"), std::string::npos)
      << ast;
  // A weak declaration's type is the `shared` form it reads as; `u: weak =
  // x;` infers the rest, so its QualifiedType has no inner type.
  EXPECT_NE(ast.find("QualifiedType <7:6-7:16> 'shared'\n"), std::string::npos)
      << ast;
  EXPECT_NE(ast.find("VarDecl <8:3-8:14> 'u' weak type\n"), std::string::npos)
      << ast;
  EXPECT_NE(ast.find("QualifiedType <8:6-8:10> 'shared'\n"), std::string::npos)
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
      "fn main() -> int { n: weak view Point? = x; return 0; }",
      "fn main() -> int { n: weak inout Point? = x; return 0; }",
      "fn main() -> int { let n: weak = x; return 0; }",
      "fn main() -> int { weak let n = x; return 0; }",
      "fn main() -> int { x = weak; return 0; }",
      "fn f(n: weak Point?) { }\nfn main() -> int { return 0; }",
      "fn f() -> weak Point? { return None; }\nfn main() -> int { return 0; }",
      "class C { weak fn f() { } }\nfn main() -> int { return 0; }",
      "class C { weak; }\nfn main() -> int { return 0; }",
      "struct S { p: weak Point?; }\nfn main() -> int { return 0; }",
      "n: weak = x;\nfn main() -> int { return 0; }",
  };
  for (const char *src : cases) {
    auto [ok, _] = parse(src);
    EXPECT_FALSE(ok) << src;
  }
}

// `weak mut T` is the `shared mut` form, marked weak: a weak reference that
// changes its `shared mut` object.  The spelling before the qualifier,
// `weak NAME: T`, is an error naming the declaration as it is written now.
TEST(Weak, WeakMutAndTheOldSpelling) {
  auto [ok, driver] = parse(R"(class Node {
  prev: weak mut Node?;
}
fn main() -> int {
  let x: shared mut = Node(None);
  w: weak mut = x;
  return 0;
})");
  ASSERT_TRUE(ok);
  std::string ast = dumpAST(*driver);
  EXPECT_NE(ast.find("VarDecl <2:3-2:23> 'prev' weak type\n"),
            std::string::npos)
      << ast;
  EXPECT_NE(ast.find("QualifiedType <2:9-2:23> 'shared mut'"),
            std::string::npos)
      << ast;
  const std::pair<const char *, const char *> old[] = {
      {"class Node { weak next: Node?; }\nfn main() -> int { return 0; }",
       "'weak' goes after the colon, before the type: write 'next: weak "
       "Node?'"},
      {"class Node { weak up: shared mut Node?; }\n"
       "fn main() -> int { return 0; }",
       "write 'up: weak mut Node?'"},
      {"fn main() -> int { weak w = 1; return 0; }", "write 'w: weak = ...'"},
      {"class N { v: int; }\nfn main() -> int { w: weak shared N? = None; "
       "return 0; }",
       "'weak' takes 'mut' or nothing"},
      {"class N { v: int; }\nfn main() -> int { let w: weak = N(1); return 0; "
       "}",
       "a 'weak' local cannot be 'let'"},
  };
  for (const auto &[src, msg] : old) {
    auto r = parseWithDiags(src);
    EXPECT_FALSE(r.Ok) << src;
    EXPECT_NE(r.Text.find(msg), std::string::npos) << src << "\n" << r.Text;
  }
}
