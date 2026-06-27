// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "ASTPrinter.h"
#include "CodeGen.h"
#include "DiagEngine.h"
#include "JIT.h"
#include "ParserDriver.h"
#include "Sema.h"
#include "Version.h"

#include "Runtime.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdlib>
#include <filesystem>
#include <vector>

#include <llvm/Linker/Linker.h>

// -- Command-line options ---------------------------------------------------

static llvm::cl::opt<std::string> InputFilename(llvm::cl::Positional,
                                                llvm::cl::desc("<source-file>"),
                                                llvm::cl::Required);

static llvm::cl::opt<bool> DumpAST("dump-ast",
                                   llvm::cl::desc("Print the AST in tree form"),
                                   llvm::cl::init(false));

static llvm::cl::opt<bool>
    TraceParsing("trace-parser",
                 llvm::cl::desc("Enable Bison parser debug traces"),
                 llvm::cl::init(false));

static llvm::cl::opt<bool>
    TraceScanning("trace-scanner",
                  llvm::cl::desc("Enable Flex scanner debug traces"),
                  llvm::cl::init(false));

static llvm::cl::opt<bool> EmitLLVM("emit-llvm",
                                    llvm::cl::desc("Emit LLVM IR to stdout"),
                                    llvm::cl::init(false));

static llvm::cl::opt<bool> CheckOnly(
    "check-only",
    llvm::cl::desc("Run parsing and semantic analysis only (no codegen)"),
    llvm::cl::init(false));

static llvm::cl::opt<unsigned>
    OptLevel("O", llvm::cl::desc("Optimization level (0–3)"), llvm::cl::Prefix,
             llvm::cl::init(0));

static llvm::cl::opt<bool>
    TrackHeap("track-heap",
              llvm::cl::desc("Track runtime heap allocations and dump "
                             "statistics (incl. leaks) at exit"),
              llvm::cl::init(false));

// Arguments forwarded to the Paykan program (everything after the source file).
static llvm::cl::list<std::string>
    ProgramArgs(llvm::cl::ConsumeAfter, llvm::cl::desc("<program arguments>"));

// -- Entry point -------------------------------------------------------------

// Prints the PaykanLang version and the LLVM version the compiler is built
// against, then leaves the caller to exit.
static void printVersion() {
  llvm::outs() << "PaykanLang " << paykan::kVersion << "\n"
               << "LLVM " << LLVM_VERSION_STRING << "\n";
}

int main(int argc, char *argv[]) {
  // Handle --version / -v before LLVM's command-line parser runs, since the
  // required positional <source-file> would otherwise reject these flags.
  for (int i = 1; i < argc; ++i) {
    llvm::StringRef arg(argv[i]);
    if (arg == "--version" || arg == "-version" || arg == "-v") {
      printVersion();
      return EXIT_SUCCESS;
    }
  }

  llvm::cl::ParseCommandLineOptions(argc, argv, "Paykan language compiler\n");

  paykan::parser::ParserDriver driver(TraceParsing, TraceScanning);
  int result = driver.parseFile(InputFilename);

  if (result != 0) {
    llvm::errs() << "parsing failed with " << driver.getErrorCount()
                 << " error(s)\n";
    return EXIT_FAILURE;
  }

  auto *root = driver.getRoot();

  if (DumpAST) {
    paykan::ast::ASTPrinter printer(llvm::outs());
    printer.visit(root);
    return EXIT_SUCCESS;
  }

  // -- Semantic analysis ----------------------------------------------------
  std::string projectRoot =
      std::filesystem::path(InputFilename.getValue()).parent_path().string();
  paykan::sema::DiagEngine diag(llvm::errs());
  diag.setSourceInfo(driver.getCurrentFile(), &driver.getSourceLines());
  paykan::sema::Sema sema(driver.getASTContext(), diag, projectRoot);
  auto semaCtx = sema.run(root);

  if (!semaCtx)
    return EXIT_FAILURE;

  if (CheckOnly)
    return EXIT_SUCCESS;

  // -- Code generation ------------------------------------------------------
  auto llvmCtx = std::make_unique<llvm::LLVMContext>();
  paykan::codegen::CodeGen cg(semaCtx, *llvmCtx, InputFilename, projectRoot);
  if (!cg.run(root)) {
    llvm::errs() << "code generation failed (module verification error)\n";
    return EXIT_FAILURE;
  }
  // Map -O<n> to LLVM optimization level.
  static const llvm::OptimizationLevel levels[] = {
      llvm::OptimizationLevel::O0,
      llvm::OptimizationLevel::O1,
      llvm::OptimizationLevel::O2,
      llvm::OptimizationLevel::O3,
  };
  unsigned lvl = OptLevel < 4 ? OptLevel : 3;
  cg.optimize(levels[lvl]);

#ifndef NDEBUG
  {
    std::string errMsg;
    llvm::raw_string_ostream errStream(errMsg);
    if (llvm::verifyModule(cg.getModule(), &errStream)) {
      llvm::errs() << "LLVM IR verification failed:\n" << errMsg << "\n";
      return EXIT_FAILURE;
    }
  }
#endif

  if (EmitLLVM) {
    cg.getModule().print(llvm::outs(), nullptr);
    return EXIT_SUCCESS;
  }

  // -- JIT execution ------------------------------------------------------
  // Link imported modules into the main module.
  auto importedModules = cg.takeImportedModules();
  auto mainModule = cg.takeModule();
  for (auto &impMod : importedModules) {
    if (llvm::Linker::linkModules(*mainModule, std::move(impMod))) {
      llvm::errs() << "failed to link imported module\n";
      return EXIT_FAILURE;
    }
  }
  // Select the tracking allocator before any program allocation happens, so
  // that every block is allocated and freed by the same back-end.
  if (TrackHeap) {
    Paykan_heap_set_tracking(1);
    Paykan_heap_reset();
  }

  // Build the args vector: args[0] = script path, args[1..] = program args.
  std::vector<std::string> progArgs;
  progArgs.push_back(InputFilename.getValue());
  for (const auto &a : ProgramArgs)
    progArgs.push_back(a);

  auto resultOrErr = paykan::jit::runModule(
      std::move(mainModule), std::move(llvmCtx), std::move(progArgs));

  if (TrackHeap)
    Paykan_heap_dump();

  if (!resultOrErr) {
    llvm::errs() << "JIT error: " << resultOrErr.takeError() << "\n";
    return EXIT_FAILURE;
  }
  return *resultOrErr;
}
