// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Recursive-descent frontend lexer tests, through the --dump-tokens output (one
// token per line: `line:col-line:col KIND text`).

#include "paykan/Frontend.h"
#include "paykan/frontends/RecursiveDescent.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace paykan::frontend::recursive_descent;

namespace {

struct TokenDump {
  std::vector<std::string> Lines;
  std::string Diags;
  unsigned Errors;
};

TokenDump dump(const std::string &src) {
  std::ostringstream os, diagOS;
  paykan::sema::DiagEngine diag(diagOS);
  unsigned errors = dumpTokens(src, os, &diag);
  TokenDump d{{}, diagOS.str(), errors};
  std::string line;
  std::istringstream in(os.str());
  while (std::getline(in, line))
    d.Lines.push_back(line);
  return d;
}

} // namespace

TEST(Lexer, KeywordsPunctuationAndLocations) {
  auto d = dump("fn main() -> int {\n  return 0;\n}");
  std::vector<std::string> expected = {
      "1:1-1:3 KW_FN fn",   "1:4-1:8 IDENT main",       "1:8-1:9 LPAREN (",
      "1:9-1:10 RPAREN )",  "1:11-1:13 ARROW ->",       "1:14-1:17 IDENT int",
      "1:18-1:19 LBRACE {", "2:3-2:9 KW_RETURN return", "2:10-2:11 INT 0",
      "2:11-2:12 SEMI ;",   "3:1-3:2 RBRACE }",         "3:2-3:2 EOF",
  };
  EXPECT_EQ(d.Lines, expected);
  EXPECT_EQ(d.Errors, 0u);
}

TEST(Lexer, LongestMatchOperators) {
  auto d = dump(":: : -> - <= < >= > == = != ! && ||");
  std::vector<std::string> kinds;
  kinds.reserve(d.Lines.size());
  for (const auto &l : d.Lines)
    kinds.push_back(l.substr(l.find(' ') + 1,
                             l.find(' ', l.find(' ') + 1) - l.find(' ') - 1));
  std::vector<std::string> expected = {
      "COLON_COLON", "COLON",      "ARROW",   "MINUS", "LESS_EQ",
      "LESS",        "GREATER_EQ", "GREATER", "EQ_EQ", "ASSIGN",
      "NOT_EQ",      "NOT",        "AND_AND", "OR_OR", "EOF"};
  EXPECT_EQ(kinds, expected);
}

TEST(Lexer, KeywordVersusIdentifier) {
  auto d = dump("True Truex _ _x if iffy None");
  ASSERT_EQ(d.Lines.size(), 8u);
  EXPECT_EQ(d.Lines[0], "1:1-1:5 KW_TRUE True");
  EXPECT_EQ(d.Lines[1], "1:6-1:11 IDENT Truex");
  EXPECT_EQ(d.Lines[2], "1:12-1:13 UNDERSCORE _");
  EXPECT_EQ(d.Lines[3], "1:14-1:16 IDENT _x");
  EXPECT_EQ(d.Lines[4], "1:17-1:19 KW_IF if");
  EXPECT_EQ(d.Lines[5], "1:20-1:24 IDENT iffy");
  EXPECT_EQ(d.Lines[6], "1:25-1:29 KW_NONE None");
}

TEST(Lexer, ParamModeAndLetKeywords) {
  auto d = dump("view inout viewer _inout let lets");
  ASSERT_EQ(d.Lines.size(), 7u);
  EXPECT_EQ(d.Lines[0], "1:1-1:5 KW_VIEW view");
  EXPECT_EQ(d.Lines[1], "1:6-1:11 KW_INOUT inout");
  EXPECT_EQ(d.Lines[2], "1:12-1:18 IDENT viewer");
  EXPECT_EQ(d.Lines[3], "1:19-1:25 IDENT _inout");
  EXPECT_EQ(d.Lines[4], "1:26-1:29 KW_LET let");
  EXPECT_EQ(d.Lines[5], "1:30-1:34 IDENT lets");
}

TEST(Lexer, NumberLiterals) {
  auto d = dump("1 1.5 1. 1e5 1.e5 1E+2 2e-3 1e 1.5e");
  std::vector<std::string> expected = {
      "1:1-1:2 INT 1",        "1:3-1:6 FLOAT 1.5",    "1:7-1:9 FLOAT 1.",
      "1:10-1:13 FLOAT 1e5",  "1:14-1:18 FLOAT 1.e5", "1:19-1:23 FLOAT 1E+2",
      "1:24-1:28 FLOAT 2e-3", "1:29-1:30 INT 1",      "1:30-1:31 IDENT e",
      "1:32-1:35 FLOAT 1.5",  "1:35-1:36 IDENT e",    "1:36-1:36 EOF"};
  EXPECT_EQ(d.Lines, expected);
}

