// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The AST interchange format (paykan/ast/Interchange.h,
// docs/plugins/ast-format.md):
//   - the round trip over the whole samples corpus: the recursive-descent
//     frontend's AST, written and read back, is the same AST (its --dump-ast
//     text, and its interchange text, are identical);
//   - the reader rejects every malformed input with a positioned message.

#include <gtest/gtest.h>

#include <unistd.h>

#include "ASTPrinter.h"
#include "ParserDriver.h"
#include "paykan/ast/Interchange.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef PAYKAN_SAMPLES_DIR
#error "PAYKAN_SAMPLES_DIR must be defined via CMake compile definition"
#endif

namespace {

using namespace paykan;
namespace ic = paykan::ast::interchange;

std::string dump(ast::TranslationUnit *tu) {
  std::ostringstream os;
  ast::ASTPrinter printer(os);
  printer.visit(tu);
  return os.str();
}

// The format version is the release's, so the texts below spell it `1`: it
// is the real version (kVersion) where read() and write() see the text, and
// `1` again in what they return.  WritesTheDocumentedForm checks the real
// header.
const std::string kVersion = versionString(ic::kFormatVersion);
const std::string kSpelled = "(paykan-ast 1";
const std::string kReal = "(paykan-ast " + kVersion;

std::string replaceAll(std::string s, const std::string &from,
                       const std::string &to) {
  for (size_t at = s.find(from); at != std::string::npos;
       at = s.find(from, at + to.size()))
    s.replace(at, from.size(), to);
  return s;
}

std::string writeReal(const ast::TranslationUnit &tu) {
  std::ostringstream os;
  std::string error;
  EXPECT_TRUE(ic::write(tu, os, error)) << error;
  return os.str();
}

std::string write(const ast::TranslationUnit &tu) {
  return replaceAll(writeReal(tu), kReal, kSpelled);
}

/// A position "1:<column>:" past the version on the first line, in the
/// spelled text's columns.
std::string spelledColumns(const std::string &error) {
  const size_t shift = kVersion.size() - 1;
  const size_t afterVersion = kSpelled.size() + 1; // column after `1`
  if (!error.starts_with("1:"))
    return error;
  size_t end = error.find(':', 2);
  if (end == std::string::npos)
    return error;
  size_t column = std::stoul(error.substr(2, end - 2));
  if (column > afterVersion)
    column -= shift;
  return "1:" + std::to_string(column) + error.substr(end);
}

/// read() of @p text: the unit, or null with @p error.
ast::TranslationUnit *read(const std::string &text, ast::ASTContext &ctx,
                           std::string &error) {
  ic::ReadError err;
  std::string real = text;
  if (real.starts_with(kSpelled + " ") || real.starts_with(kSpelled + "\n"))
    real.replace(0, kSpelled.size(), kReal);
  ast::TranslationUnit *tu = ic::read(real, ctx, err);
  error = real == text ? err.str() : spelledColumns(err.str());
  error = replaceAll(error, "version " + kVersion, "version 1");
  return tu;
}

std::vector<std::filesystem::path> corpus() {
  std::vector<std::filesystem::path> files;
  for (const auto &e :
       std::filesystem::recursive_directory_iterator(PAYKAN_SAMPLES_DIR))
    if (e.path().extension() == ".pkn")
      files.push_back(e.path());
  std::sort(files.begin(), files.end());
  return files;
}

TEST(ASTInterchange, RoundTripsTheSamplesCorpus) {
  unsigned roundTripped = 0;
  for (const auto &file : corpus()) {
    parser::ParserDriver driver("recursive-descent");
    std::ostringstream quiet;
    sema::DiagEngine diag(quiet);
    driver.setDiagEngine(&diag);
    if (driver.parseFile(file.string()) != 0)
      continue; // a syntax-error sample has no AST to carry
    std::string text = write(*driver.getRoot());
    ast::ASTContext ctx;
    std::string error;
    ast::TranslationUnit *back = read(text, ctx, error);
    ASSERT_NE(back, nullptr) << file << ": " << error << "\n" << text;
    EXPECT_EQ(dump(back), dump(driver.getRoot())) << file;
    EXPECT_EQ(write(*back), text) << file;
    ++roundTripped;
  }
  EXPECT_GT(roundTripped, 80u);
}

TEST(ASTInterchange, WritesTheDocumentedForm) {
  parser::ParserDriver driver("recursive-descent");
  auto path = std::filesystem::temp_directory_path() /
              ("paykan_interchange_" + std::to_string(::getpid()) + ".pkn");
  {
    std::ofstream(path) << "fn main() -> int {\n  return 0;\n}\n";
  }
  ASSERT_EQ(driver.parseFile(path.string()), 0);
  std::filesystem::remove(path);
  EXPECT_EQ(writeReal(*driver.getRoot()),
            kReal + "\n"
                    "  (unit @1:1-3:2\n"
                    "    (fn @1:1-3:2 \"main\"\n"
                    "      (type-params)\n"
                    "      (params)\n"
                    "      (named-type \"int\")\n"
                    "      (block @1:19-1:19\n"
                    "        (return @2:3-2:12\n"
                    "          (int @2:10-2:11 0))))))\n");
}

// Everything the reader builds: names resolve like the recursive-descent
// frontend's types, strings and numbers decode, comments are skipped.
TEST(ASTInterchange, ReadsEveryNode) {
  const std::string text = R"((paykan-ast 1 ; a comment
  (unit @1:1-9:9
    (import @1:1-1:9 false "a::b" (module "m" "") (module "n" "alias"))
    (import true "" (module "io" ""))
    (enum @2:1-2:9 "Color" "Red" "Green")
    (class @3:1-3:9 "Box" "" (type-params "T")
      (fields (var "item" (named-type "T") _))
      (methods (fn "get" (type-params) (params) (named-type "T")
        (block (return (member (ident "self") "item"))))))
    (class "Point" "Obj" (type-params) (fields) (methods))
    (fn @4:1-9:9 "main" (type-params) (params (param "args" (array-type (named-type "Str")) (qual view)))
      (named-type "int")
      (block
        (decl (var "x" (optional-type (named-type "int")) (none)))
        (decl (var "t" (tuple-type (named-type "int") (named-type "m::T")) (tuple (int 1) (int -2))))
        (decl (var "g" (generic-type "Box" (named-type "float")) _))
        (assign (ident "y") (float 1.5))
        (expr (call "f" (type-args (named-type "bool")) (float inf) (float -inf) (float nan) (float 2e3)))
        (if (bool true) (block (break)) (if (bool false) (block (continue)) (block)))
        (while (unary not (bool false)) (block))
        (member-assign (ident "p") "x" (char 97))
        (subscript-assign (ident "a") (int 0) (string "q\"\\\n\t\r\x41"))
        (match (ident "v")
          (type-arm "b" (named-type "Point") (block))
          (value-arm "" (int 3) (block))
          (wildcard-arm "" (block)))
        (destructure (targets (target @5:1-5:2 "a" _) (target "" (named-type "int"))) (ident "t"))
        (expr (method-call (ident "s") "len"))
        (expr (ternary (ident "c") (array) (array (int 1))))
        (expr (subscript (ident "a") (binary add (int 1) (int 2))))
        (expr (enum-value "Color" "Red"))
        (expr (tuple-index (ident "t") 1))
        (expr (array-repeat (array (int 1) (int 2)) 3))
        (return _)))))
)";
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *tu = read(text, ctx, error);
  ASSERT_NE(tu, nullptr) << error;
  EXPECT_EQ(tu->getImports().size(), 2u);
  EXPECT_EQ(tu->getEnumDecls().size(), 1u);
  EXPECT_EQ(tu->getGenericClassDecls().size(), 1u);
  EXPECT_EQ(tu->getClassDecls().size(), 1u);
  ASSERT_EQ(tu->getFuncDecls().size(), 1u);
  EXPECT_TRUE(tu->getImports()[1]->isSystem());
  EXPECT_EQ(*tu->getImports()[0]->getModules()[1].Alias, "alias");
  // Builtins and bootstrap classes are the canonical types; other names are
  // stubs for Sema, a qualified one always.
  auto *main = tu->getFuncDecls()[0];
  EXPECT_EQ(main->getReturnType(), ctx.getIntTy());
  auto *argTy = ast::cast<ast::ArrayType>(main->getParams()[0].ParamType);
  EXPECT_EQ(argTy->getElementType(), ctx.getStrTy());
  EXPECT_EQ(main->getParams()[0].Mode, ast::ParamMode::View);
  auto &stmts = main->getBody()->getStatements();
  auto *tdecl =
      ast::cast<ast::VarDecl>(ast::cast<ast::DeclStmt>(stmts[1])->getDecl());
  auto *tt = ast::cast<ast::TupleType>(tdecl->getType());
  EXPECT_NE(tt->getElementType(1), ctx.lookupType("T"));
  EXPECT_EQ(ast::cast<ast::ClassType>(tt->getElementType(1))->getName(),
            "m::T");
  auto *sa = ast::cast<ast::SubscriptAssignStmt>(stmts[8]);
  EXPECT_EQ(ast::cast<ast::StringLiteral>(sa->getValue())->getValue(),
            "q\"\\\n\t\rA");
  auto *ds = ast::cast<ast::DestructureStmt>(stmts[10]);
  EXPECT_EQ(ds->getTargets()[0].Loc.getLineStart(), 5u);
  EXPECT_TRUE(ds->getTargets()[1].isSkip());
  auto *rep = ast::cast<ast::ArrayLiteralExpr>(
      ast::cast<ast::ExprStmt>(stmts[stmts.size() - 2])->getExpr());
  EXPECT_TRUE(rep->isRepeat());
  EXPECT_EQ(rep->getRepeatCount(), 3);
  EXPECT_EQ(
      ast::cast<ast::ArrayLiteralExpr>(rep->getElements()[0])->getLength(), 2);
  // And it writes back to the same AST.
  std::string again = write(*tu);
  ast::ASTContext ctx2;
  ast::TranslationUnit *tu2 = read(again, ctx2, error);
  ASSERT_NE(tu2, nullptr) << error << "\n" << again;
  EXPECT_EQ(write(*tu2), again);
  EXPECT_EQ(dump(tu2), dump(tu));
}

