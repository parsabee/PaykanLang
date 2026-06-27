// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Smoke tests for the paykan driver CLI (--check-only, --dump-ast, --version).
// The PAYKAN_BIN CMake variable is injected so tests find the binary.

#include <gtest/gtest.h>

#include "Version.h"

#include <array>
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
