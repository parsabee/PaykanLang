// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Reentrant lexer of the recursive-descent frontend for the Paykan language.
//
// Pull-based: the parser calls next() for one token at a time.  The lexer
// holds no global state, so any number of lexers may run concurrently (one
// per translation unit, or per imported module).  Tokens, their classes and
// their lexical rules are specified in docs/grammar.md.

#pragma once

#include "AST.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace paykan::frontend::recursive_descent {

/// Token kinds.  One enumerator per terminal of docs/grammar.md.
enum class Tok : uint8_t {
  Eof,
  // Literals and names
  Ident,
  Int,
  Float,
  Char,
  String,
  // Keywords
  KwImport,
  KwAs,
  KwClass,
  KwEnum,
  KwMatch,
  KwMov,
  KwFn,
  KwNative,
  KwReturn,
  KwIf,
  KwThen,
  KwElse,
  KwWhile,
  KwBreak,
  KwContinue,
  KwTrue,
  KwFalse,
  KwNone,
  KwView, // parameter modes
  KwInout,
  Underscore, // `_` (destructuring skip / match wildcard); `_x` is an Ident
  // Punctuation and operators
  Assign,     // =
  Minus,      // -
  Plus,       // +
  Star,       // *
  Slash,      // /
  Percent,    // %
  Less,       // <
  Greater,    // >
  LessEq,     // <=
  GreaterEq,  // >=
  EqEq,       // ==
  NotEq,      // !=
  Not,        // !
  Question,   // ?
  Colon,      // :
  ColonColon, // ::
  Dot,        // .
  Semi,       // ;
  Comma,      // ,
  LParen,     // (
  RParen,     // )
  LBrace,     // {
  RBrace,     // }
  LBracket,   // [
  RBracket,   // ]
  Arrow,      // ->
  AndAnd,     // &&
  OrOr,       // ||
};

/// Human-readable name of a token kind for diagnostics and --dump-tokens:
/// punctuation is quoted (`';'`), everything else is a noun (`identifier`).
const char *describe(Tok k);

/// Stable upper-case spelling of a token kind for --dump-tokens (IDENT, INT,
/// KW_FN, SEMI, ...).
const char *tokenKindName(Tok k);

struct Token {
  Tok Kind = Tok::Eof;
  /// Source range: 1-based line/column; the end column is one past the last
  /// character (the convention every frontend follows, so that AST node
  /// locations match exactly).
  ast::SourceLocation Loc;
  /// The raw source text of the token (a view into the lexer's buffer).
  std::string_view Text;
  /// Byte offset of the first character in the source buffer.  Lets the
  /// parser tell `t.0` (adjacent, a tuple index) from `t. 0` (an error).
  size_t Offset = 0;
  /// Decoded values for literal tokens.
  int64_t IntValue = 0;
  double FloatValue = 0.0;
  char CharValue = 0;
  /// Decoded string value (escapes processed) for String tokens.
  std::string StrValue;

  bool is(Tok k) const { return Kind == k; }
};

class Lexer {
public:
  /// Called for every lexical error with its location and message.
  using ErrorHandler =
      std::function<void(ast::SourceLocation, const std::string &)>;

  Lexer(std::string_view source, ErrorHandler onError);

  /// Scan and return the next token.  Lexical errors are reported through
  /// the handler and the offending text is skipped; the scan then continues,
  /// so next() always eventually returns Eof.
  Token next();

private:
  std::string_view Src;
  size_t Pos = 0;
  size_t Line = 1;
  size_t Col = 1;
  ErrorHandler OnError;
  /// Kind and end offset of the previously returned token, which decide
  /// whether a run of digits is a tuple index (`t.0`) or a number literal.
  Tok PrevKind = Tok::Eof;
  size_t PrevEnd = 0;

  bool atEnd() const { return Pos >= Src.size(); }
  char cur() const { return Src[Pos]; }
  char peekAt(size_t k) const {
    return Pos + k < Src.size() ? Src[Pos + k] : '\0';
  }
  void advance(size_t n = 1) {
    Pos += n;
    Col += n;
  }

  void skipTrivia();
  Token make(Tok k, size_t start, size_t startLine, size_t startCol);
  Token lexNumber(size_t start, size_t line, size_t col, bool afterDot);
  Token lexIdentOrKeyword(size_t start, size_t line, size_t col);
  bool lexChar(size_t start, size_t line, size_t col, Token &out);
  bool lexString(size_t start, size_t line, size_t col, Token &out);
  void error(size_t startLine, size_t startCol, const std::string &msg);
};

} // namespace paykan::frontend::recursive_descent