// Floats read back exactly as written (the writer's %.17g), subnormals and
// the extremes included.
TEST(ASTInterchange, ReadsFloatsExactly) {
  for (double v : {0.1, -2.5, 0.0, -0.0, 5e-324, 2.2250738585072014e-308,
                   1.7976931348623157e308, -1.7976931348623157e308}) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    std::string text = std::string("(paykan-ast 1 (unit (fn \"f\" "
                                   "(type-params) (params) _ (block "
                                   "(expr (float ") +
                       buf + "))))))";
    ast::ASTContext ctx;
    std::string error;
    ast::TranslationUnit *tu = read(text, ctx, error);
    ASSERT_NE(tu, nullptr) << buf << ": " << error;
    auto &stmts = tu->getFuncDecls()[0]->getBody()->getStatements();
    auto *lit = ast::cast<ast::FloatLiteral>(
        ast::cast<ast::ExprStmt>(stmts[0])->getExpr());
    double got = lit->getValue();
    EXPECT_EQ(got, v) << buf;
    EXPECT_EQ(std::signbit(got), std::signbit(v)) << buf;
  }
}

// Every malformed input is an error with its position.
TEST(ASTInterchange, RejectsMalformedInput) {
  struct Case {
    const char *Text;
    const char *Error;
  };
  const Case cases[] = {
      {"", "1:1: the text is empty: expected (paykan-ast ...)"},
      {"x", "1:1: expected (paykan-ast ...)"},
      {"(paykan-ast 1 (unit)) x", "1:23: unexpected text after the "
                                  "(paykan-ast ...) form"},
      {"( paykan-ast)", "1:1: expected (paykan-ast <version> (unit ...))"},
      {"(\"x\")", "1:2: expected a tag after '('"},
      {"(1)", "1:3: expected a tag after '(', not '1'"},
      {"(paykan-ast 1 (unit)", "1:21: unterminated list (paykan-ast ...) "
                               "from 1:1"},
      {"(other 1 (unit))", "1:1: expected (paykan-ast <version> (unit ...)), "
                           "not (other ...)"},
      {"(paykan-ast 2 (unit))", "1:13: unsupported AST format version 2 "
                                "(this paykan reads version 1)"},
      {"(paykan-ast 99999999999999999999 (unit))",
       "1:13: unsupported AST format version 99999999999999999999 (this "
       "paykan reads version 1)"},
      {"(paykan-ast 1 (block))", "1:15: expected (unit ...)"},
      {"(paykan-ast 1 (unit \"x\"))",
       "1:21: expected a declaration: (import ...), (enum ...), (class ...), "
       "(struct ...), (fn ...), (trait ...) or (impl ...)"},
      {"(paykan-ast 1 (unit (var \"x\" _ _)))",
       "1:21: (var ...) is not a top-level declaration (import, enum, class, "
       "struct, fn, trait, impl)"},
      {"(paykan-ast 1 (unit (import false \"\")))",
       "1:21: (import ...) names no module"},
      {"(paykan-ast 1 (unit (import maybe \"\")))",
       "1:29: expected true or false: whether the import is a system import "
       "(::name)"},
      {"(paykan-ast 1 (unit (import false \"\" (mod \"m\"))))",
       "1:38: expected (module \"name\" \"alias\")"},
      {"(paykan-ast 1 (unit (enum \"\")))",
       "1:27: empty name: the enum's name"},
      {"(paykan-ast 1 (unit (enum)))",
       "1:21: (enum ...) is missing the enum's name"},
      {"(paykan-ast 1 (unit (enum @1:1 \"E\")))",
       "1:27: malformed location '@1:1' (expected "
       "@line:column-line:column)"},
      {"(paykan-ast 1 (unit (class \"C\" \"\" (type-params) (fields (int 1)) "
       "(methods))))",
       "1:57: expected a field: (var ...)"},
      {"(paykan-ast 1 (unit (class \"C\" \"\" (type-params) (fields (var \"f\" "
       "_ _)) (methods))))",
       "1:57: a field has a type and no initialiser"},
      {"(paykan-ast 1 (unit (class \"C\" \"\" (type-params) (fields) "
       "(methods (var \"x\" _ _)))))",
       "1:67: expected a method: (fn ...)"},
      {"(paykan-ast 1 (unit (class \"C\" \"\" (type-params) (fields) "
       "(methods) 1)))",
       "1:68: unexpected field in (class ...)"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params (p)) _ "
       "(block))))",
       "1:51: expected (param \"name\" TYPE)"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (if))))",
       "1:54: expected (block ...): the function's body"},
      {"(paykan-ast 1 (unit (fn \"f\" (tparams) (params) _ (block))))",
       "1:29: expected (type-params ...)"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) x (block))))",
       "1:52: expected a list: the return type or _"},
      // Types.
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) (int) "
       "(block))))",
       "1:52: (int ...) is not a type (named-type, array-type, "
       "fixed-array-type, optional-type, tuple-type, fn-type, generic-type)"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) (optional-type "
       "(named-type \"void\")) (block))))",
       "1:52: optional type 'void?' is not supported"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) (optional-type "
       "(optional-type (named-type \"int\"))) (block))))",
       "1:52: nested optional type 'int?"
       "?' is not supported"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) (tuple-type "
       "(named-type \"int\")) (block))))",
       "1:52: a tuple type needs at least two element types"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) (generic-type "
       "\"Box\") (block))))",
       "1:52: a generic type needs a type argument"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) (array-type) "
       "(block))))",
       "1:52: (array-type ...) is missing the element type"},
      // Statements and expressions.
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(jump)))))",
       "1:61: (jump ...) is not a statement"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(if (bool true) (block) (while (bool true) (block)))))))",
       "1:85: the else branch is a (block ...) or an (if ...)"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(destructure (targets (target \"a\" _)) (ident \"t\"))))))",
       "1:74: a destructuring needs at least two targets"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(destructure (targets (t)) (ident \"t\"))))))",
       "1:83: expected (target \"name\" TYPE-or-_)"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(match (ident \"x\") (arm \"\" (block)))))))",
       "1:80: (arm ...) is not a match arm (type-arm, value-arm, "
       "wildcard-arm)"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(match (ident \"x\") 1)))))",
       "1:80: expected a match arm"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(assign (var \"x\" _ _) (int 1))))))",
       "1:69: expected (ident ...)"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (frob))))))",
       "1:67: (frob ...) is not an expression"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (unary minus (int 1)))))))",
       "1:74: unknown unary operator 'minus' (neg, not)"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (binary plus (int 1) (int 2)))))))",
       "1:75: unknown binary operator 'plus'"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (binary \"add\" (int 1) (int 2)))))))",
       "1:75: expected a symbol: the operator"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (char 256))))))",
       "1:67: a char is a byte value, 0 to 255"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (float 1.5x))))))",
       "1:74: not a floating-point number: 1.5x"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (float 1e999))))))",
       "1:74: not a floating-point number: 1e999"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (float -1e999))))))",
       "1:74: not a floating-point number: -1e999"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (float 1e-400))))))",
       "1:74: not a floating-point number: 1e-400"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (float 0x1p3))))))",
       "1:74: not a floating-point number: 0x1p3"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (float 1.5e))))))",
       "1:74: not a floating-point number: 1.5e"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (int \"1\"))))))",
       "1:72: expected an integer: the value"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (array-repeat (int 0) 0))))))",
       "1:67: an array has at least one element"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (array-repeat (int 0) -3))))))",
       "1:67: an array has at least one element"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (array-repeat (int 0)))))))",
       "1:67: (array-repeat ...) is missing the count"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (array-repeat (int 0) n))))))",
       "1:89: expected an integer: the count"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (array-repeat))))))",
       "1:67: (array-repeat ...) is missing the repeated value"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (tuple (int 1)))))))",
       "1:67: a tuple needs at least two elements"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (tuple-index (ident \"t\") -1))))))",
       "1:67: a tuple index is not negative"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (call \"f\" (type-args 1)))))))",
       "1:88: expected a type"},
      {"(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
       "(expr (ident \"x\" \"y\"))))))",
       "1:78: unexpected field in (ident ...)"},
      // Strings.
      {"(paykan-ast 1 (unit (enum \"E\" \"a\\q\")))",
       "1:34: unknown escape \\q in a string"},
      {"(paykan-ast 1 (unit (enum \"E\" \"a\\x4\")))",
       "1:34: \\x needs two hexadecimal digits"},
      {"(paykan-ast 1 (unit (enum \"E\" \"a\nb\")))",
       "1:33: a newline in a string (write it as \\n)"},
      {"(paykan-ast 1 (unit (enum \"E\" \"ab", "1:34: unterminated string"},
  };
  for (const Case &c : cases) {
    ast::ASTContext ctx;
    std::string error;
    EXPECT_EQ(read(c.Text, ctx, error), nullptr) << c.Text;
    EXPECT_EQ(error, c.Error) << c.Text;
  }
}

