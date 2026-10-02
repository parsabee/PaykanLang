// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// PIR -> LLVM IR translation (the `llvm` backend's code generator).

#pragma once

#include "paykan/Status.h"
#include "paykan/pir/PIR.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <string>

namespace paykan::backend::llvm_backend {

/// Translate one PIR module of @p program into an LLVM module.
///
/// Symbols of the main module (index 0) keep their names (`main` stays
/// `main`); symbols defined by another module are prefixed with that
/// module's name (`"lib::add"`), and `extern fn ... module "lib"` references
/// resolve to those, so the modules of a program link together without
/// clashes.  Runtime symbols (`extern fn` without a module, `extern obj`,
/// `extern vtable`) keep their C names and are resolved by the JIT.
StatusOr<std::unique_ptr<llvm::Module>>
translateModule(const pir::Program &program, size_t index,
                llvm::LLVMContext &ctx);

/// Translate a whole program into one linked LLVM module (no cache).
StatusOr<std::unique_ptr<llvm::Module>>
translateProgram(const pir::Program &program, llvm::LLVMContext &ctx,
                 const std::string &moduleName);

/// Translate a whole program into one linked LLVM module, serving imported
/// modules from the bitcode cache under <projectRoot>/.paykan_cache when
/// their PIR is unchanged (and refreshing the cache otherwise).  The main
/// module is never cached.
StatusOr<std::unique_ptr<llvm::Module>>
compileProgram(const pir::Program &program, llvm::LLVMContext &ctx,
               const std::string &moduleName, const std::string &projectRoot);

/// Run LLVM's default optimisation pipeline at -O<level> (0 = none; levels
/// above 3 are clamped to 3).
void optimizeModule(llvm::Module &module, unsigned level);

} // namespace paykan::backend::llvm_backend