TEST(Lexer, TupleIndexIsDigitsGluedToDot) {
  // `t.0.1` is IDENT DOT INT DOT INT, never IDENT DOT FLOAT(0.1); with a
  // space after the dot the digits are an ordinary number again.
  auto d = dump("t.0.1 t. 0.1 1.5.3");
  std::vector<std::string> expected = {
      "1:1-1:2 IDENT t", "1:2-1:3 DOT .",       "1:3-1:4 INT 0",
      "1:4-1:5 DOT .",   "1:5-1:6 INT 1",       "1:7-1:8 IDENT t",
      "1:8-1:9 DOT .",   "1:10-1:13 FLOAT 0.1", "1:14-1:17 FLOAT 1.5",
      "1:17-1:18 DOT .", "1:18-1:19 INT 3",     "1:19-1:19 EOF"};
  EXPECT_EQ(d.Lines, expected);
}

TEST(Lexer, RangeTokens) {
  // `0..n` is INT DOT_DOT IDENT, never FLOAT(0.) DOT; `..=` is one token;
  // `for` is a keyword, and `in` and `step` identifiers.
  auto d = dump("for i in 0..n 1..=9 step x...y t.0..2 1.5..2");
  std::vector<std::string> expected = {
      "1:1-1:4 KW_FOR for",   "1:5-1:6 IDENT i",
      "1:7-1:9 IDENT in",     "1:10-1:11 INT 0",
      "1:11-1:13 DOT_DOT ..", "1:13-1:14 IDENT n",
      "1:15-1:16 INT 1",      "1:16-1:19 DOT_DOT_EQ ..=",
      "1:19-1:20 INT 9",      "1:21-1:25 IDENT step",
      "1:26-1:27 IDENT x",    "1:27-1:29 DOT_DOT ..",
      "1:29-1:30 DOT .",      "1:30-1:31 IDENT y",
      "1:32-1:33 IDENT t",    "1:33-1:34 DOT .",
      "1:34-1:35 INT 0",      "1:35-1:37 DOT_DOT ..",
      "1:37-1:38 INT 2",      "1:39-1:42 FLOAT 1.5",
      "1:42-1:44 DOT_DOT ..", "1:44-1:45 INT 2",
      "1:45-1:45 EOF"};
  EXPECT_EQ(d.Lines, expected);
  EXPECT_EQ(d.Errors, 0u);
}

// `=>` (an expression body) is one token; `= >` is two, and `==` and `>=`
// are unchanged.
TEST(Lexer, FatArrowToken) {
  auto d = dump("=> = > == >= =>=");
  std::vector<std::string> kinds;
  kinds.reserve(d.Lines.size());
  for (const auto &l : d.Lines)
    kinds.push_back(l.substr(l.find(' ') + 1,
                             l.find(' ', l.find(' ') + 1) - l.find(' ') - 1));
  std::vector<std::string> expected = {"FAT_ARROW", "ASSIGN",     "GREATER",
                                       "EQ_EQ",     "GREATER_EQ", "FAT_ARROW",
                                       "ASSIGN",    "EOF"};
  EXPECT_EQ(kinds, expected);
  EXPECT_EQ(d.Errors, 0u);
}

TEST(Lexer, CompoundAssignmentTokens) {
  // Each operator glued to `=` is one token; `->` stays the arrow, and a
  // space keeps the two apart.
  auto d = dump("+= -= *= /= %= -> - = =-");
  std::vector<std::string> kinds;
  kinds.reserve(d.Lines.size());
  for (const auto &l : d.Lines)
    kinds.push_back(l.substr(l.find(' ') + 1,
                             l.find(' ', l.find(' ') + 1) - l.find(' ') - 1));
  std::vector<std::string> expected = {
      "PLUS_ASSIGN",    "MINUS_ASSIGN", "STAR_ASSIGN", "SLASH_ASSIGN",
      "PERCENT_ASSIGN", "ARROW",        "MINUS",       "ASSIGN",
      "ASSIGN",         "MINUS",        "EOF"};
  EXPECT_EQ(kinds, expected);
  EXPECT_EQ(d.Errors, 0u);
}

