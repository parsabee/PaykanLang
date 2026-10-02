// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Test utilities that compile and run Paykan programs through CodeGen + JIT.

#pragma once

#include "TestUtils.h"

#include "CodeGen.h"
#include "JIT.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/Error.h>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace paykan::test {

/// Compile and JIT-execute source. Returns {exitCode, stdout, stderr,
/// compiledOk}.
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
  std::ostringstream diagOS;
  sema::DiagEngine diag(diagOS);
  diag.setSourceInfo(driver->getCurrentFile(), &driver->getSourceLines());
  sema::Sema sema(driver->getASTContext(), diag, projectRoot);
  auto semaCtx = sema.run(driver->getRoot());
  if (!semaCtx)
    return {-1, "", diagOS.str(), false};

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

  std::ostringstream diagOS;
  sema::DiagEngine diag(diagOS);
  diag.setSourceInfo(driver->getCurrentFile(), &driver->getSourceLines());
  sema::Sema sema(driver->getASTContext(), diag, projectRoot);
  auto semaCtx = sema.run(driver->getRoot());
  if (!semaCtx)
    return {-1, "", diagOS.str(), false};

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

  auto resultOrErr = jit::runModule(std::move(mainModule), std::move(llvmCtx),
                                    std::move(args));
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

  std::ostringstream diagOS;
  sema::DiagEngine diag(diagOS);
  diag.setSourceInfo(fileDriver.getCurrentFile(), &fileDriver.getSourceLines());
  sema::Sema sema(fileDriver.getASTContext(), diag, projectRoot);
  auto semaCtx = sema.run(fileDriver.getRoot());
  if (!semaCtx)
    return {-1, "", diagOS.str(), false};

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
