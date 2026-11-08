// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// The AST interchange format (paykan/ast/Interchange.h) for the nodes of
// the concurrency prototype (docs/design/concurrency.md,
// samples/concurrency/README.md): an `async fn`, a `parallel for`, a `lock`
// block, the qualifiers on a place's type (`mut`, `shared`, `shared mut`,
// `atomic shared`, `locked`, `locked mut`, and `x: shared = e` with the
// type inferred), `atomic T`, and the `move` and `await` operators.  As
// InterchangeTests.cpp does for the rest of the language:
//   - each round-trips: the recursive-descent frontend's AST, written and
//     read back, is the same AST (its --dump-ast text, and its interchange
//     text, are identical), and the text is the documented form;
//   - the reader rejects each malformed form with a message.
// The --dump-ast text shows no `async` and no `parallel`, so for those the
// written text and the flags on the read AST are what is compared.

#include <gtest/gtest.h>

#include <unistd.h>

#include "ASTPrinter.h"
#include "ParserDriver.h"
#include "Sema.h"
#include "paykan/ast/Interchange.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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
// `1` again in what they return (as InterchangeTests.cpp spells it).
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

std::string write(const ast::TranslationUnit &tu) {
  std::ostringstream os;
  std::string error;
  EXPECT_TRUE(ic::write(tu, os, error)) << error;
  return replaceAll(os.str(), kReal, kSpelled);
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

/// The unit of a program written to a scratch file and parsed by the
/// recursive-descent frontend, or null after a syntax error.
ast::TranslationUnit *parseProgram(parser::ParserDriver &driver,
                                   const std::string &source,
                                   const std::string &tag) {
  auto path = std::filesystem::temp_directory_path() /
              ("paykan_interchange_concurrency_" + tag + "_" +
               std::to_string(::getpid()) + ".pkn");
  std::ofstream(path) << source;
  std::ostringstream quiet;
  sema::DiagEngine diag(quiet);
  driver.setDiagEngine(&diag);
  int rc = driver.parseFile(path.string());
  std::filesystem::remove(path);
  return rc == 0 ? driver.getRoot() : nullptr;
}

/// @p text on one line: every run of whitespace is one space.
std::string flat(const std::string &text) {
  std::string out;
  for (char c : text)
    if (!std::isspace(static_cast<unsigned char>(c)))
      out += c;
    else if (!out.empty() && out.back() != ' ')
      out += ' ';
  return out;
}

/// The reader refuses @p doc, with @p what in its message.
void expectRejected(const std::string &doc, const char *what) {
  ast::ASTContext ctx;
  std::string error;
  EXPECT_EQ(read(doc, ctx, error), nullptr) << doc;
  EXPECT_NE(error.find(what), std::string::npos) << doc << ": " << error;
}

/// `fn f() { STMT }`.
std::string fnOf(const std::string &stmt) {
  return "(paykan-ast 1 (unit (fn \"f\" (type-params) (params) _ (block " +
         stmt + "))))";
}

/// `fn f(a: TYPE) { }`.
std::string paramOf(const std::string &type) {
  return "(paykan-ast 1 (unit (fn \"f\" (type-params) (params (param \"a\" " +
         type + ")) _ (block))))";
}

} // namespace

// An `async fn` ends with `(async)`, after the body and after a `(qual
// view)` / `(qual static)`, before an `(inferred)`.
TEST(ConcurrencyInterchange, AsyncFn) {
  parser::ParserDriver driver("recursive-descent");
  ast::TranslationUnit *tu =
      parseProgram(driver,
                   "async fn f(n: int) -> int { return n; }\n"
                   "fn g() { }\n",
                   "async");
  ASSERT_NE(tu, nullptr);
  const std::string text = write(*tu);
  EXPECT_NE(text.find("      (block @1:28-1:28\n"
                      "        (return @1:29-1:38\n"
                      "          (ident @1:36-1:37 \"n\")))\n"
                      "      (async))\n"
                      "    (fn @2:1-2:11 \"g\""),
            std::string::npos)
      << text;
  // `f` is marked, and only it.
  EXPECT_EQ(text.find("(async)"), text.rfind("(async)")) << text;
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *back = read(text, ctx, error);
  ASSERT_NE(back, nullptr) << error;
  ASSERT_EQ(back->getFuncDecls().size(), 2u);
  EXPECT_TRUE(back->getFuncDecls()[0]->isAsync());
  EXPECT_FALSE(back->getFuncDecls()[1]->isAsync());
  EXPECT_EQ(dump(back), dump(tu));
  EXPECT_EQ(write(*back), text);

  // Without a location, and after a borrowed result's mode.
  ast::TranslationUnit *hand =
      read("(paykan-ast 1 (unit (fn \"f\" (type-params) (params (param \"s\" "
           "(named-type \"Str\") (qual view))) (named-type \"Str\") (qual "
           "view) (block (return (ident \"s\"))) (async))))",
           ctx, error);
  ASSERT_NE(hand, nullptr) << error;
  EXPECT_TRUE(hand->getFuncDecls()[0]->isAsync());
  EXPECT_EQ(hand->getFuncDecls()[0]->getResultMode(), ast::ParamMode::View);

  struct Case {
    const char *Fn;
    const char *Error;
  };
  for (const Case &c :
       {Case{"(fn \"f\" (type-params) (params) _ (block) (async x))",
             "(async) takes no fields"},
        // It follows the body...
        Case{"(fn \"f\" (type-params) (params) _ (async) (block))",
             "expected (block ...): the function's body"},
        // ... and a `view fn`'s (qual view), once.
        Case{"(fn \"f\" (type-params) (params) _ (block) (async) (async))",
             "unexpected field in (fn ...)"},
        Case{"(fn \"f\" (type-params) (params) _ (block) (async) (qual view))",
             "unexpected field in (fn ...)"}})
    expectRejected("(paykan-ast 1 (unit " + std::string(c.Fn) + "))", c.Error);
}