TEST(Lexer, CommentsAndWhitespace) {
  auto d = dump("a // comment\n\t b\r\n");
  std::vector<std::string> expected = {"1:1-1:2 IDENT a", "2:3-2:4 IDENT b",
                                       "3:1-3:1 EOF"};
  EXPECT_EQ(d.Lines, expected);
}

TEST(Lexer, StringAndCharLiterals) {
  auto d = dump(R"("a\"b" 'x' '\n' '\'')");
  ASSERT_EQ(d.Lines.size(), 5u);
  EXPECT_EQ(d.Lines[0], R"(1:1-1:7 STRING "a\"b")");
  EXPECT_EQ(d.Lines[1], "1:8-1:11 CHAR 'x'");
  EXPECT_EQ(d.Lines[2], R"(1:12-1:16 CHAR '\n')");
  EXPECT_EQ(d.Lines[3], R"(1:17-1:21 CHAR '\'')");
}

TEST(Lexer, LexicalErrorsAreReportedAndSkipped) {
  auto d = dump("a @ b & c | d");
  EXPECT_EQ(d.Errors, 3u);
  EXPECT_NE(d.Diags.find("invalid character '@'"), std::string::npos);
  EXPECT_NE(d.Diags.find("invalid character '&'"), std::string::npos);
  EXPECT_NE(d.Diags.find("invalid character '|'"), std::string::npos);
  // The valid tokens around the bad bytes are all still delivered.
  ASSERT_EQ(d.Lines.size(), 5u);
  EXPECT_EQ(d.Lines[3], "1:13-1:14 IDENT d");
}

TEST(Lexer, UnterminatedLiterals) {
  auto d = dump("\"abc\n'a\n'\\\nx");
  EXPECT_NE(d.Diags.find("1:1: error: unterminated string literal"),
            std::string::npos);
  EXPECT_NE(d.Diags.find("2:1: error: unterminated character literal"),
            std::string::npos);
  // Line tracking survives the broken literals.
  EXPECT_EQ(d.Lines.back(), "4:2-4:2 EOF");
  EXPECT_EQ(d.Lines[d.Lines.size() - 2], "4:1-4:2 IDENT x");
}

TEST(Lexer, FloatRange) {
  auto d = dump("5e-324 1e-310 1e999 1e-400 0e-999");
  EXPECT_EQ(d.Errors, 2u) << d.Diags;
  EXPECT_NE(d.Diags.find("1:15: error: float is out of range: 1e999"),
            std::string::npos)
      << d.Diags;
  EXPECT_NE(d.Diags.find("1:21: error: float is out of range: 1e-400"),
            std::string::npos)
      << d.Diags;
}

TEST(Lexer, MultiCharacterLiteralIsOneError) {
  // Each whole literal (an escape counts as one character) is one error, and
  // lexing goes on after its closing quote.
  auto d = dump("'ab' x '\\n\\'' y");
  EXPECT_EQ(d.Errors, 2u) << d.Diags;
  EXPECT_NE(d.Diags.find("1:1: error: character literal must contain exactly "
                         "one character"),
            std::string::npos)
      << d.Diags;
  ASSERT_EQ(d.Lines.size(), 3u);
  EXPECT_EQ(d.Lines[0], "1:6-1:7 IDENT x");
  EXPECT_EQ(d.Lines[1], "1:15-1:16 IDENT y");
}

// An integer literal is 0 to 2^64-1 (docs/design/bitwise.md): Sema checks
// one above INT64_MAX against the type its context gives it.
TEST(Lexer, IntegerRange) {
  auto d = dump("9223372036854775807 9223372036854775808 18446744073709551615");
  EXPECT_EQ(d.Errors, 0u) << d.Diags;
  ASSERT_EQ(d.Lines.size(), 4u);
  EXPECT_EQ(d.Lines[2], "1:41-1:61 INT 18446744073709551615");
  auto over = dump("18446744073709551616 x");
  EXPECT_EQ(over.Errors, 1u) << over.Diags;
  EXPECT_NE(over.Diags.find("1:1: error: integer is out of range: "
                            "18446744073709551616 (an integer literal is at "
                            "most 18446744073709551615)"),
            std::string::npos)
      << over.Diags;
  ASSERT_EQ(over.Lines.size(), 3u);
  EXPECT_EQ(over.Lines[1], "1:22-1:23 IDENT x");
}

