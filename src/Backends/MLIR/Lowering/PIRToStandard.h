// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The shared lowering of the mlir backend (docs/design/mlir-backend.md): the
// `pir` dialect into func, scf and arith, plus the pir ops no upstream
// dialect lowers to both LLVM and EmitC.  Both targets start from its
// output.

#pragma once

#include <mlir/IR/BuiltinOps.h>
#include <mlir/Pass/Pass.h>
#include <mlir/Support/LLVM.h>

#include <memory>
#include <string>

namespace paykan::mlir_backend {

/// The attribute the import puts on the program's main module: its `@main`
/// keeps the name `main`, the program's entry point.
inline constexpr const char *kMainModuleAttr = "pir.main";

/// The attribute the lowering puts on what the runtime defines and Runtime.h
/// declares (a runtime or C library function, a runtime global): the C
/// path declares none of it itself and calls such a function through
/// Runtime.h's prototype.  A native function or another module's symbol
/// has none: it is declared from its signature.
inline constexpr const char *kRuntimeAttr = "pir.runtime";

/// The attribute the import puts on a shared function or class (an
/// instantiation of a template, pir::Function::Shared): it links under a
/// name of its own, whichever module holds the copy a program keeps.
inline constexpr const char *kSharedAttr = "pir.shared";

/// The link name of the symbol @p name of PIR module @p module, the same in
/// every module of the program: `pk_<module>_S<name>`, both parts escaped
/// so the result is a C identifier and no two (module, name) pairs share
/// one (letters and digits stay, `_` becomes `__`, any other byte `_` and
/// its two hex digits).  A shared symbol's has no module: `pk__S<name>`
/// (an escaped module never starts `_S`).
std::string linkName(llvm::StringRef module, llvm::StringRef name);

/// The vtable symbol of class @p name of PIR module @p module:
/// `pkvt_<module>_S<class>`, escaped as linkName() (a shared class's has no
/// module).
std::string vtableName(llvm::StringRef module, llvm::StringRef name);

/// Lower @p module, one imported PIR module (Import/PIRImport.h), in place.
mlir::LogicalResult lowerToStandard(mlir::ModuleOp module);

/// The lowering as a pass over a program: every module nested in the
/// operated-on module that holds pir functions (or that module itself).
std::unique_ptr<mlir::Pass> createPIRToStandardPass();

/// Load the dialects the lowering produces (func, scf, arith, pir).
void loadStandardDialects(mlir::MLIRContext &ctx);

} // namespace paykan::mlir_backend
