// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// A program's LLVM modules, as the mlir backend's LLVM path translates them
// (or any translation of one PIR module to LLVM IR): the per-project
// bitcode cache of imported modules, linking, and -O<n>.

#pragma once

#include "paykan/Status.h"
#include "paykan/pir/PIR.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <cstdint>
#include <functional>
#include <iosfwd>
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
/// The code module @p index was compiled to ahead of time (its `.pkm`'s OBJ
/// section) as an LLVM module in the program's context, or null (and null
/// for every module when unset): its PIR declares that code.  Or why it
/// cannot be read.
using CompiledCode =
    std::function<StatusOr<std::unique_ptr<llvm::Module>>(size_t index)>;

/// @p bitcode, the compiled code of module @p name, read into @p ctx; null
/// when it is empty.
StatusOr<std::unique_ptr<llvm::Module>>
readCompiledCode(std::span<const uint8_t> bitcode, const std::string &name,
                 llvm::LLVMContext &ctx);

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
/// otherwise; why an entry was not used goes to @p verbose, when given).
/// The main module is never cached.  Then link the part of each module's
/// @p compiled code the program uses: only what the program (or code linked
/// before it) references, importers before the modules they import.  The
/// linked module verifies.
StatusOr<std::unique_ptr<llvm::Module>>
compileProgram(const pir::Program &program, llvm::LLVMContext &ctx,
               const std::string &moduleName, const std::string &projectRoot,
               const ModuleCache &cache, const ModuleText &text,
               const TranslateModule &translate, const CompiledCode &compiled,
               std::ostream *verbose);

/// A bitcode cache entry, read: the key it was written for and its
/// bitcode (a view of the entry's bytes).
struct CacheEntry {
  std::string Key;
  llvm::StringRef Bitcode;
};

/// The bytes of the cache entry of @p bitcode, translated for @p key: a
/// header (a magic number, the key, the SHA-256 of the bitcode), then the
/// bitcode.
std::string encodeCacheEntry(llvm::StringRef key, llvm::StringRef bitcode);
/// The cache entry @p bytes holds, or why they are not one whose bitcode
/// is as written (a short header, another magic number, a hash mismatch).
StatusOr<CacheEntry> readCacheEntry(llvm::StringRef bytes);

/// "" when @p module verifies; otherwise the first problem the verifier
/// finds, as one line.
std::string verifyModuleText(const llvm::Module &module);

/// Take the "Debug Info Version" flag off @p module, the translation of a
/// module that is written as bitcode (a cache entry, a `.pkm`'s OBJ).
/// LLVM's bitcode reader verifies a module that carries the flag and stops
/// the process when it does not verify; without it, a damaged module is
/// read as it is and verifyModuleText reports it.  The translation has no
/// debug info, so the flag says nothing.
void dropDebugInfoVersion(llvm::Module &module);

/// Link @p parts (main module first) into one module named @p name.
StatusOr<std::unique_ptr<llvm::Module>>
linkModules(const pir::Program &program,
            std::vector<std::unique_ptr<llvm::Module>> parts,
            const std::string &name);

/// Run LLVM's default pipeline for -O@p level (none for -O0).
void optimizeModule(llvm::Module &module, unsigned level);

} // namespace paykan::llvm_codegen
