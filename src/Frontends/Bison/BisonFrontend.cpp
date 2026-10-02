// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "BisonFrontend.h"

#include <exception>
#include <memory>

namespace paykan::frontend::bison {

// -- Frontend interface ------------------------------------------------------

ParseResult BisonFrontend::parse(std::string_view filename,
                                 std::string_view source, ast::ASTContext &ctx,
                                 sema::DiagEngine &diag, const Options &opts) {
  Ctx = &ctx;
  Root = nullptr;
  CurFile = std::string(filename);
  ErrorCount = 0;
  Diags = &diag;
  TraceParsing = opts.TraceParsing;
  TraceScanning = opts.TraceScanning;
  Lookahead.clear();
  PrevWasIdent = false;

  Location.initialize(&CurFile);
  scanBegin(source);
  int result = 1;
  // Bison's lalr1.cc skeleton and our actions use exceptions internally
  // (syntax_error); the parser catches those itself.  Nothing may cross the
  // frontend interface, so anything else that escapes -- a std::bad_alloc, a
  // bug in an action -- becomes a diagnostic here.
  try {
    yy::parser parser(*this);
    parser.set_debug_level(TraceParsing);
    result = parser.parse();
  } catch (const std::exception &e) {
    ++ErrorCount;
    diag.error(ast::SourceLocation(),
               std::string("internal parser error: ") + e.what());
  } catch (...) {
    ++ErrorCount;
    diag.error(ast::SourceLocation(), "internal parser error");
  }
  scanEnd();

  ParseResult r;
  r.Root = (result == 0) ? Root : nullptr;
  r.ErrorCount = ErrorCount;
  // A failed parse that reported nothing (should not happen) still fails.
  if (result != 0 && r.ErrorCount == 0)
    r.ErrorCount = 1;
  Diags = nullptr;
  Ctx = nullptr;
  return r;
}

static std::unique_ptr<Frontend> createBisonFrontend() {
  return std::make_unique<BisonFrontend>();
}

} // namespace paykan::frontend::bison

PAYKAN_REGISTER_FRONTEND(bison, "bison",
                         &paykan::frontend::bison::createBisonFrontend);

// -- yylex wrapper: type-argument disambiguation ------------------------------
//
// `IDENT <` is ambiguous with one token of lookahead: `i < n` is a comparison
// while `first<int>(xs)` opens a type-argument list.  The grammar is LALR(1),
// so the decision is made here instead, C#-style: on a '<' that directly
// follows an identifier, scan ahead over the tokens a type-argument list may
// contain (identifiers, '::', ',', '[', ']', '?' for optional types, '(' ')'
// for tuple types, nested '<' '>') to the matching '>'; when that '>' is
// immediately followed by '(' the '<' is delivered as TYPELESS, otherwise as
// the ordinary LESS.  The scanned tokens are queued and replayed to the
// parser afterwards, so nothing is lost.
//
// Brackets are matched during the scan: a ')' or ']' that closes a bracket
// opened before the '<' (`f(a < b, c) > (d)`) cannot belong to a type list
// and ends the scan, so that call stays a comparison.  A comparison can then
// only be misread when it has exactly the shape of a generic call,
// `a < b > (c)` -- which the grammar rejects anyway (relational operators do
// not chain) -- or when two comparisons straddle a comma inside an argument
// list, `f(a < b, c > (d))`; parenthesising either comparison disambiguates.
// docs/grammar.md section 7 specifies the rule both frontends implement.
//
// Example token streams (what the parser sees):
//
//   i < n                 IDENT LESS IDENT
//   first<int>(xs)        IDENT TYPELESS IDENT MORE LPAREN IDENT RPAREN
//   Box<Str?>("v")        IDENT TYPELESS IDENT QUESTION MORE LPAREN ...
//   f(a < b, c) > (d)     ... IDENT LESS IDENT COMMA IDENT RPAREN MORE ...
//                         (the RPAREN closes the call's '(' before the scan
//                         reaches a '>', so the '<' stays LESS)
//
// Why a wrapper rather than a grammar change: telling the two readings apart
// needs unbounded lookahead (the type list can nest, `Map<Str, Box<int>[]>`),
// which an LALR(1) grammar cannot express without conflicts, and switching
// Bison to GLR would make every parse pay for one rare ambiguity.  Flex's
// scanner is therefore generated as yylex_raw (see YY_DECL in
// BisonFrontend.h) and this function, which the parser calls as yylex, sits
// between the two.  The wrapper only exists in the Bison plugin: the
// recursive-descent frontend backtracks over `< types > (` directly.
//
// Declarations need no special case: `class Box<T> {` is followed by '{', so
// its '<' stays LESS, while `fn first<T>(xs: T[])` matches the pattern and
// becomes TYPELESS; the grammar's typeArgOpen accepts either token.

