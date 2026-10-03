// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "Parser.h"
#include "paykan/Frontend.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace paykan::frontend::recursive_descent {

using namespace paykan::ast;

namespace {

// The nesting limit is frontend::kMaxNesting.  Recursive descent uses the
// native stack, so pathological inputs (a fuzzer's `((((((...`) are rejected
// with a diagnostic instead of overflowing it.  One level costs under 2 KiB of
// stack in an unoptimised build (a few frames per level), so 512 levels stay
// well inside the 8 MiB main-thread stack even with a sanitizer's larger
// frames.

/// Build the ImportDecl for `import [::]a::b::name [as alias];`.  The path is
/// split at its last "::" into the base path ("a::b", empty when there is no
/// separator) and the module name ("name").
ImportDecl *makeSingleImport(ASTContext &C, SourceLocation loc,
                             const std::string &path, const std::string &alias,
                             bool isSystem) {
  auto sep = path.rfind("::");
  std::string base = (sep == std::string::npos) ? "" : path.substr(0, sep);
  std::string name = (sep == std::string::npos) ? path : path.substr(sep + 2);
  return C.make<ImportDecl>(
      loc, C.intern(base), isSystem,
      std::vector<ImportDecl::Module>{{&C.intern(name), &C.intern(alias)}});
}

/// Build the ImportDecl for `import [::]base::{a, b as c};` (base may be
/// empty for `import ::{...};`).
ImportDecl *
makeImportList(ASTContext &C, SourceLocation loc, const std::string &base,
               const std::vector<std::pair<std::string, std::string>> &mods,
               bool isSystem) {
  std::vector<ImportDecl::Module> ms;
  ms.reserve(mods.size());
  for (const auto &[name, alias] : mods)
    ms.push_back({&C.intern(name), &C.intern(alias)});
  return C.make<ImportDecl>(loc, C.intern(base), isSystem, std::move(ms));
}

bool isRelational(Tok k) {
  return k == Tok::Less || k == Tok::Greater || k == Tok::LessEq ||
         k == Tok::GreaterEq || k == Tok::EqEq || k == Tok::NotEq;
}

// Binary operator precedence (higher binds tighter); 0 = not a binary
// operator.  Relational operators are non-associative (see parseBinary).
int binaryPrecedence(Tok k) {
  switch (k) {
  case Tok::OrOr:
    return 1;
  case Tok::AndAnd:
    return 2;
  case Tok::Less:
  case Tok::Greater:
  case Tok::LessEq:
  case Tok::GreaterEq:
  case Tok::EqEq:
  case Tok::NotEq:
    return 3;
  case Tok::Plus:
  case Tok::Minus:
    return 4;
  case Tok::Star:
  case Tok::Slash:
  case Tok::Percent:
    return 5;
  default:
    return 0;
  }
}

BinaryOpcode binaryOpcode(Tok k) {
  switch (k) {
  case Tok::OrOr:
    return BinaryOpcode::Or;
  case Tok::AndAnd:
    return BinaryOpcode::And;
  case Tok::Less:
    return BinaryOpcode::Lt;
  case Tok::Greater:
    return BinaryOpcode::Gt;
  case Tok::LessEq:
    return BinaryOpcode::Le;
  case Tok::GreaterEq:
    return BinaryOpcode::Ge;
  case Tok::EqEq:
    return BinaryOpcode::Eq;
  case Tok::NotEq:
    return BinaryOpcode::Ne;
  case Tok::Plus:
    return BinaryOpcode::Add;
  case Tok::Minus:
    return BinaryOpcode::Sub;
  case Tok::Star:
    return BinaryOpcode::Mul;
  case Tok::Slash:
    return BinaryOpcode::Div;
  default:
    return BinaryOpcode::Mod;
  }
}

bool isLiteralToken(Tok k) {
  return k == Tok::Int || k == Tok::Float || k == Tok::KwTrue ||
         k == Tok::KwFalse || k == Tok::Char || k == Tok::String ||
         k == Tok::KwNone;
}

} // namespace

// -- Construction -----------------------------------------------------------

Parser::Parser(ASTContext &ctx, std::string_view source,
               sema::DiagEngine *diags)
    : Ctx(ctx), Diags(diags),
      Lex(source, [this](SourceLocation loc, const std::string &msg) {
        // Lexical errors are real whatever the parser is doing, including
        // while it scans ahead speculatively: report them unconditionally.
        // The token being lexed now (the next one in the buffer) is the
        // first one after the bad text, which is dropped (an out-of-range
        // integer is kept, as 0): a syntax error there is a follow-on.
        // The lexer runs ahead of the parser (lookahead, speculation), so
        // the error is held until the parser reaches that token, keeping
        // diagnostics in source order (flushLexErrors).
        LexErrorTokens.push_back(Buf.size());
        PendingLexErrors.push_back({Buf.size(), loc, msg});
      }) {}

void Parser::flushLexErrors(size_t upTo) {
  size_t n = 0;
  for (; n < PendingLexErrors.size() && PendingLexErrors[n].Token <= upTo; ++n)
    report(PendingLexErrors[n].Loc, PendingLexErrors[n].Msg);
  PendingLexErrors.erase(PendingLexErrors.begin(),
                         PendingLexErrors.begin() + static_cast<long>(n));
}

ParseOutput parseSource(ASTContext &ctx, std::string_view source,
                        sema::DiagEngine *diags) {
  Parser p(ctx, source, diags);
  ParseOutput out;
  out.Root = p.parseTranslationUnit();
  p.flushLexErrors(Parser::kNoToken); // whatever the parser did not reach
  out.ErrorCount = p.getErrorCount();
  return out;
}

