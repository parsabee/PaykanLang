// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Smoke tests for the paykan driver CLI (--check-only, --dump-ast, --version).
// The PAYKAN_BIN CMake variable is injected so tests find the binary.

#include <gtest/gtest.h>

#include "Version.h"

#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef PAYKAN_BIN
#error "PAYKAN_BIN must be defined via CMake compile definition"
#endif

static const char *kPaykan = PAYKAN_BIN;

// Write source to a temp .pkn file and return its path.
static std::string writeTmp(const std::string &src) {
  auto tmp = std::filesystem::temp_directory_path() /
             ("drv_test_" + std::to_string(getpid()) + "_" +
              std::to_string(rand()) + ".pkn");
  std::ofstream ofs(tmp);
  ofs << src;
  return tmp.string();
}

struct CmdResult {
  int exitCode;
  std::string out;
};

static CmdResult run(const std::string &cmd) {
  std::array<char, 4096> buf{};
  std::string out;
  FILE *fp = popen(cmd.c_str(), "r");
  if (!fp)
    return {-1, ""};
  while (fgets(buf.data(), buf.size(), fp))
    out += buf.data();
  int rc = pclose(fp);
  return {WEXITSTATUS(rc), out};
}

// ---------------------------------------------------------------------------
// --check-only
// ---------------------------------------------------------------------------

TEST(Driver, CheckOnlySucceedsOnValidInput) {
  auto src = writeTmp("fn main() -> int { return 0; }");
  auto [rc, _] = run(std::string(kPaykan) + " --check-only " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 0);
}

TEST(Driver, CheckOnlyFailsOnSyntaxError) {
  auto src = writeTmp("fn main() -> int { return }");
  auto [rc, out] = run(std::string(kPaykan) + " --check-only " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_FALSE(out.empty());
}

TEST(Driver, CheckOnlyFailsOnSemanticError) {
  auto src = writeTmp("fn main() -> int { x: int = \"hello\"; return 0; }");
  auto [rc, out] = run(std::string(kPaykan) + " --check-only " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_FALSE(out.empty());
}

// ---------------------------------------------------------------------------
// --version / -v
// ---------------------------------------------------------------------------

TEST(Driver, VersionFlagPrintsVersion) {
  auto [rc, out] = run(std::string(kPaykan) + " --version 2>&1");
  EXPECT_EQ(rc, 0);
  EXPECT_NE(out.find("PaykanLang"), std::string::npos);
  EXPECT_NE(out.find(paykan::kVersion), std::string::npos);
  EXPECT_NE(out.find("LLVM"), std::string::npos);
}

TEST(Driver, ShortVersionFlagMatchesLong) {
  auto longOut = run(std::string(kPaykan) + " --version 2>&1");
  auto shortOut = run(std::string(kPaykan) + " -v 2>&1");
  EXPECT_EQ(shortOut.exitCode, 0);
  EXPECT_EQ(shortOut.out, longOut.out);
}

// ---------------------------------------------------------------------------
// integer divide / modulo by zero trap
// ---------------------------------------------------------------------------

TEST(Driver, IntDivByZeroTraps) {
  auto src =
      writeTmp("fn main() -> int { z: int = 0; q: int = 7 / z; return q; }");
  auto [rc, out] = run(std::string(kPaykan) + " " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_NE(out.find("division or modulo by zero"), std::string::npos) << out;
}

TEST(Driver, IntModByZeroTraps) {
  auto src =
      writeTmp("fn main() -> int { z: int = 0; r: int = 7 % z; return r; }");
  auto [rc, out] = run(std::string(kPaykan) + " " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_NE(out.find("division or modulo by zero"), std::string::npos) << out;
}

// INT64_MIN / -1 does not fit in an int: a runtime panic, not SIGFPE.
TEST(Driver, IntDivOverflowTraps) {
  auto src = writeTmp("fn main() -> int { m: int = -9223372036854775807 - 1; "
                      "d: int = -1; q: int = m / d; return q; }");
  auto [rc, out] = run(std::string(kPaykan) + " " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_NE(rc, 128 + SIGFPE) << out;
  EXPECT_NE(out.find("integer overflow in division"), std::string::npos) << out;
}

// INT64_MIN % -1 is mathematically 0 and must not trap.
TEST(Driver, IntModMinByNegOneIsZero) {
  auto src = writeTmp("fn main() -> int { m: int = -9223372036854775807 - 1; "
                      "d: int = -1; r: int = m % d; println(StrInt(r)); "
                      "return 7 % d + 3; }");
  auto [rc, out] = run(std::string(kPaykan) + " " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 3) << out;
  EXPECT_EQ(out, "0\n");
}

TEST(Driver, IntDivNonZeroSucceeds) {
  auto src = writeTmp("fn main() -> int { return 17 / 5; }");
  auto [rc, _] = run(std::string(kPaykan) + " " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 3); // 17 / 5 == 3, returned as the process exit code
}

// ---------------------------------------------------------------------------
// --dump-ast
// ---------------------------------------------------------------------------

TEST(Driver, DumpAstProducesOutput) {
  auto src = writeTmp("fn main() -> int { return 42; }");
  auto [rc, out] = run(std::string(kPaykan) + " --dump-ast " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 0);
  EXPECT_FALSE(out.empty());
}

TEST(Driver, DumpAstContainsFuncDecl) {
  auto src = writeTmp("fn main() -> int { return 0; }");
  auto [rc, out] = run(std::string(kPaykan) + " --dump-ast " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 0);
  EXPECT_NE(out.find("main"), std::string::npos);
}

TEST(Driver, DumpAstShowsOptionalType) {
  auto src = writeTmp("fn main() -> int { s: Str? = None; return 0; }");
  auto [rc, out] = run(std::string(kPaykan) + " --dump-ast " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 0);
  EXPECT_NE(out.find("OptionalType"), std::string::npos) << out;
}

TEST(Driver, OptionalPrimitiveIsRejectedAtParse) {
  auto src = writeTmp("fn main() -> int { x: int? = None; return 0; }");
  auto [rc, out] = run(std::string(kPaykan) + " --check-only " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_NE(out.find("optional primitive types are not supported yet"),
            std::string::npos)
      << out;
}
