// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "ParserDriverImpl.h"
#include <cerrno>
#include <cstring> // strerror
#include <fstream>
#include <iostream>

namespace paykan::parser {

// -- ParserDriver (public PIMPL wrapper) -------------------------------------

ParserDriver::ParserDriver(bool TraceParsing, bool TraceScanning)
    : PImpl(std::make_unique<Impl>(TraceParsing, TraceScanning)) {}

ParserDriver::~ParserDriver() = default;

int ParserDriver::parseFile(const std::string &filename) {
  PImpl->CurFile = filename;
  PImpl->ErrorCount = 0;
  PImpl->SourceLines.clear();

  std::ifstream in(filename);
  std::string line;
  while (std::getline(in, line))
    PImpl->SourceLines.push_back(line);

  PImpl->Location.initialize(&PImpl->CurFile);
  if (!PImpl->scanBegin()) {
    // The input file could not be opened.  Report through the attached
    // DiagEngine when there is one (same channel as every other compiler
    // diagnostic) with an invalid location -- there is no source to point
    // at -- and fall back to stderr otherwise.  Either way the parse fails
    // cleanly; library code must never exit() the whole process.
    ++PImpl->ErrorCount;
    std::string msg = "cannot open '" + filename + "': " + std::strerror(errno);
    if (PImpl->Diags)
      PImpl->Diags->error(ast::SourceLocation(), msg);
    else
      std::cerr << "error: " << msg << "\n";
    return 1;
  }
  int result = PImpl->parse(*this);
  PImpl->scanEnd();
  return (result != 0 || PImpl->ErrorCount > 0) ? 1 : 0;
}

ast::TranslationUnit *ParserDriver::getRoot() { return PImpl->Root; }

ast::ASTContext &ParserDriver::getASTContext() { return PImpl->Ctx; }

unsigned ParserDriver::getErrorCount() const { return PImpl->ErrorCount; }

const std::string &ParserDriver::getCurrentFile() const {
  return PImpl->CurFile;
}

const std::vector<std::string> &ParserDriver::getSourceLines() const {
  return PImpl->SourceLines;
}

void ParserDriver::setDiagEngine(sema::DiagEngine *diag) {
  PImpl->Diags = diag;
}

// -- ParserDriver::Impl ------------------------------------------------------

int ParserDriver::Impl::parse(ParserDriver &drv) {
  yy::parser parser(drv);
  parser.set_debug_level(TraceParsing);
  return parser.parse();
}

} // namespace paykan::parser
