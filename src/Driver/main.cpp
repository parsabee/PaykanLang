// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "ASTPrinter.h"
#include "CodeGen.h"
#include "DiagEngine.h"
#include "JIT.h"
#include "Options.h"
#include "ParserDriver.h"
#include "Sema.h"
#include "Version.h"

#include "Runtime.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using paykan::driver::Options;

// Prints the PaykanLang version and the LLVM version the compiler is built
// against, then leaves the caller to exit.
static void printVersion() {
  std::cout << "PaykanLang " << paykan::kVersion << "\n"
            << "LLVM " << LLVM_VERSION_STRING << "\n";
}

int main(int argc, char *argv[]) {
  auto parsed = paykan::driver::parseCommandLine(argc, argv);
  const Options &opts = parsed.Opts;
  if (opts.ShowVersion) {
    printVersion();
    return EXIT_SUCCESS;
  }
  if (opts.ShowHelp) {
    paykan::driver::printUsage(std::cout, argv[0]);
    return EXIT_SUCCESS;
  }
  if (!parsed.Error.empty()) {
    std::cerr << "paykan: " << parsed.Error << ". Try: '" << argv[0]
              << " --help'\n";
    return EXIT_FAILURE;
  }

  // One DiagEngine shared by every pass, wired into the parser up front so
  // syntax errors come out in the same rich source-located format as sema
  // errors (file:line:col + snippet + caret) instead of the yacc-style
  // fallback.  SourceLines lives inside the driver and is filled by
  // parseFile before the parser runs, so handing its address over now is
  // safe -- the vector itself never moves.
  paykan::sema::DiagEngine diag(std::cerr);
  paykan::parser::ParserDriver driver(opts.TraceParsing, opts.TraceScanning);
  diag.setSourceInfo(opts.InputFilename, &driver.getSourceLines());
  driver.setDiagEngine(&diag);
  int result = driver.parseFile(opts.InputFilename);

  if (result != 0) {
    std::cerr << "parsing failed with " << driver.getErrorCount()
              << " error(s)\n";
    return EXIT_FAILURE;
  }

  auto *root = driver.getRoot();

  if (opts.DumpAST) {
    paykan::ast::ASTPrinter printer(std::cout);
    printer.visit(root);
    return EXIT_SUCCESS;
  }

  // -- Semantic analysis ----------------------------------------------------
  std::string projectRoot =
      std::filesystem::path(opts.InputFilename).parent_path().string();
  // Sema reuses the DiagEngine constructed above (already carrying the
  // source info for the parsed file), so error counts accumulate across
  // passes and all diagnostics share one output stream.
  paykan::sema::Sema sema(driver.getASTContext(), diag, projectRoot);
  auto semaCtx = sema.run(root);

  if (!semaCtx)
    return EXIT_FAILURE;

  if (opts.CheckOnly)
    return EXIT_SUCCESS;

  // -- Code generation ------------------------------------------------------
  auto llvmCtx = std::make_unique<llvm::LLVMContext>();
  paykan::codegen::CodeGen cg(semaCtx, *llvmCtx, opts.InputFilename,
                              projectRoot);
  if (!cg.run(root)) {
    std::cerr << "code generation failed (module verification error)\n";
    return EXIT_FAILURE;
  }
  cg.optimize(opts.OptLevel);

#ifndef NDEBUG
  {
    std::string errMsg;
    if (!cg.verify(errMsg)) {
      std::cerr << "LLVM IR verification failed:\n" << errMsg << "\n";
      return EXIT_FAILURE;
    }
  }
#endif

  if (opts.EmitLLVM) {
    cg.getModule().print(llvm::outs(), nullptr);
    return EXIT_SUCCESS;
  }

  // -- JIT execution ------------------------------------------------------
  // Link imported modules into the main module.
  if (!cg.linkImportedModules()) {
    std::cerr << "failed to link imported module\n";
    return EXIT_FAILURE;
  }
  auto mainModule = cg.takeModule();
  // Select the tracking allocator before any program allocation happens, so
  // that every block is allocated and freed by the same back-end.
  if (opts.TrackHeap) {
    Paykan_heap_set_tracking(1);
    Paykan_heap_reset();
  }

  // Build the args vector: args[0] = script path, args[1..] = program args.
  std::vector<std::string> progArgs;
  progArgs.push_back(opts.InputFilename);
  progArgs.insert(progArgs.end(), opts.ProgramArgs.begin(),
                  opts.ProgramArgs.end());

  auto resultOrErr = paykan::jit::runModule(std::move(mainModule),
                                            std::move(llvmCtx), progArgs);

  if (opts.TrackHeap)
    Paykan_heap_dump();

  if (!resultOrErr) {
    std::cerr << "JIT error: " << llvm::toString(resultOrErr.takeError())
              << "\n";
    return EXIT_FAILURE;
  }
  return *resultOrErr;
}
