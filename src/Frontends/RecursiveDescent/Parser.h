// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Recursive-descent parser (precedence climbing for expressions) for the
// Paykan language.  Implements docs/grammar.md; any other frontend must
// build the same AST, node for node and location for location.

#pragma once

#include "AST.h"
#include "ASTContext.h"
#include "DiagEngine.h"
#include "Lexer.h"
#include "paykan/frontends/RecursiveDescent.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace paykan::frontend::recursive_descent {

class Parser {
public:
  /// @p source must outlive the parser (tokens view into it).  Diagnostics
  /// go through @p diags when given, otherwise to stderr.
  Parser(ast::ASTContext &ctx, std::string_view source,
         sema::DiagEngine *diags);

  /// Parse a whole translation unit.  Always returns a TranslationUnit (a
  /// partial one after errors); check getErrorCount() for success.
  ast::TranslationUnit *parseTranslationUnit();

  unsigned getErrorCount() const { return ErrorCount; }

  static constexpr size_t kNoToken = static_cast<size_t>(-1);
  /// Report the held-back lexical errors up to token index @p upTo
  /// (kNoToken: all of them).
  void flushLexErrors(size_t upTo);

private:
  ast::ASTContext &Ctx;
  sema::DiagEngine *Diags;
  Lexer TheLexer;
  unsigned ErrorCount = 0;

  // -- Token stream with unbounded lookahead and backtracking
  std::vector<Token> Buf; // tokens scanned so far
  size_t Pos = 0;         // index of the current token in Buf
  // End location of the most recently consumed token; the end of every
  // composite node (see docs/grammar.md, "Source locations").
  ast::SourceLocation PrevEnd;

  const Token &peek(size_t k = 0);
  const Token &cur() { return peek(0); }
  Tok kind(size_t k = 0) { return peek(k).Kind; }
  bool at(Tok k) { return kind() == k; }
  Token consume();
  bool accept(Tok k);
  /// Consume @p k or report "expected X" (unless speculating) and return false.
  bool expect(Tok k, const char *context = nullptr);
  /// expect(Tok::RBrace) for a brace opened at @p open; a reported error
  /// gets a note pointing at the unclosed `{`.
  bool expectCloseBrace(const char *context, ast::SourceLocation open);

  // -- Speculative parsing
  // While Speculating > 0 no diagnostic is emitted; a failed attempt rewinds
  // Pos.  Used for the one decision that needs unbounded lookahead:
  // `name < ... > (` (generic call) versus `name < expr` (comparison).
  unsigned Speculating = 0;
  unsigned Depth = 0; // nesting depth guard (see enterNesting)
  bool enterNesting();
  void leaveNesting() { --Depth; }

  // -- Diagnostics
  void error(ast::SourceLocation loc, const std::string &msg);
  /// Count and emit one error (error() minus the speculation check and the
  /// flush of pending lexical errors).
  void report(ast::SourceLocation loc, const std::string &msg);
  /// Report "unexpected <current token>; <expected>".  Returns false, and
  /// reports nothing, when the error would only be a follow-on: the current
  /// token is the one right after a lexical error (the lexer reported the
  /// real problem there), or a syntax error was already reported at it.
  bool errorAtCurrent(const std::string &expected);
  /// Buf indices lexed right after a lexical error, in order.
  std::vector<size_t> LexErrorTokens;
  /// Lexical errors not reported yet, in source order: each is reported when
  /// the parser reaches its token (Token) or reports an error there or
  /// later, and every one by the end of the parse.
  struct PendingLexError {
    size_t Token;
    ast::SourceLocation Loc;
    std::string Msg;
  };
  std::vector<PendingLexError> PendingLexErrors;
  size_t LastSyntaxError = kNoToken; // Buf index of the last syntax error
  ast::SourceLocation span(ast::SourceLocation from) const {
    return ast::SourceLocation(from.getLineStart(), from.getColumnStart(),
                               PrevEnd.getLineEnd(), PrevEnd.getColumnEnd());
  }
  const std::string &intern(std::string_view s) {
    return Ctx.intern(std::string(s));
  }

  // -- Error recovery
  void skipToTopLevelBoundary();
  void skipToMemberBoundary();
  void skipToStatementBoundary();
  /// Inside a block whose '{' was consumed: skip through its matching '}'.
  void skipPastMatchingBrace();

  // -- Declarations
  ast::ImportDecl *parseImportDecl();
  bool parseImportedModuleList(
      std::vector<std::pair<std::string, std::string>> &out);
  bool parseModulePath(std::string &out);
  ast::ClassDecl *parseClassDecl();
  ast::EnumDecl *parseEnumDecl();
  ast::FuncDecl *parseFuncDecl();
  bool parseTypeParamList(std::vector<const std::string *> &out);
  bool parseParamList(std::vector<ast::Param> &out);
  ast::VarDecl *parseVarDecl();

  // -- Statements
  ast::CompoundStmt *parseBlock();
  void parseStatementsUntilBrace(ast::CompoundStmt *into);
  /// Returns false on error.  @p out is null for an empty statement `;`.
  bool parseStatement(ast::Stmt *&out);
  bool parseExprOrAssignStatement(ast::Stmt *&out);
  bool
  parseDestructureStatement(ast::SourceLocation start,
                            std::vector<ast::DestructureStmt::Target> targets,
                            ast::Stmt *&out);
  bool parseDestructureTarget(ast::DestructureStmt::Target &out);
  ast::Stmt *parseIfStmt();
  ast::Stmt *parseWhileStmt();
  ast::Stmt *parseMatchStmt();
  ast::MatchArm *parseMatchArm();
  bool ifStartsStatement();

  // -- Types
  ast::Type *parseTypeAnnotation();
  ast::Type *parsePrimaryType();
  bool parseTypeArgList(std::vector<ast::Type *> &out);

  // -- Expressions
  ast::Expr *parseExpression();
  ast::Expr *parseTernary();
  ast::Expr *parseBinary(int minPrec);
  ast::Expr *parseUnary();
  ast::Expr *parsePostfix();
  ast::Expr *parsePrimary();
  ast::Expr *parseLiteral();
  bool parseArgumentList(Tok closer, std::vector<ast::Expr *> &out);
  /// `< typeArgList > (`: on success the type arguments are in @p out and
  /// the current token is the `(`; otherwise the position is rewound.
  bool tryGenericCallArgs(std::vector<ast::Type *> &out);
};

} // namespace paykan::frontend::recursive_descent
