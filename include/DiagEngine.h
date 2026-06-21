// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Central diagnostic engine shared by all compiler passes.

#pragma once

#include "AST.h"

#include <string>
#include <vector>

// Forward declaration — callers that need raw_ostream methods must include
// <llvm/Support/raw_ostream.h> themselves.
namespace llvm { class raw_ostream; }

namespace paykan {
namespace sema {

// A single diagnostic emitted during compilation.
struct Diagnostic {
  enum Severity { Error, Warning, Note };

  Severity Level;
  ast::SourceLocation Loc;
  std::string Message;
};

// Centralised diagnostic emitter used by the Parser, Sema, and any other
// compiler pass that needs to report errors or warnings.
//
// All passes share one DiagEngine instance created in the driver, so that
// error counts accumulate across passes and the output stream is controlled
// from a single point.
//
// Usage:
//   DiagEngine diag(llvm::errs());
//   diag.setSourceInfo("foo.pkn", &lines);
//   diag.error(loc, "undeclared variable 'x'");
//   if (diag.hasErrors()) { ... }
//
class DiagEngine {
  llvm::raw_ostream &OS;
  std::string SourceName;
  const std::vector<std::string> *SourceLines = nullptr;
  unsigned ErrorCount = 0;
  std::vector<Diagnostic> Diagnostics;

  void emit(Diagnostic::Severity level, ast::SourceLocation loc,
            const std::string &msg);

public:
  explicit DiagEngine(llvm::raw_ostream &os) : OS(os) {}

  // Non-copyable, non-movable (owns the raw_ostream reference).
  DiagEngine(const DiagEngine &) = delete;
  DiagEngine &operator=(const DiagEngine &) = delete;

  // Provide source context for diagnostic messages.
  // Call once per file (or re-call when switching files).
  void setSourceInfo(std::string name, const std::vector<std::string> *lines) {
    SourceName = std::move(name);
    SourceLines = lines;
  }

  void error(ast::SourceLocation loc, const std::string &msg);
  void warning(ast::SourceLocation loc, const std::string &msg);
  void note(ast::SourceLocation loc, const std::string &msg);

  unsigned getErrorCount() const { return ErrorCount; }
  bool hasErrors() const { return ErrorCount > 0; }
  const std::vector<Diagnostic> &getDiagnostics() const { return Diagnostics; }

  llvm::raw_ostream &getOS() { return OS; }
};

} // namespace sema
} // namespace paykan
