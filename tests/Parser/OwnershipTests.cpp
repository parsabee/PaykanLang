// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: the ownership prototype's syntax (frontend::Options::Ownership,
// docs/design/ownership-proto.md) -- `view` / `inout` parameters and `let` --
// and that without the option the words stay names.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan;
using namespace paykan::test;
using ast::Qualifier;

namespace {

/// The VarDecl of the @p i-th statement of @p fn's body.
ast::VarDecl *local(ast::FuncDecl *fn, size_t i) {
  auto *ds = ast::dyn_cast<ast::DeclStmt>(fn->getBody()->getStatements()[i]);
  return ds ? ast::dyn_cast<ast::VarDecl>(ds->getDecl()) : nullptr;
}

/// The diagnostics of parsing @p source with the ownership syntax.
std::string parseErrors(const std::string &source) {
  auto path = writeTempFile(source);
  frontend::Options opts;
  opts.Ownership = true;
  parser::ParserDriver driver("recursive-descent", opts);
  std::ostringstream out;
  sema::DiagEngine diag(out);
  driver.setDiagEngine(&diag);
  driver.parseFile(path);
  std::filesystem::remove(path);
  return out.str();
}

} // namespace

TEST(Ownership, ParameterQualifiers) {
  auto [ok, driver] = parseOwnership(R"(
class C {
  n: int;
  fn add(inout to: int, view k: int, s: Str) { }
}
fn f(view a: int, inout b: float, c: int) { }
fn main() -> int { return 0; })");
  ASSERT_TRUE(ok);
  const auto &m = driver->getRoot()->getClassDecls()[0]->getMethods()[0];
  const auto &f = driver->getRoot()->getFuncDecls()[0];
  const Qualifier want[] = {Qualifier::Inout, Qualifier::View, Qualifier::None};
  for (size_t i = 0; i < 3; ++i)
    EXPECT_EQ(m->getParams()[i].Qual, want[i]) << i;
  EXPECT_EQ(f->getParams()[0].Qual, Qualifier::View);
  EXPECT_EQ(f->getParams()[1].Qual, Qualifier::Inout);
  EXPECT_EQ(f->getParams()[2].Qual, Qualifier::None);
  std::string dump = dumpAST(*driver);
  EXPECT_NE(dump.find("'add' 'to' inout 'k' view"), std::string::npos) << dump;
}

TEST(Ownership, Let) {
  auto [ok, driver] = parseOwnership(R"(
fn main() -> int {
  let a = 1;
  let b: int = 2;
  c: int = 3;
  return a + b + c;
})");
  ASSERT_TRUE(ok);
  auto *fn = driver->getRoot()->getFuncDecls()[0];
  const std::pair<bool, bool> want[] = {
      {true, false}, {true, true}, {false, true}}; // let, has a type
  for (size_t i = 0; i < std::size(want); ++i) {
    auto *vd = local(fn, i);
    ASSERT_NE(vd, nullptr) << i;
    EXPECT_EQ(vd->isLet(), want[i].first) << i;
    EXPECT_EQ(vd->getType() != nullptr, want[i].second) << i;
  }
  EXPECT_NE(dumpAST(*driver).find("'b' let type"), std::string::npos);
}

TEST(Ownership, QualifiersOnlyOnParameters) {
  const char *bad[] = {
      "fn main() -> int { view x: int = 1; return 0; }",
      "class C { inout n: int; }",
      "fn f() -> view int { return 1; }",
  };
  for (const char *src : bad) {
    EXPECT_FALSE(parseOwnership(src).Ok) << src;
    EXPECT_NE(
        parseErrors(src).find("'view' and 'inout' apply only to parameters"),
        std::string::npos)
        << src << "\n"
        << parseErrors(src);
  }
  const char *malformed[] = {
      "fn f(x: inout int) { }", // the qualifier goes before the name
      "fn f(inout) { }",
      "fn f(view inout x: int) { }",
      "fn main() -> int { let x: int; return 0; }", // let needs a value
      "fn main() -> int { let x; return 0; }",
      "fn main() -> int { let a, b = (1, 2); return 0; }",
  };
  for (const char *src : malformed)
    EXPECT_FALSE(parseOwnership(src).Ok) << src;
}

TEST(Ownership, WordsAreNamesWithoutTheOption) {
  const std::string src = R"(
fn inout(view: int) -> int { return view; }
fn main() -> int {
  let = 1;
  view: int = 2;
  return inout(let + view);
})";
  EXPECT_TRUE(parse(src).Ok);
  EXPECT_TRUE(semaCheck(src).Ok);
  EXPECT_FALSE(parseOwnership(src).Ok);
  // The retired words of the first model are names with the option too.
  EXPECT_TRUE(parseOwnership("fn main() -> int { own = 1; mut = own; cp = "
                             "mut; mv = cp; return mv; }")
                  .Ok);
}
