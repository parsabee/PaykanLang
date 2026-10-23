// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Parser tests: `mov` is retired (#145).  The keyword stays reserved and every
// frontend reports frontend::kMovRemoved at it, once per use.

#include "TestUtils.h"
#include <gtest/gtest.h>

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
