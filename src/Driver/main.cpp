// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "ParserDriver.h"
#include "ASTPrinter.h"

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdlib>

// ── Command-line options (LLVM cl style) ────────────────────────────────────

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

// ── Entry point ─────────────────────────────────────────────────────────────

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
  } else {
    llvm::outs() << "parse ok — " << root->getBody()->size()
                 << " top-level statement(s)\n";
  }
  return EXIT_SUCCESS;
}
