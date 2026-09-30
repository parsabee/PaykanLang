// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "ParserDriverImpl.h"
#include <cerrno>
#include <cstring> // strerror
#include <fstream>
#include <iostream>

namespace paykan::parser {

// -- ParserDriver (public PIMPL wrapper) -------------------------------------

ParserDriver::ParserDriver(bool TraceParsing, bool TraceScanning)
    : PImpl(std::make_unique<Impl>(TraceParsing, TraceScanning)) {}

ParserDriver::~ParserDriver() = default;

int ParserDriver::parseFile(const std::string &filename) {
  PImpl->CurFile = filename;
  PImpl->ErrorCount = 0;
  PImpl->SourceLines.clear();

  std::ifstream in(filename);
  std::string line;
  while (std::getline(in, line))
    PImpl->SourceLines.push_back(line);

  PImpl->Location.initialize(&PImpl->CurFile);
  if (!PImpl->scanBegin()) {
    // The input file could not be opened.  Report through the attached
    // DiagEngine when there is one (same channel as every other compiler
    // diagnostic) with an invalid location -- there is no source to point
    // at -- and fall back to stderr otherwise.  Either way the parse fails
    // cleanly; library code must never exit() the whole process.
    ++PImpl->ErrorCount;
    std::string msg = "cannot open '" + filename + "': " + std::strerror(errno);
    if (PImpl->Diags)
      PImpl->Diags->error(ast::SourceLocation(), msg);
    else
      std::cerr << "error: " << msg << "\n";
    return 1;
  }
  int result = PImpl->parse(*this);
  PImpl->scanEnd();
  return (result != 0 || PImpl->ErrorCount > 0) ? 1 : 0;
}

ast::TranslationUnit *ParserDriver::getRoot() { return PImpl->Root; }

ast::ASTContext &ParserDriver::getASTContext() { return PImpl->Ctx; }

unsigned ParserDriver::getErrorCount() const { return PImpl->ErrorCount; }

const std::string &ParserDriver::getCurrentFile() const {
  return PImpl->CurFile;
}

const std::vector<std::string> &ParserDriver::getSourceLines() const {
  return PImpl->SourceLines;
}

void ParserDriver::setDiagEngine(sema::DiagEngine *diag) {
  PImpl->Diags = diag;
}

// -- ParserDriver::Impl ------------------------------------------------------

int ParserDriver::Impl::parse(ParserDriver &drv) {
  Lookahead.clear();
  PrevWasIdent = false;
  yy::parser parser(drv);
  parser.set_debug_level(TraceParsing);
  return parser.parse();
}

} // namespace paykan::parser

// -- yylex wrapper: type-argument disambiguation ------------------------------
//
// `IDENT <` is ambiguous with one token of lookahead: `i < n` is a comparison
// while `first<int>(xs)` opens a type-argument list.  The grammar is LALR(1),
// so the decision is made here instead, C#-style: on a '<' that directly
// follows an identifier, scan ahead over the tokens a type-argument list may
// contain (identifiers, '::', ',', '[', ']', nested '<' '>') to the matching
// '>'; when that '>' is immediately followed by '(' the '<' is delivered as
// TYPELESS, otherwise as the ordinary LESS.  The scanned tokens are queued and
// replayed to the parser afterwards, so nothing is lost.
//
// A comparison can only be misread when it has exactly the shape of a generic
// call, `a < b > (c)` -- which the grammar rejects anyway (relational operators
// do not chain) -- or when two comparisons straddle a comma inside an argument
// list, `f(a < b, c > (d))`; parenthesising either comparison disambiguates.

namespace {

using symbol_kind = yy::parser::symbol_kind;

yy::parser::symbol_type nextToken(paykan::parser::ParserDriver &drv) {
  auto &im = paykan::parser::impl(drv);
  if (!im.Lookahead.empty()) {
    yy::parser::symbol_type tok(std::move(im.Lookahead.front()));
    im.Lookahead.pop_front();
    return tok;
  }
  return yylex_raw(drv);
}

bool isTypeArgToken(symbol_kind::symbol_kind_type k) {
  return k == symbol_kind::S_IDENT || k == symbol_kind::S_COLONCOLON ||
         k == symbol_kind::S_COMMA || k == symbol_kind::S_LSQUARE ||
         k == symbol_kind::S_RSQUARE;
}

} // namespace

yy::parser::symbol_type yylex(paykan::parser::ParserDriver &drv) {
  auto &im = paykan::parser::impl(drv);
  yy::parser::symbol_type tok = nextToken(drv);
  const bool afterIdent = im.PrevWasIdent;
  im.PrevWasIdent = tok.kind() == symbol_kind::S_IDENT;
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
    im.Lookahead.push_front(std::move(*it));

  if (opensTypeArgs)
    return yy::parser::make_TYPELESS(tok.location);
  return tok;
}