unsigned dumpTokens(std::string_view source, std::ostream &os,
                    sema::DiagEngine *diags) {
  unsigned errors = 0;
  Lexer lex(source, [&](SourceLocation loc, const std::string &msg) {
    ++errors;
    if (diags)
      diags->error(loc, msg);
    else
      std::cerr << "error @" << loc.getLineStart() << "."
                << loc.getColumnStart() << ": " << msg << "\n";
  });
  for (;;) {
    Token t = lex.next();
    os << t.Loc.getLineStart() << ":" << t.Loc.getColumnStart() << "-"
       << t.Loc.getLineEnd() << ":" << t.Loc.getColumnEnd() << " "
       << tokenKindName(t.Kind);
    if (!t.Text.empty())
      os << " " << t.Text;
    os << "\n";
    if (t.Kind == Tok::Eof)
      return errors;
  }
}

// -- Token stream -------------------------------------------------------------

const Token &Parser::peek(size_t k) {
  while (Pos + k >= Buf.size()) {
    if (!Buf.empty() && Buf.back().Kind == Tok::Eof)
      return Buf.back();
    Buf.push_back(Lex.next());
  }
  return Buf[Pos + k];
}

Token Parser::consume() {
  Token t = peek();
  if (t.Kind != Tok::Eof) {
    PrevEnd = t.Loc;
    ++Pos;
  }
  return t;
}

bool Parser::accept(Tok k) {
  if (!at(k))
    return false;
  consume();
  return true;
}

bool Parser::expect(Tok k, const char *context) {
  if (accept(k))
    return true;
  std::string msg = std::string("expected ") + describe(k);
  if (context)
    msg += std::string(" ") + context;
  errorAtCurrent(msg);
  return false;
}

bool Parser::expectCloseBrace(const char *context, SourceLocation open) {
  if (accept(Tok::RBrace))
    return true;
  if (errorAtCurrent(std::string("expected '}' ") + context) && Diags)
    Diags->note(open, "to match this '{'");
  return false;
}

bool Parser::enterNesting() {
  if (++Depth > kMaxNesting) {
    error(cur().Loc, "nesting too deep (more than " +
                         std::to_string(kMaxNesting) + " levels)");
    --Depth;
    return false;
  }
  return true;
}

// -- Diagnostics
// ---------------------------------------------------------------

void Parser::error(SourceLocation loc, const std::string &msg) {
  if (Speculating)
    return;
  flushLexErrors(Pos); // the lexical errors before this point come first
  report(loc, msg);
}

void Parser::report(SourceLocation loc, const std::string &msg) {
  ++ErrorCount;
  if (Diags) {
    Diags->error(loc, msg);
  } else {
    std::cerr << "error @" << loc.getLineStart() << "." << loc.getColumnStart()
              << "-" << loc.getLineEnd() << "." << loc.getColumnEnd() << ": "
              << msg << "\n";
  }
}

bool Parser::errorAtCurrent(const std::string &expected) {
  if (Speculating)
    return false;
  const Token &t = cur();
  // Each error recovery resumes at a later token, so a second syntax error
  // at the same token (an unclosed block at end of file, reported once per
  // open block) and one right after a lexical error (where the dropped text
  // left a hole in the token stream) are follow-ons of the first.  The file
  // has failed already either way.
  if (Pos == LastSyntaxError ||
      std::find(LexErrorTokens.begin(), LexErrorTokens.end(), Pos) !=
          LexErrorTokens.end())
    return false;
  LastSyntaxError = Pos;
  std::string found = describe(t.Kind);
  if (t.Kind == Tok::Ident)
    found += " '" + std::string(t.Text) + "'";
  error(t.Loc, "unexpected " + found + "; " + expected);
  return true;
}

// -- Error recovery
// ------------------------------------------------------------
//
// Each level skips to a token that can start its next construct, matching
// braces on the way so that a broken nested block does not swallow the rest
// of the enclosing one.

void Parser::skipToTopLevelBoundary() {
  unsigned depth = 0;
  for (;;) {
    switch (kind()) {
    case Tok::Eof:
      return;
    case Tok::KwFn:
    case Tok::KwClass:
    case Tok::KwEnum:
    case Tok::KwImport:
      if (depth == 0)
        return;
      break;
    case Tok::LBrace:
      ++depth;
      break;
    case Tok::RBrace:
      if (depth > 0)
        --depth;
      break;
    default:
      break;
    }
    consume();
  }
}

void Parser::skipToMemberBoundary() {
  unsigned depth = 0;
  for (;;) {
    switch (kind()) {
    case Tok::Eof:
      return;
    case Tok::KwFn:
      if (depth == 0)
        return;
      break;
    case Tok::Semi:
      if (depth == 0) {
        consume();
        return;
      }
      break;
    case Tok::LBrace:
      ++depth;
      break;
    case Tok::RBrace:
      if (depth == 0)
        return;
      --depth;
      break;
    default:
      break;
    }
    consume();
  }
}

void Parser::skipToStatementBoundary() {
  unsigned depth = 0;
  for (;;) {
    switch (kind()) {
    case Tok::Eof:
      return;
    case Tok::Semi:
      if (depth == 0) {
        consume();
        return;
      }
      break;
    case Tok::LBrace:
      ++depth;
      break;
    case Tok::RBrace:
      if (depth == 0)
        return;
      --depth;
      break;
    default:
      break;
    }
    consume();
  }
}

// -- Translation unit
// -----------------------------------------------------------

