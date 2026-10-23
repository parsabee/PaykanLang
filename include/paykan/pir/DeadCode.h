// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The dead-code pass (docs/pir.md §12): what no root of a program reaches,
// erased before anything is lowered.  A binary's root is its entry point; a
// module written to a `.pkm` has the roots of what its importers may use.

#pragma once

#include "paykan/pir/PIR.h"
#include "paykan/pir/Pass.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace paykan::pir {

/// What a program keeps whatever else reaches it.
struct Roots {
  /// Whether function @p f of module @p m is a root.
  std::function<bool(const Module &m, const Function &f)> Functions;
  /// Whether class @p c of module @p m is a root (none when unset).
  std::function<bool(const Module &m, const Class &c)> Classes;

  /// The `@main` of @p program's main module: what a binary runs.
  static Roots entryPoint(const Program &program);
};

/// Every module-level name the body of @p f refers to, each once, in the
/// order first referred to: the callees of `call` and `fn.addr`, the `@sym`
/// operands (functions and globals), and the classes `new`, `vcall`,
/// `vtable.addr` and the field instructions name.  A compiled
/// declaration's Uses are these names of its compiled body.
std::vector<std::string> references(const Function &f);

/// Erase from @p program every function, class and global that no root
/// reaches, the modules themselves staying.  A root reaches:
///
/// * from a function definition, the names its body refers to
///   (references()); from a compiled declaration, its Uses;
/// * from an extern naming another module, that module's definition (in
///   its PIR or compiled);
/// * from a class, its superclass and its vtable's functions; from an
///   extern class, the class in the module it names.
///
/// A name is looked up among the module's functions and globals and among
/// its classes: a class and its constructor share theirs.  A class no
/// instruction names is not reached through the functions that use its
/// objects (a `vcall` of another module's class carries only the slot's
/// signature): whoever made the object reached it.
void dropDeadCode(Program &program, const Roots &roots);

/// dropDeadCode as a pass ("dead-code").
std::unique_ptr<Pass> createDeadCodePass(Roots roots);

} // namespace paykan::pir
