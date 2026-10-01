// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Test utilities that compile and run Paykan programs through a backend.
//
// The backend is selected per test process by the PAYKAN_TEST_BACKEND
// environment variable ("llvm" or "c"; the build's default backend when
// unset), so the same CodeGen suite runs once per enabled backend (see
// tests/CMakeLists.txt) and the backends act as differential oracles for
// each other: every test asserts the same stdout, exit code and zero live
// heap blocks whichever backend ran it.
//
//   * llvm: CodeGen + ORC JIT in-process (the legacy path).
//   * c:    lowering -> PIR -> C -> $CC, run as a child process.  The child
//           runs with the tracking allocator when the test process has it
//           enabled, and reports its live-block count back through its heap
//           dump, which LeakGuard then checks.

#pragma once

#include "TestUtils.h"

#include "paykan/backends/c/CBackend.h"
#include "paykan/lowering/Lowering.h"

#if PAYKAN_TEST_HAVE_LLVM
#include "JIT.h"
#include "PIRToLLVM.h"
#include "paykan/pir/Verifier.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/Error.h>
#endif

#include <gtest/gtest.h>

#include <csignal>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include "Runtime.h"
}

namespace paykan::test {

/// Compile and execute source. Returns {exitCode, stdout, stderr,
/// compiledOk}.
struct RunResult {
  int ExitCode;
  std::string StdOut;
  std::string StdErr;
  bool CompileOk;
};

/// The backend the compile-and-run helpers use.
inline const std::string &testBackend() {
  static const std::string backend = [] {
    const char *env = std::getenv("PAYKAN_TEST_BACKEND");
    if (env && env[0])
      return std::string(env);
#if PAYKAN_TEST_HAVE_LLVM
    return std::string("llvm");
#else
    return std::string("c");
#endif
  }();
  return backend;
}

namespace detail {

/// Live heap blocks reported by the last program run (see
/// liveBlocksAfterRun).
inline int64_t &lastRunLiveBlocks() {
  static int64_t blocks = 0;
  return blocks;
}

/// Parse + Sema.  On failure returns a RunResult describing it.
struct Analysed {
  std::unique_ptr<parser::ParserDriver> Driver;
  sema::SemaContext Ctx;
  std::string Path;
  bool Ok = false;
  RunResult Failure{-1, "", "", false};
};

inline Analysed analyse(const std::string &source,
                        const std::string &projectRoot) {
  Analysed a;
  auto path = writeTempFile(source);
  a.Driver = std::make_unique<parser::ParserDriver>();
  int rc = a.Driver->parseFile(path);
  std::filesystem::remove(path);
  a.Path = path;
  if (rc != 0) {
    a.Failure = {-1, "", "parse error", false};
    return a;
  }
  std::ostringstream diagOS;
  sema::DiagEngine diag(diagOS);
  diag.setSourceInfo(a.Driver->getCurrentFile(), &a.Driver->getSourceLines());
  sema::Sema sema(a.Driver->getASTContext(), diag, projectRoot);
  a.Ctx = sema.run(a.Driver->getRoot());
  if (!a.Ctx) {
    a.Failure = {-1, "", diagOS.str(), false};
    return a;
  }
  a.Ok = true;
  return a;
}

inline Analysed analyseFile(const std::string &filePath,
                            const std::string &projectRoot) {
  Analysed a;
  a.Driver = std::make_unique<parser::ParserDriver>();
  a.Path = filePath;
  if (a.Driver->parseFile(filePath) != 0) {
    a.Failure = {-1, "", "parse error", false};
    return a;
  }
  std::ostringstream diagOS;
  sema::DiagEngine diag(diagOS);
  diag.setSourceInfo(a.Driver->getCurrentFile(), &a.Driver->getSourceLines());
  sema::Sema sema(a.Driver->getASTContext(), diag, projectRoot);
  a.Ctx = sema.run(a.Driver->getRoot());
  if (!a.Ctx) {
    a.Failure = {-1, "", diagOS.str(), false};
    return a;
  }
  a.Ok = true;
  return a;
}

#if PAYKAN_TEST_HAVE_LLVM
/// The LLVM backend: lowering -> PIR -> LLVM IR (through the bitcode cache,
/// like the driver) -> JIT, in process.
inline RunResult runLLVM(Analysed &a, const std::vector<std::string> *args,
                         const std::string &projectRoot) {
  pir::Program program;
  std::ostringstream errs;
  if (!lowering::lowerProgram(a.Ctx, a.Driver->getRoot(), a.Path, projectRoot,
                              program, errs))
    return {-1, "", "lowering failed: " + errs.str(), false};
  if (auto verrs = pir::verify(program); !verrs.empty())
    return {-1, "", "verifier: " + pir::formatErrors(verrs), false};
  auto llvmCtx = std::make_unique<llvm::LLVMContext>();
  auto module = backend::llvm_backend::compileProgram(program, *llvmCtx, "test",
                                                      projectRoot);
  if (!module)
    return {-1, "", module.status().message(), false};

  auto [savedOut, outPath] = redirectFdToTempFile(STDOUT_FILENO);
  auto [savedErr, errPath] = redirectFdToTempFile(STDERR_FILENO);
  auto resultOrErr =
      args ? jit::runModule(std::move(*module), std::move(llvmCtx), *args)
           : jit::runModule(std::move(*module), std::move(llvmCtx));
  fflush(stdout);
  fflush(stderr);
  restoreFd(STDOUT_FILENO, savedOut);
  restoreFd(STDERR_FILENO, savedErr);
  std::string outStr = drainAndRemoveTempFile(outPath);
  std::string errStr = drainAndRemoveTempFile(errPath);
  lastRunLiveBlocks() = Paykan_heap_live_blocks();
  if (!resultOrErr) {
    llvm::consumeError(resultOrErr.takeError());
    return {-1, outStr, errStr, false};
  }
  return {*resultOrErr, outStr, errStr, true};
}
#endif

/// Remove the runtime's heap-statistics block from a child's stderr and
/// return the live-block count it reported (-1 when there is none).
inline int64_t extractHeapReport(std::string &err) {
  const std::string header = "paykan heap stats:\n";
  size_t pos = err.find(header);
  if (pos == std::string::npos)
    return -1;
  size_t end = pos + header.size();
  int64_t live = -1;
  // Indented lines belong to the block ("  key : value", "  ** LEAK ... **").
  while (end < err.size() && err.compare(end, 2, "  ") == 0) {
    size_t eol = err.find('\n', end);
    std::string line = err.substr(
        end, eol == std::string::npos ? std::string::npos : eol - end);
    const std::string key = "  live blocks       : ";
    if (line.rfind(key, 0) == 0)
      live = std::strtoll(line.c_str() + key.size(), nullptr, 10);
    end = eol == std::string::npos ? err.size() : eol + 1;
  }
  err.erase(pos, end - pos);
  return live;
}

inline RunResult runC(Analysed &a, const std::vector<std::string> *args,
                      const std::string &projectRoot) {
  pir::Program program;
  std::ostringstream errs;
  if (!lowering::lowerProgram(a.Ctx, a.Driver->getRoot(), a.Path, projectRoot,
                              program, errs))
    return {-1, "", "lowering failed: " + errs.str(), false};

  bool track = Paykan_heap_tracking_enabled() != 0;
  std::vector<std::string> argv;
  if (args)
    argv = *args;
  else
    argv.push_back(a.Path);

  auto [savedOut, outPath] = redirectFdToTempFile(STDOUT_FILENO);
  auto [savedErr, errPath] = redirectFdToTempFile(STDERR_FILENO);
  backend_c::Toolchain tc;
  int rc = backend_c::buildAndRun(program, argv, track, tc, errs);
  fflush(stdout);
  fflush(stderr);
  restoreFd(STDOUT_FILENO, savedOut);
  restoreFd(STDERR_FILENO, savedErr);
  std::string outStr = drainAndRemoveTempFile(outPath);
  std::string errStr = drainAndRemoveTempFile(errPath);

  if (rc < 0)
    return {-1, outStr, errStr + errs.str(), false};
  if (rc > 128) {
    // The program died from a signal (a runtime panic aborts).  The JIT
    // path dies with the test process; do the same so death tests see it.
    fputs(outStr.c_str(), stdout);
    fputs(errStr.c_str(), stderr);
    fflush(stdout);
    fflush(stderr);
    raise(rc - 128);
  }
  int64_t live = extractHeapReport(errStr);
  lastRunLiveBlocks() = track ? (live < 0 ? -1 : live) : 0;
  return {rc, outStr, errStr, true};
}

inline RunResult runAnalysed(Analysed &a, const std::vector<std::string> *args,
                             const std::string &projectRoot) {
  if (!a.Ok)
    return a.Failure;
#if PAYKAN_TEST_HAVE_LLVM
  if (testBackend() == "llvm")
    return runLLVM(a, args, projectRoot);
#endif
  if (testBackend() == "c")
    return runC(a, args, projectRoot);
  return {-1, "", "unknown test backend '" + testBackend() + "'", false};
}

} // namespace detail

/// The number of live heap blocks after the last compileAndRun* call: the
/// in-process count for the JIT, the child's reported count for the C
/// backend (-1 if it reported none).
inline int64_t liveBlocksAfterRun() { return detail::lastRunLiveBlocks(); }

/// RAII guard: enable the tracking allocator before a run and check that the
/// program left zero live blocks after it.
struct LeakGuard {
  LeakGuard() {
    // Flush harness output still buffered in stdout, so it is not captured as
    // the program's output once the run redirects the descriptor.
    fflush(stdout);
    Paykan_heap_set_tracking(1);
    Paykan_heap_reset();
    detail::lastRunLiveBlocks() = 0;
  }
  ~LeakGuard() { Paykan_heap_set_tracking(0); }
  void expectNoLeaks(const char *label = "") const {
    int64_t live = liveBlocksAfterRun();
    EXPECT_EQ(live, 0) << "heap leak in: " << label << " (" << live
                       << " live blocks)";
  }
};

inline RunResult compileAndRun(const std::string &source,
                               const std::string &projectRoot = "") {
  auto a = detail::analyse(source, projectRoot);
  return detail::runAnalysed(a, nullptr, projectRoot);
}

/// Like compileAndRun but forwards explicit program arguments to main.
inline RunResult compileAndRunWithArgs(const std::string &source,
                                       std::vector<std::string> args,
                                       const std::string &projectRoot = "") {
  auto a = detail::analyse(source, projectRoot);
  return detail::runAnalysed(a, &args, projectRoot);
}

/// Compile and run from a .pkn file on disk.
inline RunResult compileAndRunFile(const std::string &filePath) {
  std::string projectRoot =
      std::filesystem::path(filePath).parent_path().string();
  auto a = detail::analyseFile(filePath, projectRoot);
  return detail::runAnalysed(a, nullptr, projectRoot);
}

} // namespace paykan::test