TranslationUnit *Parser::parseTranslationUnit() {
  std::vector<ImportDecl *> imports;
  std::vector<ClassDecl *> classes, genericClasses;
  std::vector<FuncDecl *> funcs, genericFuncs;
  std::vector<EnumDecl *> enums;
  // The unit's location starts at 1:1 and ends with its last declaration;
  // an empty unit is <1:1-1:1>.
  SourceLocation loc(1, 1, 1, 1);

  while (!at(Tok::Eof)) {
    bool ok = true;
    size_t before = Pos;
    switch (kind()) {
    case Tok::KwImport:
      if (auto *d = parseImportDecl())
        imports.push_back(d);
      else
        ok = false;
      break;
    case Tok::KwClass:
      if (auto *d = parseClassDecl())
        (d->isGeneric() ? genericClasses : classes).push_back(d);
      else
        ok = false;
      break;
    case Tok::KwFn:
      if (auto *d = parseFuncDecl())
        (d->isGeneric() ? genericFuncs : funcs).push_back(d);
      else
        ok = false;
      break;
    case Tok::KwEnum:
      if (auto *d = parseEnumDecl())
        enums.push_back(d);
      else
        ok = false;
      break;
    default:
      errorAtCurrent("expected 'import', 'class', 'enum' or 'fn' at top level");
      ok = false;
      break;
    }
    if (!ok) {
      if (Pos == before)
        consume(); // guarantee progress
      skipToTopLevelBoundary();
    } else {
      loc = span(loc);
    }
  }

  return Ctx.make<TranslationUnit>(
      loc, std::move(imports), std::move(classes), std::move(funcs),
      std::move(enums), std::move(genericClasses), std::move(genericFuncs));
}

// -- Imports
// -----------------------------------------------------------------------
//
// importDecl ::= "import" "::"? modulePath ( "as" IDENT | "::" "{" list "}" )?
// ";"
//              | "import" "::" "{" list "}" ";"

bool Parser::parseModulePath(std::string &out) {
  if (!at(Tok::Ident)) {
    errorAtCurrent("expected a module name");
    return false;
  }
  out = std::string(consume().Text);
  while (at(Tok::ColonColon) && kind(1) == Tok::Ident) {
    consume();
    out += "::";
    out += std::string(consume().Text);
  }
  return true;
}

bool Parser::parseImportedModuleList(
    std::vector<std::pair<std::string, std::string>> &out) {
  do {
    if (!at(Tok::Ident)) {
      errorAtCurrent("expected an imported module name");
      return false;
    }
    std::string name(consume().Text);
    std::string alias;
    if (accept(Tok::KwAs)) {
      if (!at(Tok::Ident)) {
        errorAtCurrent("expected an alias after 'as'");
        return false;
      }
      alias = std::string(consume().Text);
    }
    out.emplace_back(std::move(name), std::move(alias));
  } while (accept(Tok::Comma));
  return true;
}

ImportDecl *Parser::parseImportDecl() {
  SourceLocation start = consume().Loc; // "import"
  bool isSystem = accept(Tok::ColonColon);

  if (isSystem && at(Tok::LBrace)) {
    consume();
    std::vector<std::pair<std::string, std::string>> mods;
    if (!parseImportedModuleList(mods) || !expect(Tok::RBrace) ||
        !expect(Tok::Semi, "after import"))
      return nullptr;
    return makeImportList(Ctx, span(start), "", mods, true);
  }

  std::string path;
  if (!parseModulePath(path))
    return nullptr;

  if (accept(Tok::KwAs)) {
    if (!at(Tok::Ident)) {
      errorAtCurrent("expected an alias after 'as'");
      return nullptr;
    }
    std::string alias(consume().Text);
    if (!expect(Tok::Semi, "after import"))
      return nullptr;
    return makeSingleImport(Ctx, span(start), path, alias, isSystem);
  }

  if (at(Tok::ColonColon) && kind(1) == Tok::LBrace) {
    consume();
    consume();
    std::vector<std::pair<std::string, std::string>> mods;
    if (!parseImportedModuleList(mods) || !expect(Tok::RBrace) ||
        !expect(Tok::Semi, "after import"))
      return nullptr;
    return makeImportList(Ctx, span(start), path, mods, isSystem);
  }

  if (!expect(Tok::Semi, "after import"))
    return nullptr;
  return makeSingleImport(Ctx, span(start), path, "", isSystem);
}

// -- Classes and enums
// ---------------------------------------------------------------
//
// classDecl ::= "class" IDENT ( "<" typeParamList ">" )? ( ":" modulePath )?
//               "{" classMember* "}"
// classMember ::= varDecl ";" | funcDecl

bool Parser::parseTypeParamList(std::vector<const std::string *> &out) {
  do {
    if (!at(Tok::Ident)) {
      errorAtCurrent("expected a type parameter name");
      return false;
    }
    out.push_back(&intern(consume().Text));
  } while (accept(Tok::Comma));
  return true;
}

VarDecl *Parser::parseVarDecl() {
  SourceLocation start = cur().Loc;
  if (!at(Tok::Ident)) {
    errorAtCurrent("expected a name");
    return nullptr;
  }
  const std::string &name = intern(consume().Text);
  if (!expect(Tok::Colon, "after the name (a declaration needs a type)"))
    return nullptr;
  Type *ty = parseTypeAnnotation();
  if (!ty)
    return nullptr;
  return Ctx.make<VarDecl>(span(start), name, ty, nullptr);
}

ClassDecl *Parser::parseClassDecl() {
  SourceLocation start = consume().Loc; // "class"
  if (!at(Tok::Ident)) {
    errorAtCurrent("expected a class name after 'class'");
    return nullptr;
  }
  const std::string &name = intern(consume().Text);

  std::vector<const std::string *> typeParams;
  if (accept(Tok::Less)) {
    if (!parseTypeParamList(typeParams) ||
        !expect(Tok::Greater, "to close the type parameter list"))
      return nullptr;
  }

  std::string superName;
  if (accept(Tok::Colon)) {
    if (!parseModulePath(superName))
      return nullptr;
  }

  SourceLocation open = cur().Loc;
  if (!expect(Tok::LBrace, "to open the class body"))
    return nullptr;
  if (!enterNesting())
    return nullptr;

  ClassBody body;
  bool ok = true;
  while (!at(Tok::RBrace) && !at(Tok::Eof)) {
    size_t before = Pos;
    if (at(Tok::KwFn)) {
      if (auto *m = parseFuncDecl()) {
        body.Methods.push_back(m);
        continue;
      }
    } else if (at(Tok::Ident)) {
      if (auto *f = parseVarDecl()) {
        if (expect(Tok::Semi, "after the field declaration")) {
          body.Fields.push_back(f);
          continue;
        }
      }
    } else {
      errorAtCurrent("expected a field ('name: Type;') or a method ('fn') in "
                     "the class body");
    }
    ok = false;
    if (Pos == before)
      consume(); // guarantee progress
    skipToMemberBoundary();
  }
  leaveNesting();
  if (!expectCloseBrace("to close the class body", open) || !ok)
    return nullptr;
  return Ctx.make<ClassDecl>(span(start), name, intern(superName),
                             std::move(body.Fields), std::move(body.Methods),
                             std::move(typeParams));
}

