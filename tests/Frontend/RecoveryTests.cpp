// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Error recovery of the recursive-descent frontend: several independent errors
// in one file are all reported, at statement, class-member and top-level
// boundaries, and a file with errors always fails.

#include "paykan/frontends/RecursiveDescent.h"

#include <gtest/gtest.h>

#include <chrono>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace paykan::frontend::recursive_descent;

namespace {

struct Result {
  unsigned Errors;
  std::vector<paykan::sema::Diagnostic> Diags;
  std::string Text;
};

Result parse(const std::string &src) {
  paykan::ast::ASTContext ctx;
  std::ostringstream os;
  paykan::sema::DiagEngine diag(os);
  auto out = parseSource(ctx, src, &diag);
  EXPECT_NE(out.Root, nullptr);
  return {out.ErrorCount, diag.getDiagnostics(), os.str()};
}

std::vector<size_t> errorLines(const Result &r) {
  std::vector<size_t> lines;
  lines.reserve(r.Diags.size());
  for (const auto &d : r.Diags)
    lines.push_back(d.Loc.getLineStart());
  return lines;
}

} // namespace

TEST(Recovery, StatementBoundaries) {
  auto r = parse("import ::io; fn main() -> int {\n"
                 "  x: int = ;\n"     // 2
                 "  y: int = 1 + ;\n" // 3
                 "  z: int = 2;\n"
                 "  return 0\n" // 5: missing ';', found '}'
                 "}\n");
  EXPECT_EQ(r.Errors, 3u);
  EXPECT_EQ(errorLines(r), (std::vector<size_t>{2, 3, 6}));
}

TEST(Recovery, BrokenNestedBlockDoesNotSwallowTheEnclosingOne) {
  auto r = parse("import ::io; fn main() -> int {\n"
                 "  if x {\n"
                 "    a = ;\n" // 3
                 "    b = 1;\n"
                 "  }\n"
                 "  c = ;\n" // 6
                 "  return 0;\n"
                 "}\n"
                 "fn f() { d = ; }\n"); // 9
  EXPECT_EQ(r.Errors, 3u);
  EXPECT_EQ(errorLines(r), (std::vector<size_t>{3, 6, 9}));
}

TEST(Recovery, ClassMemberBoundaries) {
  auto r = parse("class A {\n"
                 "  x: ;\n" // 2
                 "  y: int;\n"
                 "  fn f() { return ; }\n"
                 "  fn g( { }\n" // 5
                 "  fn h() -> int { return 1; }\n"
                 "  z: Str\n" // 7: missing ';' before 'fn'
                 "  fn k() {}\n"
                 "}\n"
                 "fn main() -> int { return 0; }\n");
  EXPECT_EQ(r.Errors, 3u);
  EXPECT_EQ(errorLines(r), (std::vector<size_t>{2, 5, 8}));
}

TEST(Recovery, TopLevelBoundaries) {
  auto r = parse("import ;\n"   // 1
                 "enum E { }\n" // 2
                 "class { }\n"  // 3
                 "fn main() -> int { return 0; }\n"
                 "garbage tokens here\n" // 5
                 "fn f() {}\n");
  EXPECT_EQ(r.Errors, 4u);
  EXPECT_EQ(errorLines(r), (std::vector<size_t>{1, 2, 3, 5}));
}

TEST(Recovery, MatchArmBoundaries) {
  auto r = parse("import ::io; fn main() -> int {\n"
                 "  match x {\n"
                 "    Foo { a = ; }\n" // 3
                 "    1 + 1 { }\n"     // 4
                 "    _ { b = 1; }\n"
                 "  }\n"
                 "  return 0;\n"
                 "}\n");
  EXPECT_EQ(r.Errors, 2u);
  EXPECT_EQ(errorLines(r), (std::vector<size_t>{3, 4}));
}