// A `parallel for` ends with `(parallel)`, after the `(inclusive)` of
// `..=`.
TEST(ConcurrencyInterchange, ParallelFor) {
  parser::ParserDriver driver("recursive-descent");
  ast::TranslationUnit *tu =
      parseProgram(driver,
                   "fn f(xs: view float[]) -> float {\n"
                   "  s = 0.0;\n"
                   "  parallel for i in 0..xs.len() { s += xs[i]; }\n"
                   "  parallel for i in 0..=3 step 2 { s += 1.0; }\n"
                   "  for i in 0..2 { s += 1.0; }\n"
                   "  return s;\n"
                   "}\n",
                   "parallel");
  ASSERT_NE(tu, nullptr);
  const std::string text = write(*tu);
  const std::string one = flat(text);
  for (const char *form :
       {"(var @3:16-3:17 \"i\" _ _) (int @3:21-3:22 0) (method-call "
        "@3:24-3:32 (ident @3:24-3:26 \"xs\") \"len\") _ (block @3:34-3:34 "
        "(assign @3:35-3:46 (ident @3:35-3:36 \"s\") (binary @3:35-3:45 add "
        "(ident @3:35-3:36 \"s\") (subscript @3:40-3:45 (ident @3:40-3:42 "
        "\"xs\") (ident @3:43-3:44 \"i\"))))) (parallel))",
        "(float @4:41-4:44 1.0)))) (inclusive) (parallel))",
        // The plain loop has no trailer.
        "(float @5:24-5:27 1.0))))) (return @6:3-6:12"})
    EXPECT_NE(one.find(form), std::string::npos) << form << "\n" << text;
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *back = read(text, ctx, error);
  ASSERT_NE(back, nullptr) << error;
  const auto &stmts = back->getFuncDecls()[0]->getBody()->getStatements();
  ASSERT_EQ(stmts.size(), 5u);
  EXPECT_TRUE(ast::cast<ast::ForStmt>(stmts[1])->isParallel());
  EXPECT_TRUE(ast::cast<ast::ForStmt>(stmts[2])->isParallel());
  EXPECT_TRUE(ast::cast<ast::ForStmt>(stmts[2])->isInclusive());
  EXPECT_FALSE(ast::cast<ast::ForStmt>(stmts[3])->isParallel());
  EXPECT_EQ(dump(back), dump(tu));
  EXPECT_EQ(write(*back), text);

  // Without locations, with and without the (inclusive) before it.
  ast::TranslationUnit *hand =
      read(fnOf("(for (var \"i\" _ _) (int 0) (int 3) _ (block) (parallel)) "
                "(for (var \"\" (named-type \"u8\") _) (int 0) (int 3) (int 2) "
                "(block) (inclusive) (parallel))"),
           ctx, error);
  ASSERT_NE(hand, nullptr) << error;
  for (ast::Stmt *s : hand->getFuncDecls()[0]->getBody()->getStatements())
    EXPECT_TRUE(ast::cast<ast::ForStmt>(s)->isParallel());

  struct Case {
    const char *For;
    const char *Error;
  };
  for (const Case &c :
       {Case{"(for (var \"i\" _ _) (int 0) (int 3) _ (block) (parallel x))",
             "(parallel) takes no fields"},
        // It follows the body and the (inclusive), once.
        Case{"(for (var \"i\" _ _) (int 0) (int 3) _ (parallel) (block))",
             "expected (block ...): the loop's body"},
        Case{"(for (var \"i\" _ _) (int 0) (int 3) _ (block) (parallel) "
             "(inclusive))",
             "unexpected field in (for ...)"},
        Case{"(for (var \"i\" _ _) (int 0) (int 3) _ (block) (parallel) "
             "(parallel))",
             "unexpected field in (for ...)"}})
    expectRejected(fnOf(c.For), c.Error);
}

