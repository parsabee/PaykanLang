// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Shared test utilities for Paykan compiler unit tests (frontend + Sema).
// Helpers that compile and run programs live in CodeGenTestUtils.h.

#pragma once

#include "ParserDriver.h"
#include "Sema.h"

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

/// Write source to a temp file, returning its path.
inline std::string writeTempFile(const std::string &source) {
  // Use a unique name per call (atomic counter + pid for cross-process safety).
  static std::atomic<int> counter{0};
  auto pathStr = (std::filesystem::temp_directory_path() /
                  ("paykan_test_" + std::to_string(getpid()) + "_" +
                   std::to_string(counter++) + ".pkn"))
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
  auto path = (std::filesystem::temp_directory_path() /
               ("paykan_cap_" + std::to_string(getpid()) + "_" +
                std::to_string(cnt++) + ".txt"))
                  .string();
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

/// Parse source code. Returns {success, driver (moved)}.
struct ParseResult {
  bool Ok;
  std::unique_ptr<parser::ParserDriver> Driver;
};

inline ParseResult parse(const std::string &source) {
  auto path = writeTempFile(source);
  auto driver = std::make_unique<parser::ParserDriver>();
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
  sema::Sema sema(driver->getASTContext(), diag, "");
  auto semaCtx = sema.run(driver->getRoot());
  return {semaCtx.Ok, diagOS.str(), semaCtx.ErrorCount};
}

} // namespace paykan::test
