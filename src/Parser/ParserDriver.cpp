// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "ParserDriverImpl.h"
#include <fstream>

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
  PImpl->scanBegin();
  int result = PImpl->parse(*this);
  PImpl->scanEnd();
  return result;
}

ast::TranslationUnit *ParserDriver::getRoot() { return PImpl->Root; }

ast::ASTContext &ParserDriver::getASTContext() { return PImpl->Ctx; }

unsigned ParserDriver::getErrorCount() const { return PImpl->ErrorCount; }

const std::string &ParserDriver::getCurrentFile() const { return PImpl->CurFile; }

const std::vector<std::string> &ParserDriver::getSourceLines() const {
  return PImpl->SourceLines;
}

// -- ParserDriver::Impl ------------------------------------------------------

int ParserDriver::Impl::parse(ParserDriver &drv) {
  yy::parser parser(drv);
  parser.set_debug_level(TraceParsing);
  return parser.parse();
}

} // namespace paykan::parser