TEST(ASTInterchange, NestingIsBounded) {
  auto nested = [](unsigned depth) {
    // (paykan-ast 1 (unit (fn ... (block (expr <depth - 6 unary lists>)))))
    std::string text = "(paykan-ast 1 (unit (fn \"f\" (type-params) (params) "
                       "_ (block (expr ";
    unsigned unary = depth - 6;
    for (unsigned i = 0; i < unary; ++i)
      text += "(unary neg ";
    text += "(int 1)";
    text += std::string(unary, ')') + ")))))";
    return text;
  };
  ast::ASTContext ctx;
  std::string error;
  EXPECT_NE(read(nested(ic::kMaxDepth - 1), ctx, error), nullptr) << error;
  EXPECT_EQ(read(nested(ic::kMaxDepth + 10), ctx, error), nullptr);
  EXPECT_NE(error.find("nesting too deep (more than 2048 levels)"),
            std::string::npos)
      << error;
}

// A `view fn` ends with `(qual view)`; nothing else qualifies a function.
TEST(ASTInterchange, ViewFn) {
  parser::ParserDriver driver("recursive-descent");
  auto path =
      std::filesystem::temp_directory_path() /
      ("paykan_interchange_view_" + std::to_string(::getpid()) + ".pkn");
  {
    std::ofstream(path) << "class C { n: int;\n"
                           "  view fn get() -> int { return self.n; }\n"
                           "  fn set() { self.n = 1; } }\n";
  }
  ASSERT_EQ(driver.parseFile(path.string()), 0);
  std::filesystem::remove(path);
  std::string text = write(*driver.getRoot());
  EXPECT_EQ(text.find("(qual view)"), text.rfind("(qual view)")) << text;
  EXPECT_NE(text.find("(qual view)"), std::string::npos) << text;
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *back = read(text, ctx, error);
  ASSERT_NE(back, nullptr) << error;
  const auto &methods = back->getClassDecls()[0]->getMethods();
  EXPECT_TRUE(methods[0]->isView());
  EXPECT_FALSE(methods[1]->isView());
  EXPECT_EQ(dump(back), dump(driver.getRoot()));
  EXPECT_EQ(write(*back), text);

  for (const char *bad : {"(qual inout)", "(qual)", "(qual frob)"}) {
    std::string doc = "(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ "
                      "(block) " +
                      std::string(bad) + ")))";
    EXPECT_EQ(read(doc, ctx, error), nullptr) << bad;
    EXPECT_NE(error.find("expected (qual view)"), std::string::npos)
        << bad << ": " << error;
  }
}

