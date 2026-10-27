// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0

#include "Lexer.h"
#include "paykan/Frontend.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <string_view>
#include <utility>

namespace paykan::frontend::recursive_descent {

namespace {

bool isIdentStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isIdentChar(char c) { return isIdentStart(c) || isDigit(c); }

/// Whether @p s names a numeric type: a literal directly followed by one
/// (`255u8`) is the suffix error.
bool isNumericTypeName(std::string_view s) {
  static constexpr std::string_view kNames[] = {
      "i8",  "i16", "i32", "i64", "u8",  "u16",   "u32",
      "u64", "f16", "f32", "f64", "int", "float", "unsigned"};
  return std::find(std::begin(kNames), std::end(kNames), s) != std::end(kNames);
}

/// The value of the hexadecimal digit @p c, either case; 16 for any other
/// character.
unsigned digitValue(char c) {
  if (isDigit(c))
    return static_cast<unsigned>(c - '0');
  if (c >= 'a' && c <= 'f')
    return static_cast<unsigned>(c - 'a' + 10);
  if (c >= 'A' && c <= 'F')
    return static_cast<unsigned>(c - 'A' + 10);
  return 16;
}

/// The value of the digits of @p digits in @p base, skipping each `_`, into
/// @p out; false when it is 2^64 or more.  Every character is a digit of the
/// base or `_`.
bool accumulate(std::string_view digits, unsigned base, uint64_t &out) {
  uint64_t v = 0;
  for (char c : digits) {
    if (c == '_')
      continue;
    const uint64_t d = digitValue(c);
    if (v > (UINT64_MAX - d) / base)
      return false;
    v = v * base + d;
  }
  out = v;
  return true;
}

/// The error of an integer literal of 2^64 or more, spelled @p text.
std::string outOfRange(const std::string &text) {
  return "integer is out of range: " + text +
         " (an integer literal is at most 18446744073709551615)";
}

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
    {"fn", Tok::KwFn},         {"native", Tok::KwNative},
    {"impl", Tok::KwImpl},     {"static", Tok::KwStatic},
    {"return", Tok::KwReturn}, {"if", Tok::KwIf},
    {"then", Tok::KwThen},     {"else", Tok::KwElse},
    {"elif", Tok::KwElif}, // an if-statement's `else if`
    {"while", Tok::KwWhile},   {"for", Tok::KwFor},
    {"break", Tok::KwBreak},   {"continue", Tok::KwContinue},
    {"True", Tok::KwTrue},     {"False", Tok::KwFalse},
    {"None", Tok::KwNone},     {"_", Tok::Underscore},
    {"view", Tok::KwView},     {"inout", Tok::KwInout},
    {"let", Tok::KwLet},       {"weak", Tok::KwWeak},
    {"trait", Tok::KwTrait},   {"struct", Tok::KwStruct},
};

// The table spells the grammar's keywords, frontend::kKeywords, each once
// (LexerTests checks that each of those lexes as a keyword).
static_assert(std::size(kKeywords) == std::size(frontend::kKeywords));

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
  case Tok::KwNative:
    return "'native'";
  case Tok::KwImpl:
    return "'impl'";
  case Tok::KwStatic:
    return "'static'";
  case Tok::KwTrait:
    return "'trait'";
  case Tok::KwStruct:
    return "'struct'";
  case Tok::KwReturn:
    return "'return'";
  case Tok::KwIf:
    return "'if'";
  case Tok::KwThen:
    return "'then'";
  case Tok::KwElse:
    return "'else'";
  case Tok::KwElif:
    return "'elif'";
  case Tok::KwWhile:
    return "'while'";
  case Tok::KwFor:
    return "'for'";
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
  case Tok::KwWeak:
    return "'weak'";
  case Tok::Underscore:
    return "'_'";
  case Tok::Assign:
    return "'='";
  case Tok::PlusAssign:
    return "'+='";
  case Tok::MinusAssign:
    return "'-='";
  case Tok::StarAssign:
    return "'*='";
  case Tok::SlashAssign:
    return "'/='";
  case Tok::PercentAssign:
    return "'%='";
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
  case Tok::DotDot:
    return "'..'";
  case Tok::DotDotEq:
    return "'..='";
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
  case Tok::FatArrow:
    return "'=>'";
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
  case Tok::KwNative:
    return "KW_NATIVE";
  case Tok::KwImpl:
    return "KW_IMPL";
  case Tok::KwStatic:
    return "KW_STATIC";
  case Tok::KwTrait:
    return "KW_TRAIT";
  case Tok::KwStruct:
    return "KW_STRUCT";
  case Tok::KwReturn:
    return "KW_RETURN";
  case Tok::KwIf:
    return "KW_IF";
  case Tok::KwThen:
    return "KW_THEN";
  case Tok::KwElse:
    return "KW_ELSE";
  case Tok::KwElif:
    return "KW_ELIF";
  case Tok::KwWhile:
    return "KW_WHILE";
  case Tok::KwFor:
    return "KW_FOR";
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
  case Tok::KwWeak:
    return "KW_WEAK";
  case Tok::Underscore:
    return "UNDERSCORE";
  case Tok::Assign:
    return "ASSIGN";
  case Tok::PlusAssign:
    return "PLUS_ASSIGN";
  case Tok::MinusAssign:
    return "MINUS_ASSIGN";
  case Tok::StarAssign:
    return "STAR_ASSIGN";
  case Tok::SlashAssign:
    return "SLASH_ASSIGN";
  case Tok::PercentAssign:
    return "PERCENT_ASSIGN";
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
  case Tok::DotDot:
    return "DOT_DOT";
  case Tok::DotDotEq:
    return "DOT_DOT_EQ";
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
  case Tok::FatArrow:
    return "FAT_ARROW";
  case Tok::AndAnd:
    return "AND_AND";
  case Tok::OrOr:
    return "OR_OR";
  }
  return "UNKNOWN";
}

