// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: `view` and `inout` parameters.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// Not checked or lowered yet: every parameter with a mode is an error, in
// functions, methods, constructors and generic templates alike.
TEST(ParamMode, ModesAreNotSupportedYet) {
  auto r = semaCheck(R"(
    fn bump(inout n: int) { }
    fn show(view n: int) { }
    fn first<T>(view xs: T[]) -> int { return 0; }
    class C { fn __init__(inout n: int) { } }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 4u) << r.Diagnostics;
  for (const char *diag :
       {":2:5: error: 'inout' parameters are not supported yet",
        ":3:5: error: 'view' parameters are not supported yet",
        ":4:5: error: 'view' parameters are not supported yet",
        ":5:15: error: 'inout' parameters are not supported yet"})
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
}
