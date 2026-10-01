// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Public entry points of the handwritten frontend (src/Frontends/Handwritten).

#pragma once

#include "AST.h"
#include "ASTContext.h"
#include "DiagEngine.h"

#include <iosfwd>
#include <string_view>

namespace paykan::frontend::handwritten {

struct ParseOutput {
  /// The parsed unit, owned by the ASTContext.  After errors it is partial
  /// (and must not be handed to Sema); it is never null.
  ast::TranslationUnit *Root = nullptr;
  /// Number of lexical and syntax errors reported.
  unsigned ErrorCount = 0;
};

/// Parse @p source (the full text of one file) into @p ctx.  Diagnostics go
/// through @p diags when given, otherwise to stderr.  Reentrant: no state
/// outlives the call except the nodes created in @p ctx.
ParseOutput parseSource(ast::ASTContext &ctx, std::string_view source,
                        sema::DiagEngine *diags);

/// Print the token stream of @p source, one token per line, as
/// `line:col-line:col KIND text` (the --dump-tokens format).  Lexical errors
/// are reported through @p diags (or stderr); returns their number.
unsigned dumpTokens(std::string_view source, std::ostream &os,
                    sema::DiagEngine *diags);

} // namespace paykan::frontend::handwritten
