// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Shared test utilities for Paykan compiler unit tests.

#pragma once

#include "ParserDriver.h"
#include "Sema.h"
#include "CodeGen.h"
#include "JIT.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/raw_ostream.h>

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sstream>

namespace paykan::test {

/// Write source to a temp file, returning its path.
inline std::string writeTempFile(const std::string &source) {
  // Use a unique name per call (atomic counter + pid for cross-process safety).
  static std::atomic<int> counter{0};
  auto pathStr = (std::filesystem::temp_directory_path() /
             ("paykan_test_" + std::to_string(getpid()) + "_" +
              std::to_string(counter++) + ".pkn")).string();
  std::ofstream ofs(pathStr);
  ofs << source;
  ofs.close();
  return pathStr;
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

  std::string diagStr;
  llvm::raw_string_ostream diagOS(diagStr);
  sema::Sema sema(driver->getASTContext(), diagOS);
  bool ok = sema.run(driver->getRoot());
  return {ok, diagStr, sema.getErrorCount()};
}

/// Compile and JIT-execute source. Returns {exitCode, stdout, stderr, compiledOk}.
struct RunResult {
  int ExitCode;
  std::string StdOut;
  std::string StdErr;
  bool CompileOk;
};

inline RunResult compileAndRun(const std::string &source) {
  auto [parseOk, driver] = parse(source);
  if (!parseOk)
    return {-1, "", "parse error", false};

  // Sema
  std::string diagStr;
  llvm::raw_string_ostream diagOS(diagStr);
  sema::Sema sema(driver->getASTContext(), diagOS);
  if (!sema.run(driver->getRoot()))
    return {-1, "", diagStr, false};

  // CodeGen
  auto llvmCtx = std::make_unique<llvm::LLVMContext>();
  codegen::CodeGen cg(driver->getASTContext(), *llvmCtx, "test");
  if (!cg.run(driver->getRoot()))
    return {-1, "", "codegen failed", false};

  // Capture stdout by redirecting fd 1 to a pipe.
  int pipefd[2];
  if (pipe(pipefd) != 0)
    return {-1, "", "pipe() failed", false};

  // Save original stdout/stderr.
  int savedStdout = dup(STDOUT_FILENO);
  int savedStderr = dup(STDERR_FILENO);
  dup2(pipefd[1], STDOUT_FILENO);

  int errPipe[2];
  pipe(errPipe);
  dup2(errPipe[1], STDERR_FILENO);

  // JIT run
  auto resultOrErr = jit::runModule(cg.takeModule(), std::move(llvmCtx));
  fflush(stdout);
  fflush(stderr);

  // Restore stdout/stderr.
  dup2(savedStdout, STDOUT_FILENO);
  dup2(savedStderr, STDERR_FILENO);
  close(savedStdout);
  close(savedStderr);
  close(pipefd[1]);
  close(errPipe[1]);

  // Read captured output.
  std::string outStr, errStr;
  char buf[4096];
  ssize_t n;
  while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
    outStr.append(buf, n);
  close(pipefd[0]);

  while ((n = read(errPipe[0], buf, sizeof(buf))) > 0)
    errStr.append(buf, n);
  close(errPipe[0]);

  if (!resultOrErr) {
    llvm::consumeError(resultOrErr.takeError());
    return {-1, outStr, errStr, false};
  }

  return {*resultOrErr, outStr, errStr, true};
}

} // namespace paykan::test
