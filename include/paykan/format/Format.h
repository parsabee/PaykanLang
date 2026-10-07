// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The Paykan source formatter (`paykan fmt`).
//
// Lays a file out in two columns: code on the left, and on the right either
// a trailing comment or the body of a short function.  Code is never
// re-wrapped or re-spaced; only the right column is managed:
//
//   * a comment after code starts at the right column (or two spaces after
//     code that already reaches it); a comment-only line continuing such a
//     comment (starting in the same column) moves with it;
//   * a function or method whose body has no nested block and no comment
//     collapses onto one line, `fn sig(...)` on the left and `{ body }` at
//     the right column, when the line fits the width limit; a comment that
//     trailed its first or last line moves to its own line above it;
//   * trailing whitespace is removed and the file ends in one newline.
//
// The token stream (kinds and spellings) and the comments, in order, are
// unchanged; format() checks that before returning.

#pragma once

#include <string>
#include <string_view>

namespace paykan::format {

struct Style {
  /// 1-based column where the right column (comments, short bodies) starts.
  unsigned RightColumn = 46;
  /// A function collapses only if its one-line form is at most this wide.
  unsigned MaxWidth = 100;
};

struct Result {
  bool Ok = false;
  /// The formatted text, when Ok.
  std::string Text;
  /// Why the file could not be formatted (`line:col: message`), when not Ok.
  std::string Error;
};

/// Format @p source, the full text of one `.pkn` file.  Fails on a lexical
/// error; syntax errors are not diagnosed (the formatter works on tokens).
Result format(std::string_view source, const Style &style = {});

} // namespace paykan::format
