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

/// Build the ImportDecl for `import [::]a::b::name [as alias];`.  The path is
/// split at its last "::" into the base path ("a::b", empty when there is no
/// separator) and the module name ("name").
inline ImportDecl *makeSingleImport(ASTContext &C, SourceLocation loc,
                                    const std::string &path,
                                    const std::string &alias, bool isSystem) {
  auto sep = path.rfind("::");
  std::string base = (sep == std::string::npos) ? "" : path.substr(0, sep);
  std::string name = (sep == std::string::npos) ? path : path.substr(sep + 2);
  return C.make<ImportDecl>(
      loc, C.intern(base), isSystem,
      std::vector<ImportDecl::Module>{{&C.intern(name), &C.intern(alias)}});
}

/// Build the ImportDecl for `import [::]base::{a, b as c};` (base may be
/// empty for `import ::{...};`).  Each (name, alias) pair becomes one module.
inline ImportDecl *
makeImportList(ASTContext &C, SourceLocation loc, const std::string &base,
               const std::vector<std::pair<std::string, std::string>> &mods,
               bool isSystem) {
  std::vector<ImportDecl::Module> ms;
  ms.reserve(mods.size());
  for (const auto &[name, alias] : mods)
    ms.push_back({&C.intern(name), &C.intern(alias)});
  return C.make<ImportDecl>(loc, C.intern(base), isSystem, std::move(ms));
}

} // namespace paykan::parser
