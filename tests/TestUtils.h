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

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sstream>

#include <llvm/Linker/Linker.h>

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
                std::to_string(cnt++) + ".txt")).string();
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

  std::string diagStr;
  llvm::raw_string_ostream diagOS(diagStr);
  sema::DiagEngine diag(diagOS);
  diag.setSourceInfo(driver->getCurrentFile(), &driver->getSourceLines());
  sema::Sema sema(driver->getASTContext(), diag, "");
  auto semaCtx = sema.run(driver->getRoot());
  return {semaCtx.Ok, diagStr, semaCtx.ErrorCount};
}

/// Compile and JIT-execute source. Returns {exitCode, stdout, stderr, compiledOk}.
struct RunResult {
  int ExitCode;
  std::string StdOut;
  std::string StdErr;
  bool CompileOk;
};

inline RunResult compileAndRun(const std::string &source,
                               const std::string &projectRoot = "") {
  auto [parseOk, driver] = parse(source);
  if (!parseOk)
    return {-1, "", "parse error", false};

  // Sema
  std::string diagStr;
  llvm::raw_string_ostream diagOS(diagStr);
  sema::DiagEngine diag(diagOS);
  diag.setSourceInfo(driver->getCurrentFile(), &driver->getSourceLines());
  sema::Sema sema(driver->getASTContext(), diag, projectRoot);
  auto semaCtx = sema.run(driver->getRoot());
  if (!semaCtx)
    return {-1, "", diagStr, false};

  // CodeGen
  auto llvmCtx = std::make_unique<llvm::LLVMContext>();
  codegen::CodeGen cg(semaCtx, *llvmCtx, "test", projectRoot);
  if (!cg.run(driver->getRoot()))
    return {-1, "", "codegen failed", false};

  // Link imported modules into main module.
  auto importedModules = cg.takeImportedModules();
  auto mainModule = cg.takeModule();
  for (auto &impMod : importedModules) {
    if (llvm::Linker::linkModules(*mainModule, std::move(impMod)))
      return {-1, "", "link failed", false};
  }

  // Capture stdout/stderr via temp files (pipe-free to avoid hang-on-crash).
  auto [savedOut, outPath] = redirectFdToTempFile(STDOUT_FILENO);
  auto [savedErr, errPath] = redirectFdToTempFile(STDERR_FILENO);

  auto resultOrErr = jit::runModule(std::move(mainModule), std::move(llvmCtx));
  fflush(stdout);
  fflush(stderr);

  restoreFd(STDOUT_FILENO, savedOut);
  restoreFd(STDERR_FILENO, savedErr);

  std::string outStr = drainAndRemoveTempFile(outPath);
  std::string errStr = drainAndRemoveTempFile(errPath);

  if (!resultOrErr) {
    llvm::consumeError(resultOrErr.takeError());
    return {-1, outStr, errStr, false};
  }
  return {*resultOrErr, outStr, errStr, true};
}

/// Like compileAndRun but forwards explicit program arguments to main.
inline RunResult compileAndRunWithArgs(const std::string &source,
                                       std::vector<std::string> args,
                                       const std::string &projectRoot = "") {
  auto [parseOk, driver] = parse(source);
  if (!parseOk)
    return {-1, "", "parse error", false};

  std::string diagStr;
  llvm::raw_string_ostream diagOS(diagStr);
  sema::DiagEngine diag(diagOS);
  diag.setSourceInfo(driver->getCurrentFile(), &driver->getSourceLines());
  sema::Sema sema(driver->getASTContext(), diag, projectRoot);
  auto semaCtx = sema.run(driver->getRoot());
  if (!semaCtx)
    return {-1, "", diagStr, false};

  auto llvmCtx = std::make_unique<llvm::LLVMContext>();
  codegen::CodeGen cg(semaCtx, *llvmCtx, "test", projectRoot);
  if (!cg.run(driver->getRoot()))
    return {-1, "", "codegen failed", false};

  auto importedModules = cg.takeImportedModules();
  auto mainModule = cg.takeModule();
  for (auto &impMod : importedModules)
    if (llvm::Linker::linkModules(*mainModule, std::move(impMod)))
      return {-1, "", "link failed", false};

  auto [savedOut, outPath] = redirectFdToTempFile(STDOUT_FILENO);
  auto [savedErr, errPath] = redirectFdToTempFile(STDERR_FILENO);

  auto resultOrErr =
      jit::runModule(std::move(mainModule), std::move(llvmCtx), std::move(args));
  fflush(stdout);
  fflush(stderr);

  restoreFd(STDOUT_FILENO, savedOut);
  restoreFd(STDERR_FILENO, savedErr);

  std::string outStr = drainAndRemoveTempFile(outPath);
  std::string errStr = drainAndRemoveTempFile(errPath);

  if (!resultOrErr) {
    llvm::consumeError(resultOrErr.takeError());
    return {-1, outStr, errStr, false};
  }
  return {*resultOrErr, outStr, errStr, true};
}

/// Compile and run from a .pkn file on disk.
inline RunResult compileAndRunFile(const std::string &filePath) {
  std::string projectRoot =
      std::filesystem::path(filePath).parent_path().string();

  parser::ParserDriver fileDriver;
  if (fileDriver.parseFile(filePath) != 0)
    return {-1, "", "parse error", false};

  std::string diagStr;
  llvm::raw_string_ostream diagOS(diagStr);
  sema::DiagEngine diag(diagOS);
  diag.setSourceInfo(fileDriver.getCurrentFile(), &fileDriver.getSourceLines());
  sema::Sema sema(fileDriver.getASTContext(), diag, projectRoot);
  auto semaCtx = sema.run(fileDriver.getRoot());
  if (!semaCtx)
    return {-1, "", diagStr, false};

  auto llvmCtx = std::make_unique<llvm::LLVMContext>();
  codegen::CodeGen cg(semaCtx, *llvmCtx, "test", projectRoot);
  if (!cg.run(fileDriver.getRoot()))
    return {-1, "", "codegen failed", false};

  auto importedModules = cg.takeImportedModules();
  auto mainModule = cg.takeModule();
  for (auto &impMod : importedModules) {
    if (llvm::Linker::linkModules(*mainModule, std::move(impMod)))
      return {-1, "", "link failed", false};
  }

  int pipefd[2];
  if (pipe(pipefd) != 0)
    return {-1, "", "pipe() failed", false};
  int savedStdout = dup(STDOUT_FILENO);
  int savedStderr = dup(STDERR_FILENO);
  dup2(pipefd[1], STDOUT_FILENO);
  int errPipe[2];
  pipe(errPipe);
  dup2(errPipe[1], STDERR_FILENO);

  auto resultOrErr = jit::runModule(std::move(mainModule), std::move(llvmCtx));
  fflush(stdout);
  fflush(stderr);

  dup2(savedStdout, STDOUT_FILENO);
  dup2(savedStderr, STDERR_FILENO);
  close(savedStdout);
  close(savedStderr);
  close(pipefd[1]);
  close(errPipe[1]);

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