// A `lock cell as v: inout { }` is `(lock LOC EXPR VAR BLOCK)`: the cell,
// the binding as a `var` with no type, no initialiser and its mode, `(qual
// inout)` or `(qual view)`, then the body.
TEST(ConcurrencyInterchange, LockBlocks) {
  parser::ParserDriver driver("recursive-descent");
  ast::TranslationUnit *tu =
      parseProgram(driver,
                   "fn f(c: locked Counts) -> int {\n"
                   "  n = 0;\n"
                   "  lock c as v: inout { v.total = 1; }\n"
                   "  lock c as u: view { n = u.total; }\n"
                   "  return n;\n"
                   "}\n",
                   "lock");
  ASSERT_NE(tu, nullptr);
  const std::string text = write(*tu);
  const std::string one = flat(text);
  for (const char *form :
       {"(lock @3:3-3:38 (ident @3:8-3:9 \"c\") (var @3:13-3:38 \"v\" _ _ "
        "(qual inout)) (block @3:23-3:23 (member-assign @3:24-3:36 (ident "
        "@3:24-3:25 \"v\") \"total\" (int @3:34-3:35 1))))",
        "(lock @4:3-4:37 (ident @4:8-4:9 \"c\") (var @4:13-4:37 \"u\" _ _ "
        "(qual view)) (block @4:22-4:22 (assign @4:23-4:35 (ident @4:23-4:24 "
        "\"n\") (member @4:27-4:34 (ident @4:27-4:28 \"u\") \"total\"))))"})
    EXPECT_NE(one.find(form), std::string::npos) << form << "\n" << text;
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *back = read(text, ctx, error);
  ASSERT_NE(back, nullptr) << error;
  const auto &stmts = back->getFuncDecls()[0]->getBody()->getStatements();
  ASSERT_EQ(stmts.size(), 4u);
  auto *inout = ast::cast<ast::LockStmt>(stmts[1]);
  EXPECT_EQ(ast::cast<ast::Identifier>(inout->getCell())->getName(), "c");
  EXPECT_EQ(inout->getBinding()->getName(), "v");
  EXPECT_EQ(inout->getBinding()->getMode(), ast::ParamMode::Inout);
  EXPECT_EQ(inout->getBinding()->getType(), nullptr);
  EXPECT_EQ(inout->getBinding()->getInitExpr(), nullptr);
  EXPECT_EQ(inout->getBody()->getStatements().size(), 1u);
  auto *view = ast::cast<ast::LockStmt>(stmts[2]);
  EXPECT_EQ(view->getBinding()->getName(), "u");
  EXPECT_EQ(view->getBinding()->getMode(), ast::ParamMode::View);
  const std::string d = dump(back);
  EXPECT_NE(d.find("LockStmt <3:3-3:38> 'v' inout\n"), std::string::npos) << d;
  EXPECT_NE(d.find("LockStmt <4:3-4:37> 'u' view\n"), std::string::npos) << d;
  EXPECT_EQ(d, dump(tu));
  EXPECT_EQ(write(*back), text);

  // Without locations; the cell is any expression.
  ast::TranslationUnit *hand =
      read(fnOf("(lock (member (ident \"self\") \"cell\") (var \"v\" _ _ "
                "(qual view)) (block))"),
           ctx, error);
  ASSERT_NE(hand, nullptr) << error;

  // The binding is a borrow with no initialiser, and nothing else.
  const char *const kBinding =
      "a lock block's binding is a view or an inout with no initialiser";
  struct Case {
    const char *Var;
    const char *Error;
  };
  for (const Case &c :
       {Case{"(var \"v\" _ (ident \"c\") (qual inout))", kBinding},
        Case{"(var \"v\" (named-type \"Counts\") (ident \"c\") (qual view))",
             kBinding},
        Case{"(var \"v\" _ _)", kBinding},
        Case{"(var \"v\" _ _ (let))", kBinding},
        Case{"(var \"v\" _ _ (qual view) (weak))",
             "a weak declaration is neither let nor a borrow"},
        Case{"(var \"v\" _ _ (qual mut))",
             "expected (qual view) or (qual inout)"},
        Case{"(var \"\" _ _ (qual view))", "empty name: the variable's name"}})
    expectRejected(
        fnOf("(lock (ident \"c\") " + std::string(c.Var) + " (block))"),
        c.Error);
  for (const Case &c :
       {Case{"(lock (ident \"c\") (block))", "expected (var ...)"},
        Case{"(lock (ident \"c\") (var \"v\" _ _ (qual view)))",
             "(lock ...) is missing the lock block's body"},
        Case{"(lock (ident \"c\") (var \"v\" _ _ (qual view)) (expr (int 1)))",
             "expected (block ...): the lock block's body"},
        Case{"(lock (var \"v\" _ _ (qual view)) (block))",
             "(var ...) is not an expression"},
        Case{"(lock)", "(lock ...) is missing the locked cell"}})
    expectRejected(fnOf(c.Var), c.Error);
}

