// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// CodeGen: import resolution and per-module code generation.

#include "CodeGen.h"
#include "ModuleUtils.h"
#include "Names.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Support/MemoryBuffer.h>

namespace paykan {
namespace codegen {

// ---------------------------------------------------------------------------
// Static helpers (file-scope)
// ---------------------------------------------------------------------------

/// Declare fn under qualifiedName in mod if not already present.
static void declareExternAs(llvm::Module *mod, llvm::Function &fn,
                            const std::string &qualifiedName) {
  if (mod->getFunction(qualifiedName))
    return;
  llvm::Function::Create(fn.getFunctionType(), llvm::Function::ExternalLinkage,
                         qualifiedName, mod);
}

// ---------------------------------------------------------------------------
// visitImportDecl
// ---------------------------------------------------------------------------

llvm::Value *CodeGen::visitImportDecl(ast::ImportDecl *) {
  // Import processing happens in processImports() before visiting the TU.
  return nullptr;
}

// ---------------------------------------------------------------------------
// addImportAliases
// ---------------------------------------------------------------------------

void CodeGen::addImportAliases(llvm::Module *defMod,
                               const std::string &qualifier,
                               const std::string &modulePath) {
  auto addAlias = [&](llvm::Function &fn, const std::string &qn) {
    // Declare the qualified name in the importing (current) module so its
    // references resolve at link time.
    declareExternAs(Module.get(), fn, qn);
    // Define the alias in the module that owns the function (idempotent: a
    // diamond may reach the same module under the same qualifier twice).
    if (!defMod->getFunction(qn) && !defMod->getNamedAlias(qn))
      llvm::GlobalAlias::create(fn.getFunctionType(), fn.getAddressSpace(),
                                llvm::GlobalValue::ExternalLinkage, qn, &fn,
                                defMod);
  };
  for (auto &fn : defMod->functions()) {
    if (fn.isDeclaration())
      continue;
    addAlias(fn, qualifier + names::kQualSep + fn.getName().str());
    if (qualifier != modulePath)
      addAlias(fn, modulePath + names::kQualSep + fn.getName().str());
  }
}

// ---------------------------------------------------------------------------
// processImports
// ---------------------------------------------------------------------------

void CodeGen::processImports(ast::TranslationUnit *tu) {
  for (auto *imp : tu->getImports()) {
    for (auto &m : imp->getModules()) {
      std::string modulePath = imp->modulePath(m);
      std::string qualifier = m.qualifier();

      // Resolve the module path to a file (same logic as Sema).
      auto relPath = module_utils::modulePathToRelative(modulePath);

      llvm::SmallString<256> fullBuf;
      if (imp->isSystem()) {
        const char *env = std::getenv(names::kPaykanStdlibEnv);
        if (env && env[0]) {
          fullBuf = env;
        } else {
          fullBuf = ProjectRoot;
          llvm::sys::path::append(fullBuf, names::kStdlibDir);
        }
      } else {
        fullBuf = ProjectRoot;
      }
      llvm::sys::path::append(fullBuf, relPath);

      std::string resolved = module_utils::realPath(fullBuf);
      if (resolved.empty())
        continue; // Sema already reported the error.

      // Already generated somewhere in the import graph?  Generate the body
      // only once, but still wire up THIS import site's qualifier aliases
      // against the module that defines it (handles diamond imports, including
      // the same module reached under different qualifiers).
      if (auto regIt = ImportRegistry->find(resolved);
          regIt != ImportRegistry->end()) {
        if (regIt->second)
          addImportAliases(regIt->second, qualifier, modulePath);
        continue;
      }

      // -- Bitcode cache check ----------------------------------------------
      auto cachePath = module_utils::getCachePath(resolved, ProjectRoot);
      if (!module_utils::isSourceNewer(resolved, cachePath)) {
        // Load cached bitcode.
        auto bufOrErr = llvm::MemoryBuffer::getFile(cachePath);
        if (bufOrErr) {
          auto modOrErr =
              llvm::parseBitcodeFile((*bufOrErr)->getMemBufferRef(), LLVMCtx);
          if (modOrErr) {
            auto &cachedMod = *modOrErr;
            llvm::Module *defMod = cachedMod.get();
            addImportAliases(defMod, qualifier, modulePath);
            (*ImportRegistry)[resolved] = defMod;
            ImportedModules.push_back(std::move(cachedMod));

            auto ctxIt = SemaCtx.ImportedContexts.find(resolved);
            if (ctxIt != SemaCtx.ImportedContexts.end() && ctxIt->second->Root)
              processImports(ctxIt->second->Root);

            if (ctxIt != SemaCtx.ImportedContexts.end()) {
              for (auto &[name, _] : ctxIt->second->ASTCtx->getClassTypes()) {
                if (name == names::kObj || name == names::kString)
                  continue;
                if (auto *ct = ASTCtx.lookupClassType(name))
                  Classes.ImportedClassQualifiers.try_emplace(ct, qualifier);
              }
            }
            continue;
          }
        }
        // If loading failed, fall through to recompile.
      }

      // -- Full codegen using the pre-computed SemaContext ------------------
      auto ctxIt = SemaCtx.ImportedContexts.find(resolved);
      if (ctxIt == SemaCtx.ImportedContexts.end())
        continue;
      auto &importedSemaCtx = *ctxIt->second;

      // #33 (parallel imported-module codegen) seam:
      //
      // To compile imports in parallel under ORCv2 each imported module would
      // need its OWN llvm::LLVMContext (an LLVMContext is not thread-safe), so
      // this line would become:
      //
      //     auto importTSCtx =
      //         std::make_unique<llvm::orc::ThreadSafeContext>(
      //             std::make_unique<llvm::LLVMContext>());
      //     CodeGen importCG(importedSemaCtx, *importTSCtx.getContext(),
      //                      resolved, ProjectRoot);
      //
      // and the resulting module would be handed to the JIT as its own
      // ThreadSafeModule rather than linked into the parent module.
      //
      // That is currently BLOCKED end-to-end: the driver (src/Driver/main.cpp)
      // and the test harness (tests/TestUtils.h) both consume imported modules
      // via llvm::Linker::linkModules() into the single parent context, and the
      // cross-module qualifier aliases below are created in the imported
      // module's context referencing the parent module's functions — both of
      // which require all modules to share ONE context.  Switching to
      // per-import contexts therefore requires rewiring the link-merge pipeline
      // into a multi-ThreadSafeModule JIT add (a separate, larger change).
      // Until then imports share the parent LLVMCtx and codegen runs serially.
      CodeGen importCG(importedSemaCtx, LLVMCtx, resolved, ProjectRoot,
                       ImportRegistry);
      if (!importCG.run(importedSemaCtx.Root))
        continue;

      auto impMod = importCG.takeModule();
      llvm::Module *defMod = impMod.get();

      // Declare externals in this module + add qualifier aliases in the
      // imported module that defines the functions.
      addImportAliases(defMod, qualifier, modulePath);
      (*ImportRegistry)[resolved] = defMod;

      // -- Write bitcode to cache ------------------------------------------
      {
        auto parentDir = llvm::sys::path::parent_path(cachePath);
        std::error_code mkdirEc;
        if (!parentDir.empty())
          mkdirEc = llvm::sys::fs::create_directories(parentDir);
        // Cache is best-effort: only write if the directory is in place.
        if (!mkdirEc) {
          std::error_code ec;
          llvm::raw_fd_ostream cacheOut(cachePath, ec);
          if (!ec)
            llvm::WriteBitcodeToFile(*impMod, cacheOut);
        }
      }

      ImportedModules.push_back(std::move(impMod));
      for (auto &mm : importCG.takeImportedModules())
        ImportedModules.push_back(std::move(mm));

      for (auto &[name, _] : importedSemaCtx.ASTCtx->getClassTypes()) {
        if (name == names::kObj || name == names::kString)
          continue;
        if (auto *ct = ASTCtx.lookupClassType(name))
          Classes.ImportedClassQualifiers.try_emplace(ct, qualifier);
      }
    } // for each module in imp->getModules()
  }
}

} // namespace codegen
} // namespace paykan