// The lexer's keywords are the grammar's, frontend::kKeywords, which the
// AST interchange reader holds a plugin's names to: each lexes as a keyword
// (the lexer's table has as many, a static_assert in Lexer.cpp checks).
TEST(Lexer, KeywordsAreTheFrontendsList) {
  for (std::string_view k : paykan::frontend::kKeywords) {
    auto d = dump(std::string(k));
    ASSERT_EQ(d.Lines.size(), 2u) << k;
    EXPECT_EQ(d.Lines[0].find(" IDENT "), std::string::npos)
        << k << ": " << d.Lines[0];
    EXPECT_FALSE(paykan::frontend::isIdentifier(k)) << k;
  }
  for (const char *ident : {"x", "_x", "X9", "__init__", "typeof", "self"})
    EXPECT_TRUE(paykan::frontend::isIdentifier(ident)) << ident;
  for (const char *other : {"", "9x", "a b", "a::b", "a-b", "$x"})
    EXPECT_FALSE(paykan::frontend::isIdentifier(other)) << other;
}

// The scanner never sees a sign: `-` is its own token, and the magnitude
// after it an ordinary INT, INT64_MIN's and one more alike.
TEST(Lexer, TheSignIsNotPartOfALiteral) {
  for (const char *text : {"-9223372036854775808", "-9223372036854775809"}) {
    auto d = dump(text);
    EXPECT_EQ(d.Errors, 0u) << text << ": " << d.Diags;
    ASSERT_EQ(d.Lines.size(), 3u) << text;
    EXPECT_EQ(d.Lines[0], "1:1-1:2 MINUS -") << text;
    EXPECT_EQ(d.Lines[1], std::string("1:2-1:21 INT ") + (text + 1)) << text;
  }
}

// Hexadecimal and binary literals (docs/design/bitwise.md): `_` between two
// digits, either case of a hexadecimal digit, never a `.`: `0x10..0x20` is a
// range, `0xFF.x` a member of 255 and `0x1e5` the integer 485.  A tuple index
// after a `.` is still digits only.
TEST(Lexer, HexAndBinaryLiterals) {
  auto d = dump("0xFF 0x1e5 0b1010_0101 0xFFFF_0000 0xab_CD 0x10..0x20 0xFF.x");
  std::vector<std::string> expected = {
      "1:1-1:5 INT 0xFF",          "1:6-1:11 INT 0x1e5",
      "1:12-1:23 INT 0b1010_0101", "1:24-1:35 INT 0xFFFF_0000",
      "1:36-1:43 INT 0xab_CD",     "1:44-1:48 INT 0x10",
      "1:48-1:50 DOT_DOT ..",      "1:50-1:54 INT 0x20",
      "1:55-1:59 INT 0xFF",        "1:59-1:60 DOT .",
      "1:60-1:61 IDENT x",         "1:61-1:61 EOF"};
  EXPECT_EQ(d.Lines, expected);
  EXPECT_EQ(d.Errors, 0u) << d.Diags;
  auto widest = dump("0xFFFF_FFFF_FFFF_FFFF 0b" + std::string(64, '1'));
  EXPECT_EQ(widest.Errors, 0u) << widest.Diags;
  auto tuple = dump("t.0x1");
  std::vector<std::string> index = {"1:1-1:2 IDENT t", "1:2-1:3 DOT .",
                                    "1:3-1:4 INT 0", "1:4-1:6 IDENT x1",
                                    "1:6-1:6 EOF"};
  EXPECT_EQ(tuple.Lines, index);
  EXPECT_EQ(tuple.Errors, 0u) << tuple.Diags;
}

