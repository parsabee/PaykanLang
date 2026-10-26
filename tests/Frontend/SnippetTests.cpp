// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The source snippet the diagnostic engine prints under a diagnostic, for
// locations no frontend's own source gives: a column past its line's end,
// as a frontend plugin's AST may carry (its columns are of the program it
// parsed, its lines those of the file it was given).

#include "DiagEngine.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

namespace {

// What @p diag prints for one error at line @p line, column @p column of
// @p lines.
std::string snippet(const std::vector<std::string> &lines, size_t line,
                    size_t column) {
  std::ostringstream os;
  paykan::sema::DiagEngine diag(os);
  diag.setSourceInfo("f.ast", &lines);
  diag.error(paykan::ast::SourceLocation(line, column, line, column + 2),
             "use of undeclared variable 'zz'");
  return os.str();
}

} // namespace

// The caret stands after the line's last character, wherever the column
// is past it: the caret line is no longer than the line.
TEST(Snippet, ACaretPastItsLineStandsAfterIt) {
  const std::vector<std::string> lines = {"(paykan-ast 0.3.0",
                                          "  (unit @1:1-1:86922"};
  EXPECT_EQ(snippet(lines, 1, 86917),
            "f.ast:1:86917: error: use of undeclared variable 'zz'\n"
            "  1 | (paykan-ast 0.3.0\n"
            "    |                  ^\n"
            "  2 |   (unit @1:1-1:86922\n");
  // A long line's window ends with it, the caret after its last character.
  const std::vector<std::string> longLine = {std::string(300, 'x')};
  EXPECT_EQ(snippet(longLine, 1, 5000),
            "f.ast:1:5000: error: use of undeclared variable 'zz'\n"
            "  1 | ..." +
                std::string(117, 'x') + "\n    | " + std::string(120, ' ') +
                "^\n");
}