// enumDecl ::= "enum" IDENT "{" IDENT ( "," IDENT )* ","? "}"
EnumDecl *Parser::parseEnumDecl() {
  SourceLocation start = consume().Loc; // "enum"
  if (!at(Tok::Ident)) {
    errorAtCurrent("expected an enum name after 'enum'");
    return nullptr;
  }
  const std::string &name = intern(consume().Text);
  if (!expect(Tok::LBrace, "to open the enum body"))
    return nullptr;

  std::vector<const std::string *> variants;
  if (!at(Tok::Ident)) {
    errorAtCurrent("expected a variant name (an enum needs at least one)");
    return nullptr;
  }
  variants.push_back(&intern(consume().Text));
  while (accept(Tok::Comma)) {
    if (!at(Tok::Ident))
      break; // trailing comma
    variants.push_back(&intern(consume().Text));
  }
  if (!expect(Tok::RBrace, "to close the enum body"))
    return nullptr;
  return Ctx.make<EnumDecl>(span(start), name, std::move(variants));
}

// -- Functions
// ------------------------------------------------------------------------
//
// funcDecl ::= "fn" IDENT ( "<" typeParamList ">" )? "(" paramList ")"
//              ( "->" typeAnnotation )? block
// paramList ::= ( param ( "," param )* )?
// param ::= IDENT ":" typeAnnotation

bool Parser::parseParamList(std::vector<Param> &out) {
  if (at(Tok::RParen))
    return true;
  do {
    if (!at(Tok::Ident)) {
      errorAtCurrent("expected a parameter name");
      return false;
    }
    const std::string &name = intern(consume().Text);
    if (!expect(Tok::Colon, "after the parameter name"))
      return false;
    Type *ty = parseTypeAnnotation();
    if (!ty)
      return false;
    out.push_back(Param{&name, ty});
  } while (accept(Tok::Comma));
  return true;
}

FuncDecl *Parser::parseFuncDecl() {
  SourceLocation start = consume().Loc; // "fn"
  if (!at(Tok::Ident)) {
    errorAtCurrent("expected a function name after 'fn'");
    return nullptr;
  }
  const std::string &name = intern(consume().Text);

  std::vector<const std::string *> typeParams;
  if (accept(Tok::Less)) {
    if (!parseTypeParamList(typeParams) ||
        !expect(Tok::Greater, "to close the type parameter list"))
      return nullptr;
  }

  if (!expect(Tok::LParen, "after the function name"))
    return nullptr;
  std::vector<Param> params;
  if (!parseParamList(params) ||
      !expect(Tok::RParen, "to close the parameter list"))
    return nullptr;

  Type *retTy = nullptr;
  if (accept(Tok::Arrow)) {
    retTy = parseTypeAnnotation();
    if (!retTy)
      return nullptr;
  }

  CompoundStmt *body = parseBlock();
  if (!body)
    return nullptr;
  return Ctx.make<FuncDecl>(span(start), name, std::move(params), retTy, body,
                            std::move(typeParams));
}

// -- Blocks and statements
// ------------------------------------------------------------

// The CompoundStmt's location is the empty range right after its `{` (the
// Bison frontend's location of an empty production); it is not widened by
// the statements added to it.
CompoundStmt *Parser::parseBlock() {
  if (!at(Tok::LBrace)) {
    errorAtCurrent("expected '{'");
    return nullptr;
  }
  Token lbrace = consume();
  SourceLocation after(lbrace.Loc.getLineEnd(), lbrace.Loc.getColumnEnd(),
                       lbrace.Loc.getLineEnd(), lbrace.Loc.getColumnEnd());
  auto *block = Ctx.make<CompoundStmt>(after);
  if (!enterNesting())
    return nullptr;
  parseStatementsUntilBrace(block);
  leaveNesting();
  if (!expectCloseBrace("to close the block", lbrace.Loc))
    return nullptr;
  return block;
}

void Parser::parseStatementsUntilBrace(CompoundStmt *into) {
  while (!at(Tok::RBrace) && !at(Tok::Eof)) {
    Stmt *s = nullptr;
    size_t before = Pos;
    if (parseStatement(s)) {
      if (s)
        into->addStatement(s);
      continue;
    }
    if (Pos == before)
      consume(); // guarantee progress
    skipToStatementBoundary();
  }
}

