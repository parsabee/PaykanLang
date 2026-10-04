// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The frontend interface: source text -> AST + diagnostics.
//
// A frontend is a lexer and parser for the Paykan language.  Everything after
// the AST (Sema, lowering, the backends) is shared, so frontends differ only
// in how they parse.  Every frontend must produce exactly the same AST for
// the same input; docs/grammar.md is the grammar they implement.
//
// Frontends are plugins (see paykan/Registry.h): a frontend library
// implements Frontend and registers a factory with PAYKAN_REGISTER_FRONTEND.
// The driver selects one with --frontend=<name>; parser::ParserDriver
// (ParserDriver.h) is the facade the rest of the compiler uses.
//
// This header is standard C++ only and must stay that way: the core never
// includes a plugin header, and plugins never depend on each other.

#pragma once

#include "AST.h"
#include "ASTContext.h"
#include "DiagEngine.h"
#include "paykan/Registry.h"

#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>

namespace paykan::frontend {

/// Deepest nesting of blocks, parentheses, brackets, type applications,
/// prefix operators and conditional expressions a frontend accepts
/// (docs/grammar.md section 9).  Every frontend rejects deeper input with
/// `nesting too deep (more than kMaxNesting levels)`, so that pathological
/// input can overflow neither a recursive-descent parser's native stack nor
/// the recursive passes after parsing (Sema, the AST printer).
inline constexpr unsigned kMaxNesting = 512;

/// Per-parse options.  A frontend ignores the ones it does not support.
struct Options {
  /// Debug traces of the parser / scanner (--trace-parser,
  /// --trace-scanner), when the frontend has them.
  bool TraceParsing = false;
  bool TraceScanning = false;
};

/// Result of parsing one source file.
struct ParseResult {
  /// The translation unit, owned by the ASTContext the parse was given.
  /// Null when the frontend could not produce one; may be non-null with
  /// errors when the frontend recovered from them.
  ast::TranslationUnit *Root = nullptr;
  /// Number of syntax errors reported.  The parse succeeded iff this is 0
  /// and Root is non-null.
  unsigned ErrorCount = 0;
};

class Frontend {
public:
  virtual ~Frontend() = default;

  /// The name the frontend is registered under ("recursive-descent", ...).
  virtual std::string_view name() const = 0;

  /// Parse @p source, the full text of @p filename, into @p ctx.
  ///
  /// @p filename is used for locations and diagnostics only; the frontend
  /// does not read the file.  Every syntax error is reported through @p diag
  /// (already carrying the file's name and lines, so diagnostics come out in
  /// the clang-style snippet format) and counted in the result.  Nothing may
  /// escape this call: a frontend built with exceptions catches them all and
  /// turns them into diagnostics.
  virtual ParseResult parse(std::string_view filename, std::string_view source,
                            ast::ASTContext &ctx, sema::DiagEngine &diag,
                            const Options &opts) = 0;

  /// Write the token stream of @p source to @p os, one token per line
  /// (--dump-tokens).  Returns false when the frontend does not support it;
  /// the default does not.
  virtual bool dumpTokens(std::string_view filename, std::string_view source,
                          std::ostream &os) {
    (void)filename;
    (void)source;
    (void)os;
    return false;
  }
};

using Registry = plugin::Registry<Frontend>;

/// The frontend used when none is named: PAYKAN_DEFAULT_FRONTEND from the
/// build configuration.
std::string_view defaultFrontend();

} // namespace paykan::frontend

/// Registers a frontend plugin.  @p ID is an identifier unique within the
/// plugin library, @p NAME the name users select it by, @p FACTORY a
/// `std::unique_ptr<paykan::frontend::Frontend> (*)()`.
#define PAYKAN_REGISTER_FRONTEND(ID, NAME, FACTORY)                            \
  static const ::paykan::plugin::Registration<::paykan::frontend::Frontend>    \
      paykanFrontendRegistration_##ID(NAME, FACTORY)
