// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// What a module's `.pkm` keeps of its code (docs/design/pkm.md §5.13).  A
// module is lowered with a copy of every instantiation it uses; before its
// file is written, what neither it nor an importer can reach is dropped,
// and what is left is split: the shared copies of instantiations stay PIR
// (CODE), which a program shares and prunes, and everything else is
// declared in CODE and compiled by the backend (OBJ).

#pragma once

#include "paykan/pir/PIR.h"
#include "paykan/pkm/Interface.h"

#include <set>
#include <string>

namespace paykan::modules {

/// The instantiations an importer of a module may get a value of without
/// naming its type: those the module's interface @p iface spells (its
/// functions' signatures; its own classes' and structs' fields, methods and
/// superclasses, private members aside), the ones those types' own fields
/// and methods spell, and the type arguments of each.  An importer that
/// gets such a value calls the module's copy of its type, so the module
/// keeps those copies whole.
std::set<std::string> exposedTypes(const pkm::Interface &iface);

/// Erase from @p module (as the lowering made it) what neither it nor an
/// importer can reach (pir::dropDeadCode).  Its own functions and classes
/// stay, and its copies of the instantiations @p iface exposes; copies,
/// closures and local instantiations nothing of that reaches go, and so do
/// the declarations and constants nothing uses.
void trimModuleCode(pir::Module &module, const pkm::Interface &iface);

struct ModuleCode {
  /// CODE: the module's shared copies of instantiations, a compiled
  /// declaration of each of its other functions (with what its compiled
  /// body uses) and classes an importer or a copy may name, and the
  /// declarations and constants those use.
  pir::Module Code;
  /// What a backend compiles into OBJ: the module's other definitions,
  /// with its shared copies declared.
  pir::Module Compiled;
};

/// @p module (trimmed) as its CODE and the part a backend compiles.
ModuleCode splitModuleCode(pir::Module module);

/// Whether @p code (a module's CODE) defines everything the other modules
/// of @p program declare from it: a program whose modules were lowered
/// together may use a module's copy its CODE no longer has.
bool providesFor(const pir::Module &code, const pir::Program &program);

} // namespace paykan::modules
