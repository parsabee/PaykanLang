// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Program functions spelled like runtime symbols (#117).  Runtime externs
// are `$rt.<symbol>` in PIR (docs/pir.md), so a user function named
// `PaykanString_new`, `Paykan_println` or `Paykan_panic_div_by_zero` is an
// ordinary program function, with a different signature or the runtime's
// own, and the program's runtime calls still reach the runtime.  Each program
// runs with every frontend on the backend under test, and must print the
// expected output and leave zero live heap blocks.  (The runtime's
// division-by-zero panic next to a user `Paykan_panic_div_by_zero` is
// checked in DriverTests.cpp, as a panic aborts the process.)

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan;
using namespace paykan::test;

namespace {

void expectRunsOnEveryFrontend(const char *label, const std::string &source,
                               const std::string &expected) {
  for (const std::string &fe : frontend::Registry::get().names()) {
    LeakGuard g;
    auto r = compileAndRunWithFrontend(source, fe);
    ASSERT_TRUE(r.CompileOk) << label << " [" << fe << "]: " << r.StdErr;
    EXPECT_EQ(r.ExitCode, 0) << label << " [" << fe << "]: " << r.StdErr;
    EXPECT_EQ(r.StdOut, expected) << label << " [" << fe << "]";
    g.expectNoLeaks((std::string(label) + " [" + fe + "]").c_str());
  }
}

} // namespace

// Signatures unlike the runtime's: these used to be an ICE (an assertion in
// the lowering, or a PIR verification failure).
TEST(RuntimeName, UserFunctionsWithOtherSignatures) {
  expectRunsOnEveryFrontend("OtherSignatures", R"(
    fn PaykanString_new(x: int) -> int { return x + 1; }
    fn Paykan_println(a: int, b: int) -> int { return a * b; }
    fn Paykan_panic_div_by_zero(x: int) -> Str { return "user " + Str(x); }
    fn PaykanString_concat() -> int { return 9; }
    fn main() -> int {
      println("hi");
      s = "a" + "b";
      println(s);
      println(Str<int>(PaykanString_new(2)));
      println(Str<int>(Paykan_println(6, 7)));
      println(Paykan_panic_div_by_zero(10 / 2));
      println(Str<int>(PaykanString_concat()));
      return 0;
    }
  )",
                            "hi\nab\n3\n42\nuser 5\n9\n");
}

// The runtime's own signatures (or as close as Paykan types spell them):
// these used to be a silent miscompile (the user function ran in place of
// the runtime's) or a crash.
TEST(RuntimeName, UserFunctionsWithTheRuntimeSignatures) {
  expectRunsOnEveryFrontend(
      "SameSignatures", R"(
    fn Paykan_panic_div_by_zero() { println("user div"); }
    fn Paykan_panic_int_to_char(n: int) { println("user char " + Str(n)); }
    fn Paykan_println(s: Str) { print("[" + s + "]\n"); }
    fn PaykanString_new(s: Str, n: int) -> Str { return s + Str(n); }
    fn PaykanObject_None() -> int { return 4; }
    fn two() -> int { return 2; }
    fn main() -> int {
      Paykan_panic_div_by_zero();
      Paykan_panic_int_to_char(300);
      println(Str<int>(10 / two()));
      println(Str<char>(char<int>(65)));
      Paykan_println("x");
      println(PaykanString_new("s", 1));
      a: Str? = None;
      println(a);
      println(Str<int>(PaykanObject_None()));
      return 0;
    }
  )",
      "user div\nuser char 300\n5\nA\n[x]\ns1\nNone\n4\n");
}