TEST(Recovery, LexicalErrorsDoNotStopTheParse) {
  auto r = parse("import ::io; fn main() -> int {\n"
                 "  x: int = 1 @ 2;\n" // 2: '@' (the `2` it strands is not
                 "  y: int = 3;\n"     //    reported again)
                 "  return 0 $;\n"     // 4: '$'
                 "}\n");
  ASSERT_EQ(r.Diags.size(), 2u) << r.Text;
  EXPECT_NE(r.Diags[0].Message.find("invalid character '@'"),
            std::string::npos);
  EXPECT_EQ(r.Diags[0].Loc.getLineStart(), 2u);
  EXPECT_NE(r.Text.find("invalid character '$'"), std::string::npos);
}

TEST(Recovery, BindingMessagesSurvive) {
  auto r = parse("fn main() -> int {\n"
                 "  a: void? = None;\n"
                 "  b: Str?? = None;\n"
                 "  c: Str = t.00;\n"
                 "  f(1) = 2;\n"
                 "  d: int = a < b < c;\n"
                 "  return 0;\n"
                 "}\n");
  EXPECT_EQ(r.Errors, 5u);
  EXPECT_NE(r.Text.find("optional type 'void?' is not supported"),
            std::string::npos);
  EXPECT_NE(r.Text.find("nested optional type 'Str?"
                        "?' is not supported"),
            std::string::npos);
  EXPECT_NE(r.Text.find("tuple index must not have leading zeros: .00"),
            std::string::npos);
  EXPECT_NE(r.Text.find("left-hand side of '=' must be an identifier, field "
                        "access, or subscript"),
            std::string::npos);
  EXPECT_NE(r.Text.find("relational operators cannot be chained"),
            std::string::npos);
}

TEST(Recovery, DeepNestingIsRejectedNotCrashed) {
  std::string src = "import ::io; fn main() -> int { x = ";
  for (int i = 0; i < 5000; ++i)
    src += '(';
  src += '1';
  for (int i = 0; i < 5000; ++i)
    src += ')';
  src += "; return 0; }";
  auto r = parse(src);
  EXPECT_GE(r.Errors, 1u);
  EXPECT_NE(r.Text.find("nesting too deep"), std::string::npos);
}

// Past the limit, nested brackets are skipped whole, through the closer
// matching the one too many, so the limit is the only error and the
// statement after them is parsed (#58).  The statement's own recovery used
// to stop at the first `;` in them, the count of an array `[v; n]`, and
// each of 20000 levels was an error of its own: 38 seconds, each error
// printing the whole line.
TEST(Recovery, NestingTooDeepInBracketsIsTheOnlyError) {
  const int n = 20000;
  const std::vector<std::pair<std::string, std::string>> levels = {
      {"[", "; 1]"},    // an array of arrays
      {"[(", "); 1]"},  // parentheses
      {"[a[", "]; 1]"}, // subscripts
      {"[f(", "); 1]"}, // calls
  };
  for (const auto &[open, close] : levels) {
    std::string src = "fn main() -> int { x = ";
    for (int i = 0; i < n; ++i)
      src += open;
    src += "0";
    for (int i = 0; i < n; ++i)
      src += close;
    src += ";\ny = ; return 0; }\n";
    const auto start = std::chrono::steady_clock::now();
    auto r = parse(src);
    const auto took = std::chrono::steady_clock::now() - start;
    EXPECT_EQ(r.Errors, 2u) << open;
    ASSERT_FALSE(r.Diags.empty()) << open;
    EXPECT_EQ(r.Diags[0].Message, "nesting too deep (more than 512 levels)")
        << open;
    EXPECT_EQ(errorLines(r), (std::vector<size_t>{1, 2})) << open;
    EXPECT_LT(took, std::chrono::seconds(5)) << open;
  }
}