Lexer::Lexer(std::string_view source, ErrorHandler onError)
    : Src(source), OnError(std::move(onError)) {}

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
        k = n == '>'   ? (len = 2, Tok::Arrow)
            : n == '=' ? (len = 2, Tok::MinusAssign)
                       : Tok::Minus;
        break;
      case '<':
        k = n == '=' ? (len = 2, Tok::LessEq) : Tok::Less;
        break;
      case '>':
        k = n == '=' ? (len = 2, Tok::GreaterEq) : Tok::Greater;
        break;
      case '=':
        if (n == '=')
          k = (len = 2, Tok::EqEq);
        else if (n == '>')
          k = (len = 2, Tok::FatArrow);
        else
          k = Tok::Assign;
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
        k = n == '=' ? (len = 2, Tok::PlusAssign) : Tok::Plus;
        break;
      case '*':
        k = n == '=' ? (len = 2, Tok::StarAssign) : Tok::Star;
        break;
      case '/':
        k = n == '=' ? (len = 2, Tok::SlashAssign) : Tok::Slash;
        break;
      case '%':
        k = n == '=' ? (len = 2, Tok::PercentAssign) : Tok::Percent;
        break;
      case '?':
        k = Tok::Question;
        break;
      case '.':
        if (n == '.')
          k = peekAt(2) == '=' ? (len = 3, Tok::DotDotEq)
                               : (len = 2, Tok::DotDot);
        else
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

