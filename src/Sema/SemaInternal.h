// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Private helpers shared across Sema translation units.
// Not part of the public API — do not include from outside src/Sema/.

#pragma once

#include "AST.h"

#include <cassert>
#include <vector>

/// Marks a point that must never be reached (an exhaustive switch over an
/// enum has run out of cases).  Asserts in debug builds; in release builds
/// the compiler may assume the point is unreachable.
#define PAYKAN_UNREACHABLE(msg)                                                \
  do {                                                                         \
    assert(false && (msg));                                                    \
    __builtin_unreachable();                                                   \
  } while (0)

namespace paykan {
namespace sema {
namespace detail {

/// Returns true if `stmt` always terminates with a return on every path.
bool stmtAlwaysReturns(ast::Stmt *s);

/// Returns true if every path through the list of statements ends in a return.
bool blockAlwaysReturns(const std::vector<ast::Stmt *> &stmts);

/// Returns true if the arms of `ms` cover every value of its subject: a
/// wildcard arm; every variant of an enum subject; both `True` and `False`
/// for a bool subject; or, for an optional `T?` subject, a `None` arm together
/// with a type arm naming `T` itself.  Requires the subject's resolved type
/// and the arms' resolved types (i.e. the match has passed Sema), so each
/// variant arm names a distinct, valid variant and counting them suffices.
bool matchIsExhaustive(ast::MatchStmt *ms);

} // namespace detail
} // namespace sema
} // namespace paykan