namespace {

using symbol_kind = yy::parser::symbol_kind;
using paykan::frontend::bison::BisonFrontend;

// The next token in source order: tokens queued by an earlier scan-ahead come
// first, and only when the queue is empty is the Flex scanner asked for more.
// Every token the wrapper reads goes through here, so a token is never lost
// or read twice.
yy::parser::symbol_type nextToken(BisonFrontend &drv) {
  if (!drv.Lookahead.empty()) {
    yy::parser::symbol_type tok(std::move(drv.Lookahead.front()));
    drv.Lookahead.pop_front();
    return tok;
  }
  return yylex_raw(drv);
}

// Tokens that may appear inside a type-argument list (besides the nested
// '<' / '>' handled by the scan itself): names and module paths, commas,
// array suffixes `[]`, the optional suffix `?`, and parentheses for tuple
// types such as `(int, Str)`.  Seeing any other token proves the '<' was a
// comparison.
bool isTypeArgToken(symbol_kind::symbol_kind_type k) {
  return k == symbol_kind::S_IDENT || k == symbol_kind::S_COLONCOLON ||
         k == symbol_kind::S_COMMA || k == symbol_kind::S_LSQUARE ||
         k == symbol_kind::S_RSQUARE || k == symbol_kind::S_QUESTION ||
         k == symbol_kind::S_LPAREN || k == symbol_kind::S_RPAREN;
}

} // namespace

yy::parser::symbol_type yylex(BisonFrontend &drv) {
  yy::parser::symbol_type tok = nextToken(drv);
  // PrevWasIdent tracks the token most recently *handed to the parser*
  // (including tokens replayed from the queue), not the last token scanned,
  // so `a < b` inside a replayed run is still judged against its real
  // predecessor.
  const bool afterIdent = drv.PrevWasIdent;
  drv.PrevWasIdent = tok.kind() == symbol_kind::S_IDENT;
  // Fast path: only a '<' directly after an identifier is ambiguous.  Every
  // other token, including a '<' after ')' or a literal, passes through.
  if (tok.kind() != symbol_kind::S_LESS || !afterIdent)
    return tok;

  // Scan ahead for `... > (`.  Everything read is kept in `scanned` so it can
  // be replayed; nothing is consumed on the parser's behalf.
  //   depth           open '<' (starts at 1 for the '<' being classified)
  //   parens/squares  '(' and '[' opened *inside* the candidate type list;
  //                   a closer with no matching opener belongs to an
  //                   enclosing expression, so the scan stops there.
  std::vector<yy::parser::symbol_type> scanned;
  int depth = 1;
  int parens = 0, squares = 0;
  bool opensTypeArgs = false;
  for (;;) {
    yy::parser::symbol_type t = nextToken(drv);
    auto k = t.kind();
    scanned.push_back(std::move(t));
    if (k == symbol_kind::S_LESS) {
      ++depth;
    } else if (k == symbol_kind::S_MORE) {
      if (--depth == 0) {
        // Matching '>' found: the list is type arguments only if a call's
        // '(' follows.  That token is read too and queued with the rest.
        yy::parser::symbol_type after = nextToken(drv);
        opensTypeArgs = after.kind() == symbol_kind::S_LPAREN;
        scanned.push_back(std::move(after));
        break;
      }
    } else if (k == symbol_kind::S_LPAREN) {
      ++parens;
    } else if (k == symbol_kind::S_RPAREN) {
      if (parens-- == 0)
        break; // closes a bracket opened before the '<': not a type list
    } else if (k == symbol_kind::S_LSQUARE) {
      ++squares;
    } else if (k == symbol_kind::S_RSQUARE) {
      if (squares-- == 0)
        break;
    } else if (!isTypeArgToken(k)) {
      break; // anything else (operators, literals, EOF) ends a type list
    }
  }
  // Replay the scanned tokens after this one, in source order.  They go to
  // the *front* of the queue (in reverse, so the first scanned ends up first)
  // because the queue may already hold tokens from an outer scan-ahead that
  // come later in the source; a nested `IDENT <` inside a replayed run is
  // classified again when the parser reaches it.
  for (auto it = scanned.rbegin(); it != scanned.rend(); ++it)
    drv.Lookahead.push_front(std::move(*it));

  // Only the '<' itself changes kind; its location is kept so diagnostics
  // still point at the source character.

  if (opensTypeArgs)
    return yy::parser::make_TYPELESS(tok.location);
  return tok;
}