// A qualified type is `(qualified-type LOC "spelling" TYPE)`, the
// qualifier spelled as written (`mut`, `shared`, `shared mut`, `atomic
// shared`, `locked`, `locked mut`) and the type it qualifies: on a
// parameter, a field, a local and a result alike.  It is a type, not a
// `(qual ...)` mode; the bare type, unique and immutable, is unwritten.
TEST(ConcurrencyInterchange, QualifiedTypes) {
  parser::ParserDriver driver("recursive-descent");
  ast::TranslationUnit *tu =
      parseProgram(driver,
                   "class Pair {\n"
                   "  left: mut Node;\n"
                   "  right: shared Node;\n"
                   "  cfg: atomic shared Node;\n"
                   "  cell: locked mut Node;\n"
                   "}\n"
                   "fn f(a: mut Node, b: shared Node, c: shared mut Node, "
                   "d: atomic shared Node, e: locked Node, g: locked mut Node) "
                   "-> locked mut Node {\n"
                   "  let x: mut Node = a;\n"
                   "  let y: shared mut Node = c;\n"
                   "  return g;\n"
                   "}\n"
                   "fn h(n: shared Node) -> shared mut Node { return n; }\n",
                   "qualified");
  ASSERT_NE(tu, nullptr);
  const std::string text = write(*tu);
  const std::string one = flat(text);
  for (const char *form :
       {"(var @2:3-2:17 \"left\" (qualified-type @2:9-2:17 \"mut\" "
        "(named-type @2:13-2:17 \"Node\")) _)",
        "(var @3:3-3:21 \"right\" (qualified-type @3:10-3:21 \"shared\" "
        "(named-type @3:17-3:21 \"Node\")) _)",
        "(var @4:3-4:26 \"cfg\" (qualified-type @4:8-4:26 \"atomic shared\" "
        "(named-type @4:22-4:26 \"Node\")) _)",
        "(var @5:3-5:24 \"cell\" (qualified-type @5:9-5:24 \"locked mut\" "
        "(named-type @5:20-5:24 \"Node\")) _)",
        "(param @7:6-7:17 \"a\" (qualified-type @7:9-7:17 \"mut\" "
        "(named-type @7:13-7:17 \"Node\")))",
        "(param @7:19-7:33 \"b\" (qualified-type @7:22-7:33 \"shared\" "
        "(named-type @7:29-7:33 \"Node\")))",
        "(param @7:35-7:53 \"c\" (qualified-type @7:38-7:53 \"shared mut\" "
        "(named-type @7:49-7:53 \"Node\")))",
        "(param @7:55-7:76 \"d\" (qualified-type @7:58-7:76 \"atomic shared\" "
        "(named-type @7:72-7:76 \"Node\")))",
        "(param @7:78-7:92 \"e\" (qualified-type @7:81-7:92 \"locked\" "
        "(named-type @7:88-7:92 \"Node\")))",
        // The result's, where the return type goes.
        "(param @7:94-7:112 \"g\" (qualified-type @7:97-7:112 \"locked mut\" "
        "(named-type @7:108-7:112 \"Node\")))) (qualified-type @7:117-7:132 "
        "\"locked mut\" (named-type @7:128-7:132 \"Node\")) (block "
        "@7:134-7:134",
        "(var @8:3-8:22 \"x\" (qualified-type @8:10-8:18 \"mut\" "
        "(named-type @8:14-8:18 \"Node\")) (ident @8:21-8:22 \"a\") (let))",
        "(var @9:3-9:29 \"y\" (qualified-type @9:10-9:25 \"shared mut\" "
        "(named-type @9:21-9:25 \"Node\")) (ident @9:28-9:29 \"c\") (let))",
        "(qualified-type @12:25-12:40 \"shared mut\" (named-type "
        "@12:36-12:40 \"Node\")) (block @12:42-12:42"})
    EXPECT_NE(one.find(form), std::string::npos) << form << "\n" << text;
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *back = read(text, ctx, error);
  ASSERT_NE(back, nullptr) << error;
  // Each reads back to its qualifier, on the class type it qualifies.
  struct Expected {
    ast::QualForm Form;
    bool Mut;
    const char *Spelling;
  };
  const Expected params[] = {
      {ast::QualForm::Unique, true, "mut"},
      {ast::QualForm::Shared, false, "shared"},
      {ast::QualForm::Shared, true, "shared mut"},
      {ast::QualForm::AtomicShared, false, "atomic shared"},
      {ast::QualForm::Locked, false, "locked"},
      {ast::QualForm::Locked, true, "locked mut"}};
  auto *f = back->getFuncDecls()[0];
  ASSERT_EQ(f->getParams().size(), 6u);
  for (size_t i = 0; i < 6; ++i) {
    auto *qt = ast::cast<ast::QualifiedType>(f->getParams()[i].ParamType);
    EXPECT_EQ(qt->getQualifier().Form, params[i].Form) << i;
    EXPECT_EQ(qt->getQualifier().Mut, params[i].Mut) << i;
    EXPECT_STREQ(qt->getQualifier().spelling(), params[i].Spelling) << i;
    EXPECT_EQ(ast::cast<ast::ClassType>(qt->getInnerType())->getName(), "Node")
        << i;
  }
  auto *result = ast::cast<ast::QualifiedType>(f->getReturnType());
  EXPECT_EQ(result->getQualifier().Form, ast::QualForm::Locked);
  EXPECT_TRUE(result->getQualifier().Mut);
  const auto &fields = back->getClassDecls()[0]->getFields();
  ASSERT_EQ(fields.size(), 4u);
  EXPECT_STREQ(ast::cast<ast::QualifiedType>(fields[2]->getType())
                   ->getQualifier()
                   .spelling(),
               "atomic shared");
  const std::string d = dump(back);
  EXPECT_NE(d.find("QualifiedType <7:117-7:132> 'locked mut'\n"),
            std::string::npos)
      << d;
  EXPECT_EQ(d, dump(tu));
  EXPECT_EQ(write(*back), text);

  // Without locations, each spelling reads.
  for (const char *spelling : {"mut", "shared", "shared mut", "atomic shared",
                               "locked", "locked mut"}) {
    ast::TranslationUnit *hand =
        read(paramOf("(qualified-type \"" + std::string(spelling) +
                     "\" (named-type \"Node\"))"),
             ctx, error);
    ASSERT_NE(hand, nullptr) << spelling << ": " << error;
    auto *qt = ast::cast<ast::QualifiedType>(
        hand->getFuncDecls()[0]->getParams()[0].ParamType);
    EXPECT_STREQ(qt->getQualifier().spelling(), spelling);
  }

  struct Case {
    const char *Type;
    const char *Error;
  };
  const char *const kForms =
      "(mut, shared, shared mut, atomic shared, locked, locked mut)";
  for (const Case &c :
       {Case{"(qualified-type \"mutable\" (named-type \"Node\"))",
             "unknown qualifier \"mutable\" "},
        // Unique is unwritten, and `atomic T` is an (atomic-type ...).
        Case{"(qualified-type \"\" (named-type \"Node\"))",
             "unknown qualifier \"\" "},
        Case{"(qualified-type \"atomic\" (named-type \"int\"))",
             "unknown qualifier \"atomic\" "},
        // The forms the parser rejects are unknown here too: a frozen
        // object and a cell take no `mut`.
        Case{"(qualified-type \"atomic shared mut\" (named-type \"Node\"))",
             "unknown qualifier \"atomic shared mut\" "},
        Case{"(qualified-type \"atomic mut\" (named-type \"int\"))",
             "unknown qualifier \"atomic mut\" "},
        // As written: the form first, one space, in lower case.
        Case{"(qualified-type \"mut shared\" (named-type \"Node\"))",
             "unknown qualifier \"mut shared\" "},
        Case{"(qualified-type \"mut locked\" (named-type \"Node\"))",
             "unknown qualifier \"mut locked\" "},
        Case{"(qualified-type \"shared  mut\" (named-type \"Node\"))",
             "unknown qualifier \"shared  mut\" "},
        Case{"(qualified-type \"Shared\" (named-type \"Node\"))",
             "unknown qualifier \"Shared\" "},
        Case{"(qualified-type \"Mut\" (named-type \"Node\"))",
             "unknown qualifier \"Mut\" "},
        Case{"(qualified-type shared (named-type \"Node\"))",
             "expected a string: the qualifier"},
        Case{"(qualified-type)",
             "(qualified-type ...) is missing the qualifier"},
        Case{"(qualified-type \"shared\" 3)",
             "expected a list: the qualified type"},
        Case{"(qualified-type \"shared\" (int 1))", "(int ...) is not a type"},
        Case{"(qualified-type \"shared\" (named-type \"Node\") (named-type "
             "\"Node\"))",
             "unexpected field in (qualified-type ...)"},
        // A type takes one qualifier, as the parser reads it (`shared
        // shared Node`, `shared atomic int`).
        Case{"(qualified-type \"shared\" (qualified-type \"mut\" "
             "(named-type \"Node\")))",
             "a type takes one qualifier: \"shared\" qualifies a qualified "
             "type"},
        Case{"(qualified-type \"shared\" (atomic-type (named-type "
             "\"int\")))",
             "a type takes one qualifier: \"shared\" qualifies an atomic "
             "type"},
        // A qualifier is not a parameter's mode.
        Case{"(named-type \"Node\") (qual shared)",
             "expected (qual view) or (qual inout)"},
        Case{"(named-type \"Node\") (qual mut)",
             "expected (qual view) or (qual inout)"}})
    expectRejected(paramOf(c.Type), c.Error);
  for (const char *spelling :
       {"mutable", "", "atomic", "atomic shared mut", "mut shared"})
    expectRejected(paramOf("(qualified-type \"" + std::string(spelling) +
                           "\" (named-type \"Node\"))"),
                   kForms);
}

