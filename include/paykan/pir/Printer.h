// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// PIR text printer (docs/pir.md §10).  The output round-trips through the
// parser (paykan/pir/Parser.h).

#pragma once

#include "paykan/pir/PIR.h"

#include <iosfwd>
#include <string>

namespace paykan::pir {

/// Print one module.
void print(const Module &m, std::ostream &os);

/// Print a whole program: its modules in order, separated by a blank line.
void print(const Program &p, std::ostream &os);

std::string toString(const Module &m);
std::string toString(const Program &p);

/// Print a single operand / type the way the module printer does
/// (`%name.3`, `42`, `1.5`, `true`, `'a'`, `null`, `@sym`; `i64`, `box`...).
void print(const Operand &op, std::ostream &os);
void print(const Signature &sig, std::ostream &os);

} // namespace paykan::pir
