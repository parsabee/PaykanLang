// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Parser tests: arithmetic, literals, variables, basic errors

#include "ASTPrinter.h"
#include "TestUtils.h"
#include <gtest/gtest.h>
#include <sstream>

using namespace paykan::test;

// The printed AST of @p src (empty when it does not parse).
static std::string dumpAST(const std::string &src) {
  auto [ok, driver] = parse(src);
  if (!ok)
    return "";
  std::ostringstream os;
  paykan::ast::ASTPrinter printer(os);
  printer.visit(driver->getRoot());
  return os.str();
}

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

// -- String literal line discipline (B6)
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

// -- Integer literal range (B7)

// -- Adjacent string literals (#197, docs/grammar.md §1)
// Two or more STRING tokens with only whitespace, newlines or comments
// between them are one literal, its pieces joined at compile time.

TEST(Arith, AdjacentStringLiteralsJoin) {
  auto dump = dumpAST("fn main() -> int {\n"
                      "  a = \"ab\" \"c\";\n"
                      "  b = \"one \"\n"
                      "      \"two \"   // a comment between the pieces\n"
                      "      \"three\";\n"
                      "  return 0;\n"
                      "}\n");
  ASSERT_FALSE(dump.empty());
  // One literal, spanning every piece.
  EXPECT_NE(dump.find("StringLiteral <2:7-2:15> \"abc\""), std::string::npos)
      << dump;
  EXPECT_NE(dump.find("StringLiteral <3:7-5:14> \"one two three\""),
            std::string::npos)
      << dump;
  EXPECT_EQ(dump.find("\"ab\""), std::string::npos) << dump;
}

TEST(Arith, AdjacentStringLiteralsDecodeEachPiece) {
  // Each piece's escapes are decoded on their own, then joined: `"\\" "n"`
  // is a backslash and an `n`, not a newline.
  auto [ok, driver] = parse("fn main() -> int {\n"
                            "  s = \"tab\\t\" \"\\\\\" \"n\" \"q\\\"\";\n"
                            "  return 0;\n"
                            "}\n");
  ASSERT_TRUE(ok);
  auto *fn = driver->getRoot()->getFuncDecls()[0];
  auto *as = paykan::ast::dyn_cast<paykan::ast::AssignStmt>(
      fn->getBody()->getStatements()[0]);
  ASSERT_NE(as, nullptr);
  auto *lit = paykan::ast::dyn_cast<paykan::ast::StringLiteral>(as->getValue());
  ASSERT_NE(lit, nullptr);
  EXPECT_EQ(lit->getValue(), "tab\t\\nq\"");
}

TEST(Arith, AdjacentStringLiteralsAreOnePrimary) {
  // A postfix applies to the joined literal; a match arm's pattern and a
  // native function's symbol join too.
  auto dump = dumpAST("native fn f() = \"pk_\" \"f\";\n"
                      "fn main() -> int {\n"
                      "  c = \"ab\" \"c\"[2];\n"
                      "  n = \"ab\" \"c\".len();\n"
                      "  match \"x\" { \"x\" \"y\" { } _ { } }\n"
                      "  return 0;\n"
                      "}\n");
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("native 'pk_f'"), std::string::npos) << dump;
  EXPECT_NE(dump.find("SubscriptExpr"), std::string::npos) << dump;
  EXPECT_NE(dump.find("\"abc\""), std::string::npos) << dump;
  EXPECT_NE(dump.find("\"xy\""), std::string::npos) << dump;
}

TEST(Arith, StringThenCharDoesNotJoin) {
  // A character literal is no piece of a string.
  auto r = parseWithDiags("fn main() -> int { s = \"a\" 'b'; return 0; }\n");
  EXPECT_FALSE(r.Ok);
  ASSERT_FALSE(r.Diags.empty());
  EXPECT_NE(r.Diags[0].Message.find("unexpected character literal"),
            std::string::npos)
      << r.Diags[0].Message;
  // Each piece is still single-line.
  EXPECT_FALSE(
      parse("fn main() -> int { s = \"a\" \"b\nc\"; return 0; }\n").Ok);
}

TEST(Arith, IntLiteralMaxAccepted) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = 9223372036854775807;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// An integer literal reaches 2^64-1 (docs/design/bitwise.md): the upper
// half of `unsigned` is written directly, and only 2^64 or more is the
// lexer's error.  Sema checks a literal against its context's type.
TEST(Arith, IntLiteralsReachTheUnsignedRange) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      u: unsigned = 18446744073709551615;
      v: unsigned = 9223372036854775808;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
  auto r = parseWithDiags("fn main() -> int {\n"
                          "  x = 18446744073709551616;\n"
                          "  return 0;\n"
                          "}\n");
  ASSERT_FALSE(r.Ok);
  ASSERT_EQ(r.Diags.size(), 1u) << r.Text;
  EXPECT_EQ(r.Diags[0].Message, "integer is out of range: "
                                "18446744073709551616 (an integer literal is "
                                "at most 18446744073709551615)");
  EXPECT_EQ(r.Diags[0].Loc.getColumnStart(), 7u);
}

