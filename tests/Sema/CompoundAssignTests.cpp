// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: compound assignment (`t op= v`, docs/language/02-basics.md,
// "Compound assignment"), which the parser builds as the assignment
// `t = t' op v`, `t'` a copy of the target at the target's place.  An error
// in the target is reported once, not again for its copy, and the value's
// own errors still are.

#include "TestUtils.h"
#include <gtest/gtest.h>

#include <string>

using namespace paykan::test;

namespace {

/// @p stmts in a `main` that has an `int` array `xs`, an `unsigned` one
/// `us`, a `u8` one `bs`, a `P` `p` and an `H` `h`.
std::string inMain(const std::string &stmts) {
  return "class P { n: int; u: unsigned; }\n"
         "class H { a: int[2]; }\n"
         "fn mk(a: int) -> P { return P(a, 1); }\n"
         "fn arr(a: int) -> int[2] { return [a, a]; }\n"
         "fn main() -> int {\n"
         "  xs = [1];\n"
         "  us: unsigned[1] = [1];\n"
         "  bs: u8[1] = [1];\n"
         "  p = P(1, 1);\n"
         "  h = H([1, 2]);\n"
         "  " +
         stmts +
         "\n"
         "  return 0;\n"
         "}\n";
}

/// Expect @p stmts to fail with exactly the errors @p errors, in order.
void expectErrors(const std::string &stmts,
                  std::initializer_list<const char *> errors) {
  auto r = semaCheck(inMain(stmts));
  EXPECT_FALSE(r.Ok) << stmts;
  EXPECT_EQ(r.ErrorCount, errors.size()) << stmts << "\n" << r.Diagnostics;
  size_t at = 0;
  for (const char *e : errors) {
    at = r.Diagnostics.find(std::string("error: ") + e, at);
    EXPECT_NE(at, std::string::npos) << stmts << ": " << e << "\n"
                                     << r.Diagnostics;
  }
}

} // namespace

// An error in a target's index, in its array, or in its receiver is one
// error, for each of the ten operators.  It was reported again for the
// target's copy, which aborts a debug build (`the same diagnostic was
// reported twice`), and an index of another type twice, in two wordings.
// A field the receiver lacks was one error already.
TEST(CompoundAssign, AnErrorInTheTargetIsReportedOnce) {
  struct Form {
    const char *Target;
    const char *Error;
  };
  const std::initializer_list<Form> ints = {
      {"xs[y]", "use of undeclared variable 'y'"},
      {"xs[\"a\"]", "array index must be 'int', got 'Str'"},
      {"xs[ys[0]]", "use of undeclared variable 'ys'"},
      {"h.a[y]", "use of undeclared variable 'y'"},
      {"arr(zzq)[0]", "use of undeclared variable 'zzq'"},
      {"mk(zzq).n", "use of undeclared variable 'zzq'"},
      {"p.nope", "no field 'nope' in class 'P'"},
  };
  const std::initializer_list<Form> bits = {
      {"us[y]", "use of undeclared variable 'y'"},
      {"us[\"a\"]", "array index must be 'int', got 'Str'"},
      {"mk(zzq).u", "use of undeclared variable 'zzq'"},
  };
  for (const Form &f : ints)
    for (const char *op : {"+=", "-=", "*=", "/=", "%="})
      expectErrors(std::string(f.Target) + " " + op + " 1;", {f.Error});
  for (const Form &f : bits)
    for (const char *op : {"&=", "|=", "^=", "<<=", ">>="})
      expectErrors(std::string(f.Target) + " " + op + " 1;", {f.Error});
}

// The value's own errors are reported, after the target's: only the copy
// of the target is not checked again.  A literal still takes the target's
// type.
TEST(CompoundAssign, TheValueIsStillChecked) {
  expectErrors("xs[y] += zw;", {"use of undeclared variable 'y'",
                                "use of undeclared variable 'zw'"});
  expectErrors("mk(zzq).n -= zw;", {"use of undeclared variable 'zzq'",
                                    "use of undeclared variable 'zw'"});
  expectErrors("bs[y] += 300;",
               {"use of undeclared variable 'y'",
                "the literal 300 does not fit in 'u8' (0 to 255)"});
  expectErrors("us[\"a\"] <<= zw;", {"array index must be 'int', got 'Str'",
                                     "use of undeclared variable 'zw'"});
  expectErrors("xs[0] += zw;", {"use of undeclared variable 'zw'"});
  expectErrors("xs[0] += \"a\";",
               {"operator '+' is not defined for types 'int' and 'Str'"});
  auto r = semaCheck(inMain("xs[0] += 2; us[0] <<= 3; bs[0] -= 255; "
                            "p.n *= xs[0]; h.a[1] %= 2; mk(1).u ^= 1;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// An index of another type is said alike in an assignment and in a read.
TEST(CompoundAssign, AnIndexOfAnotherTypeIsSaidOneWay) {
  expectErrors("xs[\"a\"] = 1;", {"array index must be 'int', got 'Str'"});
  expectErrors("x = xs[True];", {"array index must be 'int', got 'bool'"});
  expectErrors("h.a[1.5] = 1;", {"array index must be 'int', got 'float'"});
}
