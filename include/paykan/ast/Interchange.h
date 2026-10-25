// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The AST interchange format: the parsed program as text, the way a
// frontend plugin hands it to paykan (docs/plugins/ast-format.md is the
// specification).
//
// An S-expression form of exactly the AST a frontend builds (before Sema),
// source locations included:
//
//   (paykan-ast 0.2.0
//     (unit @1:1-3:2
//       (fn @1:1-3:2 "main" (type-params) (params) (named-type "int")
//         (block @1:19-3:2
//           (return @2:3-2:12 (int @2:10-2:11 0))))))
//
// write() prints a translation unit; read() parses the text back into an
// ASTContext, checking it as it goes, and builds exactly the nodes the
// recursive-descent frontend builds for the same program, so that
// read(write(unit)) is the same AST (tests/AST/InterchangeTests.cpp checks
// that over the whole samples corpus).

#pragma once

#include "AST.h"
#include "ASTContext.h"
#include <Version.h>

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

namespace paykan::ast::interchange {

/// The format version write() produces and read() accepts: the release's,
/// like every format (kVersionNumber), written after `paykan-ast` as the
/// dotted version (`0.2.0`).
inline constexpr uint32_t kFormatVersion = kVersionNumber;

/// The deepest list nesting read() accepts and write() writes: 4 x 512 + 2048
/// + 16.  Every AST within the frontends' limits fits: each of their 512
/// levels of nesting (frontend::kMaxNesting) is at most four lists deep (a
/// block, then `(decl (var ... (lambda ...)))` around the next block), each
/// of 2048 links of a chain (frontend::kMaxChain) is one, and the
/// declarations around them are a few.  It bounds the reader's recursion
/// over the text; read() then holds the AST to the frontends' limits
/// themselves, counted as they count them, and so bounds what an AST from a
/// plugin can make Sema recurse through.
inline constexpr unsigned kMaxDepth = 4112;

/// Write @p unit in the interchange format.  Returns false, with the reason
/// in @p error, for a node no frontend produces (a type Sema resolved, say):
/// only a parsed, unchecked AST can be written; and for a tree nested deeper
/// than kMaxDepth lists, which read() would not accept.
bool write(const TranslationUnit &unit, std::ostream &os, std::string &error);

/// Where and why read() failed.
struct ReadError {
  /// 1-based position in the text (0 when unknown).
  size_t Line = 0;
  size_t Column = 0;
  std::string Message;

  /// "<line>:<col>: <message>".
  std::string str() const;
};

/// Parse @p text into @p ctx and return its translation unit, or null with
/// @p error set.  The text must be one `(paykan-ast <version> (unit ...))`
/// form of a supported version, with nothing but whitespace and comments
/// after it.
TranslationUnit *read(std::string_view text, ASTContext &ctx, ReadError &error);

} // namespace paykan::ast::interchange