// `x: shared = e`, with the type inferred, is a `(qualified-type LOC
// "shared")` with no inner type.
TEST(ConcurrencyInterchange, InferredQualifiedLocals) {
  parser::ParserDriver driver("recursive-descent");
  ast::TranslationUnit *tu = parseProgram(driver,
                                          "fn f(b: shared Node) {\n"
                                          "  y: shared = b;\n"
                                          "  z: shared mut = b;\n"
                                          "  w: mut = b;\n"
                                          "  v: atomic shared = b;\n"
                                          "  u: locked = b;\n"
                                          "  t: locked mut = b;\n"
                                          "}\n",
                                          "inferred");
  ASSERT_NE(tu, nullptr);
  const std::string text = write(*tu);
  EXPECT_NE(text.find("(qualified-type @2:6-2:12 \"shared\")\n"),
            std::string::npos)
      << text;
  const std::string one = flat(text);
  for (const char *form :
       {"(var @2:3-2:16 \"y\" (qualified-type @2:6-2:12 \"shared\") (ident "
        "@2:15-2:16 \"b\"))",
        "(var @3:3-3:20 \"z\" (qualified-type @3:6-3:16 \"shared mut\") "
        "(ident @3:19-3:20 \"b\"))",
        "(var @4:3-4:13 \"w\" (qualified-type @4:6-4:9 \"mut\") (ident "
        "@4:12-4:13 \"b\"))",
        "(var @5:3-5:23 \"v\" (qualified-type @5:6-5:19 \"atomic shared\") "
        "(ident @5:22-5:23 \"b\"))",
        "(var @6:3-6:16 \"u\" (qualified-type @6:6-6:12 \"locked\") (ident "
        "@6:15-6:16 \"b\"))",
        "(var @7:3-7:20 \"t\" (qualified-type @7:6-7:16 \"locked mut\") "
        "(ident @7:19-7:20 \"b\"))"})
    EXPECT_NE(one.find(form), std::string::npos) << form << "\n" << text;
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *back = read(text, ctx, error);
  ASSERT_NE(back, nullptr) << error;
  const auto &stmts = back->getFuncDecls()[0]->getBody()->getStatements();
  ASSERT_EQ(stmts.size(), 6u);
  for (size_t i = 0; i < stmts.size(); ++i) {
    auto *var =
        ast::cast<ast::VarDecl>(ast::cast<ast::DeclStmt>(stmts[i])->getDecl());
    auto *qt = ast::cast<ast::QualifiedType>(var->getType());
    EXPECT_EQ(qt->getInnerType(), nullptr) << i;
    EXPECT_NE(var->getInitExpr(), nullptr) << i;
  }
  const std::string d = dump(back);
  EXPECT_NE(d.find("VarDecl <2:3-2:16> 'y' type\n"), std::string::npos) << d;
  EXPECT_NE(d.find("QualifiedType <2:6-2:12> 'shared'\n"), std::string::npos)
      << d;
  EXPECT_NE(d.find("QualifiedType <4:6-4:9> 'mut'\n"), std::string::npos) << d;
  EXPECT_EQ(d, dump(tu));
  EXPECT_EQ(write(*back), text);

  // Without a location.
  ast::TranslationUnit *hand =
      read(fnOf("(decl (var \"y\" (qualified-type \"shared\") (ident \"b\"))) "
                "(decl (var \"w\" (qualified-type \"mut\") (ident \"b\")))"),
           ctx, error);
  ASSERT_NE(hand, nullptr) << error;
  const auto &handStmts = hand->getFuncDecls()[0]->getBody()->getStatements();
  ASSERT_EQ(handStmts.size(), 2u);
  auto *var = ast::cast<ast::VarDecl>(
      ast::cast<ast::DeclStmt>(handStmts[0])->getDecl());
  auto *qt = ast::cast<ast::QualifiedType>(var->getType());
  EXPECT_EQ(qt->getQualifier().Form, ast::QualForm::Shared);
  EXPECT_EQ(qt->getInnerType(), nullptr);
  auto *mut = ast::cast<ast::QualifiedType>(
      ast::cast<ast::VarDecl>(ast::cast<ast::DeclStmt>(handStmts[1])->getDecl())
          ->getType());
  EXPECT_EQ(mut->getQualifier(), (ast::Qualifier{ast::QualForm::Unique, true}));
  EXPECT_EQ(mut->getInnerType(), nullptr);
}