// An `if` takes up to 510 `elif`s in a function's body.  Past them the
// block one level too deep is skipped, and so is the rest of the chain, each
// branch as deep or deeper, braces in its condition too: the limit is the
// only error, where the next `elif`'s level was a second, and the statement
// after the chain is parsed.
TEST(Recovery, ElifsPastTheLimitAreOneError) {
  for (int n : {510, 511, 600}) {
    std::string src = "fn main() -> int { x = 0;\n  if x == 0 { x = 1; }";
    for (int i = 1; i <= n; ++i)
      src += " elif x == " + std::to_string(i) + " { x = 1; }";
    if (n > 510)
      src += " elif ok(fn() -> bool { return [True; 1][0]; }) { x = 1; }";
    src += " else { x = 2; }\n  y = ; return 0; }\n";
    auto r = parse(src);
    std::vector<size_t> lines{3};
    if (n > 510) {
      lines.insert(lines.begin(), 2);
      ASSERT_FALSE(r.Diags.empty()) << n;
      EXPECT_EQ(r.Diags[0].Message, "nesting too deep (more than 512 levels)")
          << n;
    }
    EXPECT_EQ(r.Errors, lines.size()) << n;
    EXPECT_EQ(errorLines(r), lines) << n;
  }
}

TEST(Recovery, NoDiagEngineFallsBackToStderr) {
  paykan::ast::ASTContext ctx;
  auto out = parseSource(ctx, "fn main() -> int { return }", nullptr);
  EXPECT_EQ(out.ErrorCount, 1u);
  ASSERT_NE(out.Root, nullptr);
}

// Each real error is reported once (#79): no follow-on syntax error right
// after a lexical error, and an unclosed block at end of file is one error
// (not one per open block), with a note at the brace left open.

TEST(Recovery, NoFollowOnAfterALexicalError) {
  auto r = parse("fn a() -> int {\n"
                 "  s = \"unterminated;\n" // 2
                 "  return 0;\n"
                 "}\n"
                 "fn b() -> int {\n"
                 "  x = 5 @ 3;\n" // 6
                 "  return 0;\n"
                 "}\n"
                 "fn c() -> int {\n"
                 "  c = 'ab';\n" // 10
                 "  return 0;\n"
                 "}\n"
                 "fn d() -> int {\n"
                 "  x = 1 +;\n" // 14: still reported
                 "  return 0;\n"
                 "}\n");
  EXPECT_EQ(r.Errors, 4u) << r.Text;
  EXPECT_EQ(errorLines(r), (std::vector<size_t>{2, 6, 10, 14})) << r.Text;
  ASSERT_EQ(r.Diags.size(), 4u);
  EXPECT_NE(r.Diags[0].Message.find("unterminated string literal"),
            std::string::npos);
  EXPECT_NE(r.Diags[1].Message.find("invalid character '@'"),
            std::string::npos);
  EXPECT_EQ(r.Diags[2].Message, "character literal must contain exactly one "
                                "character (use a string literal for text)");
  EXPECT_EQ(r.Diags[3].Message, "unexpected ';'; expected an expression");
}

TEST(Recovery, UnclosedBlocksAtEndOfFileAreOneError) {
  auto r = parse("import ::io; fn main() -> int {\n"
                 "  while True {\n"
                 "    if x {\n"
                 "  return 0;\n");
  EXPECT_EQ(r.Errors, 1u) << r.Text;
  ASSERT_EQ(r.Diags.size(), 2u) << r.Text;
  EXPECT_EQ(r.Diags[0].Level, paykan::sema::Diagnostic::Error);
  EXPECT_EQ(r.Diags[0].Message,
            "unexpected end of file; expected '}' to close the block");
  EXPECT_EQ(r.Diags[1].Level, paykan::sema::Diagnostic::Note);
  EXPECT_EQ(r.Diags[1].Message, "to match this '{'");
  EXPECT_EQ(r.Diags[1].Loc.getLineStart(), 3u); // the innermost open block
  EXPECT_EQ(r.Diags[1].Loc.getColumnStart(), 10u);

  auto c = parse("class A {\n  fn f() {\n");
  EXPECT_EQ(c.Errors, 1u) << c.Text;
  auto m = parse("import ::io; fn main() -> int { match 1 { _ { \n");
  EXPECT_EQ(m.Errors, 1u) << m.Text;
}