// A borrowed result is a `(qual view)` / `(qual inout)` after the return
// type, which it needs.
TEST(ASTInterchange, BorrowedResults) {
  const std::string text =
      "(paykan-ast 1 (unit (fn \"f\" (type-params) (params (param \"a\" "
      "(named-type \"Str\") (qual view))) (named-type \"Str\") (qual view) "
      "(block))))";
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *tu = read(text, ctx, error);
  ASSERT_NE(tu, nullptr) << error;
  EXPECT_EQ(tu->getFuncDecls()[0]->getResultMode(), ast::ParamMode::View);
  std::string written = write(*tu);
  // The parameter's and the result's.
  EXPECT_NE(written.find("(qual view)"), written.rfind("(qual view)"))
      << written;
  ast::ASTContext ctx2;
  ast::TranslationUnit *back = read(written, ctx2, error);
  ASSERT_NE(back, nullptr) << error;
  EXPECT_EQ(dump(back), dump(tu));

  const std::string bad = "(paykan-ast 1 (unit (fn \"f\" (type-params) "
                          "(params) _ (qual view) (block))))";
  EXPECT_EQ(read(bad, ctx, error), nullptr);
  EXPECT_NE(error.find("a borrowed result needs a type"), std::string::npos)
      << error;
}

// A parameter's mode is the optional `(qual view)` / `(qual inout)` item.
TEST(ASTInterchange, ParamModes) {
  parser::ParserDriver driver("recursive-descent");
  auto path =
      std::filesystem::temp_directory_path() /
      ("paykan_interchange_modes_" + std::to_string(::getpid()) + ".pkn");
  {
    std::ofstream(path) << "fn bump(n: inout int, by: view int, k: int) { }\n"
                           "class C {}\n"
                           "impl C { fn __init__(n: view int) { } }\n";
  }
  ASSERT_EQ(driver.parseFile(path.string()), 0);
  std::filesystem::remove(path);
  std::string text = write(*driver.getRoot());
  std::string flat; // one line: every run of whitespace is one space
  for (char c : text)
    if (!std::isspace(static_cast<unsigned char>(c)))
      flat += c;
    else if (!flat.empty() && flat.back() != ' ')
      flat += ' ';
  for (const char *param : {"(param \"n\" (named-type \"int\") (qual inout))",
                            "(param \"by\" (named-type \"int\") (qual view))",
                            "(param \"k\" (named-type \"int\"))"})
    EXPECT_NE(flat.find(param), std::string::npos) << param << "\n" << text;
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *back = read(text, ctx, error);
  ASSERT_NE(back, nullptr) << error;
  EXPECT_EQ(dump(back), dump(driver.getRoot()));
  EXPECT_EQ(write(*back), text);

  for (const char *bad : {"(qual)", "(qual frob)", "(qual view inout)",
                          "(qual \"view\")", "(qual let)"}) {
    std::string doc = "(paykan-ast 1 (unit (fn \"f\" (type-params) (params "
                      "(param \"n\" (named-type \"int\") " +
                      std::string(bad) + ")) _ (block))))";
    EXPECT_EQ(read(doc, ctx, error), nullptr) << bad;
    EXPECT_NE(error.find("expected (qual view) or (qual inout)"),
              std::string::npos)
        << bad << ": " << error;
  }
}

// A `let` local is a `var` with a trailing `(let)`.
// `T[n]` is `(fixed-array-type TYPE INT)`, with a length of at least 1 or
// `_`, inferred.
TEST(ASTInterchange, FixedArrayTypes) {
  const std::string text =
      "(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block (decl "
      "(var \"a\" (fixed-array-type (fixed-array-type (named-type \"int\") "
      "3) 2) (array-repeat (array-repeat (int 0) 3) 2))) (decl (var \"b\" "
      "(fixed-array-type (named-type \"int\") _) (array (int 1))))))))";
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *tu = read(text, ctx, error);
  ASSERT_NE(tu, nullptr) << error;
  std::string d = dump(tu);
  EXPECT_NE(d.find("FixedArrayType 2\n"), std::string::npos) << d;
  EXPECT_NE(d.find("FixedArrayType 3\n"), std::string::npos) << d;
  EXPECT_NE(d.find("FixedArrayType _\n"), std::string::npos) << d;
  std::string written = write(*tu);
  EXPECT_NE(written.find("(fixed-array-type\n"), std::string::npos) << written;
  EXPECT_NE(written.find(" _)"), std::string::npos) << written;
  ast::ASTContext ctx2;
  ast::TranslationUnit *back = read(written, ctx2, error);
  ASSERT_NE(back, nullptr) << error;
  EXPECT_EQ(dump(back), d);

  struct Case {
    const char *Type;
    const char *Error;
  };
  for (const Case &c :
       {Case{"(fixed-array-type (named-type \"int\") 0)",
             "a fixed-size array has at least one element"},
        Case{"(fixed-array-type (named-type \"int\"))", "the length"},
        Case{"(fixed-array-type (named-type \"int\") n)",
             "expected an integer or _: the length"},
        Case{"(fixed-array-type 3)", "the element type"}}) {
    std::string doc = "(paykan-ast 1 (unit (fn \"f\" (type-params) (params "
                      "(param \"a\" " +
                      std::string(c.Type) + ")) _ (block))))";
    EXPECT_EQ(read(doc, ctx, error), nullptr) << c.Type;
    EXPECT_NE(error.find(c.Error), std::string::npos)
        << c.Type << ": " << error;
  }
}

