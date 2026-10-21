// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// A program's LLVM modules, as the mlir backend's LLVM path translates them
// (or any translation of one PIR module to LLVM IR): the per-project
// bitcode cache of imported modules, linking, and -O<n>.

#pragma once

#include "paykan/Status.h"
#include "paykan/pir/PIR.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace paykan::llvm_codegen {

/// A backend's translation of module @p index of the program into LLVM IR.
using TranslateModule =
    std::function<StatusOr<std::unique_ptr<llvm::Module>>(size_t index)>;
/// What a backend translates module @p index from, as text: the cache key
/// covers it.
using ModuleText = std::function<std::string(size_t index)>;
/// The bitcode module @p index's code was compiled to ahead of time (its
/// `.pkm`'s OBJ section), or nothing (and nothing for every module when
/// unset): its PIR declares that code.
using CompiledCode = std::function<std::span<const uint8_t>(size_t index)>;

/// Where a backend's translations are cached: each entry is
/// <projectRoot>/.paykan_cache/<module name as a path><Extension>, and its
/// key names Backend, so two backends never serve each other's entries.
struct ModuleCache {
  std::string Backend;
  std::string Extension;
};

/// Translate every module of @p program with @p translate into one linked
/// module named @p moduleName, serving imported modules from the bitcode
/// cache when what @p text gives for them is unchanged (and refreshing it
/// otherwise).  The main module is never cached.  Then link the part of
/// each module's @p compiled code the program uses: only what the program
/// (or code linked before it) references, importers before the modules
/// they import.
StatusOr<std::unique_ptr<llvm::Module>>
compileProgram(const pir::Program &program, llvm::LLVMContext &ctx,
               const std::string &moduleName, const std::string &projectRoot,
               const ModuleCache &cache, const ModuleText &text,
               const TranslateModule &translate, const CompiledCode &compiled);

/// Link @p parts (main module first) into one module named @p name.
StatusOr<std::unique_ptr<llvm::Module>>
linkModules(const pir::Program &program,
            std::vector<std::unique_ptr<llvm::Module>> parts,
            const std::string &name);

/// Run LLVM's default pipeline for -O@p level (none for -O0).
void optimizeModule(llvm::Module &module, unsigned level);

} // namespace paykan::llvm_codegen
