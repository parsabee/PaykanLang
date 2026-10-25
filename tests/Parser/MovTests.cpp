// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Parser tests: `mov` is retired (#145).  The keyword stays reserved and every
// frontend reports frontend::kMovRemoved at it, once per use.

#include "TestUtils.h"
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

using namespace paykan::test;

TEST(Mov, RemovedKeywordIsOneErrorPerUse) {
  const std::pair<const char *, unsigned> cases[] = {
      {"fn main() -> int { a: int = 3; b = mov a; return b; }", 1},
      {"fn take(s: Str) -> Str { return mov s; }\n"
       "fn main() -> int { return 0; }",
       1},
      {"import ::io; fn make() -> Str { return \"x\"; }\n"
       "fn main() -> int { t = mov make(); io::println(mov t); return 0; }",
       2},
  };
  for (const auto &[src, errors] : cases) {
    auto path = writeTempFile(src);
    for (const std::string &fe : paykan::frontend::Registry::get().names()) {
      paykan::parser::ParserDriver drv(fe);
      std::ostringstream os;
      paykan::sema::DiagEngine diag(os);
      drv.setDiagEngine(&diag);
      EXPECT_NE(drv.parseFile(path), 0) << fe << ": " << src;
      EXPECT_EQ(drv.getErrorCount(), errors) << fe << ":\n" << os.str();
      EXPECT_NE(os.str().find("error: 'mov' was removed in v0.2.0; ownership "
                              "transfers are inferred"),
                std::string::npos)
          << fe << ":\n"
          << os.str();
    }
    std::filesystem::remove(path);
  }
}

// The removal is reported at the `mov`, before the operand is parsed, so the
// diagnostics come in source order (docs/grammar.md section 9): an outer
// `mov`'s before an inner one's, and before a syntax error in its operand.
TEST(Mov, ReportedInSourceOrder) {
  const std::string removed =
      "error: " + std::string(paykan::frontend::kMovRemoved);
  const std::pair<const char *, std::vector<std::string>> cases[] = {
      {"fn main() -> int { s = \"a\"; u = mov mov s; return 0; }",
       {"1:33: " + removed, "1:37: " + removed}},
      {"fn main() -> int { b = mov ; return 0; }",
       {"1:24: " + removed, "1:28: error: unexpected ';'"}},
  };
  for (const auto &[src, errors] : cases) {
    auto path = writeTempFile(src);
    for (const std::string &fe : paykan::frontend::Registry::get().names()) {
      paykan::parser::ParserDriver drv(fe);
      std::ostringstream os;
      paykan::sema::DiagEngine diag(os);
      diag.setSourceInfo(path, &drv.getSourceLines());
      drv.setDiagEngine(&diag);
      EXPECT_NE(drv.parseFile(path), 0) << fe << ": " << src;
      EXPECT_EQ(drv.getErrorCount(), errors.size()) << fe << ":\n" << os.str();
      size_t at = 0;
      for (const std::string &e : errors) {
        at = os.str().find(path + ":" + e, at);
        ASSERT_NE(at, std::string::npos) << fe << ": " << e << " in order in\n"
                                         << os.str();
      }
    }
    std::filesystem::remove(path);
  }
}
