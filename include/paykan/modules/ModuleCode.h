// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// What a module's `.pkm` keeps of its code (docs/design/pkm.md §5.13).  A
// module is lowered with a copy of every instantiation it uses; before its
// file is written, what neither it nor an importer can reach is dropped,
// and what is left is split: the shared copies of instantiations stay PIR
// (CODE), which a program shares and prunes, and everything else is
// declared in CODE and compiled by the backend (OBJ).

#pragma once

#include "paykan/Status.h"
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

/// Whether the interface @p iface describes its types consistently
/// (docs/design/pkm.md §8.4 step 4): no name is a class and a struct or an
/// enum, and each class's superclass is a class it describes, whose vtable
/// slots begin the class's.  The first inconsistency when not.
Status checkInterface(const pkm::Interface &iface);

/// Whether @p code, a module's CODE, defines (in its PIR or compiled) what
/// the module's interface @p iface says it defines (docs/design/pkm.md §8.4
/// step 7), and what an importer finds in it by name: each class of the
/// module, with its superclass, its own fields in their order, the slots of
/// its vtable, its initializer, its `impl` methods and its constructor,
/// which takes what its initializer takes; each copy of an instantiation it
/// holds (a `shared class`), for what it has; each method of each of its
/// structs; and each function, a native one's too.  The fields, functions,
/// methods and slots have the PIR types the interface's types give them
/// (every type an interface names has one: a scalar, a reference's `box`, an
/// optional's, a value held in memory passed by its address, a slice as its
/// address and length).  And each slot of each class @p code defines names
/// the function an importer's subclass inherits for it.  The first
/// difference when not.
Status checkCodeMatchesInterface(const pir::Module &code,
                                 const pkm::Interface &iface);

} // namespace paykan::modules
