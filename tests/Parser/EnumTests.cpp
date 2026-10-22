// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: enum declarations (docs/language/07-enums.md), the variants
// with fields of a tagged union, and the forms the grammar rejects.

#include "ASTPrinter.h"
#include "TestUtils.h"
#include <gtest/gtest.h>
#include <sstream>

using namespace paykan::test;
using namespace paykan::ast;

// Parse and return the printed AST (empty string on parse failure).
static std::string dumpAST(const std::string &src) {
  auto [ok, driver] = parse(src);
  if (!ok)
    return "";
  std::ostringstream os;
  ASTPrinter printer(os);
  printer.visit(driver->getRoot());
  return os.str();
}

static std::string withMain(const std::string &decls) {
  return decls + "\nfn main() -> int { return 0; }\n";
}

TEST(EnumParse, VariantsWithFields) {
  auto [ok, driver] = parse(withMain("enum MyType {\n"
                                     "  NoElement,\n"
                                     "  WithPrimitive(i: int, f: float),\n"
                                     "  WithObject(obj: Obj, str: Str),\n"
                                     "  WithStruct(buffer: mem::Buffer<int>)\n"
                                     "}"));
  ASSERT_TRUE(ok);
  auto *ed = driver->getRoot()->getEnumDecls()[0];
  ASSERT_EQ(ed->getNumVariants(), 4u);
  EXPECT_TRUE(ed->hasPayloads());
  EXPECT_TRUE(ed->getPayload(0).empty());
  ASSERT_EQ(ed->getPayload(1).size(), 2u);
  EXPECT_EQ(ed->getPayload(1)[0]->getName(), "i");
  EXPECT_EQ(ed->getPayload(1)[1]->getName(), "f");
  ASSERT_EQ(ed->getPayload(3).size(), 1u);
  EXPECT_TRUE(isa<GenericType>(ed->getPayload(3)[0]->getType()));
}

TEST(EnumParse, PrintedWithItsFields) {
  auto dump = dumpAST(withMain("enum E {\n  A,\n  B(x: int, s: Str)\n}"));
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("EnumDecl <1:1-4:2> 'E' {A, B(x, s)}"), std::string::npos)
      << dump;
  EXPECT_NE(dump.find("VarDecl <3:5-3:11> 'x' type"), std::string::npos)
      << dump;
}

TEST(EnumParse, PlainEnumHasNoPayloads) {
  auto [ok, driver] = parse(withMain("enum Dir { North, South, }"));
  ASSERT_TRUE(ok);
  EXPECT_FALSE(driver->getRoot()->getEnumDecls()[0]->hasPayloads());
}

TEST(EnumParse, VariantCallAndBindingArms) {
  auto dump = dumpAST("enum E { A, B(x: int) }\n"
                      "fn main() -> int {\n"
                      "  e = E::B(1);\n"
                      "  match e { A {} b: B {} _ {} }\n"
                      "  match e { b: inout B {} _ {} }\n"
                      "  return 0;\n"
                      "}\n");
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("CallExpr <3:7-3:14> 'E::B'"), std::string::npos) << dump;
  EXPECT_NE(dump.find("MatchArm binding='b'\n"), std::string::npos) << dump;
  EXPECT_NE(dump.find("MatchArm binding='b' inout"), std::string::npos) << dump;
}

TEST(EnumParse, MalformedVariantsRejected) {
  // A variant without fields has no parentheses.
  auto empty = parse(withMain("enum E { A() }"));
  EXPECT_FALSE(empty.Ok);
  // A field is a name and a type: no mode, no bare type, no default.
  EXPECT_FALSE(parse(withMain("enum E { A(x: view int) }")).Ok);
  EXPECT_FALSE(parse(withMain("enum E { A(int) }")).Ok);
  EXPECT_FALSE(parse(withMain("enum E { A(x: int = 1) }")).Ok);
  EXPECT_FALSE(parse(withMain("enum E { A(x: int,) }")).Ok);
  EXPECT_FALSE(parse(withMain("enum E { A(x: int }")).Ok);
  EXPECT_TRUE(parse(withMain("enum E { A(x: int), B, }")).Ok);
}