// `[v; n]` is `(array-repeat EXPR INT)`, the value and its count (at least
// 1), and reads back as the same literal.
TEST(ASTInterchange, RepeatArrayLiterals) {
  parser::ParserDriver driver("recursive-descent");
  auto path =
      std::filesystem::temp_directory_path() /
      ("paykan_interchange_repeat_" + std::to_string(::getpid()) + ".pkn");
  {
    std::ofstream(path) << "fn main() -> int {\n"
                           "  xs = [0; 3];\n"
                           "  x = 1;\n"
                           "  g = [[x + 1; 2]; 16384];\n"
                           "  return 0;\n"
                           "}\n";
  }
  ASSERT_EQ(driver.parseFile(path.string()), 0);
  std::filesystem::remove(path);
  std::string text = write(*driver.getRoot());
  std::string flat; // one line: every run of whitespace is one space
  for (char c : text)
    if (!std::isspace(static_cast<unsigned char>(c)))
      flat += c;
    else if (!flat.empty() && flat.back() != ' ')
      flat += ' ';
  for (const char *e :
       {"(array-repeat @2:8-2:14 (int @2:9-2:10 0) 3)",
        "(array-repeat @4:7-4:26 (array-repeat @4:8-4:18 (binary @4:9-4:14 add "
        "(ident @4:9-4:10 \"x\") (int @4:13-4:14 1)) 2) 16384)"})
    EXPECT_NE(flat.find(e), std::string::npos) << e << "\n" << text;
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *back = read(text, ctx, error);
  ASSERT_NE(back, nullptr) << error;
  std::string d = dump(back);
  EXPECT_EQ(d, dump(driver.getRoot()));
  EXPECT_NE(d.find("ArrayLiteralExpr <2:8-2:14> 3 copies\n"), std::string::npos)
      << d;
  EXPECT_NE(d.find("16384 copies\n"), std::string::npos) << d;
  EXPECT_EQ(write(*back), text);
  // A list of one element stays a list.
  std::string one = "(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ "
                    "(block (expr (array (int 7)))))))";
  ast::TranslationUnit *list = read(one, ctx, error);
  ASSERT_NE(list, nullptr) << error;
  EXPECT_NE(dump(list).find("ArrayLiteralExpr 1 elements\n"), std::string::npos)
      << dump(list);
}

// A `weak` local or class field is a `var` with a trailing `(weak)`.
TEST(ASTInterchange, WeakDeclarations) {
  const std::string text =
      "(paykan-ast 1 (unit "
      "(class \"Node\" \"\" (type-params) (fields (var \"parent\" "
      "(optional-type (named-type \"Node\")) _ (weak))) (methods)) "
      "(fn \"f\" (type-params) (params) _ (block "
      "(decl (var \"w\" (optional-type (named-type \"Node\")) (ident \"x\") "
      "(weak))) (decl (var \"u\" _ (ident \"x\") (weak))) "
      "(decl (var \"e\" (optional-type (named-type \"Node\")) _ (weak)))))))";
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *tu = read(text, ctx, error);
  ASSERT_NE(tu, nullptr) << error;
  std::string d = dump(tu);
  EXPECT_NE(d.find("VarDecl 'parent' weak type\n"), std::string::npos) << d;
  EXPECT_NE(d.find("VarDecl 'w' weak type\n"), std::string::npos) << d;
  EXPECT_NE(d.find("VarDecl 'u' weak\n"), std::string::npos) << d;
  EXPECT_NE(d.find("VarDecl 'e' weak type\n"), std::string::npos) << d;
  std::string written = write(*tu);
  EXPECT_NE(written.find("(weak)"), std::string::npos) << written;
  ast::ASTContext ctx2;
  ast::TranslationUnit *back = read(written, ctx2, error);
  ASSERT_NE(back, nullptr) << error;
  EXPECT_EQ(dump(back), d);

  struct Case {
    const char *Var;
    const char *Error;
  };
  for (const Case &c :
       {Case{"(var \"n\" _ (ident \"x\") (weak x))", "(weak) takes no fields"},
        Case{"(var \"n\" _ (ident \"x\") (let) (weak))",
             "a weak declaration is neither let nor a borrow"},
        Case{"(var \"n\" _ (ident \"x\") (qual view) (weak))",
             "a weak declaration is neither let nor a borrow"},
        Case{"(var \"n\" _ _ (weak))",
             "a weak declaration has a type or an initialiser"}}) {
    std::string doc = "(paykan-ast 1 (unit (fn \"f\" (type-params) (params) "
                      "_ (block (decl " +
                      std::string(c.Var) + ")))))";
    EXPECT_EQ(read(doc, ctx, error), nullptr) << c.Var;
    EXPECT_NE(error.find(c.Error), std::string::npos) << c.Var << ": " << error;
  }
  std::string field = "(paykan-ast 1 (unit (struct \"S\" (type-params) "
                      "(fields (var \"p\" (optional-type (named-type \"P\")) "
                      "_ (weak))))))";
  EXPECT_EQ(read(field, ctx, error), nullptr);
  EXPECT_NE(error.find("a struct field cannot be weak"), std::string::npos)
      << error;
}

