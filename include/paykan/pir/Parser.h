// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// PIR text parser (docs/pir.md §10): the inverse of the printer, so backend
// tests can be written in PIR text without a frontend or the lowering.

#pragma once

#include "paykan/pir/PIR.h"

#include <optional>
#include <string>
#include <string_view>

namespace paykan::pir {

struct ParseError {
  unsigned Line = 0; // 1-based; 0 when the error has no position
  unsigned Column = 0;
  std::string Message;

  /// "line:column: message".
  std::string str() const;
};

/// Parse a program: one or more modules (each starting with `module "..."`)
/// in order.  The result is syntactically well formed but not verified; run
/// the verifier (paykan/pir/Verifier.h) before handing it to a backend.
std::optional<Program> parseProgram(std::string_view text, ParseError &err);

/// Parse exactly one module.
std::optional<Module> parseModule(std::string_view text, ParseError &err);

} // namespace paykan::pir