// `atomic T` is `(atomic-type LOC TYPE)`, on a field, a parameter, a local
// and a result alike; a task's type, `(task-type LOC TYPE)`, which no
// program writes, reads and writes the same way.
TEST(ConcurrencyInterchange, AtomicTypes) {
  parser::ParserDriver driver("recursive-descent");
  ast::TranslationUnit *tu =
      parseProgram(driver,
                   "class S {\n"
                   "  hits: atomic int;\n"
                   "  done: atomic bool;\n"
                   "}\n"
                   "fn f(c: atomic int) -> atomic int {\n"
                   "  a: atomic i8 = 1;\n"
                   "  let w: atomic f32 = 1.5;\n"
                   "  return c;\n"
                   "}\n",
                   "atomic");
  ASSERT_NE(tu, nullptr);
  const std::string text = write(*tu);
  const std::string one = flat(text);
  for (const char *form :
       {"(var @2:3-2:19 \"hits\" (atomic-type @2:9-2:19 (named-type \"int\")) "
        "_)",
        "(var @3:3-3:20 \"done\" (atomic-type @3:9-3:20 (named-type \"bool\")) "
        "_)",
        "(param @5:6-5:19 \"c\" (atomic-type @5:9-5:19 (named-type \"int\")))) "
        "(atomic-type @5:24-5:34 (named-type \"int\")) (block @5:36-5:36",
        "(var @6:3-6:19 \"a\" (atomic-type @6:6-6:15 (named-type \"i8\")) (int "
        "@6:18-6:19 1))",
        "(var @7:3-7:26 \"w\" (atomic-type @7:10-7:20 (named-type \"f32\")) "
        "(float @7:23-7:26 1.5) (let))"})
    EXPECT_NE(one.find(form), std::string::npos) << form << "\n" << text;
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *back = read(text, ctx, error);
  ASSERT_NE(back, nullptr) << error;
  // The inner type is the builtin, as a named type's is.
  const auto &fields = back->getClassDecls()[0]->getFields();
  ASSERT_EQ(fields.size(), 2u);
  EXPECT_EQ(ast::cast<ast::AtomicType>(fields[0]->getType())->getInnerType(),
            ctx.getIntTy());
  EXPECT_EQ(ast::cast<ast::AtomicType>(fields[1]->getType())->getInnerType(),
            ctx.getBoolTy());
  auto *f = back->getFuncDecls()[0];
  EXPECT_EQ(
      ast::cast<ast::AtomicType>(f->getParams()[0].ParamType)->getInnerType(),
      ctx.getIntTy());
  EXPECT_EQ(ast::cast<ast::AtomicType>(f->getReturnType())->getInnerType(),
            ctx.getIntTy());
  const std::string d = dump(back);
  EXPECT_NE(d.find("AtomicType <5:24-5:34>\n"), std::string::npos) << d;
  EXPECT_EQ(d, dump(tu));
  EXPECT_EQ(write(*back), text);

  // Without locations; and a task's type.
  ast::TranslationUnit *hand =
      read(fnOf("(decl (var \"n\" (atomic-type (named-type \"int\")) (int 0))) "
                "(decl (var \"t\" (task-type (named-type \"int\")) (ident "
                "\"x\")))"),
           ctx, error);
  ASSERT_NE(hand, nullptr) << error;
  const std::string handDump = dump(hand);
  EXPECT_NE(handDump.find("AtomicType\n"), std::string::npos) << handDump;
  EXPECT_NE(handDump.find("TaskType\n"), std::string::npos) << handDump;
  const std::string again = write(*hand);
  EXPECT_NE(again.find("(task-type\n"), std::string::npos) << again;
  ast::ASTContext ctx2;
  ast::TranslationUnit *back2 = read(again, ctx2, error);
  ASSERT_NE(back2, nullptr) << error;
  EXPECT_EQ(dump(back2), handDump);

  struct Case {
    const char *Type;
    const char *Error;
  };
  for (const Case &c :
       {Case{"(atomic-type)", "(atomic-type ...) is missing the inner type"},
        Case{"(atomic-type 3)", "expected a list: the inner type"},
        Case{"(atomic-type (int 1))", "(int ...) is not a type"},
        Case{"(atomic-type (named-type \"int\") (named-type \"int\"))",
             "unexpected field in (atomic-type ...)"},
        Case{"(task-type)", "(task-type ...) is missing the inner type"}})
    expectRejected(paramOf(c.Type), c.Error);
}

