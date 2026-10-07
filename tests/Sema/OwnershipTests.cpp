// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: the ownership prototype's syntax (docs/design/ownership-proto.md)
// is accepted with Sema::setOwnership(true) and an error without it, the way
// an AST from the interchange format or a frontend with the option reaches
// a compiler run without --ownership.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Ownership, SyntaxNeedsTheFlag) {
  const std::pair<const char *, const char *> cases[] = {
      {"fn main() -> int { x: own int = 1; return x; }", "own"},
      {"fn main() -> int { let x = 1; return x; }", "let"},
      {"fn main() -> int { x = 1; return cp x; }", "cp"},
      {"fn main() -> int { x = 1; return mv x; }", "mv"},
      {"fn f(s: mut Str) { }\nfn main() -> int { return 0; }", "mut"},
      {"fn f() -> own Str { return \"\"; }\nfn main() -> int { return 0; }",
       "own"},
      {"class C { s: own Str; }\nfn main() -> int { return 0; }", "own"},
      {"class C { fn m(self) { } }\nfn main() -> int { return 0; }", "self"},
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
}

TEST(Ownership, SyntaxIsAcceptedWithTheFlag) {
  auto r = semaRun(parseOwnership(R"(
class P {
  n: own Str;
  fn __init__(self: mut, n: own Str) { self.n = mv n; }
  fn name(self) -> own Str { return cp self.n; }
}
fn main() -> int {
  p: own = P("a");
  let q: own P = cp p;
  s: Str = q.name();
  k: own int = 1;
  return mv k - cp k;
})"),
                   /*ownership=*/true);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}