// statement ::= ";" | expression ";" | expression "=" expression ";"
//             | varDecl "=" expression ";" | destructure | "return" expr? ";"
//             | block | ifStmt | whileStmt | "break" ";" | "continue" ";"
//             | matchStmt
bool Parser::parseStatement(Stmt *&out) {
  out = nullptr;
  SourceLocation start = cur().Loc;
  switch (kind()) {
  case Tok::Semi:
    consume();
    return true;

  case Tok::KwReturn: {
    consume();
    Expr *value = nullptr;
    if (!at(Tok::Semi)) {
      value = parseExpression();
      if (!value)
        return false;
    }
    if (!expect(Tok::Semi, "after the return statement"))
      return false;
    out = Ctx.make<ReturnStmt>(span(start), value);
    return true;
  }

  case Tok::KwBreak:
    consume();
    if (!expect(Tok::Semi, "after 'break'"))
      return false;
    out = Ctx.make<BreakStmt>(span(start));
    return true;

  case Tok::KwContinue:
    consume();
    if (!expect(Tok::Semi, "after 'continue'"))
      return false;
    out = Ctx.make<ContinueStmt>(span(start));
    return true;

  case Tok::LBrace:
    out = parseBlock();
    return out != nullptr;

  case Tok::KwIf:
    if (ifStartsStatement()) {
      out = parseIfStmt();
      return out != nullptr;
    }
    return parseExprOrAssignStatement(out);

  case Tok::KwWhile:
    out = parseWhileStmt();
    return out != nullptr;

  case Tok::KwMatch:
    out = parseMatchStmt();
    return out != nullptr;

  case Tok::Underscore:
    return parseDestructureStatement(start, {}, out);

  case Tok::Ident: {
    // After a leading name the next token decides: `,` starts a
    // destructuring statement, `:` a typed declaration (which may itself be
    // the first destructuring target), anything else an expression.
    if (kind(1) == Tok::Comma)
      return parseDestructureStatement(start, {}, out);
    if (kind(1) == Tok::Colon) {
      VarDecl *decl = parseVarDecl();
      if (!decl)
        return false;
      if (at(Tok::Comma)) {
        std::vector<DestructureStmt::Target> targets;
        targets.push_back(DestructureStmt::Target{
            &decl->getName(), decl->getType(), decl->getLocation()});
        return parseDestructureStatement(start, std::move(targets), out);
      }
      if (!expect(Tok::Assign, "(a variable declaration needs an initial "
                               "value)"))
        return false;
      Expr *init = parseExpression();
      if (!init)
        return false;
      // The declaration spans name through initializer; the `;` is only part
      // of the statement.
      auto *vd = Ctx.make<VarDecl>(span(start), decl->getName(),
                                   decl->getType(), init);
      auto *ds = Ctx.make<DeclStmt>(span(start), vd);
      if (!expect(Tok::Semi, "after the declaration"))
        return false;
      out = ds;
      return true;
    }
    return parseExprOrAssignStatement(out);
  }

  default:
    return parseExprOrAssignStatement(out);
  }
}

bool Parser::parseExprOrAssignStatement(Stmt *&out) {
  SourceLocation start = cur().Loc;
  Expr *lhs = parseExpression();
  if (!lhs)
    return false;

  if (!accept(Tok::Assign)) {
    if (!expect(Tok::Semi, "after the expression"))
      return false;
    out = Ctx.make<ExprStmt>(span(start), lhs);
    return true;
  }

  // Check the target before the value so that recovery resumes at this
  // statement's `;` rather than the next one's.
  if (!isa<Identifier>(lhs) && !isa<MemberAccessExpr>(lhs) &&
      !isa<SubscriptExpr>(lhs) && !isa<TupleIndexExpr>(lhs)) {
    error(start, "left-hand side of '=' must be an identifier, field access, "
                 "or subscript");
    return false;
  }
  Expr *value = parseExpression();
  if (!value)
    return false;
  if (!expect(Tok::Semi, "after the assignment"))
    return false;
  SourceLocation loc = span(start);
  if (auto *id = dyn_cast<Identifier>(lhs)) {
    out = Ctx.make<AssignStmt>(loc, id, value);
  } else if (auto *ma = dyn_cast<MemberAccessExpr>(lhs)) {
    out = Ctx.make<MemberAssignStmt>(loc, ma->getReceiver(), ma->getFieldName(),
                                     value);
  } else if (auto *se = dyn_cast<SubscriptExpr>(lhs)) {
    out = Ctx.make<SubscriptAssignStmt>(loc, se->getArray(), se->getIndex(),
                                        value);
  } else {
    // `t.0 = v` is well-formed syntax but tuples are immutable: hand it to
    // Sema as a member assignment whose "field" is the index so the
    // rejection is a typed diagnostic rather than a parse error.
    auto *ti = cast<TupleIndexExpr>(lhs);
    out = Ctx.make<MemberAssignStmt>(
        loc, ti->getTuple(), Ctx.intern(std::to_string(ti->getIndex())), value);
  }
  return true;
}

// destructureTarget ::= IDENT | IDENT ":" typeAnnotation | "_"
bool Parser::parseDestructureTarget(DestructureStmt::Target &out) {
  SourceLocation start = cur().Loc;
  if (accept(Tok::Underscore)) {
    out = DestructureStmt::Target{&intern(""), nullptr, start};
    return true;
  }
  if (!at(Tok::Ident)) {
    errorAtCurrent("expected a destructuring target (a name or '_')");
    return false;
  }
  const std::string &name = intern(consume().Text);
  Type *ty = nullptr;
  if (accept(Tok::Colon)) {
    ty = parseTypeAnnotation();
    if (!ty)
      return false;
  }
  out = DestructureStmt::Target{&name, ty, span(start)};
  return true;
}

// destructure ::= target ( "," target )+ "=" expression ";"
// @p targets holds the targets already parsed by the caller (at most one).
bool Parser::parseDestructureStatement(
    SourceLocation start, std::vector<DestructureStmt::Target> targets,
    Stmt *&out) {
  if (targets.empty()) {
    DestructureStmt::Target t;
    if (!parseDestructureTarget(t))
      return false;
    targets.push_back(t);
  }
  if (!expect(Tok::Comma, "(destructuring needs at least two targets)"))
    return false;
  do {
    DestructureStmt::Target t;
    if (!parseDestructureTarget(t))
      return false;
    targets.push_back(t);
  } while (accept(Tok::Comma));

  if (!expect(Tok::Assign, "after the destructuring targets"))
    return false;
  Expr *value = parseExpression();
  if (!value)
    return false;
  if (!expect(Tok::Semi, "after the destructuring statement"))
    return false;
  out = Ctx.make<DestructureStmt>(span(start), std::move(targets), value);
  return true;
}