TEST(ASTInterchange, LetLocals) {
  const std::string text = "(paykan-ast 1 (unit (fn \"f\" (type-params) "
                           "(params) _ (block (decl (var \"n\" _ (int 1) "
                           "(let))) (decl (var \"m\" (named-type \"int\") "
                           "(int 2)))))))";
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *tu = read(text, ctx, error);
  ASSERT_NE(tu, nullptr) << error;
  std::string d = dump(tu);
  EXPECT_NE(d.find("VarDecl 'n' let\n"), std::string::npos) << d;
  EXPECT_NE(d.find("VarDecl 'm' type\n"), std::string::npos) << d;
  std::string written = write(*tu);
  EXPECT_NE(
      written.find("(var \"n\" _\n            (int 1)\n            (let)))"),
      std::string::npos)
      << written;
  ast::ASTContext ctx2;
  ast::TranslationUnit *back = read(written, ctx2, error);
  ASSERT_NE(back, nullptr) << error;
  EXPECT_EQ(dump(back), d);

  struct Case {
    const char *Var;
    const char *Error;
  };
  for (const Case &c :
       {Case{"(var \"n\" _ (int 1) (let x))", "(let) takes no fields"},
        Case{"(var \"n\" (named-type \"int\") _ (let))",
             "a let declaration has an initialiser"},
        Case{"(var \"n\" _ (int 1) (let) (let))",
             "unexpected field in (var ...)"}}) {
    std::string doc = "(paykan-ast 1 (unit (fn \"f\" (type-params) (params) "
                      "_ (block (decl " +
                      std::string(c.Var) + ")))))";
    EXPECT_EQ(read(doc, ctx, error), nullptr) << c.Var;
    EXPECT_NE(error.find(c.Error), std::string::npos) << c.Var << ": " << error;
  }
  std::string field = "(paykan-ast 1 (unit (class \"C\" \"\" (type-params) "
                      "(fields (var \"n\" (named-type \"int\") _ (let))) "
                      "(methods))))";
  EXPECT_EQ(read(field, ctx, error), nullptr);
  EXPECT_NE(error.find("a let declaration has an initialiser"),
            std::string::npos)
      << error;
}

// A `for` loop is its variable as a `var` with no initialiser, the range's
// ends, the step or `_`, the body, and a trailing `(inclusive)` for `..=`.
TEST(ASTInterchange, ForLoops) {
  const std::string text =
      "(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
      "(for (var \"i\" _ _) (int 0) (int 3) _ (block)) "
      "(for (var \"b\" (named-type \"u8\") _) (int 9) (int 0) "
      "(unary neg (int 3)) (block) (inclusive))))))";
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *tu = read(text, ctx, error);
  ASSERT_NE(tu, nullptr) << error;
  std::string d = dump(tu);
  EXPECT_NE(d.find("ForStmt ..\n"), std::string::npos) << d;
  EXPECT_NE(d.find("ForStmt ..= step\n"), std::string::npos) << d;
  EXPECT_NE(d.find("VarDecl 'b' type\n"), std::string::npos) << d;
  std::string written = write(*tu);
  EXPECT_NE(written.find("(inclusive)"), std::string::npos) << written;
  ast::ASTContext ctx2;
  ast::TranslationUnit *back = read(written, ctx2, error);
  ASSERT_NE(back, nullptr) << error;
  EXPECT_EQ(dump(back), d);

  struct Case {
    const char *For;
    const char *Error;
  };
  for (const Case &c :
       {Case{"(for (var \"i\" _ (int 1)) (int 0) (int 3) _ (block))",
             "a for loop's variable is a name and a type or _ only"},
        Case{"(for (var \"i\" _ _) (int 0) (int 3) _ (block) (inclusive x))",
             "(inclusive) takes no fields"},
        Case{"(for (var \"i\" _ _) (int 0) _ (block))", "the end of the range"},
        Case{"(for (var \"i\" _ _) (int 0) (int 3) _)", "the loop's body"}}) {
    std::string doc = "(paykan-ast 1 (unit (fn \"f\" (type-params) (params) "
                      "_ (block " +
                      std::string(c.For) + "))))";
    EXPECT_EQ(read(doc, ctx, error), nullptr) << c.For;
    EXPECT_NE(error.find(c.Error), std::string::npos) << c.For << ": " << error;
  }
}

// A local borrow is a `var` with a trailing `(qual view)` or
// `(qual inout)`, and an initialiser.
TEST(ASTInterchange, LocalBorrows) {
  const std::string text =
      "(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
      "(decl (var \"v\" _ (int 1) (qual view))) (decl (var \"i\" "
      "(named-type \"int\") (ident \"v\") (qual inout)))))))";
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *tu = read(text, ctx, error);
  ASSERT_NE(tu, nullptr) << error;
  std::string d = dump(tu);
  EXPECT_NE(d.find("VarDecl 'v' view\n"), std::string::npos) << d;
  EXPECT_NE(d.find("VarDecl 'i' inout type\n"), std::string::npos) << d;
  std::string written = write(*tu);
  ast::ASTContext ctx2;
  ast::TranslationUnit *back = read(written, ctx2, error);
  ASSERT_NE(back, nullptr) << error;
  EXPECT_EQ(dump(back), d);

  struct Case {
    const char *Var;
    const char *Error;
  };
  for (const Case &c : {Case{"(var \"n\" (named-type \"int\") _ (qual view))",
                             "a local borrow has an initialiser"},
                        Case{"(var \"n\" _ (int 1) (qual view) (let))",
                             "a local borrow cannot be let"},
                        Case{"(var \"n\" _ (int 1) (qual mut))",
                             "expected (qual view) or (qual inout)"}}) {
    std::string doc = "(paykan-ast 1 (unit (fn \"f\" (type-params) (params) "
                      "_ (block (decl " +
                      std::string(c.Var) + ")))))";
    EXPECT_EQ(read(doc, ctx, error), nullptr) << c.Var;
    EXPECT_NE(error.find(c.Error), std::string::npos) << c.Var << ": " << error;
  }
}

// A `match` arm's mode is a `(qual view)` / `(qual inout)` after its type.
TEST(ASTInterchange, MatchArmModes) {
  const std::string text =
      "(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block "
      "(match (ident \"a\") (type-arm \"d\" (named-type \"Dog\") (qual "
      "view) (block)) (type-arm \"c\" (named-type \"Cat\") (qual inout) "
      "(block)) (wildcard-arm \"\" (block)))))))";
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *tu = read(text, ctx, error);
  ASSERT_NE(tu, nullptr) << error;
  std::string d = dump(tu);
  EXPECT_NE(d.find("MatchArm binding='d' view\n"), std::string::npos) << d;
  EXPECT_NE(d.find("MatchArm binding='c' inout\n"), std::string::npos) << d;
  std::string written = write(*tu);
  ast::ASTContext ctx2;
  ast::TranslationUnit *back = read(written, ctx2, error);
  ASSERT_NE(back, nullptr) << error;
  EXPECT_EQ(dump(back), d);

  struct Case {
    const char *Arm;
    const char *Error;
  };
  for (const Case &c :
       {Case{"(type-arm \"\" (named-type \"Dog\") (qual view) (block))",
             "an arm without a binding has no mode"},
        Case{"(type-arm \"d\" (named-type \"Dog\") (qual mut) (block))",
             "expected (qual view) or (qual inout)"}}) {
    std::string doc = "(paykan-ast 1 (unit (fn \"f\" (type-params) (params) "
                      "_ (block (match (ident \"a\") " +
                      std::string(c.Arm) + ")))))";
    EXPECT_EQ(read(doc, ctx, error), nullptr) << c.Arm;
    EXPECT_NE(error.find(c.Error), std::string::npos) << c.Arm << ": " << error;
  }
}

