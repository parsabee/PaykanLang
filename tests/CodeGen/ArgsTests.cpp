// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// End-to-end tests for main(args: Str[]) parameter passing.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// ---------------------------------------------------------------------------
// main(args: Str[]) — program argument passing
// ---------------------------------------------------------------------------

TEST(Args, NoArgVariantStillRuns) {
  auto r = compileAndRun(R"(
    fn main() -> int {
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk);
  EXPECT_EQ(r.ExitCode, 0);
}

TEST(Args, ArgCountViaLen) {
  auto r = compileAndRunWithArgs(R"(
    fn main(args: Str[]) -> int {
      return args.len();
    }
  )",
                                 {"script.pkn", "hello", "world"});
  ASSERT_TRUE(r.CompileOk);
  EXPECT_EQ(r.ExitCode, 3);
}

TEST(Args, FirstArgIsScriptPath) {
  auto r = compileAndRunWithArgs(R"(
    fn main(args: Str[]) -> int {
      x: Str = args[0];
      print(x);
      return 0;
    }
  )",
                                 {"myscript.pkn"});
  ASSERT_TRUE(r.CompileOk);
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "myscript.pkn");
}

TEST(Args, SecondArgReadable) {
  auto r = compileAndRunWithArgs(R"(
    fn main(args: Str[]) -> int {
      print(args[1]);
      return 0;
    }
  )",
                                 {"script.pkn", "greet"});
  ASSERT_TRUE(r.CompileOk);
  EXPECT_EQ(r.StdOut, "greet");
}

TEST(Args, EmptyArgList) {
  auto r = compileAndRunWithArgs(R"(
    fn main(args: Str[]) -> int {
      return args.len();
    }
  )",
                                 {});
  ASSERT_TRUE(r.CompileOk);
  EXPECT_EQ(r.ExitCode, 0);
}

TEST(Args, ArgLenUsedInLogic) {
  auto r = compileAndRunWithArgs(R"(
    fn main(args: Str[]) -> int {
      if (args.len() > 1) {
        return 1;
      } else {
        return 0;
      }
    }
  )",
                                 {"script.pkn", "extra"});
  ASSERT_TRUE(r.CompileOk);
  EXPECT_EQ(r.ExitCode, 1);
}
