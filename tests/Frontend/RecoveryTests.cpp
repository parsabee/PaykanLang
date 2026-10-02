// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Error recovery of the recursive-descent frontend: several independent errors
// in one file are all reported, at statement, class-member and top-level
// boundaries, and a file with errors always fails.

#include "Frontends/RecursiveDescent.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
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
  auto r = parse("fn main() -> int {\n"
                 "  x: int = ;\n"     // 2
                 "  y: int = 1 + ;\n" // 3
                 "  z: int = 2;\n"
                 "  return 0\n" // 5: missing ';', found '}'
                 "}\n");
  EXPECT_EQ(r.Errors, 3u);
  EXPECT_EQ(errorLines(r), (std::vector<size_t>{2, 3, 6}));
}

TEST(Recovery, BrokenNestedBlockDoesNotSwallowTheEnclosingOne) {
  auto r = parse("fn main() -> int {\n"
                 "  if (x) {\n"
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
  auto r = parse("fn main() -> int {\n"
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
  auto r = parse("fn main() -> int {\n"
                 "  x: int = 1 @ 2;\n" // 2: '@' then a syntax error at 2
                 "  y: int = 3;\n"
                 "  return 0 $;\n" // 4: '$'
                 "}\n");
  ASSERT_GE(r.Diags.size(), 3u);
  EXPECT_NE(r.Diags[0].Message.find("invalid character '@'"),
            std::string::npos);
  EXPECT_EQ(r.Diags[0].Loc.getLineStart(), 2u);
  EXPECT_NE(r.Text.find("invalid character '$'"), std::string::npos);
}

TEST(Recovery, BindingMessagesSurvive) {
  auto r = parse("fn main() -> int {\n"
                 "  a: int? = None;\n"
                 "  b: Str?? = None;\n"
                 "  c: Str = t.00;\n"
                 "  f(1) = 2;\n"
                 "  d: int = a < b < c;\n"
                 "  return 0;\n"
                 "}\n");
  EXPECT_EQ(r.Errors, 5u);
  EXPECT_NE(r.Text.find("optional primitive types are not supported yet "
                        "('int?')"),
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
  std::string src = "fn main() -> int { x = ";
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

TEST(Recovery, NoDiagEngineFallsBackToStderr) {
  paykan::ast::ASTContext ctx;
  auto out = parseSource(ctx, "fn main() -> int { return }", nullptr);
  EXPECT_EQ(out.ErrorCount, 1u);
  ASSERT_NE(out.Root, nullptr);
}