// A statement starting with `if` is an if-statement when the parenthesised
// condition is followed by `{`; otherwise it is an expression statement whose
// expression is a ternary (`if c then a else b;`).  Scanning to the matching
// `)` costs nothing on the token buffer and avoids speculative parsing.
bool Parser::ifStartsStatement() {
  if (kind(1) != Tok::LParen)
    return false;
  unsigned depth = 0;
  for (size_t i = 1;; ++i) {
    Tok k = kind(i);
    if (k == Tok::Eof)
      return true; // unbalanced: report it as a broken if-statement
    if (k == Tok::LParen) {
      ++depth;
    } else if (k == Tok::RParen) {
      if (--depth == 0)
        return kind(i + 1) == Tok::LBrace;
    }
  }
}

// ifStmt ::= "if" "(" expression ")" block ( "else" ( block | ifStmt ) )?
Stmt *Parser::parseIfStmt() {
  SourceLocation start = consume().Loc; // "if"
  if (!expect(Tok::LParen, "after 'if'"))
    return nullptr;
  Expr *cond = parseExpression();
  if (!cond)
    return nullptr;
  if (!expect(Tok::RParen, "to close the condition"))
    return nullptr;
  CompoundStmt *thenBlock = parseBlock();
  if (!thenBlock)
    return nullptr;
  Stmt *elseBranch = nullptr;
  if (accept(Tok::KwElse)) {
    if (at(Tok::KwIf))
      elseBranch = parseIfStmt();
    else
      elseBranch = parseBlock();
    if (!elseBranch)
      return nullptr;
  }
  return Ctx.make<IfStmt>(span(start), cond, thenBlock, elseBranch);
}

// whileStmt ::= "while" "(" expression ")" block
Stmt *Parser::parseWhileStmt() {
  SourceLocation start = consume().Loc; // "while"
  if (!expect(Tok::LParen, "after 'while'"))
    return nullptr;
  Expr *cond = parseExpression();
  if (!cond)
    return nullptr;
  if (!expect(Tok::RParen, "to close the condition"))
    return nullptr;
  CompoundStmt *body = parseBlock();
  if (!body)
    return nullptr;
  return Ctx.make<WhileStmt>(span(start), cond, body);
}

// matchStmt ::= "match" expression "{" matchArm+ "}"
Stmt *Parser::parseMatchStmt() {
  SourceLocation start = consume().Loc; // "match"
  Expr *subject = parseExpression();
  if (!subject)
    return nullptr;
  SourceLocation open = cur().Loc;
  if (!expect(Tok::LBrace, "after the match subject"))
    return nullptr;
  if (!enterNesting())
    return nullptr;
  std::vector<MatchArm *> arms;
  bool ok = true;
  if (at(Tok::RBrace)) {
    errorAtCurrent("expected a match arm (a match needs at least one)");
    ok = false;
  }
  while (!at(Tok::RBrace) && !at(Tok::Eof)) {
    size_t before = Pos;
    if (MatchArm *arm = parseMatchArm()) {
      arms.push_back(arm);
      continue;
    }
    ok = false;
    if (Pos == before)
      consume();
    skipToMemberBoundary(); // next arm: a `}`-balanced skip is what we need
  }
  leaveNesting();
  if (!expectCloseBrace("to close the match", open) || !ok)
    return nullptr;
  return Ctx.make<MatchStmt>(span(start), subject, std::move(arms));
}

// matchArm ::= typeAnnotation "{" stmts "}" | IDENT ":" typeAnnotation "{" ...
// "}"
//            | literal "{" stmts "}" | "_" "{" stmts "}"
MatchArm *Parser::parseMatchArm() {
  SourceLocation start = cur().Loc;
  const std::string *binding = &intern("");
  Type *armType = nullptr;
  Expr *pattern = nullptr;

  if (accept(Tok::Underscore)) {
    // wildcard
  } else if (isLiteralToken(kind())) {
    pattern = parseLiteral();
  } else {
    if (at(Tok::Ident) && kind(1) == Tok::Colon) {
      binding = &intern(consume().Text);
      consume(); // ':'
    }
    armType = parseTypeAnnotation();
    if (!armType)
      return nullptr;
  }

  CompoundStmt *body = parseBlock();
  if (!body)
    return nullptr;
  if (pattern)
    return Ctx.make<MatchArm>(span(start), *binding, pattern, body);
  return Ctx.make<MatchArm>(span(start), *binding, armType, body);
}

// -- Types
// ------------------------------------------------------------------------------
//
// typeAnnotation ::= primaryType ( "[" "]" | "?" )*
// primaryType ::= IDENT | modulePath "::" IDENT
//               | IDENT "<" typeArgList ">" | modulePath "::" IDENT "<"
//               typeArgList ">" | "(" typeAnnotation ( "," typeAnnotation )+
//               ")"

bool Parser::parseTypeArgList(std::vector<Type *> &out) {
  do {
    Type *t = parseTypeAnnotation();
    if (!t)
      return false;
    out.push_back(t);
  } while (accept(Tok::Comma));
  return true;
}

