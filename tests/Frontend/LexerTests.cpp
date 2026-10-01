// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Recursive-descent frontend lexer tests, through the --dump-tokens output (one
// token per line: `line:col-line:col KIND text`).

#include "Frontends/RecursiveDescent.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
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

TEST(Lexer, IntegerRange) {
  auto d = dump("9223372036854775807 9223372036854775808");
  EXPECT_EQ(d.Errors, 1u);
  EXPECT_NE(d.Diags.find("integer is out of range: 9223372036854775808"),
            std::string::npos);
}
