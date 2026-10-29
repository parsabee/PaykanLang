// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The grammar's keywords and identifiers (docs/grammar.md section 1): what
// every frontend lexes as a keyword (paykan/Frontend.h), what the AST
// interchange reader holds a plugin's names to, and what each part of a
// module name is (ModuleName.h), so that an import can name every module.
//
// This header is standard C++ only and must stay that way, as
// paykan/Frontend.h, which includes it.

#pragma once

#include <string_view>

namespace paykan::frontend {

/// The grammar's keywords (docs/grammar.md section 1), which no identifier
/// spells: an identifier, IDENT, is a letter or `_` followed by letters,
/// digits and `_`, and is none of these.  The AST interchange reader holds a
/// plugin's names to it (isIdentifier).
inline constexpr std::string_view kKeywords[] = {
    "import", "as",       "class", "struct", "enum", "match", "mov",   "fn",
    "native", "return",   "if",    "then",   "else", "elif",  "while", "for",
    "break",  "continue", "True",  "False",  "None", "_",     "view",  "inout",
    "let",    "weak",     "impl",  "static", "trait"};

/// Whether @p s is one of the grammar's keywords (kKeywords).
inline bool isKeyword(std::string_view s) {
  for (std::string_view k : kKeywords)
    if (s == k)
      return true;
  return false;
}

/// Whether @p s is an identifier, the grammar's IDENT (kKeywords).
inline bool isIdentifier(std::string_view s) {
  auto letter = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };
  if (s.empty() || !letter(s[0]))
    return false;
  for (char c : s)
    if (!letter(c) && !(c >= '0' && c <= '9'))
      return false;
  return !isKeyword(s);
}

} // namespace paykan::frontend
