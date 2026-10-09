// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: `view` and `inout` parameters and locals.  The keywords may
// mark the type of a parameter of a function, a method or a constructor
// (`n: inout int`) or of a local borrow (`x: view = e;`), and nothing else.

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

// `view` and `inout` are reserved, and only mark the type of a parameter or
// a local.  There is no call-site marking.
TEST(ParamMode, KeywordsOnlyMarkAParameterType) {
  const char *const cases[] = {
      "fn main() -> int { return inout; }",
      "fn main() -> int { view: int = 1; return 0; }",
      "fn main() -> int { x: int = inout 1; return 0; }",
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
  EXPECT_NE(parseErrors(cases[0]).find(
                "error: unexpected 'inout'; expected an expression ('inout' "
                "only marks the type of a parameter or a local: 'fn f(x: "
                "inout int)', 'y: inout = x;')"),
            std::string::npos);
  EXPECT_NE(parseErrors(cases[10]).find("error: unexpected 'inout'; expected "
                                        "a type ('inout' only marks the type "
                                        "of a parameter or a local"),
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

// -- Local borrows

// `x: view = e;` and `x: inout = p;`, with the type left off or written.
TEST(LocalBorrow, Declarations) {
  auto [ok, driver] = parse(R"(fn main() -> int {
  k = 1;
  v: view = k + 1;
  w: view int = k;
  i: inout = k;
  j: inout int = k;
  return 0;
})");
  ASSERT_TRUE(ok);
  std::string ast = dumpAST(*driver);
  for (const char *decl :
       {"VarDecl <3:3-3:18> 'v' view\n", "VarDecl <4:3-4:18> 'w' view type\n",
        "VarDecl <5:3-5:15> 'i' inout\n",
        "VarDecl <6:3-6:19> 'j' inout type\n"})
    EXPECT_NE(ast.find(decl), std::string::npos) << decl << "\n" << ast;
}

// A local borrow needs an initial value, is one variable, and is not `let`;
// written before the name, the mode's error shows the fix.
TEST(LocalBorrow, Errors) {
  const char *const cases[][2] = {
      {"x: view;", "(a 'view' local needs an initial value)"},
      {"x: inout int;", "(a 'inout' local needs an initial value)"},
      {"a: view int, b = (1, 2);", "a destructuring target cannot be 'view'"},
      {"let x: view = 1;", "a local borrow cannot be 'let': a 'view' local "
                           "always names what it was given"},
      {"view x = 1;", "'view' goes after the colon, before the type: write "
                      "'x: view = ...'"},
      {"inout x: float = y;", "'inout' goes after the colon, before the "
                              "type: write 'x: inout float = ...'"},
  };
  for (const auto &c : cases) {
    std::string src = std::string("fn main() -> int {\n  y = 1.0;\n  ") + c[0] +
                      "\n  return 0;\n}";
    auto [ok, _] = parse(src);
    EXPECT_FALSE(ok) << src;
    if (testFrontend() != "recursive-descent")
      continue;
    std::string errs = parseErrors(src);
    EXPECT_NE(errs.find(c[1]), std::string::npos) << src << "\n" << errs;
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
  EXPECT_NE(parseErrors("fn main() -> int { return view; }")
                .find("('view' only marks the type of a parameter or a local: "
                      "'fn f(x: view int)', 'y: view = x;'; or a method that "
                      "does not change 'self': 'view fn len()')"),
            std::string::npos);
}

// A `match` arm's binding may borrow the subject, `n: view T` or
// `n: inout T`; written before the name, the mode's error shows the fix.
TEST(MatchArm, Modes) {
  auto [ok, driver] = parse(R"(fn main() -> int {
  match a {
    d: view Dog { }
    c: inout Cat { }
    b: Bird { }
    _ { }
  }
  return 0;
})");
  ASSERT_TRUE(ok);
  namespace ast = paykan::ast;
  const auto *match = ast::cast<ast::MatchStmt>(
      driver->getRoot()->getFuncDecls()[0]->getBody()->getStatements()[0]);
  const auto &arms = match->getArms();
  EXPECT_EQ(arms[0]->getMode(), ast::ParamMode::View);
  EXPECT_EQ(arms[1]->getMode(), ast::ParamMode::Inout);
  EXPECT_EQ(arms[2]->getMode(), ast::ParamMode::Value);
  std::string dump = dumpAST(*driver);
  for (const char *arm :
       {"MatchArm binding='d' view\n", "MatchArm binding='c' inout\n",
        "MatchArm binding='b'\n"})
    EXPECT_NE(dump.find(arm), std::string::npos) << arm << "\n" << dump;

  if (testFrontend() != "recursive-descent")
    return;
  const char *const cases[][2] = {
      {"view d: Dog { }", "'view' goes after the colon, before the type: "
                          "write 'd: view Dog'"},
      {"inout d: Dog[] { }", "'inout' goes after the colon, before the type: "
                             "write 'd: inout Dog[]'"},
  };
  for (const auto &c : cases) {
    std::string src = std::string("fn main() -> int {\n  match a {\n    ") +
                      c[0] + "\n  }\n  return 0;\n}";
    std::string errs = parseErrors(src);
    EXPECT_NE(errs.find(c[1]), std::string::npos) << src << "\n" << errs;
  }
}
