// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "BisonFrontend.h"

#include <exception>
#include <memory>
#include <string>

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
  LessIsTypeArgs = false;
  Nesting.assign(1, {yy::parser::symbol_kind::S_YYEOF, 0, 0, {}});
  NestDepth = 0;
  PrevKind = yy::parser::symbol_kind::S_YYEOF;
  PrevWasStmtElse = false;
  LastIfWasTernary = false;

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
  } catch (const NestingTooDeep &e) {
    ++ErrorCount;
    diag.error(ast::SourceLocation(e.Loc.begin.line, e.Loc.begin.column,
                                   e.Loc.end.line, e.Loc.end.column),
               "nesting too deep (more than " + std::to_string(kMaxNesting) +
                   " levels)");
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

// -- Nesting limit ------------------------------------------------------------
//
// The LALR parser keeps its stack on the heap, but the AST it builds is walked
// recursively afterwards (Sema, --dump-ast), so it enforces the same limit
// as the recursive-descent parser (frontend::kMaxNesting, docs/grammar.md
// section 9), with the same message, over the tokens it reads.  A level is
// what recursive descent counts one for:
//
//   - a bracket: `(`, `[` and `{`, and the `<` of a type-argument list --
//     except the parentheses of an `if` / `while` statement's condition;
//   - a prefix operator (`!`, `mov`, a unary `-`), until its operand ends;
//   - a conditional expression `if c then a else b`, until its else-branch
//     ends (it extends to the end of the enclosing expression).
//
// The depth is checked at every token but a closing one, so an empty `f()`,
// `[]` or `int[]` at the limit is accepted, as recursive descent accepts it;
// a `{` is checked at once (an empty block is a level).

namespace {

using sk = yy::parser::symbol_kind;

/// True for a token that can end an operand (so a `-` after it is binary).
bool endsOperand(yy::parser::symbol_kind_type k) {
  switch (k) {
  case sk::S_IDENT:
  case sk::S_INT:
  case sk::S_FLOAT:
  case sk::S_BOOL:
  case sk::S_CHAR:
  case sk::S_NONE:
  case sk::S_STRING:
  case sk::S_TUPLE_INDEX:
  case sk::S_RPAREN:
  case sk::S_RSQUARE:
  case sk::S_RBRACK:
    return true;
  default:
    return false;
  }
}

} // namespace

void BisonFrontend::trackNesting(const yy::parser::symbol_type &tok) {
  const auto k = tok.kind();
  const auto prev = PrevKind;
  const bool prevStmtElse = PrevWasStmtElse;
  PrevKind = k;
  PrevWasStmtElse = k == sk::S_ELSE && prev == sk::S_RBRACK;

  NestLevel *top = &Nesting.back();
  if (Nesting.size() > 1 && k == top->Closer) {
    NestDepth -= top->Weight + top->Prefix +
                 static_cast<unsigned>(top->Ternaries.size());
    Nesting.pop_back();
    return;
  }
  if (NestDepth > kMaxNesting)
    throw NestingTooDeep{tok.location};

  auto endPrefix = [&] {
    NestDepth -= top->Prefix;
    top->Prefix = 0;
  };
  auto endExpression = [&] {
    endPrefix();
    NestDepth -= static_cast<unsigned>(top->Ternaries.size());
    top->Ternaries.clear();
  };
  auto open = [&](yy::parser::symbol_kind_type closer, unsigned weight) {
    Nesting.push_back({closer, weight, 0, {}});
    NestDepth += weight;
  };
  // An `if` that starts a statement (rather than a conditional expression).
  auto stmtIf = [&] {
    return prev == sk::S_YYEOF || prev == sk::S_SEMICOLON ||
           prev == sk::S_LBRACK || prev == sk::S_RBRACK || prevStmtElse;
  };

  switch (k) {
  case sk::S_NOT:
  case sk::S_MOV:
    ++top->Prefix;
    ++NestDepth;
    return;
  case sk::S_MINUS:
    if (endsOperand(prev)) {
      endPrefix(); // binary minus
    } else {
      ++top->Prefix;
      ++NestDepth;
    }
    return;
  case sk::S_LPAREN:
    // The prefix operators of the current operand stay open: `-(x)`,
    // `-f(x)`.
    open(sk::S_RPAREN,
         prev == sk::S_WHILE || (prev == sk::S_IF && !LastIfWasTernary) ? 0
                                                                        : 1);
    return;
  case sk::S_LSQUARE:
    open(sk::S_RSQUARE, 1);
    return;
  case sk::S_LBRACK:
    // A block counts even when empty, as in recursive descent.
    endExpression();
    open(sk::S_RBRACK, 1);
    if (NestDepth > kMaxNesting)
      throw NestingTooDeep{tok.location};
    return;
  case sk::S_TYPELESS:
    open(sk::S_MORE, 1);
    return;
  case sk::S_LESS:
    if (prev == sk::S_IDENT && LessIsTypeArgs)
      open(sk::S_MORE, 1);
    else
      endPrefix();
    return;
  case sk::S_IF:
    endPrefix();
    LastIfWasTernary = !stmtIf();
    if (LastIfWasTernary) {
      top->Ternaries.push_back('c');
      ++NestDepth;
    }
    return;
  case sk::S_THEN:
    endPrefix();
    if (!top->Ternaries.empty() && top->Ternaries.back() == 'c')
      top->Ternaries.back() = 't';
    return;
  case sk::S_ELSE:
    endPrefix();
    if (PrevWasStmtElse)
      return;
    // Conditionals nested in the then-branch end here.
    while (!top->Ternaries.empty() && top->Ternaries.back() == 'e') {
      top->Ternaries.pop_back();
      --NestDepth;
    }
    if (!top->Ternaries.empty() && top->Ternaries.back() == 't')
      top->Ternaries.back() = 'e';
    return;
  case sk::S_COMMA:
  case sk::S_SEMICOLON:
  case sk::S_ASSIGN:
    endExpression();
    return;
  case sk::S_IDENT:
  case sk::S_INT:
  case sk::S_FLOAT:
  case sk::S_BOOL:
  case sk::S_CHAR:
  case sk::S_NONE:
  case sk::S_STRING:
  case sk::S_TUPLE_INDEX:
  case sk::S_DOT:
  case sk::S_COLONCOLON:
    return; // the operand goes on
  default:
    endPrefix(); // a binary operator, or the end of the expression
    return;
  }
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

/// The next token with type-argument disambiguation applied.
yy::parser::symbol_type lexTypeArgs(BisonFrontend &drv) {
  yy::parser::symbol_type tok = nextToken(drv);
  const bool afterIdent = drv.PrevWasIdent;
  drv.PrevWasIdent = tok.kind() == symbol_kind::S_IDENT;
  drv.LessIsTypeArgs = false;
  if (tok.kind() != symbol_kind::S_LESS || !afterIdent)
    return tok;

  // Scan ahead for `... > (`.  The tokens are lexed into the lookahead queue
  // and inspected there, in place, so the parser reads them afterwards.
  auto kindAt = [&drv](size_t i) {
    while (drv.Lookahead.size() <= i)
      drv.Lookahead.push_back(yylex_raw(drv));
    return drv.Lookahead[i].kind();
  };
  int depth = 1;
  int parens = 0, squares = 0;
  bool opensTypeArgs = false;
  for (size_t i = 0;; ++i) {
    auto k = kindAt(i);
    if (k == symbol_kind::S_LESS) {
      // Deeper than the nesting limit is an error either way; stopping here
      // keeps `a<a<a<...` (one scan per '<') from going quadratic.
      if (++depth > static_cast<int>(paykan::frontend::kMaxNesting))
        break;
    } else if (k == symbol_kind::S_MORE) {
      if (--depth == 0) {
        drv.LessIsTypeArgs = true; // the matching '>': a type list
        opensTypeArgs = kindAt(i + 1) == symbol_kind::S_LPAREN;
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

  if (opensTypeArgs)
    return yy::parser::make_TYPELESS(tok.location);
  return tok;
}

} // namespace

yy::parser::symbol_type yylex(BisonFrontend &drv) {
  yy::parser::symbol_type tok = lexTypeArgs(drv);
  drv.trackNesting(tok);
  return tok;
}
