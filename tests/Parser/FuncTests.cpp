// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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

// A parameter written without its type gives the function a type parameter
// of its own, after the written ones: `fn add(a, b) => a + b;` is
// `fn add<typeof(a), typeof(b)>(a: typeof(a), b: typeof(b)) => a + b;`, a name
// no program can write.  A repeated name adds one (Sema reports the repeat).
// Only a free function's parameters may leave out their types.
TEST(Func, UntypedParameters) {
  auto [ok, driver] = parse(R"(fn add(a, b) => a + b;
fn pick<T: Show>(a: T, b) -> T { return a; }
fn show(label: Str, v) { }
fn twice(x, x) => 1;
fn main() -> int { return 0; }
)");
  ASSERT_TRUE(ok);
  std::string d = dumpAST(*driver);
  for (const char *e : {"FuncDecl <1:1-1:23> 'add' <typeof(a), typeof(b)> "
                        "inferred\n",
                        "FuncDecl <2:1-2:45> 'pick' <T: Show, typeof(b)> ->\n",
                        "FuncDecl <3:1-3:27> 'show' <typeof(v)>\n",
                        "FuncDecl <4:1-4:21> 'twice' <typeof(x)> inferred\n"})
    EXPECT_NE(d.find(e), std::string::npos) << e << "\n" << d;

  const auto &generic = driver->getRoot()->getGenericFuncDecls();
  ASSERT_EQ(generic.size(), 4u);
  // The parameter's type names the added type parameter, and a written
  // bound list gains an empty one for it.
  const paykan::ast::FuncDecl *add = generic[0];
  const auto *aTy = paykan::ast::dyn_cast<paykan::ast::ClassType>(
      add->getParams()[0].ParamType);
  ASSERT_NE(aTy, nullptr);
  EXPECT_EQ(aTy->getName(), "typeof(a)");
  EXPECT_TRUE(add->getTypeParamBounds().empty());
  const auto &bounds = generic[1]->getTypeParamBounds();
  ASSERT_EQ(bounds.size(), 2u);
  EXPECT_EQ(bounds[0].size(), 1u);
  EXPECT_TRUE(bounds[1].empty());

  struct Case {
    const char *Src;
    const char *Error;
  };
  const std::string onlyFree = "parameter 'x' needs a type ('x: int'): only a "
                               "free function's parameters can leave it out";
  for (const Case &c :
       {Case{"fn main() -> int { f = fn(x) => x * 2; return 0; }\n",
             onlyFree.c_str()},
        Case{"class A {\n  v: int;\n  fn get(x) -> int => 1;\n}\n",
             onlyFree.c_str()},
        Case{"struct S { v: int; }\nimpl S {\n  static fn mk(x) -> int => "
             "1;\n}\n",
             onlyFree.c_str()},
        Case{"trait T { fn f(x) -> int; }\n", onlyFree.c_str()},
        Case{"native fn f(x) -> int = \"f\";\n",
             "parameter 'x' needs a type ('x: int'): a native function's "
             "parameters have types"},
        Case{"fn f(x: view) => 1;\n", "expected a type"}}) {
    auto r = parseWithDiags(c.Src);
    EXPECT_FALSE(r.Ok) << c.Src;
    EXPECT_NE(r.Text.find(c.Error), std::string::npos) << c.Src << r.Text;
  }
}

// `fn f(...) -> T => e;` is `fn f(...) -> T { return e; }`: the body is one
// `return` spanning `e`, and the declaration ends at its `;`.  Without the
// result type the declaration is marked for Sema to infer it.  A method
// takes the same form; a trait's signature has no body of either kind.
TEST(Func, ExpressionBodies) {
  auto [ok, driver] = parse(R"(fn double(x: int) => x * 2;
fn half(x: int) -> int => x / 2;
class C {
  n: int;
  fn get() -> int => self.n;
}
fn main() -> int { return double(1); }
)");
  ASSERT_TRUE(ok);
  std::string d = dumpAST(*driver);
  for (const char *e :
       {"FuncDecl <1:1-1:28> 'double' inferred\n| `-CompoundStmt <1:22-1:27>\n"
        "|   `-ReturnStmt <1:22-1:27>\n"
        "|     `-BinaryExpr <1:22-1:27> '*'\n",
        "FuncDecl <2:1-2:33> 'half' ->\n| |-BuiltinType 'int'\n"
        "| `-CompoundStmt <2:27-2:32>\n"
        "|   `-ReturnStmt <2:27-2:32>\n",
        "FuncDecl <5:3-5:29> 'get' ->"})
    EXPECT_NE(d.find(e), std::string::npos) << e << "\n" << d;

  struct Case {
    const char *Src;
    const char *Error;
  };
  for (const Case &c :
       {Case{"fn f(x: int) => x\nfn main() -> int { return 0; }\n",
             "expected ';' after the function's expression"},
        Case{"fn f(x: int) x;\nfn main() -> int { return 0; }\n",
             "expected '{' or '=>' to start the function's body"},
        Case{"trait T { fn f() -> int => 1; }\n",
             "a trait method has no body: end its signature with ';'"},
        Case{"trait T { fn f() -> int = 1; }\n",
             "a trait method has no body: end its signature with ';'"},
        // `=` is a native function's: an expression body is `=>`.
        Case{"fn double(x: int) = x * 2;\nfn main() -> int { return 0; }\n",
             "an expression body starts with '=>', not '=': 'fn double(...) "
             "=> ...;'"}}) {
    auto r = parseWithDiags(c.Src);
    EXPECT_FALSE(r.Ok) << c.Src;
    EXPECT_NE(r.Text.find(c.Error), std::string::npos) << c.Src << r.Text;
  }
}