Type *Parser::parsePrimaryType() {
  SourceLocation start = cur().Loc;

  if (accept(Tok::LParen)) {
    if (!enterNesting())
      return nullptr;
    std::vector<Type *> elems;
    Type *first = parseTypeAnnotation();
    if (!first) {
      leaveNesting();
      return nullptr;
    }
    elems.push_back(first);
    if (!expect(Tok::Comma, "(a tuple type needs at least two element types; "
                            "there are no parenthesised types)")) {
      leaveNesting();
      return nullptr;
    }
    do {
      Type *t = parseTypeAnnotation();
      if (!t) {
        leaveNesting();
        return nullptr;
      }
      elems.push_back(t);
    } while (accept(Tok::Comma));
    leaveNesting();
    if (!expect(Tok::RParen, "to close the tuple type"))
      return nullptr;
    return Ctx.make<TupleType>(span(start), std::move(elems));
  }

  if (!at(Tok::Ident)) {
    errorAtCurrent("expected a type");
    return nullptr;
  }
  std::string name(consume().Text);
  bool qualified = false;
  while (at(Tok::ColonColon) && kind(1) == Tok::Ident) {
    consume();
    name += "::";
    name += std::string(consume().Text);
    qualified = true;
  }

  if (accept(Tok::Less)) {
    if (!enterNesting())
      return nullptr;
    std::vector<Type *> args;
    bool ok = parseTypeArgList(args);
    leaveNesting();
    if (!ok || !expect(Tok::Greater, "to close the type argument list"))
      return nullptr;
    return Ctx.make<GenericType>(span(start), intern(name), std::move(args));
  }

  if (qualified) {
    // module::Type: registered under that exact qualified name by SemaImport;
    // Sema resolves it through lookupType.
    return Ctx.make<ClassType>(span(start), intern(name), nullptr);
  }
  if (Type *ty = Ctx.lookupType(name))
    return ty; // builtin (int, ...) or bootstrap class (Str, Obj, ...)
  // Unknown at parse time: a user class, possibly declared later.  Sema's
  // resolveType validates it.
  return Ctx.make<ClassType>(span(start), intern(name), nullptr);
}

Type *Parser::parseTypeAnnotation() {
  SourceLocation start = cur().Loc;
  Type *ty = parsePrimaryType();
  if (!ty)
    return nullptr;
  for (;;) {
    if (at(Tok::LBracket)) {
      // A `[` after a type can only open the `[]` suffix: report the missing
      // `]` where it is missing (as the Bison grammar does).
      consume();
      if (!expect(Tok::RBracket, "to close the array type"))
        return nullptr;
      ty = Ctx.make<ArrayType>(span(start), ty);
    } else if (at(Tok::Question)) {
      Token q = consume();
      // `void` has no value to make optional; the other builtin value types
      // (`int?`, ...) box through the runtime's boxed classes (an enum
      // spelled `Color?` is only known to Sema, which rejects it there).
      auto *bt = dyn_cast<BuiltinType>(ty);
      if (bt && bt->getTypeKind() == BuiltinType::Void) {
        error(q.Loc, "optional type 'void?' is not supported");
        return nullptr;
      }
      if (isa<OptionalType>(ty)) {
        error(q.Loc,
              "nested optional type '" + typeName(ty) + "?' is not supported");
        return nullptr;
      }
      ty = Ctx.make<OptionalType>(span(start), ty);
    } else {
      return ty;
    }
  }
}

// -- Expressions
// ---------------------------------------------------------------------

Expr *Parser::parseExpression() { return parseTernary(); }

// ternary ::= "if" binary "then" expression "else" ternary | binary
Expr *Parser::parseTernary() {
  if (!at(Tok::KwIf))
    return parseBinary(1);
  if (!enterNesting())
    return nullptr;
  SourceLocation start = consume().Loc; // "if"
  Expr *cond = parseBinary(1);
  Expr *t = nullptr;
  Expr *f = nullptr;
  if (cond && expect(Tok::KwThen, "after the condition"))
    t = parseExpression();
  if (t && expect(Tok::KwElse, "after the 'then' branch"))
    f = parseTernary();
  leaveNesting();
  if (!f)
    return nullptr;
  return Ctx.make<TernaryExpr>(span(start), cond, t, f);
}

// Precedence climbing over the table in binaryPrecedence().  Relational
// operators do not chain: `a < b < c` is a syntax error.
Expr *Parser::parseBinary(int minPrec) {
  SourceLocation start = cur().Loc;
  Expr *lhs = parseUnary();
  if (!lhs)
    return nullptr;
  for (;;) {
    Tok op = kind();
    int prec = binaryPrecedence(op);
    if (prec == 0 || prec < minPrec)
      return lhs;
    consume();
    Expr *rhs = parseBinary(prec + 1);
    if (!rhs)
      return nullptr;
    lhs = Ctx.make<BinaryExpr>(span(start), binaryOpcode(op), lhs, rhs);
    if (isRelational(op) && isRelational(kind())) {
      errorAtCurrent("relational operators cannot be chained; parenthesise "
                     "the comparison");
      return nullptr;
    }
  }
}

// unary ::= ( "!" | "-" | "mov" ) unary | postfix
Expr *Parser::parseUnary() {
  if (at(Tok::Not) || at(Tok::Minus) || at(Tok::KwMov)) {
    if (!enterNesting())
      return nullptr;
    Token op = consume();
    Expr *operand = parseUnary();
    leaveNesting();
    if (!operand)
      return nullptr;
    if (op.Kind == Tok::KwMov)
      return Ctx.make<MovExpr>(span(op.Loc), operand);
    return Ctx.make<UnaryExpr>(
        span(op.Loc), op.Kind == Tok::Not ? UnaryOpcode::Not : UnaryOpcode::Neg,
        operand);
  }
  return parsePostfix();
}

// postfix ::= primary ( "." IDENT ( "(" args ")" )? | "." INT | "[" expr "]" )*
Expr *Parser::parsePostfix() {
  SourceLocation start = cur().Loc;
  Expr *e = parsePrimary();
  if (!e)
    return nullptr;
  for (;;) {
    if (at(Tok::Dot)) {
      Token dot = consume();
      if (at(Tok::Int) && cur().Offset == dot.Offset + 1) {
        // Tuple index `t.0`: the digits glued to the dot.  Leading zeros are
        // rejected so every index has exactly one spelling.
        Token idx = consume();
        std::string digits(idx.Text);
        // Located at the whole `.N`, the one token the Bison scanner sees.
        SourceLocation at(dot.Loc.getLineStart(), dot.Loc.getColumnStart(),
                          idx.Loc.getLineEnd(), idx.Loc.getColumnEnd());
        if (digits.size() > 1 && digits[0] == '0') {
          error(at, "tuple index must not have leading zeros: ." + digits);
          return nullptr;
        }
        errno = 0;
        long long n = std::strtoll(digits.c_str(), nullptr, 10);
        if (errno == ERANGE) {
          error(at, "tuple index is out of range: ." + digits);
          return nullptr;
        }
        e = Ctx.make<TupleIndexExpr>(span(start), e, static_cast<size_t>(n));
        continue;
      }
      if (!at(Tok::Ident)) {
        errorAtCurrent("expected a field or method name after '.'");
        return nullptr;
      }
      const std::string &name = intern(consume().Text);
      if (accept(Tok::LParen)) {
        std::vector<Expr *> args;
        if (!parseArgumentList(Tok::RParen, args) ||
            !expect(Tok::RParen, "to close the argument list"))
          return nullptr;
        e = Ctx.make<MethodCallExpr>(span(start), e, name, std::move(args));
      } else {
        e = Ctx.make<MemberAccessExpr>(span(start), e, name);
      }
    } else if (at(Tok::LBracket)) {
      if (!enterNesting())
        return nullptr;
      consume();
      Expr *index = parseExpression();
      leaveNesting();
      if (!index || !expect(Tok::RBracket, "to close the subscript"))
        return nullptr;
      e = Ctx.make<SubscriptExpr>(span(start), e, index);
    } else {
      return e;
    }
  }
}

