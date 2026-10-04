// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: arithmetic, literals, variables, basic errors

#include "TestUtils.h"
#include <gtest/gtest.h>
#include <sstream>

using namespace paykan::test;

namespace {

/// Parse with a DiagEngine attached to the driver (the way the compiler
/// driver wires it), capturing both the rendered diagnostic text and the
/// structured diagnostics so tests can assert on exact source locations.
struct DiagParseResult {
  bool Ok;
  std::string Text;
  std::vector<paykan::sema::Diagnostic> Diags;
};

DiagParseResult parseWithDiags(const std::string &source) {
  auto path = writeTempFile(source);
  paykan::parser::ParserDriver driver(paykan::test::testFrontend());
  std::ostringstream os;
  paykan::sema::DiagEngine diag(os);
  diag.setSourceInfo(path, &driver.getSourceLines());
  driver.setDiagEngine(&diag);
  int rc = driver.parseFile(path);
  std::filesystem::remove(path);
  return {rc == 0, os.str(), diag.getDiagnostics()};
}

} // namespace

TEST(Arith, EmptyMain) {
  auto [ok, _] = parse("fn main() -> int { return 0; }");
  EXPECT_TRUE(ok);
}

TEST(Arith, IntegerLiteral) {
  auto [ok, _] = parse("fn main() -> int { return 42; }");
  EXPECT_TRUE(ok);
}

TEST(Arith, FloatLiteral) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: float = 3.14;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, BoolLiterals) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: bool = True;
      b: bool = False;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, StringLiteral) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      s: Str = "hello world";
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, ArithmeticPrecedence) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      return 1 + 2 * 3;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, UnaryOperators) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = -42;
      b: bool = !True;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, ParenthesizedExpression) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      return (1 + 2) * 3;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, OwnershipQualifiers) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: Str = "hello";
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, ParamOwnershipQualifiers) {
  auto [ok, _] = parse(R"(
    fn f1(a: Str) {}
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, RefExpression) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: Str = "hello";
      b: Str = a;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, RelationalOperators) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: bool = 1 < 2;
      b: bool = 3 >= 3;
      c: bool = 4 == 4;
      d: bool = 5 != 6;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, ChainedRelationalRejected) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: bool = 1 < 2 < 3;
      return 0;
    }
  )");
  EXPECT_FALSE(ok);
}

