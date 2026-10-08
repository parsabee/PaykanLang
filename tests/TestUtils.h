// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Shared test utilities for Paykan compiler unit tests (frontend + Sema).
// Helpers that compile and run programs live in CodeGenTestUtils.h.

#pragma once

#include "ASTPrinter.h"
#include "ParserDriver.h"
#include "Sema.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace paykan::test {

/// A scratch directory private to this test process.  The parser and Sema
/// suites run once per enabled frontend, and CTest may run those processes
/// concurrently, so tests that create module files under fixed names must
/// not share /tmp directly.
inline std::filesystem::path tempDir() {
  static const std::filesystem::path dir = [] {
    auto d = std::filesystem::temp_directory_path() /
             ("paykan_tests_" + std::to_string(getpid()));
    std::filesystem::create_directories(d);
    return d;
  }();
  return dir;
}

/// Write source to a temp file, returning its path.
inline std::string writeTempFile(const std::string &source) {
  // Use a unique name per call (atomic counter + pid for cross-process safety).
  static std::atomic<int> counter{0};
  auto pathStr =
      (tempDir() / ("paykan_test_" + std::to_string(counter++) + ".pkn"))
          .string();
  std::ofstream ofs(pathStr);
  ofs << source;
  ofs.close();
  return pathStr;
}

/// Read the full contents of a file into a string and delete the file.
inline std::string drainAndRemoveTempFile(const std::string &path) {
  std::ifstream ifs(path, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
  ifs.close();
  std::filesystem::remove(path);
  return content;
}

/// Redirect fd to a fresh temp file; return (saved_fd, temp_path).
/// Caller must restore the fd and call drainAndRemoveTempFile when done.
inline std::pair<int, std::string> redirectFdToTempFile(int fd) {
  static std::atomic<int> cnt{0};
  auto path =
      (tempDir() / ("paykan_cap_" + std::to_string(cnt++) + ".txt")).string();
  int saved = dup(fd);
  int tmp = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  dup2(tmp, fd);
  close(tmp);
  return {saved, path};
}

/// Restore fd from a previously saved descriptor.
inline void restoreFd(int fd, int saved) {
  dup2(saved, fd);
  close(saved);
}

/// The frontend the tests parse with: PAYKAN_TEST_FRONTEND when set (the
/// per-frontend CTest entries set it), otherwise the build's default.
inline std::string testFrontend() {
  if (const char *fe = std::getenv("PAYKAN_TEST_FRONTEND"); fe && *fe)
    return fe;
  return std::string(frontend::defaultFrontend());
}

/// The AST of @p driver printed with ASTPrinter, or "" after a failed parse.
inline std::string dumpAST(parser::ParserDriver &driver) {
  if (!driver.getRoot())
    return "";
  std::ostringstream os;
  ast::ASTPrinter printer(os);
  printer.visit(driver.getRoot());
  return os.str();
}

/// Parse source code. Returns {success, driver (moved)}.
///
/// Every frontend must build the same AST and accept the same inputs
/// (docs/grammar.md), so when more than one is built the source is also
/// parsed with every other registered frontend and the results compared;
/// a difference fails the calling test.  This makes every test input part
/// of the differential check.
struct ParseResult {
  bool Ok;
  std::unique_ptr<parser::ParserDriver> Driver;
};

inline ParseResult parse(const std::string &source) {
  auto path = writeTempFile(source);
  std::string primary = testFrontend();
  auto driver = std::make_unique<parser::ParserDriver>(primary);
  int rc = driver->parseFile(path);

  for (const std::string &other : frontend::Registry::get().names()) {
    if (other == primary)
      continue;
    parser::ParserDriver otherDriver(other);
    std::ostringstream quiet; // the primary parse already printed them
    sema::DiagEngine diag(quiet);
    otherDriver.setDiagEngine(&diag);
    int otherRc = otherDriver.parseFile(path);
    EXPECT_EQ(rc == 0, otherRc == 0) << "frontends '" << primary << "' and '"
                                     << other << "' disagree on accepting:\n"
                                     << source;
    if (rc == 0 && otherRc == 0) {
      EXPECT_EQ(dumpAST(*driver), dumpAST(otherDriver))
          << "frontends '" << primary << "' and '" << other
          << "' build different ASTs for:\n"
          << source;
    }
  }

  std::filesystem::remove(path);
  return {rc == 0, std::move(driver)};
}

/// Parse @p source with the ownership prototype's syntax
/// (frontend::Options::Ownership, docs/design/ownership-proto.md).  Only the
/// recursive-descent frontend has it, so nothing is compared.
inline ParseResult parseOwnership(const std::string &source) {
  auto path = writeTempFile(source);
  frontend::Options opts;
  opts.Ownership = true;
  auto driver =
      std::make_unique<parser::ParserDriver>("recursive-descent", opts);
  int rc = driver->parseFile(path);
  std::filesystem::remove(path);
  return {rc == 0, std::move(driver)};
}

/// Run semantic analysis on source. Returns {ok, errorMessages}.
struct SemaResult {
  bool Ok;
  std::string Diagnostics;
  unsigned ErrorCount;
};

inline SemaResult semaCheck(const std::string &source) {
  auto [parseOk, driver] = parse(source);
  if (!parseOk)
    return {false, "parse error", 1};

  std::ostringstream diagOS;
  sema::DiagEngine diag(diagOS);
  diag.setSourceInfo(driver->getCurrentFile(), &driver->getSourceLines());
  sema::Sema sema(driver->getASTContext(), diag, "", driver->getFrontendName());
  auto semaCtx = sema.run(driver->getRoot());
  return {semaCtx.Ok, diagOS.str(), semaCtx.ErrorCount};
}

} // namespace paykan::test
