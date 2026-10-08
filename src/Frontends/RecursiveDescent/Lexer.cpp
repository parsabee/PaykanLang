// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "Lexer.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace paykan::frontend::recursive_descent {

namespace {

bool isIdentStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isIdentChar(char c) { return isIdentStart(c) || isDigit(c); }

struct Keyword {
  std::string_view Spelling;
  Tok Kind;
};

// Keywords in docs/grammar.md.  `_` on its own is the UNDERSCORE token; any
// longer identifier starting with `_` is an ordinary identifier.
constexpr Keyword kKeywords[] = {
    {"import", Tok::KwImport}, {"as", Tok::KwAs},
    {"class", Tok::KwClass},   {"enum", Tok::KwEnum},
    {"match", Tok::KwMatch},   {"mov", Tok::KwMov},
    {"fn", Tok::KwFn},         {"return", Tok::KwReturn},
    {"if", Tok::KwIf},         {"then", Tok::KwThen},
    {"else", Tok::KwElse},     {"while", Tok::KwWhile},
    {"break", Tok::KwBreak},   {"continue", Tok::KwContinue},
    {"True", Tok::KwTrue},     {"False", Tok::KwFalse},
    {"None", Tok::KwNone},     {"_", Tok::Underscore},
};

// The ownership prototype's keywords (docs/design/ownership-proto.md).
constexpr Keyword kOwnershipKeywords[] = {
    {"view", Tok::KwView},
    {"inout", Tok::KwInout},
    {"let", Tok::KwLet},
};

} // namespace

const char *describe(Tok k) {
  switch (k) {
  case Tok::Eof:
    return "end of file";
  case Tok::Ident:
    return "identifier";
  case Tok::Int:
    return "integer literal";
  case Tok::Float:
    return "float literal";
  case Tok::Char:
    return "character literal";
  case Tok::String:
    return "string literal";
  case Tok::KwImport:
    return "'import'";
  case Tok::KwAs:
    return "'as'";
  case Tok::KwClass:
    return "'class'";
  case Tok::KwEnum:
    return "'enum'";
  case Tok::KwMatch:
    return "'match'";
  case Tok::KwMov:
    return "'mov'";
  case Tok::KwFn:
    return "'fn'";
  case Tok::KwReturn:
    return "'return'";
  case Tok::KwIf:
    return "'if'";
  case Tok::KwThen:
    return "'then'";
  case Tok::KwElse:
    return "'else'";
  case Tok::KwWhile:
    return "'while'";
  case Tok::KwBreak:
    return "'break'";
  case Tok::KwContinue:
    return "'continue'";
  case Tok::KwTrue:
    return "'True'";
  case Tok::KwFalse:
    return "'False'";
  case Tok::KwNone:
    return "'None'";
  case Tok::KwView:
    return "'view'";
  case Tok::KwInout:
    return "'inout'";
  case Tok::KwLet:
    return "'let'";
  case Tok::Underscore:
    return "'_'";
  case Tok::Assign:
    return "'='";
  case Tok::Minus:
    return "'-'";
  case Tok::Plus:
    return "'+'";
  case Tok::Star:
    return "'*'";
  case Tok::Slash:
    return "'/'";
  case Tok::Percent:
    return "'%'";
  case Tok::Less:
    return "'<'";
  case Tok::Greater:
    return "'>'";
  case Tok::LessEq:
    return "'<='";
  case Tok::GreaterEq:
    return "'>='";
  case Tok::EqEq:
    return "'=='";
  case Tok::NotEq:
    return "'!='";
  case Tok::Not:
    return "'!'";
  case Tok::Question:
    return "'?'";
  case Tok::Colon:
    return "':'";
  case Tok::ColonColon:
    return "'::'";
  case Tok::Dot:
    return "'.'";
  case Tok::Semi:
    return "';'";
  case Tok::Comma:
    return "','";
  case Tok::LParen:
    return "'('";
  case Tok::RParen:
    return "')'";
  case Tok::LBrace:
    return "'{'";
  case Tok::RBrace:
    return "'}'";
  case Tok::LBracket:
    return "'['";
  case Tok::RBracket:
    return "']'";
  case Tok::Arrow:
    return "'->'";
  case Tok::AndAnd:
    return "'&&'";
  case Tok::OrOr:
    return "'||'";
  }
  return "token";
}