// argumentList ::= ( expression ( "," expression )* )?
bool Parser::parseArgumentList(Tok closer, std::vector<Expr *> &out) {
  if (at(closer))
    return true;
  if (!enterNesting())
    return false;
  bool ok = true;
  do {
    Expr *arg = parseExpression();
    if (!arg) {
      ok = false;
      break;
    }
    out.push_back(arg);
  } while (accept(Tok::Comma));
  leaveNesting();
  return ok;
}

Expr *Parser::parseLiteral() {
  Token t = consume();
  switch (t.Kind) {
  case Tok::Int:
    return Ctx.make<IntegerLiteral>(t.Loc, t.IntValue);
  case Tok::Float:
    return Ctx.make<FloatLiteral>(t.Loc, t.FloatValue);
  case Tok::KwTrue:
    return Ctx.make<BoolLiteral>(t.Loc, true);
  case Tok::KwFalse:
    return Ctx.make<BoolLiteral>(t.Loc, false);
  case Tok::Char:
    return Ctx.make<CharLiteral>(t.Loc, t.CharValue);
  case Tok::String:
    return Ctx.make<StringLiteral>(t.Loc, t.StrValue);
  default:
    return Ctx.make<NoneLiteral>(t.Loc);
  }
}

// `name <` in expression position is a generic call only when a type
// argument list followed by `> (` fits; `a < b` is otherwise a comparison.
bool Parser::tryGenericCallArgs(std::vector<Type *> &out) {
  size_t saved = Pos;
  SourceLocation savedEnd = PrevEnd;
  ++Speculating;
  consume(); // '<'
  bool ok = enterNesting();
  if (ok) {
    ok = parseTypeArgList(out) && accept(Tok::Greater) && at(Tok::LParen);
    leaveNesting();
  }
  --Speculating;
  if (!ok) {
    Pos = saved;
    PrevEnd = savedEnd;
    out.clear();
  }
  return ok;
}

// primary ::= literal | IDENT | IDENT "(" args ")" | IDENT "<" types ">" "("
// args ")"
//           | modulePath "::" IDENT ( "(" args ")" | "<" types ">" "(" args ")"
//           )? | "(" expression ")" | "(" expression ( "," expression )+ ")" |
//           "[" args "]"
Expr *Parser::parsePrimary() {
  SourceLocation start = cur().Loc;
  if (isLiteralToken(kind()))
    return parseLiteral();

  switch (kind()) {
  case Tok::Ident: {
    std::string name(consume().Text);
    std::string path; // everything before the last `::`
    while (at(Tok::ColonColon) && kind(1) == Tok::Ident) {
      consume();
      if (!path.empty())
        path += "::";
      path += name;
      name = std::string(consume().Text);
    }
    std::string full = path.empty() ? name : path + "::" + name;

    std::vector<Type *> typeArgs;
    bool generic = at(Tok::Less) && tryGenericCallArgs(typeArgs);
    if (generic || at(Tok::LParen)) {
      consume(); // '('
      std::vector<Expr *> args;
      if (!parseArgumentList(Tok::RParen, args) ||
          !expect(Tok::RParen, "to close the argument list"))
        return nullptr;
      return Ctx.make<CallExpr>(span(start), intern(full), std::move(args),
                                std::move(typeArgs));
    }
    if (!path.empty())
      return Ctx.make<EnumValueExpr>(span(start), intern(path), intern(name));
    return Ctx.make<Identifier>(start, intern(name));
  }

  case Tok::LParen: {
    consume();
    if (!enterNesting())
      return nullptr;
    Expr *first = parseExpression();
    if (!first) {
      leaveNesting();
      return nullptr;
    }
    if (!at(Tok::Comma)) {
      leaveNesting();
      if (!expect(Tok::RParen, "to close the parenthesised expression"))
        return nullptr;
      return first; // a parenthesised expression is just that expression
    }
    // Tuple literal (e1, e2, ...): no 1-tuples.
    std::vector<Expr *> elems{first};
    while (accept(Tok::Comma)) {
      Expr *e = parseExpression();
      if (!e) {
        leaveNesting();
        return nullptr;
      }
      elems.push_back(e);
    }
    leaveNesting();
    if (!expect(Tok::RParen, "to close the tuple literal"))
      return nullptr;
    return Ctx.make<TupleLiteralExpr>(span(start), std::move(elems));
  }

  case Tok::LBracket: {
    consume();
    std::vector<Expr *> elems;
    if (!parseArgumentList(Tok::RBracket, elems) ||
        !expect(Tok::RBracket, "to close the array literal"))
      return nullptr;
    return Ctx.make<ArrayLiteralExpr>(span(start), std::move(elems));
  }

  default:
    errorAtCurrent("expected an expression");
    return nullptr;
  }
}

} // namespace paykan::frontend::recursive_descent
