// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "Parser.h"

#include "ASTClone.h"
#include "Names.h"
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

// -- Construction

Parser::Parser(ASTContext &ctx, std::string_view source,
               sema::DiagEngine *diags)
    : Ctx(ctx), Diags(diags),
      TheLexer(source, [this](SourceLocation loc, const std::string &msg) {
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
  Lexer lexer(source, [&](SourceLocation loc, const std::string &msg) {
    ++errors;
    if (diags)
      diags->error(loc, msg);
    else
      std::cerr << "error @" << loc.getLineStart() << "."
                << loc.getColumnStart() << ": " << msg << "\n";
  });
  for (;;) {
    Token t = lexer.next();
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

// -- Token stream

const Token &Parser::peek(size_t k) {
  while (Pos + k >= Buf.size()) {
    if (!Buf.empty() && Buf.back().Kind == Tok::Eof)
      return Buf.back();
    Buf.push_back(TheLexer.next());
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
  std::string hint;
  if (t.Kind == Tok::KwView || t.Kind == Tok::KwInout)
    hint = " (" + found +
           " only marks the type of a parameter or a local: 'fn f(x: " +
           std::string(t.Text) + " int)', 'y: " + std::string(t.Text) +
           " = x;'" +
           (t.Kind == Tok::KwView
                ? "; or a method that does not change 'self': 'view fn len()'"
                : "") +
           ")";
  else if (t.Kind == Tok::KwLet)
    hint = " ('let' only starts a local declaration: 'let x = 1;')";
  else if (t.Kind == Tok::KwWeak)
    hint = " ('weak' only starts the declaration of a weak reference, a local "
           "or a class field: 'weak y: Point? = x;')";
  error(t.Loc, "unexpected " + found + "; " + expected + hint);
  return true;
}

// -- Error recovery
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
    case Tok::KwView:
      if (depth == 0 && kind(1) == Tok::KwFn)
        return;
      break;
    case Tok::KwFn:
    case Tok::KwNative:
    case Tok::KwClass:
    case Tok::KwStruct:
    case Tok::KwEnum:
    case Tok::KwImpl:
    case Tok::KwTrait:
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
    case Tok::KwView:
      if (depth == 0 && kind(1) == Tok::KwFn)
        return;
      break;
    case Tok::KwFn:
    case Tok::KwStatic:
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

void Parser::skipPastMatchingBrace() {
  for (unsigned depth = 1; depth > 0 && !at(Tok::Eof); consume()) {
    if (at(Tok::LBrace))
      ++depth;
    else if (at(Tok::RBrace))
      --depth;
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

TranslationUnit *Parser::parseTranslationUnit() {
  std::vector<ImportDecl *> imports;
  std::vector<ClassDecl *> classes, genericClasses;
  std::vector<FuncDecl *> funcs, genericFuncs;
  std::vector<EnumDecl *> enums;
  std::vector<ImplDecl *> impls;
  std::vector<TraitDecl *> traits;
  std::vector<StructDecl *> structs, genericStructs;
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
    case Tok::KwStruct:
      if (auto *d = parseStructDecl())
        (d->isGeneric() ? genericStructs : structs).push_back(d);
      else
        ok = false;
      break;
    case Tok::KwView:
      if (kind(1) != Tok::KwFn) {
        errorAtCurrent("expected 'import', 'class', 'struct', 'enum', 'trait', "
                       "'impl' or 'fn' at top level");
        ok = false;
        break;
      }
      [[fallthrough]]; // `view fn`: Sema says only a method can be one
    case Tok::KwFn:
      if (auto *d = parseFuncDecl())
        (d->isGeneric() ? genericFuncs : funcs).push_back(d);
      else
        ok = false;
      break;
    case Tok::KwNative:
      if (auto *d = parseFuncDecl(/*native=*/true))
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
    case Tok::KwImpl:
      if (auto *d = parseImplDecl())
        impls.push_back(d);
      else
        ok = false;
      break;
    case Tok::KwTrait:
      if (auto *d = parseTraitDecl())
        traits.push_back(d);
      else
        ok = false;
      break;
    case Tok::KwStatic:
      errorAtCurrent("a 'static fn' goes in an 'impl' block: 'impl Name { "
                     "static fn ... }'");
      ok = false;
      break;
    default:
      errorAtCurrent("expected 'import', 'class', 'struct', 'enum', 'trait', "
                     "'impl', 'fn' or 'native' at top level");
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
      std::move(enums), std::move(genericClasses), std::move(genericFuncs),
      std::move(impls), std::move(traits), std::move(structs),
      std::move(genericStructs));
}

// -- Imports
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
//
// classDecl ::= "class" IDENT ( "<" typeParamList ">" )? ( ":" modulePath )?
//               "{" classMember* "}"
// classMember ::= varDecl ";" | funcDecl
// typeParamList ::= typeParam ( "," typeParam )*
// typeParam ::= IDENT ( ":" typeAnnotation ( "+" typeAnnotation )* )?

bool Parser::parseTypeParamList(std::vector<const std::string *> &out,
                                TypeParamBounds *bounds) {
  bool anyBound = false;
  do {
    if (!at(Tok::Ident)) {
      errorAtCurrent("expected a type parameter name");
      return false;
    }
    out.push_back(&intern(consume().Text));
    std::vector<Type *> traits;
    if (at(Tok::Colon)) {
      if (!bounds) {
        errorAtCurrent("a trait's type parameters cannot have bounds");
        return false;
      }
      consume();
      do {
        Type *t = parseTypeAnnotation();
        if (!t)
          return false;
        traits.push_back(t);
      } while (accept(Tok::Plus));
      anyBound = true;
    }
    if (bounds)
      bounds->push_back(std::move(traits));
  } while (accept(Tok::Comma));
  if (bounds && !anyBound)
    bounds->clear();
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
  TypeParamBounds bounds;
  if (accept(Tok::Less)) {
    if (!parseTypeParamList(typeParams, &bounds) ||
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
    if (at(Tok::KwFn) || (at(Tok::KwView) && kind(1) == Tok::KwFn)) {
      if (auto *m = parseFuncDecl()) {
        body.Methods.push_back(m);
        continue;
      }
    } else if (at(Tok::Ident) || (at(Tok::KwWeak) && kind(1) == Tok::Ident)) {
      // `weak next: Node?;`: a field holding a weak reference.
      const bool weak = accept(Tok::KwWeak);
      if (auto *f = parseVarDecl()) {
        f->setWeak(weak);
        if (expect(Tok::Semi, "after the field declaration")) {
          body.Fields.push_back(f);
          continue;
        }
      }
    } else if (at(Tok::KwWeak)) {
      errorAtCurrent("expected a field name after 'weak': 'weak next: " +
                     std::string(name) + "?;'");
    } else if (at(Tok::KwStatic)) {
      errorAtCurrent("a 'static fn' goes in an 'impl' block, not the class "
                     "body: 'impl " +
                     std::string(name) + " { static fn ... }'");
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
  auto *cd = Ctx.make<ClassDecl>(
      span(start), name, intern(superName), std::move(body.Fields),
      std::move(body.Methods), std::move(typeParams));
  cd->setTypeParamBounds(std::move(bounds));
  return cd;
}

// structDecl ::= "struct" IDENT typeParams? "{" ( varDecl ";" )+ "}"
//
// A struct body holds fields only: a method goes in an `impl` block, and a
// struct has no superclass.  Both are reported where they are written.
StructDecl *Parser::parseStructDecl() {
  SourceLocation start = consume().Loc; // "struct"
  if (!at(Tok::Ident)) {
    errorAtCurrent("expected a struct name after 'struct'");
    return nullptr;
  }
  const std::string &name = intern(consume().Text);

  std::vector<const std::string *> typeParams;
  TypeParamBounds bounds;
  if (accept(Tok::Less)) {
    if (!parseTypeParamList(typeParams, &bounds) ||
        !expect(Tok::Greater, "to close the type parameter list"))
      return nullptr;
  }
  if (at(Tok::Colon)) {
    error(cur().Loc, "a struct has no superclass: 'struct " +
                         std::string(name) +
                         " { ... }' (a struct is a value, "
                         "not an object; use a class to inherit)");
    return nullptr;
  }

  SourceLocation open = cur().Loc;
  if (!expect(Tok::LBrace, "to open the struct body"))
    return nullptr;
  if (!enterNesting())
    return nullptr;

  std::vector<VarDecl *> fields;
  bool ok = true;
  while (!at(Tok::RBrace) && !at(Tok::Eof)) {
    size_t before = Pos;
    if (at(Tok::Ident)) {
      if (auto *f = parseVarDecl()) {
        if (expect(Tok::Semi, "after the field declaration")) {
          fields.push_back(f);
          continue;
        }
      }
    } else if (at(Tok::KwWeak)) {
      errorAtCurrent("a struct field cannot be 'weak': a weak reference is a "
                     "field of a class or a local (a struct is a value, "
                     "copied with its fields)");
    } else if (at(Tok::KwFn) || at(Tok::KwStatic) ||
               (at(Tok::KwView) && kind(1) == Tok::KwFn)) {
      errorAtCurrent("a struct body holds fields only ('name: Type;'); a "
                     "method goes in an 'impl' block: 'impl " +
                     std::string(name) + " { fn ... }'");
    } else {
      errorAtCurrent("expected a field ('name: Type;') in the struct body");
    }
    ok = false;
    if (Pos == before)
      consume(); // guarantee progress
    skipToMemberBoundary();
  }
  leaveNesting();
  if (!expectCloseBrace("to close the struct body", open) || !ok)
    return nullptr;
  if (fields.empty()) {
    error(span(start), "struct '" + std::string(name) +
                           "' has no fields: a struct has at least one");
    return nullptr;
  }
  auto *sd = Ctx.make<StructDecl>(span(start), name, std::move(fields),
                                  std::move(typeParams));
  sd->setTypeParamBounds(std::move(bounds));
  return sd;
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

// implDecl ::= "impl" ( typeAnnotation "for" )? modulePath
//              "{" implMember* "}"
// implMember ::= ( "view" | "static" )? "fn" ... block
ImplDecl *Parser::parseImplDecl() {
  SourceLocation start = consume().Loc; // "impl"
  if (!at(Tok::Ident)) {
    errorAtCurrent("expected a type name after 'impl'");
    return nullptr;
  }
  // `impl Show for A`: the trait (`Into<int>`, `fmt::Show`) comes first.
  Type *trait = nullptr;
  Type *target = parseTypeAnnotation();
  if (!target)
    return nullptr;
  const char *after = "after 'impl'";
  if (accept(Tok::KwFor)) {
    trait = target;
    after = "after 'for'";
    if (!at(Tok::Ident)) {
      errorAtCurrent("expected a type name after 'for'");
      return nullptr;
    }
    target = parseTypeAnnotation();
    if (!target)
      return nullptr;
  }
  // The type by its name; a generic one (`Box<T>`) names its parameters.
  // A built-in type is Sema's to reject.
  std::string typeName;
  std::vector<const std::string *> typeParams;
  if (auto *named = dyn_cast<ClassType>(target)) {
    typeName = named->getName();
  } else if (isa<BuiltinType>(target)) {
    typeName = ast::typeName(target);
  } else if (auto *generic = dyn_cast<GenericType>(target)) {
    typeName = generic->getName();
    for (Type *arg : generic->getArgs()) {
      auto *param = dyn_cast<ClassType>(arg);
      if (!param || Ctx.lookupType(param->getName()) ||
          param->getName().find("::") != std::string::npos) {
        error(arg->getLocation(), "expected a type parameter name in 'impl " +
                                      typeName + "<...>', not the type '" +
                                      ast::typeName(arg) + "'");
        return nullptr;
      }
      typeParams.push_back(&intern(param->getName()));
    }
  } else {
    error(target->getLocation(), std::string("expected a type name ") + after);
    return nullptr;
  }

  SourceLocation open = cur().Loc;
  if (!expect(Tok::LBrace, "to open the impl block"))
    return nullptr;
  if (!enterNesting())
    return nullptr;

  std::vector<FuncDecl *> methods;
  bool ok = true;
  while (!at(Tok::RBrace) && !at(Tok::Eof)) {
    size_t before = Pos;
    bool isStatic = accept(Tok::KwStatic);
    if (isStatic && !at(Tok::KwFn)) {
      errorAtCurrent("expected 'fn' after 'static' (a 'static fn' has no "
                     "'self', so it is never a 'view fn')");
    } else if (at(Tok::KwFn) || (at(Tok::KwView) && kind(1) == Tok::KwFn)) {
      if (auto *m = parseFuncDecl()) {
        m->setStatic(isStatic);
        methods.push_back(m);
        continue;
      }
    } else {
      errorAtCurrent("expected a method ('fn', 'view fn' or 'static fn') in "
                     "the impl block; fields go in the type's declaration");
    }
    ok = false;
    if (Pos == before)
      consume(); // guarantee progress
    skipToMemberBoundary();
  }
  leaveNesting();
  if (!expectCloseBrace("to close the impl block", open) || !ok)
    return nullptr;
  return Ctx.make<ImplDecl>(span(start), intern(typeName), std::move(methods),
                            trait, std::move(typeParams));
}

// traitDecl ::= "trait" IDENT ( "<" typeParamList ">" )? "{" traitMember* "}"
// traitMember ::= "view"? "fn" IDENT "(" paramList ")"
//                 ( "->" ( "view" | "inout" )? typeAnnotation )? ";"
TraitDecl *Parser::parseTraitDecl() {
  SourceLocation start = consume().Loc; // "trait"
  if (!at(Tok::Ident)) {
    errorAtCurrent("expected a trait name after 'trait'");
    return nullptr;
  }
  const std::string &name = intern(consume().Text);
  std::vector<const std::string *> typeParams;
  if (accept(Tok::Less)) {
    if (!parseTypeParamList(typeParams, nullptr) ||
        !expect(Tok::Greater, "to close the type parameter list"))
      return nullptr;
  }

  SourceLocation open = cur().Loc;
  if (!expect(Tok::LBrace, "to open the trait body"))
    return nullptr;
  if (!enterNesting())
    return nullptr;

  std::vector<FuncDecl *> methods;
  bool ok = true;
  while (!at(Tok::RBrace) && !at(Tok::Eof)) {
    size_t before = Pos;
    if (at(Tok::KwFn) || (at(Tok::KwView) && kind(1) == Tok::KwFn)) {
      if (auto *m = parseFuncDecl(/*native=*/false, /*requirement=*/true)) {
        methods.push_back(m);
        continue;
      }
    } else {
      errorAtCurrent(
          "expected a method signature ('fn' or 'view fn') in the trait");
    }
    ok = false;
    if (Pos == before)
      consume(); // guarantee progress
    skipToMemberBoundary();
  }
  leaveNesting();
  if (!expectCloseBrace("to close the trait body", open) || !ok)
    return nullptr;
  return Ctx.make<TraitDecl>(span(start), name, std::move(typeParams),
                             std::move(methods));
}

// -- Functions
//
// funcDecl ::= "view"? "fn" IDENT ( "<" typeParamList ">" )? "(" paramList ")"
//              ( "->" ( "view" | "inout" )? typeAnnotation )? block
// paramList ::= ( param ( "," param )* )?
// param ::= IDENT ":" ( "view" | "inout" )? typeAnnotation

bool Parser::parseParamList(std::vector<Param> &out) {
  if (at(Tok::RParen))
    return true;
  do {
    if (at(Tok::KwView) || at(Tok::KwInout)) {
      prefixModeError();
      return false;
    }
    if (!at(Tok::Ident)) {
      errorAtCurrent("expected a parameter name");
      return false;
    }
    const std::string &name = intern(consume().Text);
    if (!expect(Tok::Colon, "after the parameter name"))
      return false;
    ParamMode mode = ParamMode::Value;
    if (accept(Tok::KwView))
      mode = ParamMode::View;
    else if (accept(Tok::KwInout))
      mode = ParamMode::Inout;
    Type *ty = parseTypeAnnotation();
    if (!ty)
      return false;
    out.push_back(Param{&name, ty, mode});
  } while (accept(Tok::Comma));
  return true;
}

// `inout n: int`, with the mode before the name: say where it goes.  Reads
// the rest of the parameter when it can, to show the whole fix.
void Parser::prefixModeError() {
  Token modeTok = consume();
  std::string mode(modeTok.Text);
  std::string fix = "x: " + mode + " int";
  if (at(Tok::Ident) && kind(1) == Tok::Colon) {
    std::string name(consume().Text);
    consume();     // ":"
    ++Speculating; // a bad type is not reported on its own
    Type *ty = parseTypeAnnotation();
    --Speculating;
    fix = name + ": " + mode + " " + (ty ? typeName(ty) : "T");
  }
  error(modeTok.Loc, "'" + mode +
                         "' goes after the colon, before the type: write '" +
                         fix + "'");
}

FuncDecl *Parser::parseFuncDecl(bool native, bool requirement) {
  SourceLocation start = cur().Loc;
  bool view = !native && accept(Tok::KwView); // `view fn` (a method; see Sema)
  consume();                                  // "fn", or "native" before it
  if (native && !expect(Tok::KwFn, "after 'native'"))
    return nullptr;
  if (!at(Tok::Ident)) {
    errorAtCurrent("expected a function name after 'fn'");
    return nullptr;
  }
  // `___init__` is the initializer `__init__`, private to its module
  // (Names.h): recorded as the initializer, with the mark.
  const std::string_view spelled = consume().Text; // into the source
  const bool modulePrivateInit = spelled == names::kMethodInitModulePrivate;
  const std::string &name = intern(
      modulePrivateInit ? std::string_view(names::kMethodInit) : spelled);

  std::vector<const std::string *> typeParams;
  TypeParamBounds bounds;
  if (accept(Tok::Less)) {
    if (!parseTypeParamList(typeParams, &bounds) ||
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
  ParamMode resultMode = ParamMode::Value;
  if (accept(Tok::Arrow)) {
    // `-> view T`: a borrowed result (whether it may be one is Sema's).
    if (at(Tok::KwView) || at(Tok::KwInout))
      resultMode =
          consume().Kind == Tok::KwView ? ParamMode::View : ParamMode::Inout;
    retTy = parseTypeAnnotation();
    if (!retTy)
      return nullptr;
  }

  // nativeFuncDecl ::= "native" "fn" IDENT ( "<" typeParamList ">" )?
  //                    "(" params? ")" ( "->" type )? "=" STRING ";"
  //                    -- the C symbol instead of a body
  if (native) {
    if (!expect(Tok::Assign, "after a native function's signature"))
      return nullptr;
    if (!at(Tok::String)) {
      errorAtCurrent("expected the C symbol as a string literal");
      return nullptr;
    }
    const std::string &symbol = intern(consume().StrValue);
    if (!expect(Tok::Semi, "after a native function"))
      return nullptr;
    auto *fn = Ctx.make<FuncDecl>(span(start), name, std::move(params), retTy,
                                  nullptr, std::move(typeParams));
    fn->setNativeSymbol(symbol);
    fn->setResultMode(resultMode);
    fn->setModulePrivateInit(modulePrivateInit);
    return fn;
  }

  // traitMember: the signature, then ";" instead of a body.
  if (requirement) {
    if (!typeParams.empty()) {
      error(start, "a trait method cannot be generic");
      return nullptr;
    }
    if (at(Tok::LBrace) || at(Tok::Assign)) {
      errorAtCurrent("a trait method has no body: end its signature with ';'");
      return nullptr;
    }
    if (!expect(Tok::Semi, "after a trait method's signature"))
      return nullptr;
    auto *fn = Ctx.make<FuncDecl>(span(start), name, std::move(params), retTy,
                                  nullptr);
    fn->setView(view);
    fn->setResultMode(resultMode);
    fn->setModulePrivateInit(modulePrivateInit);
    return fn;
  }

  // `fn double(x: int) -> int = x * 2;` is the function whose body is
  // `{ return x * 2; }`; without the result type, `fn double(x: int) = x *
  // 2;`, the result type is the expression's (Sema infers it).
  CompoundStmt *body = nullptr;
  const bool expressionBody = at(Tok::Assign);
  if (expressionBody) {
    consume();
    SourceLocation from = cur().Loc;
    Expr *value = parseExpression();
    if (!value)
      return nullptr;
    std::vector<Stmt *> ret{Ctx.make<ReturnStmt>(span(from), value)};
    body = Ctx.make<CompoundStmt>(span(from), std::move(ret));
    if (!expect(Tok::Semi, "after the function's expression"))
      return nullptr;
  } else {
    if (!at(Tok::LBrace)) {
      errorAtCurrent("expected '{' or '=' to start the function's body");
      return nullptr;
    }
    body = parseBlock();
    if (!body)
      return nullptr;
  }
  auto *fn = Ctx.make<FuncDecl>(span(start), name, std::move(params), retTy,
                                body, std::move(typeParams));
  fn->setInferredResult(expressionBody && !retTy);
  fn->setView(view);
  fn->setResultMode(resultMode);
  fn->setModulePrivateInit(modulePrivateInit);
  fn->setTypeParamBounds(std::move(bounds));
  return fn;
}

// -- Blocks and statements

// The CompoundStmt's location is the empty range right after its `{`
// (docs/grammar.md); it is not widened by the statements added to it.
CompoundStmt *Parser::parseBlock() {
  if (!at(Tok::LBrace)) {
    errorAtCurrent("expected '{'");
    return nullptr;
  }
  Token lbrace = consume();
  SourceLocation after(lbrace.Loc.getLineEnd(), lbrace.Loc.getColumnEnd(),
                       lbrace.Loc.getLineEnd(), lbrace.Loc.getColumnEnd());
  auto *block = Ctx.make<CompoundStmt>(after);
  if (!enterNesting()) {
    // The rest of this block is not parsed.  Skip it whole, through its
    // matching '}', and hand back an empty block: the enclosing block then
    // resumes after it, so the limit is the only error (#132).
    skipPastMatchingBrace();
    return block;
  }
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
//             | varDecl "=" expression ";" | letDecl | destructure
//             | "return" expr? ";" | block | ifStmt | whileStmt | "break" ";"
//             | "continue" ";" | matchStmt
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

  case Tok::KwFor:
    out = parseForStmt();
    return out != nullptr;

  case Tok::KwElif:
    errorAtCurrent("an 'elif' follows the block of an 'if' or another "
                   "'elif'");
    return false;

  case Tok::KwMatch:
    out = parseMatchStmt();
    return out != nullptr;

  case Tok::Underscore:
    return parseDestructureStatement(start, {}, out);

  case Tok::KwLet:
    return parseLetDecl(out);

  case Tok::KwWeak:
    return parseWeakDecl(out);

  case Tok::KwView:
  case Tok::KwInout:
    // `view x = e;`, with the mode before the name.
    if (kind(1) == Tok::Ident &&
        (kind(2) == Tok::Assign || kind(2) == Tok::Colon)) {
      prefixBorrowError();
      return false;
    }
    return parseExprOrAssignStatement(out);

  case Tok::Ident: {
    // After a leading name the next token decides: `,` starts a
    // destructuring statement, `:` a typed declaration (which may itself be
    // the first destructuring target) or, with `view` / `inout` after it, a
    // local borrow, anything else an expression.
    if (kind(1) == Tok::Comma)
      return parseDestructureStatement(start, {}, out);
    if (kind(1) == Tok::Colon &&
        (kind(2) == Tok::KwView || kind(2) == Tok::KwInout))
      return parseBorrowDecl(out);
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

// borrowDecl ::= IDENT ":" ( "view" | "inout" ) typeAnnotation? "="
//                expression ";"
bool Parser::parseBorrowDecl(Stmt *&out) {
  SourceLocation start = cur().Loc;
  const std::string &name = intern(consume().Text);
  consume(); // ":"
  ParamMode mode =
      consume().Kind == Tok::KwView ? ParamMode::View : ParamMode::Inout;
  Type *ty = nullptr;
  if (!at(Tok::Assign) && !at(Tok::Semi)) {
    ty = parseTypeAnnotation();
    if (!ty)
      return false;
  }
  if (at(Tok::Comma)) {
    error(cur().Loc, std::string("a destructuring target cannot be '") +
                         paramModeName(mode) + "'");
    return false;
  }
  const std::string needsInit = std::string("(a '") + paramModeName(mode) +
                                "' local needs an initial value)";
  if (!expect(Tok::Assign, needsInit.c_str()))
    return false;
  Expr *init = parseExpression();
  if (!init)
    return false;
  auto *vd = Ctx.make<VarDecl>(span(start), name, ty, init);
  vd->setMode(mode);
  auto *ds = Ctx.make<DeclStmt>(span(start), vd);
  if (!expect(Tok::Semi, "after the declaration"))
    return false;
  out = ds;
  return true;
}

// `view x = e;` or `inout x: int = e;`, with the mode before the name: say
// where it goes.
void Parser::prefixBorrowError() {
  Token modeTok = consume();
  std::string mode(modeTok.Text);
  std::string name(consume().Text);
  std::string fix = name + ": " + mode;
  if (accept(Tok::Colon)) {
    ++Speculating; // a bad type is not reported on its own
    Type *ty = parseTypeAnnotation();
    --Speculating;
    fix += " " + (ty ? typeName(ty) : std::string("T"));
  }
  error(modeTok.Loc, "'" + mode +
                         "' goes after the colon, before the type: write '" +
                         fix + " = ...'");
}

// letDecl ::= "let" IDENT ( ":" typeAnnotation )? "=" expression ";"
bool Parser::parseLetDecl(Stmt *&out) {
  SourceLocation start = consume().Loc; // "let"
  if (!at(Tok::Ident)) {
    errorAtCurrent("expected a variable name after 'let'");
    return false;
  }
  const std::string &name = intern(consume().Text);
  Type *ty = nullptr;
  if (accept(Tok::Colon)) {
    if (at(Tok::KwView) || at(Tok::KwInout)) {
      error(cur().Loc, "a local borrow cannot be 'let': a '" +
                           std::string(cur().Text) +
                           "' local always names what it was given");
      return false;
    }
    ty = parseTypeAnnotation();
    if (!ty)
      return false;
  }
  if (at(Tok::Comma)) {
    error(cur().Loc, "'let' declares one variable; destructuring with 'let' "
                     "is not supported yet");
    return false;
  }
  if (!expect(Tok::Assign, "(a 'let' declaration needs an initial value)"))
    return false;
  Expr *init = parseExpression();
  if (!init)
    return false;
  // Like a typed declaration, the VarDecl ends with its initializer and the
  // `;` is only part of the statement; both start at `let`.
  auto *vd = Ctx.make<VarDecl>(span(start), name, ty, init);
  vd->setLet(true);
  auto *ds = Ctx.make<DeclStmt>(span(start), vd);
  if (!expect(Tok::Semi, "after the declaration"))
    return false;
  out = ds;
  return true;
}

// weakDecl ::= "weak" IDENT ( ":" typeAnnotation )? ( "=" expression )? ";"
//
// A weak reference (docs/language/16-memory.md): the type is `T?` of a
// class `T` (Sema checks), given or inferred from the initializer; without
// an initializer the local starts as None.
bool Parser::parseWeakDecl(Stmt *&out) {
  SourceLocation start = consume().Loc; // "weak"
  if (!at(Tok::Ident)) {
    errorAtCurrent("expected a variable name after 'weak'");
    return false;
  }
  const std::string &name = intern(consume().Text);
  Type *ty = nullptr;
  if (accept(Tok::Colon)) {
    if (at(Tok::KwView) || at(Tok::KwInout)) {
      error(cur().Loc, "a 'weak' local is not a borrow: it holds a weak "
                       "reference of its own, so it cannot be '" +
                           std::string(cur().Text) + "'");
      return false;
    }
    ty = parseTypeAnnotation();
    if (!ty)
      return false;
  }
  if (at(Tok::Comma)) {
    error(cur().Loc, "'weak' declares one variable; destructuring with "
                     "'weak' is not supported");
    return false;
  }
  Expr *init = nullptr;
  if (accept(Tok::Assign)) {
    init = parseExpression();
    if (!init)
      return false;
  }
  if (!ty && !init) {
    error(start, "a 'weak' declaration needs a type or an initial value: "
                 "'weak " +
                     std::string(name) + ": Point? = x;'");
    return false;
  }
  auto *vd = Ctx.make<VarDecl>(span(start), name, ty, init);
  vd->setWeak(true);
  auto *ds = Ctx.make<DeclStmt>(span(start), vd);
  if (!expect(Tok::Semi, "after the declaration"))
    return false;
  out = ds;
  return true;
}

namespace {

/// The operator of the compound assignment token @p k (`+=` is Add).
std::optional<BinaryOpcode> compoundOpcode(Tok k) {
  switch (k) {
  case Tok::PlusAssign:
    return BinaryOpcode::Add;
  case Tok::MinusAssign:
    return BinaryOpcode::Sub;
  case Tok::StarAssign:
    return BinaryOpcode::Mul;
  case Tok::SlashAssign:
    return BinaryOpcode::Div;
  case Tok::PercentAssign:
    return BinaryOpcode::Mod;
  default:
    return std::nullopt;
  }
}

} // namespace

// A compound assignment `target op= value` is `target = target op value`
// (docs/grammar.md): the parser builds the assignment, reading the target
// with a copy of its expression.
bool Parser::parseExprOrAssignStatement(Stmt *&out) {
  SourceLocation start = cur().Loc;
  Expr *lhs = parseExpression();
  if (!lhs)
    return false;

  const std::optional<BinaryOpcode> compound = compoundOpcode(kind());
  if (!compound && !at(Tok::Assign)) {
    if (!expect(Tok::Semi, "after the expression"))
      return false;
    out = Ctx.make<ExprStmt>(span(start), lhs);
    return true;
  }
  const Token op = consume();

  // Check the target before the value so that recovery resumes at this
  // statement's `;` rather than the next one's.
  if (!isa<Identifier>(lhs) && !isa<MemberAccessExpr>(lhs) &&
      !isa<SubscriptExpr>(lhs) && !isa<TupleIndexExpr>(lhs)) {
    error(start, "left-hand side of '" + std::string(op.Text) +
                     "' must be an identifier, field access, or subscript");
    return false;
  }
  Expr *value = parseExpression();
  if (!value)
    return false;
  if (compound)
    value = Ctx.make<BinaryExpr>(span(start), *compound,
                                 ast::ASTCloner(Ctx, {}).cloneExpr(lhs), value);
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

// A statement starting with `if` is an if-statement when its condition is
// followed by `{`, and an expression statement whose expression is a ternary
// (`if c then a else b;`) when it is followed by `then`.  The first of the two
// that is outside any brackets and not a nested ternary's decides; scanning
// the token buffer costs nothing and avoids speculative parsing.
bool Parser::ifStartsStatement() {
  unsigned depth = 0;   // open ( [ {
  unsigned ternary = 0; // nested `if`s still waiting for their `then`
  for (size_t i = 1;; ++i) {
    switch (kind(i)) {
    case Tok::Eof:
      return true; // unbalanced: report it as a broken if-statement
    case Tok::LParen:
    case Tok::LBracket:
      ++depth;
      break;
    case Tok::RParen:
    case Tok::RBracket:
    case Tok::RBrace:
      if (depth == 0)
        return true; // reported as a missing block
      --depth;
      break;
    case Tok::LBrace:
      if (depth == 0)
        return true;
      ++depth;
      break;
    case Tok::Semi:
      if (depth == 0)
        return true; // reported as a missing block
      break;
    case Tok::KwIf:
      if (depth == 0)
        ++ternary;
      break;
    case Tok::KwThen:
      if (depth == 0 && ternary-- == 0)
        return false;
      break;
    default:
      break;
    }
  }
}

// ifStmt   ::= "if" expression block elseTail?
// elseTail ::= "elif" expression block elseTail? | "else" block
// An `elif` is an IfStmt nested as the else branch, as `else if` was.
Stmt *Parser::parseIfStmt() {
  SourceLocation start = consume().Loc; // "if" or "elif"
  Expr *cond = parseExpression();
  if (!cond)
    return nullptr;
  if (!at(Tok::LBrace)) {
    errorAtCurrent("expected '{' after the 'if' condition");
    return nullptr;
  }
  CompoundStmt *thenBlock = parseBlock();
  if (!thenBlock)
    return nullptr;
  Stmt *elseBranch = nullptr;
  if (at(Tok::KwElif)) {
    elseBranch = parseIfStmt();
    if (!elseBranch)
      return nullptr;
  } else if (accept(Tok::KwElse)) {
    if (at(Tok::KwIf)) {
      errorAtCurrent("an if-statement spells 'else if' as 'elif'");
      return nullptr;
    }
    elseBranch = parseBlock();
    if (!elseBranch)
      return nullptr;
  }
  return Ctx.make<IfStmt>(span(start), cond, thenBlock, elseBranch);
}

// whileStmt ::= "while" expression block
Stmt *Parser::parseWhileStmt() {
  SourceLocation start = consume().Loc; // "while"
  Expr *cond = parseExpression();
  if (!cond)
    return nullptr;
  if (!at(Tok::LBrace)) {
    errorAtCurrent("expected '{' after the 'while' condition");
    return nullptr;
  }
  CompoundStmt *body = parseBlock();
  if (!body)
    return nullptr;
  return Ctx.make<WhileStmt>(span(start), cond, body);
}

// forStmt ::= "for" IDENT ( ":" typeAnnotation )? "in" expression
//             ( ".." | "..=" ) expression ( "step" expression )? block
//
// `in` and `step` are not keywords: an identifier there (after the
// variable, after the range's end) can only be them, so a variable or a
// field may still be named `in` or `step`.
Stmt *Parser::parseForStmt() {
  SourceLocation start = consume().Loc; // "for"
  if (!at(Tok::Ident) && !at(Tok::Underscore)) {
    errorAtCurrent("expected the loop variable's name or '_' after 'for'");
    return nullptr;
  }
  // `_` binds no name: the VarDecl's name is empty.
  Token name = consume();
  Type *ty = nullptr;
  if (accept(Tok::Colon)) {
    ty = parseTypeAnnotation();
    if (!ty)
      return nullptr;
  }
  // The VarDecl spans the name and its annotation.
  auto *var = Ctx.make<VarDecl>(
      span(name.Loc), intern(name.Kind == Tok::Underscore ? "" : name.Text), ty,
      nullptr);
  if (!at(Tok::Ident) || cur().Text != "in") {
    errorAtCurrent("expected 'in' after the 'for' loop's variable");
    return nullptr;
  }
  consume();
  Expr *first = parseExpression();
  if (!first)
    return nullptr;
  if (!at(Tok::DotDot) && !at(Tok::DotDotEq)) {
    errorAtCurrent("expected '..' or '..=' in the 'for' loop's range "
                   "('start..end')");
    return nullptr;
  }
  const bool inclusive = consume().Kind == Tok::DotDotEq;
  Expr *last = parseExpression();
  if (!last)
    return nullptr;
  Expr *step = nullptr;
  if (at(Tok::Ident) && cur().Text == "step") {
    consume();
    step = parseExpression();
    if (!step)
      return nullptr;
  }
  if (!at(Tok::LBrace)) {
    errorAtCurrent(step ? "expected '{' after the 'for' loop's step"
                        : "expected 'step' or '{' after the 'for' loop's "
                          "range");
    return nullptr;
  }
  CompoundStmt *body = parseBlock();
  if (!body)
    return nullptr;
  return Ctx.make<ForStmt>(span(start), var, first, last, step, inclusive,
                           body);
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

// matchArm ::= typeAnnotation "{" stmts "}"
//            | IDENT ":" ( "view" | "inout" )? typeAnnotation "{" stmts "}"
//            | literal "{" stmts "}" | "_" "{" stmts "}"
MatchArm *Parser::parseMatchArm() {
  SourceLocation start = cur().Loc;
  const std::string *binding = &intern("");
  Type *armType = nullptr;
  Expr *pattern = nullptr;
  ParamMode mode = ParamMode::Value;

  if (accept(Tok::Underscore)) {
    // wildcard
  } else if (isLiteralToken(kind())) {
    pattern = parseLiteral();
  } else {
    if ((at(Tok::KwView) || at(Tok::KwInout)) && kind(1) == Tok::Ident &&
        kind(2) == Tok::Colon) {
      prefixModeError(); // `view n: T`
      return nullptr;
    }
    if (at(Tok::Ident) && kind(1) == Tok::Colon) {
      binding = &intern(consume().Text);
      consume(); // ':'
      if (at(Tok::KwView) || at(Tok::KwInout))
        mode =
            consume().Kind == Tok::KwView ? ParamMode::View : ParamMode::Inout;
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
  auto *arm = Ctx.make<MatchArm>(span(start), *binding, armType, body);
  arm->setMode(mode);
  return arm;
}

// -- Types
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
    // `(T1, T2)` is a tuple type and `(T1, T2) -> R` a function type: the
    // `->` after the `)` decides, so the elements are parsed alike, a
    // parameter's mode kept until it is known which it is.
    std::vector<Type *> elems;
    std::vector<ParamMode> modes;
    SourceLocation firstModeLoc;
    bool anyMode = false;
    if (!at(Tok::RParen)) {
      do {
        ParamMode mode = ParamMode::Value;
        if (at(Tok::KwView) || at(Tok::KwInout)) {
          if (!anyMode)
            firstModeLoc = cur().Loc;
          anyMode = true;
          mode = consume().Kind == Tok::KwView ? ParamMode::View
                                               : ParamMode::Inout;
        }
        Type *t = parseTypeAnnotation();
        if (!t) {
          leaveNesting();
          return nullptr;
        }
        elems.push_back(t);
        modes.push_back(mode);
      } while (accept(Tok::Comma));
    }
    leaveNesting();
    const bool functionType = at(Tok::RParen) && kind(1) == Tok::Arrow;
    if (!functionType) {
      if (elems.empty()) {
        errorAtCurrent("expected a type");
        return nullptr;
      }
      if (anyMode) {
        error(firstModeLoc, "a tuple element has no mode: 'view' and 'inout' "
                            "go on a function type's parameters ('(view Str) "
                            "-> int')");
        return nullptr;
      }
      // `((int) -> int)[3]`: a function type takes a suffix in parentheses.
      if (elems.size() == 1 && !isa<FunctionType>(elems[0]) &&
          !expect(Tok::Comma,
                  "(a tuple type needs at least two element types; there "
                  "are no parenthesised types)"))
        return nullptr;
      if (!expect(Tok::RParen, "to close the tuple type"))
        return nullptr;
      if (elems.size() == 1)
        return elems[0];
      return Ctx.make<TupleType>(span(start), std::move(elems));
    }
    consume(); // ')'
    consume(); // '->'
    if (at(Tok::KwView) || at(Tok::KwInout)) {
      error(cur().Loc, "a function type's result is a copy: '-> " +
                           std::string(cur().Text) +
                           " T' is not supported in a function type");
      return nullptr;
    }
    Type *ret = parseTypeAnnotation();
    if (!ret)
      return nullptr;
    return Ctx.make<FunctionType>(span(start), std::move(elems),
                                  std::move(modes), ret);
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
      // A `[` after a type can only open the `[]` or `[n]` suffix: report
      // the missing `]` where it is missing.
      consume();
      auto isLength = [](Tok k) {
        return k == Tok::Int || k == Tok::Underscore;
      };
      if (isLength(cur().Kind)) {
        // A run of lengths, `[2][3]`, is one array of arrays whose first
        // length is its first index's: two `int[3]`s.  `_` is a length a
        // variable declaration infers from its initializer.
        std::vector<int64_t> lengths;
        for (;;) {
          Token n = consume();
          if (n.Kind == Tok::Int && n.IntValue < 1) {
            error(n.Loc, "a fixed-size array has at least one element: its "
                         "length must be 1 or more");
            return nullptr;
          }
          if (!expect(Tok::RBracket, "to close the array type"))
            return nullptr;
          lengths.push_back(n.Kind == Tok::Int
                                ? n.IntValue
                                : FixedArrayType::kInferredLength);
          if (!(at(Tok::LBracket) && isLength(peek(1).Kind)))
            break;
          consume();
        }
        for (auto it = lengths.rbegin(); it != lengths.rend(); ++it)
          ty = Ctx.make<FixedArrayType>(span(start), ty, *it);
        continue;
      }
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
  if (t && at(Tok::KwElif))
    errorAtCurrent("a conditional expression spells 'elif' as 'else if'");
  else if (t && expect(Tok::KwElse, "after the 'then' branch"))
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

// unary ::= ( "!" | "-" ) unary | postfix   ("mov" is reserved: an error)
Expr *Parser::parseUnary() {
  if (at(Tok::Not) || at(Tok::Minus) || at(Tok::KwMov)) {
    if (!enterNesting())
      return nullptr;
    Token op = consume();
    Expr *operand = parseUnary();
    leaveNesting();
    if (!operand)
      return nullptr;
    // `mov` is retired (#145).  The operand still parses, so the removal is
    // the only diagnostic.
    if (op.Kind == Tok::KwMov) {
      error(op.Loc, std::string(kMovRemoved));
      return operand;
    }
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
    if (at(Tok::LParen)) {
      // `xs[0](1)`, `t.0(x)`, `make()(2)`: a call of a function value that
      // is not named (a named one is a CallExpr or a MethodCallExpr).
      consume();
      std::vector<Expr *> args;
      if (!parseArgumentList(Tok::RParen, args) ||
          !expect(Tok::RParen, "to close the argument list"))
        return nullptr;
      e = Ctx.make<CallValueExpr>(span(start), e, std::move(args));
      continue;
    }
    if (at(Tok::Dot)) {
      Token dot = consume();
      if (at(Tok::Int) && cur().Offset == dot.Offset + 1) {
        // Tuple index `t.0`: the digits glued to the dot.  Leading zeros are
        // rejected so every index has exactly one spelling.
        Token idx = consume();
        std::string digits(idx.Text);
        // Located at the whole `.N`, as if it were one token.
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
// argument list followed by `> (` fits, or by `> :: IDENT (` (a function of
// the instantiation, `Box<int>::of(1)`); `a < b` is otherwise a comparison.
bool Parser::tryGenericCallArgs(std::vector<Type *> &out, std::string &member) {
  size_t saved = Pos;
  SourceLocation savedEnd = PrevEnd;
  ++Speculating;
  consume(); // '<'
  bool ok = enterNesting();
  if (ok) {
    ok = parseTypeArgList(out) && accept(Tok::Greater);
    if (ok && at(Tok::ColonColon) && kind(1) == Tok::Ident &&
        kind(2) == Tok::LParen) {
      consume(); // '::'
      member = std::string(consume().Text);
    }
    ok = ok && at(Tok::LParen);
    leaveNesting();
  }
  --Speculating;
  if (!ok) {
    Pos = saved;
    PrevEnd = savedEnd;
    out.clear();
    member.clear();
  }
  return ok;
}

// primary ::= literal | IDENT | IDENT "(" args ")" | IDENT "<" types ">" "("
// args ")"
//           | IDENT "<" types ">" "::" IDENT "(" args ")"
//           | modulePath "::" IDENT ( "(" args ")" | "<" types ">" "(" args ")"
//           | "<" types ">" "::" IDENT "(" args ")" )?
//           | "(" expression ")" | "(" expression ( "," expression )+ ")" |
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
    std::string member; // `Box<int>::of(...)`: the instantiation's function
    bool generic = at(Tok::Less) && tryGenericCallArgs(typeArgs, member);
    if (!member.empty())
      full += "::" + member;
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

  case Tok::KwFn: {
    // A lambda: a function without a name, in expression position.
    consume();
    if (!expect(Tok::LParen, "after 'fn' (a lambda has no name: 'fn(x: int) "
                             "-> int { ... }')"))
      return nullptr;
    std::vector<Param> params;
    if (!parseParamList(params) ||
        !expect(Tok::RParen, "to close the parameter list"))
      return nullptr;
    Type *retTy = nullptr;
    if (accept(Tok::Arrow)) {
      if (at(Tok::KwView) || at(Tok::KwInout)) {
        error(cur().Loc, "a lambda's result is a copy: '-> " +
                             std::string(cur().Text) + " T' is not supported");
        return nullptr;
      }
      retTy = parseTypeAnnotation();
      if (!retTy)
        return nullptr;
    }
    // `fn(x: int) -> int = x * 2` is `fn(x: int) -> int { return x * 2; }`,
    // and without the result type, `fn(x: int) = x * 2`, the result type is
    // the expression's (Sema infers it).
    if (at(Tok::Assign)) {
      consume();
      SourceLocation from = cur().Loc;
      Expr *value = parseExpression();
      if (!value)
        return nullptr;
      std::vector<Stmt *> ret{Ctx.make<ReturnStmt>(span(from), value)};
      auto *body = Ctx.make<CompoundStmt>(span(from), std::move(ret));
      auto *lambda =
          Ctx.make<LambdaExpr>(span(start), std::move(params), retTy, body);
      lambda->setInferredResult(retTy == nullptr);
      return lambda;
    }
    if (!at(Tok::LBrace)) {
      errorAtCurrent("expected '{' or '=' to start the lambda's body");
      return nullptr;
    }
    CompoundStmt *body = parseBlock();
    if (!body)
      return nullptr;
    return Ctx.make<LambdaExpr>(span(start), std::move(params), retTy, body);
  }

  case Tok::LBracket: {
    consume();
    std::vector<Expr *> elems;
    if (!parseArgumentList(Tok::RBracket, elems))
      return nullptr;
    // `[v; n]`: n copies of v.
    if (elems.size() == 1 && accept(Tok::Semi)) {
      if (!at(Tok::Int)) {
        errorAtCurrent("expected the number of copies, an integer literal");
        return nullptr;
      }
      Token n = consume();
      if (n.IntValue < 1) {
        error(n.Loc, "an array has at least one element: the count must be 1 "
                     "or more");
        return nullptr;
      }
      if (!expect(Tok::RBracket, "to close the array literal"))
        return nullptr;
      return Ctx.make<ArrayLiteralExpr>(span(start), std::move(elems),
                                        n.IntValue);
    }
    if (!expect(Tok::RBracket, "to close the array literal"))
      return nullptr;
    return Ctx.make<ArrayLiteralExpr>(span(start), std::move(elems));
  }

  default:
    errorAtCurrent("expected an expression");
    return nullptr;
  }
}

} // namespace paykan::frontend::recursive_descent
