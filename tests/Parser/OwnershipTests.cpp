// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: the ownership prototype's syntax (frontend::Options::Ownership,
// docs/design/ownership-proto.md) -- qualifiers, `let`, `cp` / `mv` and an
// explicit `self` -- and that without the option the words stay names.

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

} // namespace

TEST(Ownership, Locals) {
  auto [ok, driver] = parseOwnership(R"(
class Foo { }
fn main() -> int {
  a: own Foo = Foo();
  b: mut = a;
  let c = 1;
  let d: own int = 2;
  e: own = cp a;
  f = mv b;
  g: Foo = a;
  return -cp c;
})");
  ASSERT_TRUE(ok);
  auto *fn = driver->getRoot()->getFuncDecls()[0];
  const std::tuple<Qualifier, bool, bool> want[] = {
      // qualifier, let, has a type
      {Qualifier::Own, false, true},  {Qualifier::Mut, false, false},
      {Qualifier::View, true, false}, {Qualifier::Own, true, true},
      {Qualifier::Own, false, false}, {Qualifier::View, false, true}};
  const size_t declIndex[] = {0, 1, 2, 3, 4, 6};
  for (size_t k = 0; k < std::size(want); ++k) {
    auto *vd = local(fn, declIndex[k]);
    ASSERT_NE(vd, nullptr) << k;
    EXPECT_EQ(vd->getQualifier(), std::get<0>(want[k])) << k;
    EXPECT_EQ(vd->isLet(), std::get<1>(want[k])) << k;
    EXPECT_EQ(vd->getType() != nullptr, std::get<2>(want[k])) << k;
  }
  EXPECT_TRUE(ast::isa<ast::CopyExpr>(local(fn, 4)->getInitExpr()));
  auto *f = ast::cast<ast::AssignStmt>(fn->getBody()->getStatements()[5]);
  EXPECT_TRUE(ast::isa<ast::MoveExpr>(f->getValue()));

  std::string dump = dumpAST(*driver);
  EXPECT_NE(dump.find("'d' let own type"), std::string::npos) << dump;
  EXPECT_NE(dump.find("CopyExpr"), std::string::npos) << dump;
  EXPECT_NE(dump.find("MoveExpr"), std::string::npos) << dump;
}

TEST(Ownership, FieldsParametersResultsAndSelf) {
  auto [ok, driver] = parseOwnership(R"(
class P {
  n: own Str;
  l: mut P?;
  v: Str;
  fn __init__(self: mut, n: own Str) { self.n = mv n; }
  fn get(self) -> own Str { return cp self.n; }
  fn set(self: mut Self, s: Str) { }
  fn bad(self: own) { }
  fn plain(x: int) -> mut Str { return self.n; }
}
fn f(p: mut P, q: P) -> P { return p; })");
  ASSERT_TRUE(ok);
  auto *cd = driver->getRoot()->getClassDecls()[0];
  EXPECT_EQ(cd->getFields()[0]->getQualifier(), Qualifier::Own);
  EXPECT_EQ(cd->getFields()[1]->getQualifier(), Qualifier::Mut);
  EXPECT_EQ(cd->getFields()[2]->getQualifier(), Qualifier::View);
  const auto &m = cd->getMethods();
  const std::pair<bool, Qualifier> self[] = {{true, Qualifier::Mut},
                                             {true, Qualifier::View},
                                             {true, Qualifier::Mut},
                                             {true, Qualifier::Own},
                                             {false, Qualifier::View}};
  for (size_t i = 0; i < m.size(); ++i) {
    EXPECT_EQ(m[i]->hasExplicitSelf(), self[i].first) << i;
    EXPECT_EQ(m[i]->getSelfQualifier(), self[i].second) << i;
  }
  ASSERT_EQ(m[0]->getParams().size(), 1u); // self is not a parameter
  EXPECT_EQ(m[0]->getParams()[0].Qual, Qualifier::Own);
  EXPECT_EQ(m[1]->getResultQualifier(), Qualifier::Own);
  EXPECT_EQ(m[4]->getResultQualifier(), Qualifier::Mut);
  auto *fn = driver->getRoot()->getFuncDecls()[0];
  EXPECT_EQ(fn->getParams()[0].Qual, Qualifier::Mut);
  EXPECT_EQ(fn->getParams()[1].Qual, Qualifier::View);
  EXPECT_EQ(fn->getResultQualifier(), Qualifier::View);
}

TEST(Ownership, MalformedForms) {
  const char *bad[] = {
      "fn f(x: own) { }",                           // a parameter needs a type
      "class C { x: own; }",                        // so does a field
      "fn f() -> own { }",                          // and a result
      "fn main() -> int { let x: int; return 0; }", // let needs a value
      "fn main() -> int { a: own, b = (1, 2); return 0; }",
      "class C { fn m(self: Str) { } }",
      "fn f(self) { }", // explicit self only in a method
  };
  for (const char *src : bad)
    EXPECT_FALSE(parseOwnership(src).Ok) << src;
}

TEST(Ownership, WordsAreNamesWithoutTheOption) {
  const std::string src = R"(
fn mv(own: int) -> int { return own; }
fn main() -> int {
  cp = 1;
  mut: int = 2;
  let = mv(cp);
  return let + mut;
})";
  EXPECT_TRUE(parse(src).Ok);
  EXPECT_TRUE(semaCheck(src).Ok);
  EXPECT_FALSE(parseOwnership(src).Ok);
}
