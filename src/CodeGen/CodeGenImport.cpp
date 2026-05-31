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
  llvm::Function::Create(fn.getFunctionType(),
                         llvm::Function::ExternalLinkage,
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
// processImports
// ---------------------------------------------------------------------------

void CodeGen::processImports(ast::TranslationUnit *tu) {
  for (auto *imp : tu->getImports()) {
    for (auto &m : imp->getModules()) {
      std::string modulePath = imp->modulePath(m);
      std::string qualifier  = m.qualifier();

      // Resolve the module path to a file (same logic as Sema).
      auto relPath = module_utils::modulePathToRelative(modulePath);

      llvm::SmallString<256> fullBuf;
      if (imp->isSystem()) {
        const char *env = std::getenv(names::kPaykanStdlibEnv);
        if (env && env[0])
          fullBuf = env;
          else {
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

      // Skip if already codegen'd.
      if (CodeGenedImports.count(resolved))
        continue;
      CodeGenedImports.insert(resolved);

      // -- Bitcode cache check ----------------------------------------------
      auto cachePath = module_utils::getCachePath(resolved, ProjectRoot);
      if (!module_utils::isSourceNewer(resolved, cachePath)) {
        // Load cached bitcode.
        auto bufOrErr = llvm::MemoryBuffer::getFile(cachePath);
        if (bufOrErr) {
          auto modOrErr = llvm::parseBitcodeFile((*bufOrErr)->getMemBufferRef(),
                                                 LLVMCtx);
          if (modOrErr) {
            auto &cachedMod = *modOrErr;
            // Helper: declare + alias a function under a qualified name.
            auto addAlias = [&](llvm::Function &fn, const std::string &qn) {
              declareExternAs(Module.get(), fn, qn);
              if (!cachedMod->getFunction(qn) && !cachedMod->getNamedAlias(qn))
                llvm::GlobalAlias::create(fn.getFunctionType(), fn.getAddressSpace(),
                                          llvm::GlobalValue::ExternalLinkage,
                                          qn, &fn, cachedMod.get());
            };
            for (auto &fn : cachedMod->functions()) {
              if (fn.isDeclaration()) continue;
              addAlias(fn, qualifier + names::kQualSep + fn.getName().str());
              if (qualifier != modulePath)
                addAlias(fn, modulePath + names::kQualSep + fn.getName().str());
            }
            ImportedModules.push_back(std::move(cachedMod));

            auto ctxIt = SemaCtx.ImportedContexts.find(resolved);
            if (ctxIt != SemaCtx.ImportedContexts.end() &&
                ctxIt->second->Root)
              processImports(ctxIt->second->Root);

            if (ctxIt != SemaCtx.ImportedContexts.end()) {
              for (auto &[name, _] :
                   ctxIt->second->ASTCtx->getClassTypes()) {
                if (name == names::kObj || name == names::kString) continue;
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

      CodeGen importCG(importedSemaCtx, LLVMCtx, resolved, ProjectRoot);
      if (!importCG.run(importedSemaCtx.Root))
        continue;

      auto impMod = importCG.takeModule();

      // Declare externals + add aliases in imported module for both
      // qualifier:: and modulePath::.
      auto addImpAlias = [&](llvm::Function &fn, const std::string &qn) {
        if (!Module->getFunction(qn))
          llvm::Function::Create(fn.getFunctionType(),
                                 llvm::Function::ExternalLinkage, qn, Module.get());
        if (!impMod->getFunction(qn) && !impMod->getNamedAlias(qn))
          llvm::GlobalAlias::create(fn.getFunctionType(), fn.getAddressSpace(),
                                    llvm::GlobalValue::ExternalLinkage,
                                    qn, &fn, impMod.get());
      };
      for (auto &fn : impMod->functions()) {
        if (fn.isDeclaration()) continue;
        addImpAlias(fn, qualifier + names::kQualSep + fn.getName().str());
        if (qualifier != modulePath)
          addImpAlias(fn, modulePath + names::kQualSep + fn.getName().str());
      }

      // -- Write bitcode to cache ------------------------------------------
      {
        auto parentDir = llvm::sys::path::parent_path(cachePath);
        if (!parentDir.empty())
          llvm::sys::fs::create_directories(parentDir);
        std::error_code ec;
        llvm::raw_fd_ostream cacheOut(cachePath, ec);
        if (!ec)
          llvm::WriteBitcodeToFile(*impMod, cacheOut);
      }

      ImportedModules.push_back(std::move(impMod));
      for (auto &mm : importCG.takeImportedModules())
        ImportedModules.push_back(std::move(mm));

      for (auto &[name, _] : importedSemaCtx.ASTCtx->getClassTypes()) {
        if (name == names::kObj || name == names::kString) continue;
        if (auto *ct = ASTCtx.lookupClassType(name))
          Classes.ImportedClassQualifiers.try_emplace(ct, qualifier);
      }
    } // for each module in imp->getModules()
  }
}

} // namespace codegen
} // namespace paykan
