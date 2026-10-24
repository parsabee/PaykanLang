// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// AST -> PIR lowering: the single home of Paykan's ownership semantics
// (ARC placement, scope cleanup, `match` dispatch, vtables).  Every
// backend consumes the pir::Program this produces; none re-derives ownership.
//
// Standard C++ only.

#pragma once

#include "Sema.h"
#include "paykan/pir/PIR.h"

#include <ostream>
#include <string>

namespace paykan::modules {
class ModuleResolver;
}

namespace paykan::lowering {

/// Lower the analysed translation unit @p tu (whose SemaContext is @p ctx)
/// and every module it transitively imports into @p out.  The main module,
/// read from @p mainFile, is first.  Every module is named by its canonical
/// module name (ModuleName.h): the main module by @p mainFile's stem, an
/// import by its module path -- never by a file path, so the program is the
/// same wherever its sources live.  Diagnostics for internal
/// errors (states Sema should have ruled out) are written to @p errs; returns
/// false when any occurred.  @p mainName names the main module (its file
/// stem, module_name::mainModuleName, unless the driver says otherwise).
/// A module the resolver (@p resolver, the driver's, or else the one Sema
/// made: SemaContext::Resolver) loaded from a `.pkm` file contributes its
/// decoded CODE instead of a lowered AST.
///
/// Every module lowers its own copy of each instantiation of another
/// module's template it uses (`Box<int>` of `lib::gen`), so that what a
/// module's PIR says does not depend on the program it is in, and its CODE
/// can be cached (docs/design/pkm.md §4.8).  @p copies says what happens to
/// those copies.
enum class Copies {
  /// One per program (shareInstantiations): the program to run.
  Shared,
  /// Each module's own: the modules to write to `.pkm` files.  The caller
  /// shares them with shareInstantiations before running the program.
  PerModule,
};
/// What the main module's code may know of the program it is in (@p world;
/// every import's code knows only what any module's does).
enum class World {
  /// Only what any module's code knows: the main module may be imported
  /// (--emit-pkm), and an importer may derive classes from its classes.
  Open,
  /// The main module is the program's entry (run, build, --emit-pir,
  /// --emit-c), which no module imports (that would be an import cycle):
  /// every class derived from one of its classes is its own, so a virtual
  /// call on one runs one of the overrides it lowers, and an object of one
  /// that dies holds only the fields of its class or of one of those
  /// (docs/pir.md §7).
  Closed,
};
bool lowerProgram(const sema::SemaContext &ctx, ast::TranslationUnit *tu,
                  const std::string &mainName, const std::string &projectRoot,
                  pir::Program &out, std::ostream &errs,
                  modules::ModuleResolver *resolver = nullptr,
                  Copies copies = Copies::Shared, World world = World::Open);

/// Keep one definition of each template instantiation in @p program: the
/// first module (imports first, the main module last) that defines a class
/// `Box<int>`, a function of one (`Box<int>.get`, a struct's `Pair<int>`)
/// or a function instantiation (`$lib::gen::f<int>`) keeps it; every other
/// definition becomes an extern of that module, and every extern of it
/// names that module.  Type names are unique across a program (Sema), so
/// two definitions of one such name are copies of one instantiation.
void shareInstantiations(pir::Program &program);

} // namespace paykan::lowering
