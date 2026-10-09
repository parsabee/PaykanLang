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

std::string write(const ast::TranslationUnit &tu) {
  std::ostringstream os;
  std::string error;
  EXPECT_TRUE(ic::write(tu, os, error)) << error;
  return os.str();
}

/// read() of @p text: the unit, or null with @p error.
ast::TranslationUnit *read(const std::string &text, ast::ASTContext &ctx,
                           std::string &error) {
  ic::ReadError err;
  ast::TranslationUnit *tu = ic::read(text, ctx, err);
  error = err.str();
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
  { std::ofstream(path) << "fn main() -> int {\n  return 0;\n}\n"; }
  ASSERT_EQ(driver.parseFile(path.string()), 0);
  std::filesystem::remove(path);
  EXPECT_EQ(write(*driver.getRoot()), "(paykan-ast 1\n"
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
    (fn @4:1-9:9 "main" (type-params) (params (param "args" (array-type (named-type "Str"))))
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
       "1:13: not a 64-bit integer: 99999999999999999999"},
      {"(paykan-ast 1 (block))", "1:15: expected (unit ...)"},
      {"(paykan-ast 1 (unit \"x\"))",
       "1:21: expected a declaration: (import ...), (enum ...), (class ...) "
       "or (fn ...)"},
      {"(paykan-ast 1 (unit (var \"x\" _ _)))",
       "1:21: (var ...) is not a top-level declaration (import, enum, class, "
       "fn)"},
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
       "optional-type, tuple-type, generic-type)"},
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

// A parameter's mode is the optional `(qual view)` / `(qual inout)` item.
TEST(ASTInterchange, ParamModes) {
  parser::ParserDriver driver("recursive-descent");
  auto path =
      std::filesystem::temp_directory_path() /
      ("paykan_interchange_modes_" + std::to_string(::getpid()) + ".pkn");
  {
    std::ofstream(path) << "fn bump(inout n: int, view by: int, k: int) { }\n"
                           "class C { fn __init__(view n: int) { } }\n";
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

  EXPECT_EQ(read("(paykan-ast 1 (unit (fn \"f\" (type-params \"T\") (params) "
                 "_ (native \"pk_f\"))))",
                 ctx, error),
            nullptr);
  EXPECT_NE(error.find("a native function cannot be generic"),
            std::string::npos)
      << error;
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