const char *tokenKindName(Tok k) {
  switch (k) {
  case Tok::Eof:
    return "EOF";
  case Tok::Ident:
    return "IDENT";
  case Tok::Int:
    return "INT";
  case Tok::Float:
    return "FLOAT";
  case Tok::Char:
    return "CHAR";
  case Tok::String:
    return "STRING";
  case Tok::KwImport:
    return "KW_IMPORT";
  case Tok::KwAs:
    return "KW_AS";
  case Tok::KwClass:
    return "KW_CLASS";
  case Tok::KwEnum:
    return "KW_ENUM";
  case Tok::KwMatch:
    return "KW_MATCH";
  case Tok::KwMov:
    return "KW_MOV";
  case Tok::KwFn:
    return "KW_FN";
  case Tok::KwReturn:
    return "KW_RETURN";
  case Tok::KwIf:
    return "KW_IF";
  case Tok::KwThen:
    return "KW_THEN";
  case Tok::KwElse:
    return "KW_ELSE";
  case Tok::KwWhile:
    return "KW_WHILE";
  case Tok::KwBreak:
    return "KW_BREAK";
  case Tok::KwContinue:
    return "KW_CONTINUE";
  case Tok::KwTrue:
    return "KW_TRUE";
  case Tok::KwFalse:
    return "KW_FALSE";
  case Tok::KwNone:
    return "KW_NONE";
  case Tok::KwView:
    return "KW_VIEW";
  case Tok::KwInout:
    return "KW_INOUT";
  case Tok::KwLet:
    return "KW_LET";
  case Tok::Underscore:
    return "UNDERSCORE";
  case Tok::Assign:
    return "ASSIGN";
  case Tok::Minus:
    return "MINUS";
  case Tok::Plus:
    return "PLUS";
  case Tok::Star:
    return "STAR";
  case Tok::Slash:
    return "SLASH";
  case Tok::Percent:
    return "PERCENT";
  case Tok::Less:
    return "LESS";
  case Tok::Greater:
    return "GREATER";
  case Tok::LessEq:
    return "LESS_EQ";
  case Tok::GreaterEq:
    return "GREATER_EQ";
  case Tok::EqEq:
    return "EQ_EQ";
  case Tok::NotEq:
    return "NOT_EQ";
  case Tok::Not:
    return "NOT";
  case Tok::Question:
    return "QUESTION";
  case Tok::Colon:
    return "COLON";
  case Tok::ColonColon:
    return "COLON_COLON";
  case Tok::Dot:
    return "DOT";
  case Tok::Semi:
    return "SEMI";
  case Tok::Comma:
    return "COMMA";
  case Tok::LParen:
    return "LPAREN";
  case Tok::RParen:
    return "RPAREN";
  case Tok::LBrace:
    return "LBRACE";
  case Tok::RBrace:
    return "RBRACE";
  case Tok::LBracket:
    return "LBRACKET";
  case Tok::RBracket:
    return "RBRACKET";
  case Tok::Arrow:
    return "ARROW";
  case Tok::AndAnd:
    return "AND_AND";
  case Tok::OrOr:
    return "OR_OR";
  }
  return "UNKNOWN";
}

Lexer::Lexer(std::string_view source, ErrorHandler onError, bool ownership)
    : Src(source), OnError(std::move(onError)), Ownership(ownership) {}

void Lexer::error(size_t startLine, size_t startCol, const std::string &msg) {
  if (OnError)
    OnError(ast::SourceLocation(startLine, startCol, Line, Col), msg);
}

Token Lexer::make(Tok k, size_t start, size_t startLine, size_t startCol) {
  Token t;
  t.Kind = k;
  t.Loc = ast::SourceLocation(startLine, startCol, Line, Col);
  t.Text = Src.substr(start, Pos - start);
  t.Offset = start;
  return t;
}

// Whitespace (space, tab, CR), newlines and `//` comments.
void Lexer::skipTrivia() {
  while (!atEnd()) {
    char c = cur();
    if (c == ' ' || c == '\t' || c == '\r') {
      advance();
    } else if (c == '\n') {
      ++Pos;
      ++Line;
      Col = 1;
    } else if (c == '/' && peekAt(1) == '/') {
      while (!atEnd() && cur() != '\n')
        advance();
    } else {
      break;
    }
  }
}

