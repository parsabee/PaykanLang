// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Test utilities that compile and run Paykan programs through the mlir
// backend: lowering -> PIR -> MLIR -> LLVM IR (through the bitcode cache,
// like the driver) -> ORC JIT, in process.  Every test asserts stdout, the
// exit code and zero live heap blocks (LeakGuard).  The C the backend emits
// for the same programs is checked by CStrictC11 (scripts/c_strict.py).

#pragma once

#include "TestUtils.h"

#include "JIT.h"
#include "Pipeline.h"
#include "paykan/lowering/Lowering.h"
#include "paykan/modules/ModuleResolver.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Verifier.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/Error.h>

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
                        const std::string &projectRoot,
                        const std::string &frontendName = "") {
  Analysed a;
  auto path = writeTempFile(source);
  a.Driver = std::make_unique<parser::ParserDriver>(frontendName);
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

/// JIT-run an LLVM module in process, capturing its stdout and stderr.  A JIT
/// failure (an unresolved symbol, a module the JIT rejects) returns
/// CompileOk == false with the error's message appended to StdErr, so a
/// failing test prints the reason instead of an anonymous -1.
inline RunResult runJITModule(std::unique_ptr<llvm::Module> module,
                              std::unique_ptr<llvm::LLVMContext> llvmCtx,
                              const std::vector<std::string> *args) {
  auto [savedOut, outPath] = redirectFdToTempFile(STDOUT_FILENO);
  auto [savedErr, errPath] = redirectFdToTempFile(STDERR_FILENO);
  auto resultOrErr =
      args ? jit::runModule(std::move(module), std::move(llvmCtx), *args)
           : jit::runModule(std::move(module), std::move(llvmCtx));
  fflush(stdout);
  fflush(stderr);
  restoreFd(STDOUT_FILENO, savedOut);
  restoreFd(STDERR_FILENO, savedErr);
  std::string outStr = drainAndRemoveTempFile(outPath);
  std::string errStr = drainAndRemoveTempFile(errPath);
  lastRunLiveBlocks() = Paykan_heap_live_blocks();
  if (!resultOrErr) {
    errStr += "JIT: " + llvm::toString(resultOrErr.takeError());
    return {-1, outStr, errStr, false};
  }
  return {*resultOrErr, outStr, errStr, true};
}

/// The mlir backend: PIR -> MLIR -> LLVM IR (through the bitcode cache,
/// like the driver) -> JIT, in process, with the compiled code of the
/// modules @p resolver loaded from a `.pkm` that has some.
inline RunResult
runProgramMLIR(const pir::Program &program,
               const std::vector<std::string> *args,
               const std::string &projectRoot,
               const modules::ModuleResolver *resolver = nullptr) {
  if (auto verrs = pir::verify(program); !verrs.empty())
    return {-1, "", "verifier: " + pir::formatErrors(verrs), false};
  auto llvmCtx = std::make_unique<llvm::LLVMContext>();
  auto module = mlir_backend::compileProgram(
      program, *llvmCtx, "test", projectRoot,
      [&](size_t i) -> std::span<const uint8_t> {
        const pkm::Object *o =
            resolver ? resolver->object(program.Modules[i].Name) : nullptr;
        return o ? std::span<const uint8_t>(o->Bytes)
                 : std::span<const uint8_t>();
      });
  if (!module)
    return {-1, "", module.status().message(), false};
  return runJITModule(std::move(*module), std::move(llvmCtx), args);
}

inline RunResult runMLIR(Analysed &a, const std::vector<std::string> *args,
                         const std::string &projectRoot) {
  pir::Program program;
  std::ostringstream errs;
  if (!lowering::lowerProgram(a.Ctx, a.Driver->getRoot(), a.Path, projectRoot,
                              program, errs))
    return {-1, "", "lowering failed: " + errs.str(), false};
  return runProgramMLIR(program, args, projectRoot, a.Ctx.Resolver.get());
}

inline RunResult runAnalysed(Analysed &a, const std::vector<std::string> *args,
                             const std::string &projectRoot) {
  if (!a.Ok)
    return a.Failure;
  return runMLIR(a, args, projectRoot);
}

} // namespace detail

/// The number of live heap blocks after the last compileAndRun* call.
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

/// Like compileAndRun, parsing with the frontend named @p frontendName (one
/// of frontend::Registry::get().names()) instead of the default one.
inline RunResult compileAndRunWithFrontend(const std::string &source,
                                           const std::string &frontendName) {
  auto a = detail::analyse(source, "", frontendName);
  return detail::runAnalysed(a, nullptr, "");
}

/// Parse the hand-written PIR program @p text and run it, verified as the
/// driver verifies a lowered program.
inline RunResult compileAndRunPIR(const std::string &text) {
  pir::ParseError err;
  auto program = pir::parseProgram(text, err);
  if (!program)
    return {-1, "", "PIR parse error: " + err.str(), false};
  return detail::runProgramMLIR(*program, nullptr, "");
}

/// Compile and run from a .pkn file on disk.
inline RunResult compileAndRunFile(const std::string &filePath) {
  std::string projectRoot =
      std::filesystem::path(filePath).parent_path().string();
  auto a = detail::analyseFile(filePath, projectRoot);
  return detail::runAnalysed(a, nullptr, projectRoot);
}

} // namespace paykan::test
