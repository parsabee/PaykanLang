// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Central diagnostic engine shared by all compiler passes.

#pragma once

#include "AST.h"

#include <iosfwd>
#include <string>
#include <unordered_set>
#include <vector>

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
//   DiagEngine diag(std::cerr);
//   diag.setSourceInfo("foo.pkn", &lines);
//   diag.error(loc, "undeclared variable 'x'");
//   if (diag.hasErrors()) { ... }
//
class DiagEngine {
  std::ostream &OS;
  std::string SourceName;
  const std::vector<std::string> *SourceLines = nullptr;
  unsigned ErrorCount = 0;
  std::vector<Diagnostic> Diagnostics;

  void emit(Diagnostic::Severity level, ast::SourceLocation loc,
            const std::string &msg);

  // Double-report guard (#112), active in debug builds only (the members
  // exist in every build so the class layout does not depend on NDEBUG).  A
  // diagnostic group is an error or warning plus the notes that follow it;
  // reporting a group identical to an earlier one (same file, location,
  // message and notes) is a compiler bug -- a construct resolved and reported
  // twice -- so it aborts instead of being silently deduplicated.  The same
  // error repeated with different notes (e.g. once per template instantiation,
  // each with its own "in instantiation of" note) is a different group and is
  // allowed, and so is the same message at another location.
  std::string CurrentGroup;
  std::unordered_set<std::string> SeenGroups;
  void closeGroup();

public:
  explicit DiagEngine(std::ostream &os) : OS(os) {}
  ~DiagEngine();

  // Non-copyable, non-movable (owns the stream reference).
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

  std::ostream &getOS() { return OS; }
};

} // namespace sema
} // namespace paykan
