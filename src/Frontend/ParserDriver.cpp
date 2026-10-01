// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "ParserDriver.h"

#include <cerrno>
#include <cstring> // strerror
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

#ifndef PAYKAN_DEFAULT_FRONTEND
#error "PAYKAN_DEFAULT_FRONTEND must be defined by the build"
#endif

namespace paykan {

std::string_view frontend::defaultFrontend() { return PAYKAN_DEFAULT_FRONTEND; }

namespace parser {

struct ParserDriver::Impl {
  std::string FrontendName;
  frontend::Options Opts;

  /// Arena that owns all AST nodes created during parsing.
  ast::ASTContext Ctx;
  /// Root of the parsed AST (owned by Ctx).
  ast::TranslationUnit *Root = nullptr;
  /// The name of the file being parsed.
  std::string CurFile;
  /// Source split by lines for downstream diagnostics.
  std::vector<std::string> SourceLines;
  /// Number of syntax errors encountered during parsing.
  unsigned ErrorCount = 0;
  /// Diagnostic engine attached by the caller, or nullptr.
  sema::DiagEngine *Diags = nullptr;
};

ParserDriver::ParserDriver(std::string_view frontendName,
                           frontend::Options opts)
    : PImpl(std::make_unique<Impl>()) {
  PImpl->FrontendName = frontendName.empty()
                            ? std::string(frontend::defaultFrontend())
                            : std::string(frontendName);
  PImpl->Opts = opts;
}

ParserDriver::~ParserDriver() = default;

/// What parseFile and dumpTokens share: the frontend instance and the file's
/// text.
struct ParserDriver::Prepared {
  std::unique_ptr<frontend::Frontend> FE;
  std::string Source;
};

int ParserDriver::parseFile(const std::string &filename) {
  Impl &im = *PImpl;
  im.Root = nullptr;

  // Diagnostics go to the attached engine when there is one (the same channel
  // as every other compiler diagnostic) and to stderr otherwise.  Either way
  // they carry this file's name and lines, so syntax errors print in the
  // clang-style snippet format.
  sema::DiagEngine fallback(std::cerr);
  sema::DiagEngine &diag = im.Diags ? *im.Diags : fallback;
  if (!im.Diags)
    fallback.setSourceInfo(filename, &im.SourceLines);

  Prepared p;
  if (!prepare(filename, diag, p))
    return 1;

  frontend::ParseResult r =
      p.FE->parse(filename, p.Source, im.Ctx, diag, im.Opts);
  im.Root = r.Root;
  im.ErrorCount = r.ErrorCount;
  return (im.Root == nullptr || im.ErrorCount > 0) ? 1 : 0;
}

int ParserDriver::dumpTokens(const std::string &filename, std::ostream &os) {
  Impl &im = *PImpl;
  sema::DiagEngine fallback(std::cerr);
  sema::DiagEngine &diag = im.Diags ? *im.Diags : fallback;
  if (!im.Diags)
    fallback.setSourceInfo(filename, &im.SourceLines);

  Prepared p;
  if (!prepare(filename, diag, p))
    return 1;
  if (!p.FE->dumpTokens(filename, p.Source, os)) {
    ++im.ErrorCount;
    diag.error(ast::SourceLocation(), "frontend '" + im.FrontendName +
                                          "' does not support --dump-tokens");
    return 1;
  }
  return 0;
}

/// Create the frontend and read @p filename into @p out.  On failure reports
/// the reason through @p diag (counting it as an error) and returns false.
bool ParserDriver::prepare(const std::string &filename, sema::DiagEngine &diag,
                           Prepared &out) {
  Impl &im = *PImpl;
  im.CurFile = filename;
  im.ErrorCount = 0;
  im.SourceLines.clear();

  auto fail = [&](const std::string &msg) {
    ++im.ErrorCount;
    diag.error(ast::SourceLocation(), msg);
    return false;
  };

  out.FE = frontend::Registry::get().create(im.FrontendName);
  if (!out.FE)
    return fail("unknown frontend '" + im.FrontendName + "'");

  // Read the whole file; the frontend parses the text.  A file that cannot
  // be opened is a diagnostic with no location (there is no source to point
  // at) and a failed parse; library code never exit()s the process.
  errno = 0;
  std::ifstream in(filename, std::ios::binary);
  if (!in)
    return fail("cannot open '" + filename + "': " + std::strerror(errno));
  out.Source.assign((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());

  std::istringstream lines(out.Source);
  std::string line;
  while (std::getline(lines, line))
    im.SourceLines.push_back(line);
  return true;
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

const std::string &ParserDriver::getFrontendName() const {
  return PImpl->FrontendName;
}

} // namespace parser
} // namespace paykan