TEST(Interchange, MovIsRemoved) {
  // `mov` is retired (#145): an out-of-tree frontend still emitting it gets
  // the same diagnostic as the built-in parser.
  const std::string text = "(paykan-ast 1 (unit (fn \"f\" (type-params) "
                           "(params) _ (block (expr (mov (ident \"x\")))))))";
  ast::ASTContext ctx;
  std::string error;
  EXPECT_EQ(read(text, ctx, error), nullptr);
  EXPECT_NE(error.find("'mov' was removed in v0.2.0"), std::string::npos)
      << error;
}

// A `native fn` (#198) carries (native "symbol") where the body goes.
TEST(Interchange, NativeFunctionRoundTrips) {
  const std::string text = "(paykan-ast 1\n"
                           "  (unit\n"
                           "    (fn \"put\"\n"
                           "      (type-params)\n"
                           "      (params\n"
                           "        (param \"s\"\n"
                           "          (named-type \"Str\")))\n"
                           "      (named-type \"int\")\n"
                           "      (native \"pk_put\"))))\n";
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *tu = read(text, ctx, error);
  ASSERT_NE(tu, nullptr) << error;
  auto *fn = tu->getFuncDecls()[0];
  EXPECT_TRUE(fn->isNative());
  EXPECT_EQ(fn->getNativeSymbol(), "pk_put");
  EXPECT_EQ(fn->getBody(), nullptr);
  EXPECT_EQ(write(*tu), text);

  // A generic one is a template.
  const std::string generic = "(paykan-ast 1\n"
                              "  (unit\n"
                              "    (fn \"pick\"\n"
                              "      (type-params \"T\")\n"
                              "      (params\n"
                              "        (param \"a\"\n"
                              "          (named-type \"T\")\n"
                              "          (qual view)))\n"
                              "      (named-type \"T\")\n"
                              "      (qual view)\n"
                              "      (native \"pk_pick\"))))\n";
  tu = read(generic, ctx, error);
  ASSERT_NE(tu, nullptr) << error;
  ASSERT_EQ(tu->getGenericFuncDecls().size(), 1u);
  EXPECT_TRUE(tu->getGenericFuncDecls()[0]->isNative());
  EXPECT_EQ(write(*tu), generic);
}

// A tree deeper than the format allows is refused, without recursing
// through all of it.
TEST(ASTInterchange, WriteRefusesTooDeepATree) {
  ast::ASTContext ctx;
  ast::Type *ty = ctx.getIntTy();
  for (int i = 0; i < 100000; ++i)
    ty = ctx.make<ast::ArrayType>(ast::SourceLocation(), ty);
  auto *var = ctx.make<ast::VarDecl>(ast::SourceLocation(), ctx.intern("x"), ty,
                                     nullptr);
  auto *body = ctx.make<ast::CompoundStmt>(
      ast::SourceLocation(), std::vector<ast::Stmt *>{ctx.make<ast::DeclStmt>(
                                 ast::SourceLocation(), var)});
  auto *fn = ctx.make<ast::FuncDecl>(ast::SourceLocation(), ctx.intern("f"),
                                     std::vector<ast::Param>{}, nullptr, body);
  auto *tu = ctx.make<ast::TranslationUnit>(
      ast::SourceLocation(), std::vector<ast::ImportDecl *>{},
      std::vector<ast::ClassDecl *>{}, std::vector<ast::FuncDecl *>{fn});
  std::ostringstream os;
  std::string error;
  EXPECT_FALSE(ic::write(*tu, os, error));
  EXPECT_EQ(error, "nesting too deep (more than 2048 levels)");
}

// Only a parsed AST can be written: a type Sema resolves to (an enum type)
// has no interchange form.
TEST(ASTInterchange, WriteRefusesACheckedAST) {
  ast::ASTContext ctx;
  auto *en = ctx.registerEnumType("E", ast::SourceLocation());
  auto *body = ctx.make<ast::CompoundStmt>(ast::SourceLocation());
  auto *fn = ctx.make<ast::FuncDecl>(ast::SourceLocation(), ctx.intern("f"),
                                     std::vector<ast::Param>{}, en, body);
  auto *tu = ctx.make<ast::TranslationUnit>(
      ast::SourceLocation(), std::vector<ast::ImportDecl *>{},
      std::vector<ast::ClassDecl *>{}, std::vector<ast::FuncDecl *>{fn});
  std::ostringstream os;
  std::string error;
  EXPECT_FALSE(ic::write(*tu, os, error));
  EXPECT_EQ(error, "a type no frontend produces (the AST was already checked)");
  auto *nobody =
      ctx.make<ast::FuncDecl>(ast::SourceLocation(), ctx.intern("g"),
                              std::vector<ast::Param>{}, nullptr, nullptr);
  auto *tu2 = ctx.make<ast::TranslationUnit>(
      ast::SourceLocation(), std::vector<ast::ImportDecl *>{},
      std::vector<ast::ClassDecl *>{}, std::vector<ast::FuncDecl *>{nobody});
  EXPECT_FALSE(ic::write(*tu2, os, error));
  EXPECT_EQ(error, "function 'g' has no body");
}

} // namespace

// An `impl` block (#216) is (impl "Type" (methods FN...)); a `static fn`
// ends with (qual static), as a `view fn` ends with (qual view).
// Function types (`fn-type`, with each parameter's mode), lambdas
// (`lambda`) and calls of function values that are not named
// (`call-value`) round-trip (docs/language/14-function-types.md).
TEST(Interchange, FunctionTypesRoundTrip) {
  parser::ParserDriver driver("recursive-descent");
  auto path = std::filesystem::temp_directory_path() /
              ("paykan_interchange_fn_" + std::to_string(::getpid()) + ".pkn");
  {
    std::ofstream(path) << "import ::io; fn apply(f: (view Str, inout int) -> "
                           "bool, g: () -> void) -> "
                           "(int) -> int {\n"
                           "  h = fn(x: int) -> int { return x + 1; };\n"
                           "  log = fn(s: view Str) { io::println(s); };\n"
                           "  fs: ((int) -> int)[1] = [h];\n"
                           "  n = fs[0](1) + h(2) + apply(f, g)(3);\n"
                           "  return h;\n"
                           "}\n";
  }
  ASSERT_EQ(driver.parseFile(path.string()), 0);
  std::filesystem::remove(path);
  std::string text = write(*driver.getRoot());
  EXPECT_NE(text.find("(fn-type"), std::string::npos) << text;
  EXPECT_NE(text.find("view"), std::string::npos) << text;
  EXPECT_NE(text.find("(lambda"), std::string::npos) << text;
  EXPECT_NE(text.find("(call-value"), std::string::npos) << text;
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *back = read(text, ctx, error);
  ASSERT_NE(back, nullptr) << error;
  EXPECT_EQ(dump(back), dump(driver.getRoot()));
  EXPECT_EQ(write(*back), text);
}

