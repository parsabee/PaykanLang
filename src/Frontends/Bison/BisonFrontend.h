// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The Bison/Flex frontend plugin: internals shared by the grammar actions,
// the scanner and BisonFrontend.cpp.  Private to this library.

#pragma once

#include "AST.h"
#include "ASTContext.h"
#include "DiagEngine.h"
#include "Parser.ypp.h"
#include "paykan/Frontend.h"

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace paykan::frontend::bison {
class BisonFrontend;
}

// Flex needs this macro for our custom driver.  The scanner itself is exposed
// as yylex_raw; the parser calls yylex (BisonFrontend.cpp), a thin wrapper
// that adds the one token of context the LALR(1) grammar cannot express:
// whether a '<' after an identifier opens a type-argument list (see TYPELESS).
#define YY_DECL                                                                \
  yy::parser::symbol_type yylex_raw(paykan::frontend::bison::BisonFrontend &drv)
YY_DECL;

/// Parser entry point: yylex_raw plus type-argument disambiguation.
yy::parser::symbol_type yylex(paykan::frontend::bison::BisonFrontend &drv);

using namespace paykan::ast;

namespace paykan::frontend::bison {

/// The Bison/Flex implementation of the frontend interface.  One parse at a
/// time per instance; the members are the state the generated parser and
/// scanner actions reach through `drv`.
class BisonFrontend : public Frontend {
public:
  std::string_view name() const override { return "bison"; }

  ParseResult parse(std::string_view filename, std::string_view source,
                    ast::ASTContext &ctx, sema::DiagEngine &diag,
                    const Options &opts) override;

  // -- State used by the generated parser and scanner ----------------------

  /// Arena that owns all AST nodes created during parsing (the caller's).
  ast::ASTContext *Ctx = nullptr;

  /// Root of the parsed AST (owned by *Ctx).
  TranslationUnit *Root = nullptr;

  /// The token's location used by the scanner.
  yy::location Location;

  /// The name of the file being parsed (Location points at it).
  std::string CurFile;

  /// Number of syntax errors encountered during parsing.
  unsigned ErrorCount = 0;

  /// Diagnostic engine every syntax error is routed through.
  sema::DiagEngine *Diags = nullptr;

  /// Whether to generate parser / scanner debug traces.
  bool TraceParsing = false;
  bool TraceScanning = false;

  /// Tokens already scanned ahead by the yylex wrapper (see
  /// BisonFrontend.cpp) but not yet handed to the parser, in source order.
  std::deque<yy::parser::symbol_type> Lookahead;
  /// True when the token most recently handed to the parser was an IDENT.
  bool PrevWasIdent = false;
  /// Set by the yylex wrapper for a '<' after an identifier: whether its
  /// look-ahead scan found the matching '>' (a type-argument list).
  bool LessIsTypeArgs = false;

  /// The nesting tracker (BisonFrontend.cpp): enforces frontend::kMaxNesting
  /// over the token stream the parser reads, counting what the
  /// recursive-descent parser counts.  One NestLevel per open bracket (the
  /// file itself is the outermost).
  struct NestLevel {
    yy::parser::symbol_kind_type Closer; ///< the token that closes it
    unsigned Weight; ///< 1 for a bracket, 0 for an if/while condition
    unsigned Prefix; ///< prefix operators of the operand being read
    /// Conditional expressions open at this level, innermost last: 'c' in
    /// the condition, 't' in the then-branch, 'e' in the else-branch.
    std::string Ternaries;
  };
  std::vector<NestLevel> Nesting;
  /// Sum of the levels' Weight + Prefix + Ternaries.size().
  unsigned NestDepth = 0;
  /// The kind of the previous token (S_YYEOF before the first), and whether
  /// it was an `else` of an if statement.
  yy::parser::symbol_kind_type PrevKind = yy::parser::symbol_kind::S_YYEOF;
  bool PrevWasStmtElse = false;
  /// Whether the last `if` began a conditional expression.
  bool LastIfWasTernary = false;
  /// Thrown by the yylex wrapper when the input nests too deeply; parse()
  /// reports it and gives up (Bison's error recovery cannot resynchronise
  /// inside such a construct).
  struct NestingTooDeep {
    yy::location Loc;
  };
  /// Account for @p tok, the next token the parser reads; throws
  /// NestingTooDeep past the limit.
  void trackNesting(const yy::parser::symbol_type &tok);

  /// Scanner setup over @p source (Lexer.lpp) and teardown.
  void scanBegin(std::string_view source);
  void scanEnd();
};

/// Build the ImportDecl for `import [::]a::b::name [as alias];`.  The path is
/// split at its last "::" into the base path ("a::b", empty when there is no
/// separator) and the module name ("name").
inline ImportDecl *makeSingleImport(ASTContext &C, SourceLocation loc,
                                    const std::string &path,
                                    const std::string &alias, bool isSystem) {
  auto sep = path.rfind("::");
  std::string base = (sep == std::string::npos) ? "" : path.substr(0, sep);
  std::string name = (sep == std::string::npos) ? path : path.substr(sep + 2);
  return C.make<ImportDecl>(
      loc, C.intern(base), isSystem,
      std::vector<ImportDecl::Module>{{&C.intern(name), &C.intern(alias)}});
}

/// Build the ImportDecl for `import [::]base::{a, b as c};` (base may be
/// empty for `import ::{...};`).  Each (name, alias) pair becomes one module.
inline ImportDecl *
makeImportList(ASTContext &C, SourceLocation loc, const std::string &base,
               const std::vector<std::pair<std::string, std::string>> &mods,
               bool isSystem) {
  std::vector<ImportDecl::Module> ms;
  ms.reserve(mods.size());
  for (const auto &[name, alias] : mods)
    ms.push_back({&C.intern(name), &C.intern(alias)});
  return C.make<ImportDecl>(loc, C.intern(base), isSystem, std::move(ms));
}

} // namespace paykan::frontend::bison