TEST(Arith, NestedBlocks) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = 1;
      {
        y: int = 2;
        {
          z: int = 3;
        }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, EmptyStatement) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      ;
      ;;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, UnknownTypeAccepted) {
  // Identifiers in type positions are parsed as forward-referenced class types;
  // resolution is deferred to sema.
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: FooBar = 42;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// -- String literal line discipline (B6) -------------------------------------
// String literals are single-line: a raw newline before the closing quote is
// rejected ("unterminated string literal") instead of being swallowed, which
// previously desynchronized line tracking for every later diagnostic.

TEST(Arith, RawNewlineInStringRejected) {
  auto [ok, _] = parse("fn main() -> int {\n"
                       "  s: Str = \"abc\ndef\";\n"
                       "  return 0;\n"
                       "}\n");
  EXPECT_FALSE(ok);
}

TEST(Arith, UnterminatedStringDiagnosticLocation) {
  // The broken literal opens on line 2; the diagnostic must point there and
  // must mention the single-line rule.
  auto r = parseWithDiags("fn main() -> int {\n"
                          "  s: Str = \"abc\n"
                          "}\n");
  ASSERT_FALSE(r.Ok);
  ASSERT_FALSE(r.Diags.empty());
  EXPECT_EQ(r.Diags[0].Loc.getLineStart(), 2u);
  EXPECT_NE(r.Diags[0].Message.find("unterminated string literal"),
            std::string::npos);
}

TEST(Arith, EscapedNewlineInStringAccepted) {
  // The \n escape is the supported way to embed a newline.
  auto [ok, _] = parse(R"(
    fn main() -> int {
      s: Str = "line1\nline2";
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, LineNumbersCorrectAfterStringLiterals) {
  // Two single-line strings with \n escapes precede a syntax error on line 4;
  // the diagnostic must report line 4 (line tracking must not drift).
  auto r = parseWithDiags("fn main() -> int {\n"
                          "  s: Str = \"a\\nb\";\n"
                          "  t: Str = \"c\\nd\";\n"
                          "  x: int = ;\n"
                          "  return 0;\n"
                          "}\n");
  ASSERT_FALSE(r.Ok);
  ASSERT_FALSE(r.Diags.empty());
  EXPECT_EQ(r.Diags[0].Loc.getLineStart(), 4u);
}

// -- Integer literal range (B7) ----------------------------------------------

TEST(Arith, IntLiteralMaxAccepted) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = 9223372036854775807;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, IntLiteralOverflowRejected) {
  // INT64_MAX + 1: strtoll saturates and sets ERANGE.
  auto r = parseWithDiags("fn main() -> int {\n"
                          "  x: int = 9223372036854775808;\n"
                          "  return 0;\n"
                          "}\n");
  ASSERT_FALSE(r.Ok);
  ASSERT_FALSE(r.Diags.empty());
  EXPECT_NE(r.Diags[0].Message.find("integer is out of range"),
            std::string::npos);
}

// A float literal must denote a finite float (#73): overflow to +-inf and a
// nonzero literal underflowing to 0 are errors, with the same wording on
// every frontend; subnormal literals (which strtod flags with ERANGE too)
// and zero written with a huge exponent are fine.
TEST(Arith, FloatLiteralOutOfRangeRejected) {
  for (const char *lit : {"1e999", "1.8e308", "1e-400", "2e-324"}) {
    auto r = parseWithDiags(std::string("fn main() -> int {\n  x: float = ") +
                            lit + ";\n  return 0;\n}\n");
    ASSERT_FALSE(r.Ok) << lit;
    ASSERT_FALSE(r.Diags.empty()) << lit;
    EXPECT_NE(
        r.Diags[0].Message.find(std::string("float is out of range: ") + lit),
        std::string::npos)
        << lit << ": " << r.Diags[0].Message;
    EXPECT_EQ(r.Diags[0].Loc.getLineStart(), 2u) << lit;
  }
}

TEST(Arith, FloatLiteralSubnormalAccepted) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      a: float = 5e-324;
      b: float = 1e-310;
      c: float = 2.2250738585072014e-308;
      d: float = 1.7976931348623157e308;
      e: float = 0e-999;
      f: float = 0.0e999;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Arith, IntLiteralMinMagnitudeRejected) {
  // Deliberate: `-` is a separate token, so -9223372036854775808 lexes as
  // MINUS + the bare magnitude, which overflows.  INT64_MIN must be written
  // as an expression (e.g. -9223372036854775807 - 1).
  auto r = parseWithDiags("fn main() -> int {\n"
                          "  x: int = -9223372036854775808;\n"
                          "  return 0;\n"
                          "}\n");
  ASSERT_FALSE(r.Ok);
  ASSERT_FALSE(r.Diags.empty());
  EXPECT_NE(r.Diags[0].Message.find("integer is out of range"),
            std::string::npos);
}

TEST(Arith, IntLiteralMinAsExpressionAccepted) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = -9223372036854775807 - 1;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// -- Lexical errors (C11 + catch-all) ----------------------------------------

TEST(Arith, AmpersandIsLexicalError) {
  // '&' had a dead AMP token no grammar rule consumed; it is now a plain
  // lexical error.
  auto r = parseWithDiags("fn main() -> int {\n"
                          "  x: int = 1 & 2;\n"
                          "  return 0;\n"
                          "}\n");
  ASSERT_FALSE(r.Ok);
  ASSERT_FALSE(r.Diags.empty());
  EXPECT_NE(r.Diags[0].Message.find("invalid character '&'"),
            std::string::npos);
}

TEST(Arith, InvalidCharacterRejected) {
  // A scanner without a catch-all rule could silently skip unknown bytes.
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = 1 @ 2;
      return 0;
    }
  )");
  EXPECT_FALSE(ok);
}

TEST(Arith, UnterminatedCharLiteralRejected) {
  auto r = parseWithDiags("fn main() -> int {\n"
                          "  c: char = 'a\n"
                          "}\n");
  ASSERT_FALSE(r.Ok);
  ASSERT_FALSE(r.Diags.empty());
  EXPECT_NE(r.Diags[0].Message.find("unterminated character literal"),
            std::string::npos);
}

// -- Driver error plumbing (B9/B10) ------------------------------------------

TEST(Arith, ParseErrorsRoutedThroughDiagEngine) {
  // With a DiagEngine attached, syntax errors come out in the rich
  // clang-style format (file:line:col + snippet gutter), not a bare one-line
  // stderr fallback.
  auto r = parseWithDiags("fn main() -> int {\n"
                          "  x: int = ;\n"
                          "  return 0;\n"
                          "}\n");
  ASSERT_FALSE(r.Ok);
  EXPECT_NE(r.Text.find(":2:"), std::string::npos); // file:line:col header
  EXPECT_NE(r.Text.find("error:"), std::string::npos);
  EXPECT_NE(r.Text.find(" | "), std::string::npos); // snippet gutter
  EXPECT_NE(r.Text.find('^'), std::string::npos);   // caret marker
}

TEST(Arith, UnopenableFileFailsCleanly) {
  // scanBegin used to exit(EXIT_FAILURE) inside library code; parseFile must
  // instead fail with a diagnostic and a non-zero return.
  paykan::parser::ParserDriver driver(paykan::test::testFrontend());
  std::ostringstream os;
  paykan::sema::DiagEngine diag(os);
  driver.setDiagEngine(&diag);
  int rc = driver.parseFile("/nonexistent/paykan_no_such_file.pkn");
  EXPECT_NE(rc, 0);
  EXPECT_EQ(driver.getErrorCount(), 1u);
  EXPECT_NE(os.str().find("cannot open"), std::string::npos);
}

TEST(Arith, MissingSemicolon) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = 42
      return 0;
    }
  )");
  EXPECT_FALSE(ok);
}

TEST(Arith, MissingCloseBrace) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      return 0;
  )");
  EXPECT_FALSE(ok);
}
