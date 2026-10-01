// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parses one source file with a registered frontend.

#pragma once

#include "AST.h"
#include "ASTContext.h"
#include "DiagEngine.h"
#include "paykan/Frontend.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace paykan::parser {

/// The facade through which the driver, Sema (for imports) and the tests
/// parse a file.  It owns the AST arena, reads the file, wires the
/// diagnostics, and hands the text to the selected frontend plugin
/// (paykan/Frontend.h).
class ParserDriver {
public:
  /// @p frontendName selects a registered frontend; "" means the build's
  /// default (frontend::defaultFrontend()).  An unknown name is reported when
  /// parseFile() is called.
  explicit ParserDriver(std::string_view frontendName = "",
                        frontend::Options opts = {});
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
  /// Must be called before parseFile(); nullptr falls back to stderr.
  void setDiagEngine(sema::DiagEngine *diag);

  /// The name of the frontend this driver parses with.
  const std::string &getFrontendName() const;

  /// Write the token stream of @p filename to @p os (--dump-tokens).  Returns
  /// 0 on success, non-zero when the file cannot be read or the frontend has
  /// no token dump (reported through the diagnostics like parseFile).
  int dumpTokens(const std::string &filename, std::ostream &os);

private:
  struct Impl;
  std::unique_ptr<Impl> PImpl;

  struct Prepared;
  bool prepare(const std::string &filename, sema::DiagEngine &diag,
               Prepared &out);
};

} // namespace paykan::parser