Token Lexer::next() {
  skipTrivia();
  size_t start = Pos, line = Line, col = Col;
  Token t;
  if (atEnd()) {
    t = make(Tok::Eof, start, line, col);
  } else {
    char c = cur();
    if (isDigit(c)) {
      // Digits glued to a preceding `.` are a tuple index (`t.0`, `t.0.1`),
      // never a number literal: `t.0.1` must not scan the `0.1` as a float.
      bool afterDot = PrevKind == Tok::Dot && PrevEnd == Pos;
      t = lexNumber(start, line, col, afterDot);
    } else if (isIdentStart(c)) {
      t = lexIdentOrKeyword(start, line, col);
    } else if (c == '\'') {
      if (!lexChar(start, line, col, t))
        return next();
    } else if (c == '"') {
      if (!lexString(start, line, col, t))
        return next();
    } else {
      Tok k;
      size_t len = 1;
      char n = peekAt(1);
      switch (c) {
      case ':':
        k = n == ':' ? (len = 2, Tok::ColonColon) : Tok::Colon;
        break;
      case '-':
        k = n == '>' ? (len = 2, Tok::Arrow) : Tok::Minus;
        break;
      case '<':
        k = n == '=' ? (len = 2, Tok::LessEq) : Tok::Less;
        break;
      case '>':
        k = n == '=' ? (len = 2, Tok::GreaterEq) : Tok::Greater;
        break;
      case '=':
        k = n == '=' ? (len = 2, Tok::EqEq) : Tok::Assign;
        break;
      case '!':
        k = n == '=' ? (len = 2, Tok::NotEq) : Tok::Not;
        break;
      case '&':
        if (n == '&') {
          k = Tok::AndAnd;
          len = 2;
        } else {
          advance();
          error(line, col, std::string("invalid character '") + c + "'");
          return next();
        }
        break;
      case '|':
        if (n == '|') {
          k = Tok::OrOr;
          len = 2;
        } else {
          advance();
          error(line, col, std::string("invalid character '") + c + "'");
          return next();
        }
        break;
      case '+':
        k = Tok::Plus;
        break;
      case '*':
        k = Tok::Star;
        break;
      case '/':
        k = Tok::Slash;
        break;
      case '%':
        k = Tok::Percent;
        break;
      case '?':
        k = Tok::Question;
        break;
      case '.':
        k = Tok::Dot;
        break;
      case ';':
        k = Tok::Semi;
        break;
      case ',':
        k = Tok::Comma;
        break;
      case '(':
        k = Tok::LParen;
        break;
      case ')':
        k = Tok::RParen;
        break;
      case '{':
        k = Tok::LBrace;
        break;
      case '}':
        k = Tok::RBrace;
        break;
      case '[':
        k = Tok::LBracket;
        break;
      case ']':
        k = Tok::RBracket;
        break;
      default:
        advance();
        error(line, col, std::string("invalid character '") + c + "'");
        return next();
      }
      advance(len);
      t = make(k, start, line, col);
    }
  }
  PrevKind = t.Kind;
  PrevEnd = Pos;
  return t;
}

// INT    ::= digit+
// FLOAT  ::= digit+ "." digit* exponent? | digit+ exponent
// exponent ::= ("e" | "E") ("+" | "-")? digit+
//
// Longest match (docs/grammar.md): `1.` is a float, `1e` is the
// integer 1 followed by the identifier `e`.  After a `.` (afterDot) only the
// digits are taken; the parser turns them into a tuple index and validates
// them (no leading zeros, in range) itself.
Token Lexer::lexNumber(size_t start, size_t line, size_t col, bool afterDot) {
  while (!atEnd() && isDigit(cur()))
    advance();
  if (afterDot)
    return make(Tok::Int, start, line, col);

  auto exponentLength = [&]() -> size_t {
    if (peekAt(0) != 'e' && peekAt(0) != 'E')
      return 0;
    size_t i = 1;
    if (peekAt(i) == '+' || peekAt(i) == '-')
      ++i;
    if (!isDigit(peekAt(i)))
      return 0;
    while (isDigit(peekAt(i)))
      ++i;
    return i;
  };

  bool isFloat = false;
  if (!atEnd() && cur() == '.') {
    isFloat = true;
    advance();
    while (!atEnd() && isDigit(cur()))
      advance();
    advance(exponentLength());
  } else if (size_t n = exponentLength()) {
    isFloat = true;
    advance(n);
  }

  Token t = make(isFloat ? Tok::Float : Tok::Int, start, line, col);
  std::string text(t.Text);
  if (isFloat) {
    // A literal must denote a finite float: one that overflows to ±inf, or
    // a nonzero one that underflows to 0, is an error.  A subnormal result
    // is fine although strtod reports ERANGE for it too.
    errno = 0;
    t.FloatValue = std::strtod(text.c_str(), nullptr);
    if (errno == ERANGE && (std::isinf(t.FloatValue) || t.FloatValue == 0.0)) {
      error(line, col, "float is out of range: " + text);
      t.FloatValue = 0.0;
    }
    return t;
  }
  // The scanner never sees a sign (`-` is its own token), so INT64_MIN's
  // magnitude is out of range here and must be written as an expression,
  // e.g. `-9223372036854775807 - 1`.
  errno = 0;
  long long v = std::strtoll(text.c_str(), nullptr, 10);
  if (errno == ERANGE) {
    error(line, col, "integer is out of range: " + text);
    v = 0;
  }
  t.IntValue = static_cast<int64_t>(v);
  return t;
}