// A float literal must denote a finite float (#73): overflow to +-inf and a
// nonzero literal underflowing to 0 are errors, with the same wording on
// every frontend; subnormal literals (which strtod flags with ERANGE too)
// and zero written with a huge exponent are fine.
TEST(Arith, FloatLiteralOutOfRangeRejected) {
  for (const char *lit : {"1e999", "1.8e308", "1e-400", "2e-324"}) {
    auto r = parseWithDiags(
        std::string("import ::io; fn main() -> int {\n  x: float = ") + lit +
        ";\n  return 0;\n}\n");
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

// `-` is a separate token, so -9223372036854775808 is MINUS and the bare
// magnitude, which parses; Sema rejects it in an `int` context, where
// INT64_MIN is written as an expression, `-9223372036854775807 - 1`
// (tests/Sema/SizedTypeTests.cpp).
TEST(Arith, IntLiteralMinMagnitudeParses) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: int = -9223372036854775808;
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

// Hexadecimal and binary literals are values, in every place an integer
// literal is: an expression, a fixed-size array's length and a repeat
// count, where one above INT64_MAX is out of range.
TEST(Arith, HexAndBinaryLiteralValues) {
  std::string ast = dumpAST(R"(
    fn main() -> int {
      a = 0xFF;
      b = 0x1e5;
      c = 0b1010;
      d: unsigned = 0xFFFF_FFFF_FFFF_FFFF;
      e: int[0x4] = [0; 0b11];
      return 0;
    }
  )");
  for (const char *line :
       {"IntegerLiteral <3:11-3:15> 255\n", "IntegerLiteral <4:11-4:16> 485\n",
        "IntegerLiteral <5:11-5:17> 10\n",
        "IntegerLiteral <6:21-6:42> 18446744073709551615\n",
        "FixedArrayType <7:10-7:18> 4\n", "<7:21-7:30> 3 copies\n"})
    EXPECT_NE(ast.find(line), std::string::npos) << line << ast;
  for (const char *decl : {"e: int[0x8000_0000_0000_0000] = [0];",
                           "e = [0; 0x8000_0000_0000_0000];"}) {
    auto r = parseWithDiags(std::string("fn main() -> int {\n  ") + decl +
                            "\n  return 0;\n}\n");
    ASSERT_FALSE(r.Ok) << decl;
    ASSERT_EQ(r.Diags.size(), 1u) << decl << "\n" << r.Text;
    EXPECT_EQ(r.Diags[0].Message,
              "integer is out of range: 0x8000_0000_0000_0000")
        << decl;
  }
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

// -- Lexical errors (C11 + catch-all)

namespace {

// @p e with each operator's operands in parentheses: `(a + (b << 2))`.
std::string grouped(const paykan::ast::Expr *e) {
  using namespace paykan::ast;
  if (const auto *b = dyn_cast<BinaryExpr>(e))
    return "(" + grouped(b->getLHS()) + " " + b->getOpcodeStr() + " " +
           grouped(b->getRHS()) + ")";
  if (const auto *u = dyn_cast<UnaryExpr>(e))
    return u->getOpcodeStr() + grouped(u->getOperand());
  if (const auto *id = dyn_cast<Identifier>(e))
    return id->getName();
  if (const auto *lit = dyn_cast<IntegerLiteral>(e))
    return std::to_string(lit->getValue());
  return "?";
}

// The grouping of the expression @p expr, assigned in a function's body.
std::string groupingOf(const std::string &expr) {
  auto [ok, driver] =
      parse("fn main() -> int {\n  x = " + expr + ";\n  return 0;\n}\n");
  if (!ok)
    return "parse error";
  auto *body = driver->getRoot()->getFuncDecls()[0]->getBody();
  return grouped(
      paykan::ast::cast<paykan::ast::AssignStmt>(body->getStatements()[0])
          ->getValue());
}

// The first diagnostic of a function whose body is @p body.
std::string firstError(const std::string &body) {
  auto r = parseWithDiags("fn main() -> int {\n" + body + "\n  return 0;\n}\n");
  if (r.Ok || r.Diags.empty())
    return "no error";
  return r.Diags[0].Message;
}

} // namespace

// Swift's precedence (docs/design/bitwise.md): `<< >>` above `* / % &`,
// above `+ - | ^`, above the comparisons.  Each row groups differently in
// C, which the reference lists.
TEST(Arith, BitwisePrecedenceIsSwifts) {
  const std::pair<const char *, const char *> rows[] = {
      {"a + b << 2", "(a + (b << 2))"},
      {"1 << n - 1", "((1 << n) - 1)"},
      {"a & b * c", "((a & b) * c)"},
      {"a + b & c", "(a + (b & c))"},
      {"a | b + c", "((a | b) + c)"},
      {"a | b ^ c", "((a | b) ^ c)"},
      {"x & m == 0", "((x & m) == 0)"},
      {"~a & b", "(~a & b)"},
      {"a >> b > c", "((a >> b) > c)"},
      {"a > b >> c", "(a > (b >> c))"},
      {"a < b >> c", "(a < (b >> c))"},
      {"-a << b", "(-a << b)"},
      {"a & b << c", "(a & (b << c))"},
      {"a ^ b | c & d", "((a ^ b) | (c & d))"},
      {"a & b & c", "((a & b) & c)"},
      {"~~a", "~~a"},
      {"x & ~0 != 0 && y | z == w", "(((x & ~0) != 0) && ((y | z) == w))"},
      {"(a << b) << c", "((a << b) << c)"},
      {"a << (b << c)", "(a << (b << c))"},
  };
  for (const auto &[src, expected] : rows)
    EXPECT_EQ(groupingOf(src), expected) << src;
}

// Shifts do not associate, as Swift's (docs/grammar.md section 7): the
// mechanism of the comparisons' `a < b < c`.
TEST(Arith, ShiftsDoNotChain) {
  const std::pair<const char *, const char *> rows[] = {
      {"a << b << c", "'<<'"},     {"a << b >> c", "'>>'"},
      {"a >> b >> c", "'>>'"},     {"a >> b << c", "'<<'"},
      {"a + b << c << d", "'<<'"},
  };
  for (const auto &[src, found] : rows)
    EXPECT_EQ(firstError(std::string("  x = ") + src + ";"),
              std::string("unexpected ") + found +
                  "; shift operators cannot be chained; parenthesise the "
                  "shift")
        << src;
  // At the second shift, its first token.
  auto r = parseWithDiags("fn main() -> int {\n  x = a >> b >> c;\n"
                          "  return 0;\n}\n");
  ASSERT_FALSE(r.Diags.empty());
  EXPECT_EQ(r.Diags[0].Loc.getLineStart(), 2u);
  EXPECT_EQ(r.Diags[0].Loc.getColumnStart(), 14u);
  // A shift of a shift's value is another operator's operand.
  EXPECT_EQ(groupingOf("a << b * c << d"), "((a << b) * (c << d))");
}

// `>>` is a `>` directly followed by another, and `>>=` a `>` directly
// followed by `>=` (docs/design/bitwise.md): with anything between them
// they are a comparison and an error, whose message says so.
TEST(Arith, ShiftRightIsTwoAdjacentGreaters) {
  EXPECT_EQ(groupingOf("a >> b"), "(a >> b)");
  EXPECT_EQ(groupingOf("a > b"), "(a > b)");
  EXPECT_EQ(groupingOf("a >= b"), "(a >= b)");
  EXPECT_EQ(firstError("  x = a > > b;"),
            "unexpected '>'; expected an expression ('>>' is written "
            "without a space)");
  EXPECT_EQ(firstError("  x = a >\n  > b;"),
            "unexpected '>'; expected an expression ('>>' is written "
            "without a space)");
  EXPECT_EQ(firstError("  x > >= 1;"),
            "unexpected '>='; expected an expression ('>>=' is written "
            "without a space)");
  EXPECT_EQ(firstError("  x = a >>> b;"),
            "unexpected '>'; expected an expression (there is no '>>>': "
            "'>>' of an unsigned value shifts in zeros)");
  EXPECT_EQ(firstError("  f(x >>= 1);"),
            "unexpected '>>='; expected ')' to close the argument list");
  EXPECT_EQ(firstError("  x = a >>= 1;"),
            "unexpected '>>='; expected ';' after the assignment");
  EXPECT_EQ(firstError("  x = a > ;"),
            "unexpected ';'; expected an expression");
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

// -- Driver error plumbing (B9/B10)

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
  auto [ok, _] = parse(R"(import ::io; 
    fn main() -> int {
      return 0;
  )");
  EXPECT_FALSE(ok);
}
