// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "ParserDriver.h"
#include "ASTPrinter.h"
#include "CodeGen.h"
#include "JIT.h"
#include "Sema.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdlib>

// -- Command-line options ---------------------------------------------------

static llvm::cl::opt<std::string>
    InputFilename(llvm::cl::Positional, llvm::cl::desc("<source-file>"),
                  llvm::cl::Required);

static llvm::cl::opt<bool>
    DumpAST("dump-ast", llvm::cl::desc("Print the AST in tree form"),
            llvm::cl::init(false));

static llvm::cl::opt<bool>
    TraceParsing("trace-parser",
                 llvm::cl::desc("Enable Bison parser debug traces"),
                 llvm::cl::init(false));

static llvm::cl::opt<bool>
    TraceScanning("trace-scanner",
                  llvm::cl::desc("Enable Flex scanner debug traces"),
                  llvm::cl::init(false));

static llvm::cl::opt<bool>
    EmitLLVM("emit-llvm",
             llvm::cl::desc("Emit LLVM IR to stdout"),
             llvm::cl::init(false));

static llvm::cl::opt<unsigned>
    OptLevel("O",
             llvm::cl::desc("Optimization level (0–3)"),
             llvm::cl::Prefix,
             llvm::cl::init(0));

// -- Entry point -------------------------------------------------------------

int main(int argc, char *argv[]) {
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
  paykan::sema::Sema sema(driver.getASTContext(), llvm::errs());
  sema.run(root);

  if (sema.hasErrors())
    return EXIT_FAILURE;

  // -- Code generation ------------------------------------------------------
  auto llvmCtx = std::make_unique<llvm::LLVMContext>();
  paykan::codegen::CodeGen cg(driver.getASTContext(), *llvmCtx,
                              InputFilename);
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

  if (EmitLLVM) {
    cg.getModule().print(llvm::outs(), nullptr);
    return EXIT_SUCCESS;
  }

  // -- JIT execution ------------------------------------------------------
  auto resultOrErr =
      paykan::jit::runModule(cg.takeModule(), std::move(llvmCtx));
  if (!resultOrErr) {
    llvm::errs() << "JIT error: " << resultOrErr.takeError() << "\n";
    return EXIT_FAILURE;
  }
  return *resultOrErr;
}
