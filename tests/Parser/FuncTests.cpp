// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: function declarations, calls, parameters

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Func, FunctionWithParams) {
  auto [ok, _] = parse(R"(
    fn add(a: int, b: int) -> int {
      return a + b;
    }
    fn main() -> int { return add(1, 2); }
  )");
  EXPECT_TRUE(ok);
}

TEST(Func, VoidFunction) {
  auto [ok, _] = parse(R"(import ::io;
    fn greet() {
      io::println("hi");
    }
    fn main() -> int { greet(); return 0; }
  )");
  EXPECT_TRUE(ok);
}

// `fn f(...) -> T = e;` is `fn f(...) -> T { return e; }`: the body is one
// `return` spanning `e`, and the declaration ends at its `;`.  Without the
// result type the declaration is marked for Sema to infer it.  A method
// takes the same form; a trait's signature has no body of either kind.
TEST(Func, ExpressionBodies) {
  auto [ok, driver] = parse(R"(fn double(x: int) = x * 2;
fn half(x: int) -> int = x / 2;
class C {
  n: int;
  fn get() -> int = self.n;
}
fn main() -> int { return double(1); }
)");
  ASSERT_TRUE(ok);
  std::string d = dumpAST(*driver);
  for (const char *e :
       {"FuncDecl <1:1-1:27> 'double' inferred\n| `-CompoundStmt <1:21-1:26>\n"
        "|   `-ReturnStmt <1:21-1:26>\n"
        "|     `-BinaryExpr <1:21-1:26> '*'\n",
        "FuncDecl <2:1-2:32> 'half' ->\n| |-BuiltinType 'int'\n"
        "| `-CompoundStmt <2:26-2:31>\n"
        "|   `-ReturnStmt <2:26-2:31>\n",
        "FuncDecl <5:3-5:28> 'get' ->"})
    EXPECT_NE(d.find(e), std::string::npos) << e << "\n" << d;

  struct Case {
    const char *Src;
    const char *Error;
  };
  for (const Case &c :
       {Case{"fn f(x: int) = x\nfn main() -> int { return 0; }\n",
             "expected ';' after the function's expression"},
        Case{"fn f(x: int) x;\nfn main() -> int { return 0; }\n",
             "expected '{' or '=' to start the function's body"},
        Case{"trait T { fn f() -> int = 1; }\n",
             "a trait method has no body: end its signature with ';'"}}) {
    auto r = parseWithDiags(c.Src);
    EXPECT_FALSE(r.Ok) << c.Src;
    EXPECT_NE(r.Text.find(c.Error), std::string::npos) << c.Src << r.Text;
  }
}
