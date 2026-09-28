// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser driver internals (Bison/Flex integration)

#pragma once

#include "AST.h"
#include "ASTContext.h"
#include "Parser.ypp.h"
#include "ParserDriver.h"
#include <vector>

// Flex needs this macro for our custom driver
#define YY_DECL yy::parser::symbol_type yylex(paykan::parser::ParserDriver &drv)
YY_DECL;

using namespace paykan::ast;

namespace paykan::parser {

/// The PIMPL body of ParserDriver.  Holds every field that depends on Bison or
/// Flex generated types, keeping them out of the public header.
struct ParserDriver::Impl {
  friend yy::parser;

  /// Arena that owns all AST nodes created during parsing.
  ASTContext Ctx;

  /// Root of the parsed AST (owned by Ctx).
  TranslationUnit *Root = nullptr;

  /// The token's location used by the scanner.
  yy::location Location;

  /// The name of the file being parsed.
  std::string CurFile;

  /// Source split by lines for downstream diagnostics.
  std::vector<std::string> SourceLines;

  /// Number of syntax errors encountered during parsing.
  unsigned ErrorCount = 0;

  /// Optional diagnostic engine for routing parser errors.
  sema::DiagEngine *Diags = nullptr;

  /// Whether to generate parser debug traces.
  bool TraceParsing;
  /// Whether to generate scanner debug traces.
  bool TraceScanning;

  explicit Impl(bool TraceParsing, bool TraceScanning)
      : TraceParsing(TraceParsing), TraceScanning(TraceScanning) {}

  /// Handling the scanner.  scanBegin returns false when the input file
  /// cannot be opened (errno is left set); the caller reports the failure.
  bool scanBegin();
  void scanEnd();
  int parse(ParserDriver &drv);
};

/// Returns the Impl of a ParserDriver.  Only for use by parser/lexer actions.
inline ParserDriver::Impl &impl(ParserDriver &drv) { return *drv.PImpl; }

} // namespace paykan::parser