TEST(Interchange, TraitsRoundTrip) {
  parser::ParserDriver driver("recursive-descent");
  auto path =
      std::filesystem::temp_directory_path() /
      ("paykan_interchange_trait_" + std::to_string(::getpid()) + ".pkn");
  {
    std::ofstream(path) << "trait Into<T> { view fn into() -> T; fn f(); }\n"
                           "impl Into<int> for A {\n"
                           "  view fn into() -> int { return 1; }\n"
                           "}\n"
                           "fn show<T: Show + Into<int>, U>(x: T) {}\n"
                           "class Box<T: Show> { v: T; }\n";
  }
  ASSERT_EQ(driver.parseFile(path.string()), 0);
  std::filesystem::remove(path);
  std::string text = write(*driver.getRoot());
  EXPECT_NE(text.find("(trait @1:1-1:47 \"Into\""), std::string::npos) << text;
  EXPECT_NE(text.find("(for"), std::string::npos) << text;
  EXPECT_NE(text.find("(bound \"T\""), std::string::npos) << text;
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *back = read(text, ctx, error);
  ASSERT_NE(back, nullptr) << error;
  ASSERT_EQ(back->getTraitDecls().size(), 1u);
  EXPECT_EQ(back->getTraitDecls()[0]->getMethods()[0]->getBody(), nullptr);
  EXPECT_EQ(dump(back), dump(driver.getRoot()));
  EXPECT_EQ(write(*back), text);
}

TEST(Interchange, ImplBlockRoundTrips) {
  parser::ParserDriver driver("recursive-descent");
  auto path =
      std::filesystem::temp_directory_path() /
      ("paykan_interchange_impl_" + std::to_string(::getpid()) + ".pkn");
  {
    std::ofstream(path) << "class A { n: int; }\n"
                           "impl A {\n"
                           "  view fn read() -> int { return self.n; }\n"
                           "  static fn make() -> A { return A(); }\n"
                           "}\n"
                           "impl Show for Pair<K, V> {}\n";
  }
  ASSERT_EQ(driver.parseFile(path.string()), 0);
  std::filesystem::remove(path);
  std::string text = write(*driver.getRoot());
  EXPECT_NE(text.find("(impl @2:1-5:2 \"A\"\n      (methods"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("(qual static)"), std::string::npos) << text;
  EXPECT_NE(text.find("\"Pair\"\n      (type-params \"K\" \"V\")\n      (for"),
            std::string::npos)
      << text;
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *back = read(text, ctx, error);
  ASSERT_NE(back, nullptr) << error;
  ASSERT_EQ(back->getImplDecls().size(), 2u);
  EXPECT_TRUE(back->getImplDecls()[0]->getMethods()[1]->isStatic());
  EXPECT_EQ(back->getImplDecls()[1]->getTypeParams().size(), 2u);
  EXPECT_EQ(dump(back), dump(driver.getRoot()));
  EXPECT_EQ(write(*back), text);
}

// The sized numeric types (docs/design/sized-types.md) are named types in
// the interchange form, written under their printed names (`u64` and `i64`
// are `unsigned` and `int`) and read back to the same builtin types.
TEST(Interchange, SizedTypesRoundTrip) {
  parser::ParserDriver driver("recursive-descent");
  auto path =
      std::filesystem::temp_directory_path() /
      ("paykan_interchange_sized_" + std::to_string(::getpid()) + ".pkn");
  {
    std::ofstream(path)
        << "struct P { r: u8; g: i16; b: u32; a: f32; }\n"
           "fn f(a: u64, b: i64, c: f64, d: f16) -> unsigned {\n"
           "  x: i8 = -1;\n"
           "  y: u16? = 5;\n"
           "  z: f32[2] = [0.5, 1.5];\n"
           "  return a + unsigned(b);\n"
           "}\n";
  }
  ASSERT_EQ(driver.parseFile(path.string()), 0);
  std::filesystem::remove(path);
  std::string text = write(*driver.getRoot());
  for (const char *name :
       {"\"u8\"", "\"i16\"", "\"u32\"", "\"f32\"", "\"unsigned\"", "\"int\"",
        "\"float\"", "\"f16\"", "\"i8\"", "\"u16\""})
    EXPECT_NE(text.find(name), std::string::npos) << name << "\n" << text;
  EXPECT_EQ(text.find("\"u64\""), std::string::npos) << text;
  EXPECT_EQ(text.find("\"i64\""), std::string::npos) << text;
  EXPECT_EQ(text.find("\"f64\""), std::string::npos) << text;
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *back = read(text, ctx, error);
  ASSERT_NE(back, nullptr) << error;
  EXPECT_EQ(dump(back), dump(driver.getRoot()));
  EXPECT_EQ(write(*back), text);
}

// A module-private initializer, `___init__`, is written as spelled and read
// back as the initializer with its mark.
TEST(Interchange, ModulePrivateInitializerRoundTrips) {
  parser::ParserDriver driver("recursive-descent");
  auto path =
      std::filesystem::temp_directory_path() /
      ("paykan_interchange_init_" + std::to_string(::getpid()) + ".pkn");
  {
    std::ofstream(path) << "class A { n: int; }\n"
                           "impl A {\n"
                           "  fn ___init__(n: int) { self.n = n; }\n"
                           "}\n";
  }
  ASSERT_EQ(driver.parseFile(path.string()), 0);
  std::filesystem::remove(path);
  const ast::FuncDecl *init =
      driver.getRoot()->getImplDecls()[0]->getMethods()[0];
  EXPECT_EQ(init->getName(), "__init__");
  EXPECT_TRUE(init->isModulePrivateInit());
  std::string text = write(*driver.getRoot());
  EXPECT_NE(text.find("(fn @3:3-3:39 \"___init__\""), std::string::npos)
      << text;
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *back = read(text, ctx, error);
  ASSERT_NE(back, nullptr) << error;
  const ast::FuncDecl *again = back->getImplDecls()[0]->getMethods()[0];
  EXPECT_EQ(again->getName(), "__init__");
  EXPECT_TRUE(again->isModulePrivateInit());
  EXPECT_EQ(dump(back), dump(driver.getRoot()));
  EXPECT_EQ(write(*back), text);
}