// `move x`, `move x.f` and `await t` are `(unary LOC move EXPR)` and
// `(unary LOC await EXPR)`, operators beside `neg`, `not` and `bitnot`.
TEST(ConcurrencyInterchange, MoveAndAwait) {
  parser::ParserDriver driver("recursive-descent");
  ast::TranslationUnit *tu =
      parseProgram(driver,
                   "async fn g(n: Node) -> int { return 1; }\n"
                   "async fn f(a: Node, h: Holder) -> int {\n"
                   "  let t = g(move a);\n"
                   "  k = move h.job;\n"
                   "  n = await t + 1;\n"
                   "  return await g(move k);\n"
                   "}\n",
                   "move");
  ASSERT_NE(tu, nullptr);
  const std::string text = write(*tu);
  const std::string one = flat(text);
  for (const char *form :
       {"(call @3:11-3:20 \"g\" (type-args) (unary @3:13-3:19 move (ident "
        "@3:18-3:19 \"a\")))",
        "(assign @4:3-4:18 (ident @4:3-4:4 \"k\") (unary @4:7-4:17 move "
        "(member @4:12-4:17 (ident @4:12-4:13 \"h\") \"job\")))",
        // `await t + 1` is `(await t) + 1`.
        "(binary @5:7-5:18 add (unary @5:7-5:14 await (ident @5:13-5:14 "
        "\"t\")) (int @5:17-5:18 1))",
        "(return @6:3-6:26 (unary @6:10-6:25 await (call @6:16-6:25 \"g\" "
        "(type-args) (unary @6:18-6:24 move (ident @6:23-6:24 \"k\")))))"})
    EXPECT_NE(one.find(form), std::string::npos) << form << "\n" << text;
  ast::ASTContext ctx;
  std::string error;
  ast::TranslationUnit *back = read(text, ctx, error);
  ASSERT_NE(back, nullptr) << error;
  const auto &stmts = back->getFuncDecls()[1]->getBody()->getStatements();
  ASSERT_EQ(stmts.size(), 4u);
  auto *t =
      ast::cast<ast::VarDecl>(ast::cast<ast::DeclStmt>(stmts[0])->getDecl());
  auto *moved = ast::cast<ast::UnaryExpr>(
      ast::cast<ast::CallExpr>(t->getInitExpr())->getArguments()[0]);
  EXPECT_EQ(moved->getOpcode(), ast::UnaryOpcode::Move);
  EXPECT_EQ(ast::cast<ast::Identifier>(moved->getOperand())->getName(), "a");
  auto *partial = ast::cast<ast::UnaryExpr>(
      ast::cast<ast::AssignStmt>(stmts[1])->getValue());
  EXPECT_EQ(partial->getOpcode(), ast::UnaryOpcode::Move);
  EXPECT_TRUE(ast::isa<ast::MemberAccessExpr>(partial->getOperand()));
  auto *awaited = ast::cast<ast::UnaryExpr>(
      ast::cast<ast::ReturnStmt>(stmts[3])->getReturnValue());
  EXPECT_EQ(awaited->getOpcode(), ast::UnaryOpcode::Await);
  EXPECT_TRUE(ast::isa<ast::CallExpr>(awaited->getOperand()));
  const std::string d = dump(back);
  EXPECT_NE(d.find("UnaryExpr <3:13-3:19> 'move'\n"), std::string::npos) << d;
  EXPECT_NE(d.find("UnaryExpr <5:7-5:14> 'await'\n"), std::string::npos) << d;
  EXPECT_EQ(d, dump(tu));
  EXPECT_EQ(write(*back), text);

  // Without locations.
  ast::TranslationUnit *hand =
      read(fnOf("(expr (unary move (ident \"x\"))) (expr (unary await (call "
                "\"g\" (type-args) (unary move (member (ident \"h\") "
                "\"job\")))))"),
           ctx, error);
  ASSERT_NE(hand, nullptr) << error;

  struct Case {
    const char *Expr;
    const char *Error;
  };
  for (const Case &c :
       {// `mov` is not an operator (it was removed before `move` came).
        Case{"(unary mov (ident \"x\"))", "unknown unary operator 'mov'"},
        Case{"(unary \"move\" (ident \"x\"))",
             "expected a symbol: the operator"},
        Case{"(unary move)", "(unary ...) is missing the operand"},
        Case{"(unary await (ident \"t\") (ident \"u\"))",
             "unexpected field in (unary ...)"},
        // They are unary operators, not nodes of their own.
        Case{"(move (ident \"x\"))", "(move ...) is not an expression"},
        Case{"(await (ident \"t\"))", "(await ...) is not an expression"}})
    expectRejected(fnOf("(expr " + std::string(c.Expr) + ")"), c.Error);
}
