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
// A comparison can only be misread when it has exactly the shape of a generic
// call, `a < b > (c)` -- which the grammar rejects anyway (relational operators
// do not chain) -- or when two comparisons straddle a comma inside an argument
// list, `f(a < b, c > (d))`; parenthesising either comparison disambiguates.

namespace {

using symbol_kind = yy::parser::symbol_kind;
using paykan::frontend::bison::BisonFrontend;

yy::parser::symbol_type nextToken(BisonFrontend &drv) {
  if (!drv.Lookahead.empty()) {
    yy::parser::symbol_type tok(std::move(drv.Lookahead.front()));
    drv.Lookahead.pop_front();
    return tok;
  }
  return yylex_raw(drv);
}

bool isTypeArgToken(symbol_kind::symbol_kind_type k) {
  return k == symbol_kind::S_IDENT || k == symbol_kind::S_COLONCOLON ||
         k == symbol_kind::S_COMMA || k == symbol_kind::S_LSQUARE ||
         k == symbol_kind::S_RSQUARE || k == symbol_kind::S_QUESTION ||
         k == symbol_kind::S_LPAREN || k == symbol_kind::S_RPAREN;
}

} // namespace

yy::parser::symbol_type yylex(BisonFrontend &drv) {
  yy::parser::symbol_type tok = nextToken(drv);
  const bool afterIdent = drv.PrevWasIdent;
  drv.PrevWasIdent = tok.kind() == symbol_kind::S_IDENT;
  if (tok.kind() != symbol_kind::S_LESS || !afterIdent)
    return tok;

  // Scan ahead for `... > (`.
  std::vector<yy::parser::symbol_type> scanned;
  int depth = 1;
  bool opensTypeArgs = false;
  for (;;) {
    yy::parser::symbol_type t = nextToken(drv);
    auto k = t.kind();
    scanned.push_back(std::move(t));
    if (k == symbol_kind::S_LESS) {
      ++depth;
    } else if (k == symbol_kind::S_MORE) {
      if (--depth == 0) {
        yy::parser::symbol_type after = nextToken(drv);
        opensTypeArgs = after.kind() == symbol_kind::S_LPAREN;
        scanned.push_back(std::move(after));
        break;
      }
    } else if (!isTypeArgToken(k)) {
      break; // anything else (operators, literals, EOF) ends a type list
    }
  }
  // Replay the scanned tokens after this one, in source order.
  for (auto it = scanned.rbegin(); it != scanned.rend(); ++it)
    drv.Lookahead.push_front(std::move(*it));

  if (opensTypeArgs)
    return yy::parser::make_TYPELESS(tok.location);
  return tok;
}
