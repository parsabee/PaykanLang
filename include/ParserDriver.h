// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Public interface for the parser driver

#pragma once

#include "AST.h"
#include <memory>
#include <string>

namespace paykan::parser {

class ParserDriver {
public:
  explicit ParserDriver(bool traceParsing = false,
                        bool traceScanning = false);
  ~ParserDriver();

  // Non-copyable, non-movable (owns the AST arena).
  ParserDriver(const ParserDriver &) = delete;
  ParserDriver &operator=(const ParserDriver &) = delete;

  /// Parse a source file.  Returns 0 on success, non-zero on failure.
  int parseFile(const std::string &filename);

  /// Root of the parsed AST (owned by the internal ASTContext).
  ast::TranslationUnit *getRoot();

  /// Number of syntax errors encountered during the last parse.
  unsigned getErrorCount() const;

private:
  struct Impl;
  std::unique_ptr<Impl> PImpl;

  // The impl() accessor needs access to PImpl.
  friend Impl &impl(ParserDriver &drv);
};

} // namespace paykan::parser
