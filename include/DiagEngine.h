// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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
// An error limit (setErrorLimit) bounds what is printed: once that many
// errors are out, the next one is replaced by a note saying so, and nothing
// more is printed, though every diagnostic is still counted and recorded.
//
class DiagEngine {
  std::ostream &OS;
  std::string SourceName;
  const std::vector<std::string> *SourceLines = nullptr;
  unsigned ErrorCount = 0;
  std::vector<Diagnostic> Diagnostics;

  // The errors printed against the limit, shared by the engines of one
  // compilation (shareErrorLimit).
  struct ErrorLimitState {
    unsigned Max = 0;     // 0: no limit
    unsigned Printed = 0; // errors printed
    bool Reached = false; // the note is out: nothing more is printed
  };
  ErrorLimitState OwnLimit;
  ErrorLimitState *Limit = &OwnLimit;

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
  // allowed, and so is the same message at another location.  A group whose
  // error or warning has no location, or any group while the locations are
  // not exact (setExactLocations), is not checked: it cannot be told from
  // the same report about another construct.
  std::string CurrentGroup;
  std::unordered_set<std::string> SeenGroups;
  bool ExactLocations = true;
  // Whether the open group is checked: used by debug builds only.
  [[maybe_unused]] bool CheckGroup = true;
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
  const std::string &getSourceName() const { return SourceName; }
  const std::vector<std::string> *getSourceLines() const { return SourceLines; }

  // Whether every construct the diagnostics are about has a location of its
  // own, as the built-in frontend's AST does (the default).  A frontend
  // plugin's AST may carry coarse locations or none
  // (docs/plugins/ast-format.md), where two constructs share one: the
  // double-report guard is then off, since the same error about each is no
  // repeat.
  void setExactLocations(bool exact) { ExactLocations = exact; }

  // Print at most @p max errors (0, the default, for no limit); warnings
  // and notes do not count.
  void setErrorLimit(unsigned max) { Limit->Max = max; }
  // Count this engine's errors against @p other's limit, as one compilation
  // (an imported module's file reports through an engine of its own).
  void shareErrorLimit(DiagEngine &other) { Limit = other.Limit; }

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