Token Lexer::lexIdentOrKeyword(size_t start, size_t line, size_t col) {
  while (!atEnd() && isIdentChar(cur()))
    advance();
  std::string_view text = Src.substr(start, Pos - start);
  for (const auto &kw : kKeywords)
    if (kw.Spelling == text)
      return make(kw.Kind, start, line, col);
  if (Ownership)
    for (const auto &kw : kOwnershipKeywords)
      if (kw.Spelling == text)
        return make(kw.Kind, start, line, col);
  return make(Tok::Ident, start, line, col);
}

// CHAR ::= "'" ( [^\\'\n] | "\\" [^\n] ) "'"
// Escapes: \n \t \r \\ \' \0; any other `\X` is X itself.
bool Lexer::lexChar(size_t start, size_t line, size_t col, Token &out) {
  advance(); // opening quote
  char value = 0;
  bool haveChar = false;
  if (!atEnd() && cur() == '\\') {
    if (peekAt(1) != '\n' && Pos + 1 < Src.size()) {
      char e = peekAt(1);
      switch (e) {
      case 'n':
        value = '\n';
        break;
      case 't':
        value = '\t';
        break;
      case 'r':
        value = '\r';
        break;
      case '0':
        value = '\0';
        break;
      default:
        value = e; // includes \\ and \'
        break;
      }
      advance(2);
      haveChar = true;
    }
  } else if (!atEnd() && cur() == '\'') {
    // `''`: one error for the whole literal (#119), not two unterminated ones.
    advance();
    error(line, col, "empty character literal");
    return false;
  } else if (!atEnd() && cur() != '\n') {
    value = cur();
    advance();
    haveChar = true;
  }
  if (haveChar && !atEnd() && cur() == '\'') {
    advance();
    out = make(Tok::Char, start, line, col);
    out.CharValue = value;
    return true;
  }
  if (haveChar) {
    // More characters before a closing quote on the same line (`'ab'`): one
    // error for the whole literal, rather than two unterminated ones.
    size_t i = 0;
    while (Pos + i < Src.size() && peekAt(i) != '\'' && peekAt(i) != '\n')
      i += (peekAt(i) == '\\' && peekAt(i + 1) != '\n') ? 2 : 1;
    if (Pos + i < Src.size() && peekAt(i) == '\'') {
      advance(i + 1);
      error(line, col,
            "character literal must contain exactly one character (use a "
            "string literal for text)");
      return false;
    }
  }
  // The closing quote is missing before the end of the line or file.  The
  // quote (and the one character after it) is the error token
  // (docs/grammar.md); the newline is left for line tracking.
  error(line, col, "unterminated character literal");
  return false;
}

// STRING ::= '"' ( [^\\"\n] | "\\" [^\n] )* '"'
// Single-line by design: a raw newline ends the literal with an error (the
// supported way to embed one is the \n escape).  Escapes: \n \t \r \\ \" \0;
// any other `\X` is kept verbatim as the two characters.
bool Lexer::lexString(size_t start, size_t line, size_t col, Token &out) {
  advance(); // opening quote
  std::string value;
  while (!atEnd()) {
    char c = cur();
    if (c == '"') {
      advance();
      out = make(Tok::String, start, line, col);
      out.StrValue = std::move(value);
      return true;
    }
    if (c == '\n')
      break;
    if (c == '\\') {
      if (Pos + 1 >= Src.size() || peekAt(1) == '\n') {
        // A line ending in a lone backslash: the backslash belongs to the
        // broken literal, the newline does not.
        advance();
        break;
      }
      char e = peekAt(1);
      switch (e) {
      case 'n':
        value += '\n';
        break;
      case 't':
        value += '\t';
        break;
      case 'r':
        value += '\r';
        break;
      case '\\':
        value += '\\';
        break;
      case '"':
        value += '"';
        break;
      case '0':
        value += '\0';
        break;
      default:
        value += '\\';
        value += e;
        break;
      }
      advance(2);
      continue;
    }
    value += c;
    advance();
  }
  error(line, col,
        "unterminated string literal (string literals cannot span multiple "
        "lines; use \\n)");
  return false;
}

} // namespace paykan::frontend::recursive_descent
