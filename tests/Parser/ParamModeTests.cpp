// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: `view` and `inout` parameters.  The keywords may mark the
// type of a parameter of a function, a method or a constructor (`n: inout
// int`), and nothing else.

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
    fn scale(x: inout float, by: view float, n: int) { }
    class Counter {
      n: int;
      fn __init__(start: view int) { self.n = start; }
      fn add(to: inout int, k: view int) -> int { return k; }
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

// `view` and `inout` are reserved, and only mark a parameter's type.  There
// is no call-site marking.
TEST(ParamMode, KeywordsOnlyMarkAParameterType) {
  const char *const cases[] = {
      "fn main() -> int { inout x = 1; return 0; }",
      "fn main() -> int { view: int = 1; return 0; }",
      "fn main() -> int { x: inout int = 1; return 0; }",
      "fn bump(n: inout int) { }\nfn main() -> int { bump(inout k); }",
      "fn f(x: int inout) { }\nfn main() -> int { return 0; }",
      "fn f(x: inout view int) { }\nfn main() -> int { return 0; }",
      "fn f() -> inout int { return 0; }\nfn main() -> int { return 0; }",
      "fn inout() { }\nfn main() -> int { return 0; }",
      "fn f(view: int) { }\nfn main() -> int { return 0; }",
      "class C { inout n: int; }\nfn main() -> int { return 0; }",
      "class C { n: inout int; }\nfn main() -> int { return 0; }",
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
          "marks a parameter's type: 'fn f(x: inout int)')"),
      std::string::npos);
  EXPECT_NE(parseErrors(cases[2]).find("error: unexpected 'inout'; expected a "
                                       "type ('inout' only marks a "
                                       "parameter's type"),
            std::string::npos);
  EXPECT_NE(parseErrors(cases[9]).find("unexpected 'inout'; expected a field "
                                       "('name: Type;') or a method ('fn') "
                                       "in the class body ('inout' only"),
            std::string::npos);
}

// The mode goes after the colon.  Written before the name, the error shows
// the parameter rewritten.
TEST(ParamMode, ModeBeforeTheNameShowsTheFix) {
  const char *const cases[][2] = {
      {"fn f(inout x: int) { }", "write 'x: inout int'"},
      {"fn f(n: int, view p: Point) { }", "write 'p: view Point'"},
      {"fn f(view xs: int[]) { }", "write 'xs: view int[]'"},
      {"class C { fn __init__(inout s: int) { } }", "write 's: inout int'"},
      {"fn f(inout) { }", "write 'x: inout int'"},
  };
  for (const auto &c : cases) {
    std::string src = std::string("class Point { x: int; }\n") + c[0] +
                      "\nfn main() -> int { return 0; }";
    auto [ok, _] = parse(src);
    EXPECT_FALSE(ok) << src;
    if (testFrontend() != "recursive-descent")
      continue;
    std::string errs = parseErrors(src);
    EXPECT_NE(errs.find("goes after the colon, before the type: " +
                        std::string(c[1])),
              std::string::npos)
        << src << "\n"
        << errs;
  }
}

// `view fn` marks a method that does not change `self`.  A free `view fn`
// parses (Sema rejects it); `view` before anything but `fn` at the top level
// is a syntax error.
TEST(ViewFn, MarksAMethod) {
  auto [ok, driver] = parse(R"(
    class C {
      n: int;
      view fn get() -> int { return self.n; }
      fn set(v: int) { self.n = v; }
    }
    view fn free() { }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(ok);
  const auto &methods = driver->getRoot()->getClassDecls()[0]->getMethods();
  EXPECT_TRUE(methods[0]->isView());
  EXPECT_FALSE(methods[1]->isView());
  EXPECT_TRUE(driver->getRoot()->getFuncDecls()[0]->isView());
  std::string ast = dumpAST(*driver);
  EXPECT_NE(ast.find("FuncDecl <4:7-4:46> view 'get' ->\n"), std::string::npos)
      << ast;

  for (const char *src : {"view class C { }\nfn main() -> int { return 0; }",
                          "class C { view n: int; }\nfn main() -> int { "
                          "return 0; }"}) {
    auto [bad, _] = parse(src);
    EXPECT_FALSE(bad) << src;
  }
  if (testFrontend() != "recursive-descent")
    return;
  EXPECT_NE(parseErrors("fn main() -> int { view x = 1; return 0; }")
                .find("('view' only marks a parameter's type, 'fn f(x: view "
                      "int)', or a method that does not change 'self', 'view "
                      "fn len()')"),
            std::string::npos);
}