// A malformed hexadecimal or binary literal is one error, which names the
// rule, located at the whole literal; the lexer goes on after it.
TEST(Lexer, MalformedPrefixedLiterals) {
  const std::string range =
      " (an integer literal is at most 18446744073709551615)";
  const std::string ones = "0b" + std::string(65, '1');
  const std::pair<std::string, std::string> cases[] = {
      {"0x", "expected a hexadecimal digit after '0x'"},
      {"0x.", "expected a hexadecimal digit after '0x'"},
      {"0b", "expected a binary digit after '0b'"},
      {"0xFG", "invalid digit 'G' in hexadecimal literal '0xFG'"},
      {"0b102", "invalid digit '2' in binary literal '0b102'"},
      {"0x_FF", "a '_' in a number must stand between two digits: '0x_FF'"},
      {"0xFF_", "a '_' in a number must stand between two digits: '0xFF_'"},
      {"0x1__0", "a '_' in a number must stand between two digits: '0x1__0'"},
      {"0XFF", "the prefix of a hexadecimal literal is '0x', in lower case: "
               "'0XFF'"},
      {"0B1", "the prefix of a binary literal is '0b', in lower case: '0B1'"},
      {"0o17", "there are no octal literals: '0o17' (write the value in "
               "hexadecimal, as '0x...', or in decimal)"},
      {"0x1.5", "a hexadecimal literal has no fraction: '0x1.5'"},
      {"0b1.0", "a binary literal has no fraction: '0b1.0'"},
      {"0x1_0000_0000_0000_0000",
       "integer is out of range: 0x1_0000_0000_0000_0000" + range},
      {ones, "integer is out of range: " + ones + range},
  };
  for (const auto &[text, message] : cases) {
    auto d = dump(text + " z");
    EXPECT_EQ(d.Errors, 1u) << text << ": " << d.Diags;
    EXPECT_NE(d.Diags.find("1:1: error: " + message), std::string::npos)
        << text << ": " << d.Diags;
    ASSERT_GE(d.Lines.size(), 3u) << text;
    // The literal is one token (`0x.` is `0x` and a `.`), and the next
    // token is still delivered.
    const std::string literal = text == "0x." ? "0x" : text;
    EXPECT_EQ(d.Lines[0],
              "1:1-1:" + std::to_string(literal.size() + 1) + " INT " + literal)
        << text;
    EXPECT_EQ(d.Lines[d.Lines.size() - 2],
              "1:" + std::to_string(text.size() + 2) +
                  "-1:" + std::to_string(text.size() + 3) + " IDENT z")
        << text;
  }
}

// A decimal literal takes no `_` and no suffix yet: each is one error that
// says so.  A glued identifier that is not a numeric type's name lexes as
// before (`0..10step 2`, `1else`).
TEST(Lexer, DecimalLiteralsTakeNoSeparatorOrSuffix) {
  const std::string hint =
      " (a literal takes its type from the context, as in 'x: ";
  const std::pair<std::string, std::string> cases[] = {
      {"1_000", "'_' separates digits only in hexadecimal and binary "
                "literals: '1_000'"},
      {"3.141_592", "'_' separates digits only in hexadecimal and binary "
                    "literals: '3.141_592'"},
      {"1__0", "'_' separates digits only in hexadecimal and binary "
               "literals: '1__0'"},
      {"255u8", "there are no literal suffixes: '255u8'" + hint + "u8 = 255')"},
      {"1f32", "there are no literal suffixes: '1f32'" + hint + "f32 = 1')"},
      {"2.5f32",
       "there are no literal suffixes: '2.5f32'" + hint + "f32 = 2.5')"},
      {"7unsigned",
       "there are no literal suffixes: '7unsigned'" + hint + "unsigned = 7')"},
  };
  for (const auto &[text, message] : cases) {
    auto d = dump(text + " z");
    EXPECT_EQ(d.Errors, 1u) << text << ": " << d.Diags;
    EXPECT_NE(d.Diags.find("1:1: error: " + message), std::string::npos)
        << text << ": " << d.Diags;
    ASSERT_EQ(d.Lines.size(), 3u) << text;
    EXPECT_EQ(d.Lines[1], "1:" + std::to_string(text.size() + 2) + "-1:" +
                              std::to_string(text.size() + 3) + " IDENT z")
        << text;
  }
  auto step = dump("0..10step 2");
  std::vector<std::string> expected = {"1:1-1:2 INT 0",   "1:2-1:4 DOT_DOT ..",
                                       "1:4-1:6 INT 10",  "1:6-1:10 IDENT step",
                                       "1:11-1:12 INT 2", "1:12-1:12 EOF"};
  EXPECT_EQ(step.Lines, expected);
  EXPECT_EQ(step.Errors, 0u) << step.Diags;
  auto glued = dump("if c then 1else 2");
  ASSERT_EQ(glued.Lines.size(), 7u);
  EXPECT_EQ(glued.Lines[3], "1:11-1:12 INT 1");
  EXPECT_EQ(glued.Lines[4], "1:12-1:16 KW_ELSE else");
  EXPECT_EQ(glued.Errors, 0u) << glued.Diags;
}
