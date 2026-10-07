// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The source formatter (paykan/format/Format.h): its two-column layout, and
// over the samples corpus that it succeeds, keeps every token and comment,
// and is idempotent.

#include "paykan/format/Format.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef PAYKAN_SAMPLES_DIR
#error "PAYKAN_SAMPLES_DIR must be defined via CMake compile definition"
#endif

namespace {

using paykan::format::format;

std::string fmt(const std::string &src) {
  auto r = format(src);
  EXPECT_TRUE(r.Ok) << r.Error;
  return r.Text;
}

/// @p left padded so that @p right starts at column 46 (the default), or two
/// spaces after @p left when it reaches column 44.
std::string cols(const std::string &left, const std::string &right) {
  size_t pad = left.size() + 3 <= 46 ? 45 - left.size() : 2;
  return left + std::string(pad, ' ') + right;
}

TEST(Format, TrailingCommentsStartAtTheRightColumn) {
  EXPECT_EQ(fmt("x = 1; // one\n  y = 22;      // two\n"),
            cols("x = 1;", "// one") + "\n" + cols("  y = 22;", "// two") +
                "\n");
}

TEST(Format, CodeReachingTheColumnPushesTheCommentTwoSpacesRight) {
  std::string code = "  println(\"" + std::string(40, 'a') + "\");";
  EXPECT_EQ(fmt(code + " // c\n"), code + "  // c\n");
  std::string edge(43, 'b'); // 44 wide: the comment lands at column 47
  EXPECT_EQ(fmt(edge + "; // c\n"), edge + ";  // c\n");
  std::string fits(42, 'b'); // 43 wide: the last that keeps column 46
  EXPECT_EQ(fmt(fits + "; // c\n"), cols(fits + ";", "// c") + "\n");
}

TEST(Format, OwnLineCommentsStayWhereTheyAre) {
  std::string src = "// top\nfn main() -> int {\n    // deep\n  return 0;\n}\n";
  EXPECT_EQ(fmt(src), src);
  // ... unless they continue a trailing comment, which moves them with it.
  EXPECT_EQ(fmt("x = 1; // starts\n       // continues\ny = 2;\n"),
            cols("x = 1;", "// starts") + "\n" + std::string(45, ' ') +
                "// continues\ny = 2;\n");
}

TEST(Format, ShortFunctionsCollapseOntoOneLine) {
  EXPECT_EQ(fmt("class P {\n  x: int;\n  fn __init__(x: int) {\n"
                "    self.x = x;\n  }\n  fn get() -> int { return self.x; }\n"
                "}\n"),
            "class P {\n  x: int;\n" +
                cols("  fn __init__(x: int)", "{ self.x = x; }") + "\n" +
                cols("  fn get() -> int", "{ return self.x; }") + "\n}\n");
  EXPECT_EQ(fmt("fn nothing() {\n}\n"), cols("fn nothing()", "{}") + "\n");
}

TEST(Format, SpacingInsideABodyLineIsKept) {
  EXPECT_EQ(fmt("fn f() -> int {\n  return (1,  2).0;\n}\n"),
            cols("fn f() -> int", "{ return (1,  2).0; }") + "\n");
}

TEST(Format, ACommentOnACollapsedFunctionMovesAboveIt) {
  EXPECT_EQ(fmt("  fn d(a: int) -> int {  // half\n    return a / 2;\n  }\n"),
            "  // half\n" + cols("  fn d(a: int) -> int", "{ return a / 2; }") +
                "\n");
  EXPECT_EQ(fmt("fn one() -> int { return 1; } // one\n"),
            "// one\n" + cols("fn one() -> int", "{ return 1; }") + "\n");
}

TEST(Format, FunctionsWithBlocksOrCommentsInsideStayAsTheyAre) {
  std::string nested = "fn f(b: bool) -> int {\n  if (b) { return 1; }\n"
                       "  return 0;\n}\n";
  EXPECT_EQ(fmt(nested), nested);
  std::string commented = "fn f() -> int {\n  // why\n  return 1;\n}\n";
  EXPECT_EQ(fmt(commented), commented);
  std::string trailing = "fn f() -> int {\n  return 1; // why\n}\n";
  EXPECT_EQ(fmt(trailing),
            "fn f() -> int {\n" + cols("  return 1;", "// why") + "\n}\n");
}

TEST(Format, TooWideFunctionsAreBlocks) {
  std::string body = "return \"" + std::string(60, 'x') + "\";";
  // A block stays a block...
  std::string block = "fn s() -> Str {\n  " + body + "\n}\n";
  EXPECT_EQ(fmt(block), block);
  // ... and a one-liner becomes one, a statement per line.
  EXPECT_EQ(fmt("  fn s() -> Str { a = 1;  " + body + " } // c\n"),
            cols("  fn s() -> Str {", "// c") + "\n    a = 1;\n    " + body +
                "\n  }\n");
}

TEST(Format, WhitespaceAtLineAndFileEndsIsTrimmed) {
  EXPECT_EQ(fmt("x = 1;   \n\n\ny = 2;\n\n\n"), "x = 1;\n\n\ny = 2;\n");
  EXPECT_EQ(fmt("x = 1;"), "x = 1;\n");
  EXPECT_EQ(fmt(""), "");
}

TEST(Format, LexicalErrorsAreReportedWithTheirLocation) {
  auto r = format("fn main() -> int {\n  x = 1 # 2;\n}\n");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.Error, "2:9: invalid character '#'");
}

TEST(Format, TheRightColumnIsConfigurable) {
  paykan::format::Style style;
  style.RightColumn = 20;
  auto r = format("x = 1; // c\n", style);
  ASSERT_TRUE(r.Ok) << r.Error;
  EXPECT_EQ(r.Text, "x = 1;" + std::string(13, ' ') + "// c\n");
}

/// Every sample formats, and formatting the result again changes nothing.
/// (format() itself checks that tokens and comments are unchanged.)
TEST(Format, SamplesFormatAndAreIdempotent) {
  size_t n = 0;
  for (const auto &e :
       std::filesystem::recursive_directory_iterator(PAYKAN_SAMPLES_DIR)) {
    if (!e.is_regular_file() || e.path().extension() != ".pkn" ||
        e.path().string().find(".paykan_cache") != std::string::npos)
      continue;
    std::ifstream in(e.path());
    std::stringstream s;
    s << in.rdbuf();
    auto once = format(s.str());
    ASSERT_TRUE(once.Ok) << e.path() << ": " << once.Error;
    auto twice = format(once.Text);
    ASSERT_TRUE(twice.Ok) << e.path() << ": " << twice.Error;
    EXPECT_EQ(twice.Text, once.Text) << e.path();
    ++n;
  }
  EXPECT_GT(n, 200u);
}

} // namespace
