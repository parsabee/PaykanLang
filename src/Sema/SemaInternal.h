// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Private helpers shared across Sema translation units.
// Not part of the public API — do not include from outside src/Sema/.

#pragma once

#include "AST.h"

#include <llvm/ADT/ArrayRef.h>

namespace paykan {
namespace sema {
namespace detail {

/// Returns true if `stmt` always terminates with a return on every path.
bool stmtAlwaysReturns(ast::Stmt *s);

/// Returns true if every path through the list of statements ends in a return.
bool blockAlwaysReturns(llvm::ArrayRef<ast::Stmt *> stmts);

} // namespace detail
} // namespace sema
} // namespace paykan
