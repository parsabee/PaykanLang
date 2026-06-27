// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Public interface for the parser driver

#pragma once

#include "AST.h"
#include "ASTContext.h"
#include "DiagEngine.h"
#include <memory>
#include <string>
#include <vector>

namespace paykan::parser {

class ParserDriver {
public:
  explicit ParserDriver(bool traceParsing = false, bool traceScanning = false);
  ~ParserDriver();

  // Non-copyable, non-movable (owns the AST arena).
  ParserDriver(const ParserDriver &) = delete;
  ParserDriver &operator=(const ParserDriver &) = delete;

  /// Parse a source file.  Returns 0 on success, non-zero on failure.
  int parseFile(const std::string &filename);

  /// Root of the parsed AST (owned by the internal ASTContext).
  ast::TranslationUnit *getRoot();

  /// The ASTContext that owns all AST nodes.
  ast::ASTContext &getASTContext();

  /// Number of syntax errors encountered during the last parse.
  unsigned getErrorCount() const;

  /// Absolute/relative filename of the last parsed source.
  const std::string &getCurrentFile() const;

  /// Source split by lines for diagnostics/snippets.
  const std::vector<std::string> &getSourceLines() const;

  /// Attach a DiagEngine so parser syntax errors are routed through it.
  /// Must be called before parseFile(); nullptr disables routing.
  void setDiagEngine(sema::DiagEngine *diag);

private:
  struct Impl;
  std::unique_ptr<Impl> PImpl;

  // The impl() accessor needs access to PImpl.
  friend Impl &impl(ParserDriver &drv);
};

} // namespace paykan::parser
