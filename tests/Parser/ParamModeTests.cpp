// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: `view` and `inout` parameters.  The keywords may start a
// parameter of a function, a method or a constructor, and nothing else.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

/// The diagnostics of parsing @p source with the frontend under test; "" when
/// it parses.
std::string parseErrors(const std::string &source) {
  auto path = writeTempFile(source);
  paykan::parser::ParserDriver driver(testFrontend());
  std::ostringstream os;
  paykan::sema::DiagEngine diag(os);
  driver.setDiagEngine(&diag);
  int rc = driver.parseFile(path);
  std::filesystem::remove(path);
  EXPECT_EQ(rc != 0, !os.str().empty()) << source;
  return os.str();
}

} // namespace

TEST(ParamMode, ModesOnFunctionsMethodsAndConstructors) {
  auto [ok, driver] = parse(R"(
    fn scale(inout x: float, view by: float, n: int) { }
    class Counter {
      n: int;
      fn __init__(view start: int) { self.n = start; }
      fn add(inout to: int, view k: int) -> int { return k; }
    }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(ok);
  std::string ast = dumpAST(*driver);
  // The AST printer shows the parameters that have a mode.
  EXPECT_NE(ast.find("FuncDecl <2:5-2:57> 'scale' inout 'x' view 'by'\n"),
            std::string::npos)
      << ast;
  EXPECT_NE(ast.find("'__init__' view 'start'\n"), std::string::npos) << ast;
  EXPECT_NE(ast.find("'add' inout 'to' view 'k' ->\n"), std::string::npos)
      << ast;
  const auto &params = driver->getRoot()->getFuncDecls()[0]->getParams();
  ASSERT_EQ(params.size(), 3u);
  EXPECT_EQ(params[0].Mode, paykan::ast::ParamMode::Inout);
  EXPECT_EQ(params[1].Mode, paykan::ast::ParamMode::View);
  EXPECT_EQ(params[2].Mode, paykan::ast::ParamMode::Value);
}

// `view` and `inout` are reserved, and only start a parameter.  There is no
// call-site marking.
TEST(ParamMode, KeywordsOnlyStartAParameter) {
  const char *const cases[] = {
      "fn main() -> int { inout x = 1; return 0; }",
      "fn main() -> int { view: int = 1; return 0; }",
      "fn main() -> int { x: inout int = 1; return 0; }",
      "fn bump(inout n: int) { }\nfn main() -> int { bump(inout k); }",
      "fn f(x: inout int) { }\nfn main() -> int { return 0; }",
      "fn f(x: int inout) { }\nfn main() -> int { return 0; }",
      "fn f(inout view x: int) { }\nfn main() -> int { return 0; }",
      "fn f() -> inout int { return 0; }\nfn main() -> int { return 0; }",
      "fn inout() { }\nfn main() -> int { return 0; }",
      "fn f(view: int) { }\nfn main() -> int { return 0; }",
      "class C { inout n: int; }\nfn main() -> int { return 0; }",
      "class C { n: int; }\nfn main() -> int { return C().view; }",
  };
  for (const char *src : cases) {
    auto [ok, _] = parse(src);
    EXPECT_FALSE(ok) << src;
  }
  if (testFrontend() != "recursive-descent")
    return; // the wording of a syntax error is the frontend's
  EXPECT_NE(
      parseErrors(cases[0]).find(
          "error: unexpected 'inout'; expected an expression ('inout' only "
          "marks a parameter: 'fn f(inout x: int)')"),
      std::string::npos);
  EXPECT_NE(parseErrors(cases[2]).find("error: unexpected 'inout'; expected a "
                                       "type ('inout' only marks a parameter"),
            std::string::npos);
  EXPECT_NE(parseErrors(cases[10]).find("unexpected 'inout'; expected a field "
                                        "('name: Type;') or a method ('fn') "
                                        "in the class body ('inout' only"),
            std::string::npos);
}