// INT         ::= decimal | hexadecimal | binary
// decimal     ::= digit+
// hexadecimal ::= "0x" hexDigit ( "_"? hexDigit )*
// binary      ::= "0b" [01] ( "_"? [01] )*
// FLOAT       ::= digit+ "." digit* exponent? | digit+ exponent
// exponent    ::= ("e" | "E") ("+" | "-")? digit+
//
// Longest match (docs/grammar.md): `1.` is a float, `1e` is the
// integer 1 followed by the identifier `e`, and `1..` is the integer 1
// followed by `..` (a range's start, not the float `1.`).  An integer
// literal of any base is 0 to 2^64-1 (docs/design/bitwise.md); the scanner
// never sees a sign.  A `_` stands between two digits of a hexadecimal or
// binary literal only, and a literal takes no suffix: `1_000` and `255u8`
// are one error each, while another identifier glued to a number is a token
// of its own (`0..10step 2`, `1else`).  After a `.` (afterDot) only the
// digits are taken; the parser turns them into a tuple index and validates
// them (no leading zeros, in range) itself.
Token Lexer::lexNumber(size_t start, size_t line, size_t col, bool afterDot) {
  if (!afterDot && cur() == '0') {
    const char p = peekAt(1);
    if (p == 'x' || p == 'X' || p == 'b' || p == 'B' || p == 'o' || p == 'O')
      return lexPrefixedNumber(start, line, col);
  }
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
  // A `_` followed by a digit or another `_` takes the run of digits and
  // `_` into the literal, which is then the separator error.
  auto separatorRun = [&]() {
    if (peekAt(0) != '_' || !(isDigit(peekAt(1)) || peekAt(1) == '_'))
      return false;
    while (isDigit(peekAt(0)) || peekAt(0) == '_')
      advance();
    return true;
  };

  bool isFloat = false;
  bool separated = separatorRun();
  if (!separated) {
    if (peekAt(0) == '.' && peekAt(1) != '.') {
      isFloat = true;
      advance();
      while (isDigit(peekAt(0)))
        advance();
      separated = separatorRun();
      if (!separated)
        advance(exponentLength());
    } else if (size_t n = exponentLength()) {
      isFloat = true;
      advance(n);
    }
  }
  const Tok kind = isFloat ? Tok::Float : Tok::Int;
  if (separated) {
    Token t = make(kind, start, line, col);
    error(line, col,
          "'_' separates digits only in hexadecimal and binary literals: '" +
              std::string(t.Text) + "'");
    return t;
  }
  // A numeric type's name glued to the number is a suffix, which the
  // language does not have: the literal takes its type from the context.
  if (isIdentStart(peekAt(0))) {
    size_t n = 0;
    while (isIdentChar(peekAt(n)))
      ++n;
    const std::string_view suffix = Src.substr(Pos, n);
    if (isNumericTypeName(suffix)) {
      const std::string_view digits = Src.substr(start, Pos - start);
      advance(n);
      Token t = make(kind, start, line, col);
      error(line, col,
            "there are no literal suffixes: '" + std::string(t.Text) +
                "' (a literal takes its type from the context, as in 'x: " +
                std::string(suffix) + " = " + std::string(digits) + "')");
      return t;
    }
  }

  Token t = make(kind, start, line, col);
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
  if (!accumulate(t.Text, 10, t.IntValue)) {
    error(line, col, outOfRange(text));
    t.IntValue = 0;
  }
  return t;
}

// `0x...`, `0b...` and the malformed forms around them: after the prefix the
// whole run of letters, digits and `_` is the literal, checked as one
// (`0xFG` is one error, not `0xF` and `G`), and so is a fraction after it
// (`0x1.5`).  A `.` followed by anything else is not part of it: `0xFF.x`
// is a member of 255 and `0x10..0x20` a range.  Each malformed literal is
// one error; its token is the INT 0.
Token Lexer::lexPrefixedNumber(size_t start, size_t line, size_t col) {
  const char prefix = peekAt(1);
  advance(2);
  const size_t runStart = Pos;
  while (isIdentChar(peekAt(0)))
    advance();
  const std::string_view run = Src.substr(runStart, Pos - runStart);
  const bool fraction = peekAt(0) == '.' && isDigit(peekAt(1));
  if (fraction) {
    advance();
    while (isIdentChar(peekAt(0)))
      advance();
  }
  Token t = make(Tok::Int, start, line, col);
  const std::string text(t.Text);
  const bool hex = prefix == 'x' || prefix == 'X';
  const std::string kind = hex ? "hexadecimal" : "binary";
  const unsigned base = hex ? 16 : 2;
  const auto badDigit = std::find_if(run.begin(), run.end(), [&](char c) {
    return c != '_' && digitValue(c) >= base;
  });
  std::string message;
  if (prefix == 'X' || prefix == 'B')
    message = "the prefix of a " + kind + " literal is '0" + (hex ? "x" : "b") +
              "', in lower case: '" + text + "'";
  else if (prefix == 'o' || prefix == 'O')
    message = "there are no octal literals: '" + text +
              "' (write the value in hexadecimal, as '0x...', or in decimal)";
  else if (run.empty())
    message = "expected a " + kind + " digit after '0" + prefix + "'";
  else if (run.front() == '_' || run.back() == '_' ||
           run.find("__") != std::string_view::npos)
    message = "a '_' in a number must stand between two digits: '" + text + "'";
  else if (badDigit != run.end())
    message = std::string("invalid digit '") + *badDigit + "' in " + kind +
              " literal '" + text + "'";
  else if (!accumulate(run, base, t.IntValue))
    message = outOfRange(text);
  else if (fraction)
    message = "a " + kind + " literal has no fraction: '" + text + "'";
  if (!message.empty()) {
    error(line, col, message);
    t.IntValue = 0;
  }
  return t;
}

Token Lexer::lexIdentOrKeyword(size_t start, size_t line, size_t col) {
  while (!atEnd() && isIdentChar(cur()))
    advance();
  std::string_view text = Src.substr(start, Pos - start);
  for (const auto &kw : kKeywords)
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
