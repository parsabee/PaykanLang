// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: the ownership prototype (docs/design/ownership-proto.md).  Its
// syntax is an error without Sema::setOwnership(true), the way an AST from
// the interchange format reaches a compiler run without --ownership.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Ownership, SyntaxNeedsTheFlag) {
  const std::pair<const char *, const char *> cases[] = {
      {"fn main() -> int { let x = 1; return x; }", "let"},
      {"fn f(view n: int) { }\nfn main() -> int { return 0; }", "view"},
      {"class C { fn m(inout n: int) { } }\nfn main() -> int { return 0; }",
       "inout"},
  };
  for (const auto &[src, what] : cases) {
    auto r = semaRun(parseOwnership(src));
    EXPECT_FALSE(r.Ok) << src;
    EXPECT_NE(r.Diagnostics.find(std::string("error: '") + what +
                                 "' needs --ownership (prototype)"),
              std::string::npos)
        << src << "\n"
        << r.Diagnostics;
  }
  // With it, the same programs are accepted.
  for (const auto &[src, what] : cases) {
    auto r = semaRun(parseOwnership(src), /*ownership=*/true);
    EXPECT_TRUE(r.Ok) << src << "\n" << r.Diagnostics;
  }
}
