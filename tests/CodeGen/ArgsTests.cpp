// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// End-to-end tests for main(args: view Str[]) parameter passing.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// -- main(args: view Str[]) — program argument passing

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
    fn main(args: view Str[]) -> int {
      return args.len();
    }
  )",
                                 {"script.pkn", "hello", "world"});
  ASSERT_TRUE(r.CompileOk);
  EXPECT_EQ(r.ExitCode, 3);
}

TEST(Args, FirstArgIsScriptPath) {
  auto r = compileAndRunWithArgs(R"(import ::io;
    fn main(args: view Str[]) -> int {
      x: view Str = args[0];
      io::print(x);
      return 0;
    }
  )",
                                 {"myscript.pkn"});
  ASSERT_TRUE(r.CompileOk);
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "myscript.pkn");
}

TEST(Args, SecondArgReadable) {
  auto r = compileAndRunWithArgs(R"(import ::io;
    fn main(args: view Str[]) -> int {
      io::print(args[1]);
      return 0;
    }
  )",
                                 {"script.pkn", "greet"});
  ASSERT_TRUE(r.CompileOk);
  EXPECT_EQ(r.StdOut, "greet");
}

TEST(Args, EmptyArgList) {
  auto r = compileAndRunWithArgs(R"(
    fn main(args: view Str[]) -> int {
      return args.len();
    }
  )",
                                 {});
  ASSERT_TRUE(r.CompileOk);
  EXPECT_EQ(r.ExitCode, 0);
}

TEST(Args, ArgLenUsedInLogic) {
  auto r = compileAndRunWithArgs(R"(
    fn main(args: view Str[]) -> int {
      if args.len() > 1 {
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

// `args` is a slice: it is passed on to `view Str[]` parameters, which index
// it and measure it like any other slice.
TEST(Args, PassedToSliceParameters) {
  LeakGuard g;
  auto r = compileAndRunWithArgs(R"(import ::io;
    fn longest(ss: view Str[]) -> int {
      n = 0;
      i = 0;
      while i < ss.len() {
        if ss[i].len() > n { n = ss[i].len(); }
        i = i + 1;
      }
      return n;
    }
    fn joined(ss: view Str[], from: int) -> Str {
      s = "";
      i = from;
      while i < ss.len() {
        s = s + ss[i] + ";";
        i = i + 1;
      }
      return s;
    }
    fn main(args: view Str[]) -> int {
      io::println(joined(args, 1));
      return longest(args);
    }
  )",
                                 {"s.pkn", "a", "abcdefgh", "xyz"});
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 8);
  EXPECT_EQ(r.StdOut, "a;abcdefgh;xyz;\n");
  g.expectNoLeaks("PassedToSliceParameters");
}
