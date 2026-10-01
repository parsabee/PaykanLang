// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// CodeGen: import resolution and per-module code generation.

#include "CodeGen.h"
#include "ModuleUtils.h"
#include "Names.h"
#include "Version.h"

#include <llvm/ADT/StringExtras.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/raw_ostream.h>

namespace paykan {
namespace codegen {

// ---------------------------------------------------------------------------
// Static helpers (file-scope)
// ---------------------------------------------------------------------------

/// ABI version stamped into every cached import module as a module flag.
/// A cached .bc whose flag is missing (pre-versioning compiler) or different
/// was generated against an incompatible object layout / calling convention
/// and must be recompiled, not loaded — linking it would corrupt memory at
/// runtime.  Bump this whenever generated-code ABI changes.
///   v2: two-slot object header — PaykanShared* unique-box backpointer added
///       at struct slot 1, shifting every class field GEP (PAY-1).
static constexpr uint64_t kPaykanABIVersion = 2;
static constexpr const char *kABIVersionFlag = "paykan.abi.version";

/// Identity of the running compiler binary -- size and modification time of
/// the executable -- hashed into every cache key.  kVersion only changes at a
/// release, so without this a development build with different codegen but
/// the same version string keeps serving imports compiled by the previous
/// build (a fixed leak in an imported module "stayed" until .paykan_cache was
/// deleted by hand).  Empty when the executable cannot be identified, in
/// which case the key falls back to the version fields alone.
static llvm::StringRef compilerBuildId() {
  static const std::string id = [] {
    std::string exe = llvm::sys::fs::getMainExecutable(
        nullptr, reinterpret_cast<void *>(&compilerBuildId));
    llvm::sys::fs::file_status st;
    if (exe.empty() || llvm::sys::fs::status(exe, st))
      return std::string();
    return std::to_string(st.getSize()) + ":" +
           std::to_string(
               st.getLastModificationTime().time_since_epoch().count());
  }();
  return id;
}

/// Named metadata carrying the cache key (see CodeGen::importCacheKey) the
/// cached module was generated under.  Named (not module-flag) metadata so
/// that linking modules with different keys into the main module merges
/// rather than conflicts.
static constexpr const char *kCacheKeyMD = "paykan.cache.key";

/// True when `mod` (a freshly parsed cached module) matches the running
/// compiler's ABI version and was generated under exactly @p cacheKey.
static bool cachedModuleIsValid(const llvm::Module &mod,
                                llvm::StringRef cacheKey) {
  auto *flag = llvm::mdconst::extract_or_null<llvm::ConstantInt>(
      mod.getModuleFlag(kABIVersionFlag));
  if (!flag || flag->getZExtValue() != kPaykanABIVersion)
    return false;
  auto *nmd = mod.getNamedMetadata(kCacheKeyMD);
  if (!nmd || nmd->getNumOperands() != 1 ||
      nmd->getOperand(0)->getNumOperands() != 1)
    return false;
  auto *key = llvm::dyn_cast<llvm::MDString>(nmd->getOperand(0)->getOperand(0));
  return key && key->getString() == cacheKey;
}

/// Stamp the ABI version and cache key into a freshly generated import module
/// before it is written to the cache.
static void stampCachedModule(llvm::Module &mod, llvm::StringRef cacheKey) {
  if (!mod.getModuleFlag(kABIVersionFlag))
    mod.addModuleFlag(llvm::Module::Error, kABIVersionFlag, kPaykanABIVersion);
  auto *nmd = mod.getOrInsertNamedMetadata(kCacheKeyMD);
  nmd->clearOperands();
  auto &ctx = mod.getContext();
  nmd->addOperand(llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, cacheKey)}));
}

/// Write @p mod as bitcode to @p cachePath atomically: the bitcode goes to a
/// uniquely named temporary file in the same directory which is then renamed
/// over the final path, so a concurrent or interrupted compile can never
/// observe a partially written cache entry.  Best-effort: any failure simply
/// leaves the cache entry absent (and removes the temporary file).
static void writeCacheFile(const llvm::Module &mod, llvm::StringRef cachePath) {
  auto parentDir = llvm::sys::path::parent_path(cachePath);
  if (!parentDir.empty() && llvm::sys::fs::create_directories(parentDir))
    return;

  int fd = -1;
  llvm::SmallString<256> tmpPath;
  if (llvm::sys::fs::createUniqueFile(cachePath + ".%%%%%%%%.tmp", fd, tmpPath))
    return;

  bool written;
  {
    llvm::raw_fd_ostream out(fd, /*shouldClose=*/true);
    llvm::WriteBitcodeToFile(mod, out);
    out.close();
    written = !out.has_error();
    out.clear_error(); // an error left set would abort in the destructor
  }
  if (!written || llvm::sys::fs::rename(tmpPath, cachePath))
    (void)llvm::sys::fs::remove(
        tmpPath); // best effort: nothing to do on failure
}

/// Resolve an import to the canonical path of its source file, mirroring
/// Sema::resolveModulePath.  Returns "" if the file does not exist (Sema has
/// already reported that).
static std::string resolveImportFile(llvm::StringRef projectRoot, bool isSystem,
                                     llvm::StringRef modulePath) {
  llvm::SmallString<256> full;
  if (isSystem) {
    const char *env = std::getenv(names::kPaykanStdlibEnv);
    if (env && env[0]) {
      full = env;
    } else {
      full = projectRoot;
      llvm::sys::path::append(full, names::kStdlibDir);
    }
  } else {
    full = projectRoot;
  }
  llvm::sys::path::append(full, module_utils::modulePathToRelative(modulePath));
  return module_utils::realPath(full);
}

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
// lookupImportContext / importCacheKey
// ---------------------------------------------------------------------------

const sema::SemaContext *
CodeGen::lookupImportContext(llvm::StringRef resolved) {
  if (!ImportGraph->Indexed) {
    ImportGraph->Indexed = true;
    // Depth-first over the SemaContext tree; the first context seen for a path
    // wins (there is normally exactly one).
    llvm::SmallVector<const sema::SemaContext *, 16> work{&SemaCtx};
    while (!work.empty()) {
      const auto *ctx = work.pop_back_val();
      for (auto &[path, child] : ctx->ImportedContexts) {
        if (!child)
          continue;
        if (ImportGraph->Contexts.try_emplace(path, child.get()).second)
          work.push_back(child.get());
      }
    }
  }
  auto it = ImportGraph->Contexts.find(resolved);
  return it == ImportGraph->Contexts.end() ? nullptr : it->second;
}

std::string CodeGen::importCacheKey(llvm::StringRef resolved,
                                    const sema::SemaContext &modCtx) {
  auto memo = ImportGraph->CacheKeys.find(resolved);
  if (memo != ImportGraph->CacheKeys.end())
    return memo->second;
  // Placeholder while the dependencies are hashed: Sema rejects import cycles,
  // but if one ever slipped through this turns infinite recursion into "no
  // key" (the module is simply not cached).
  ImportGraph->CacheKeys[resolved] = "";

  std::string key;
  auto source = llvm::MemoryBuffer::getFile(resolved);
  if (source && modCtx.Root) {
    llvm::SHA256 hash;
    auto add = [&hash](llvm::StringRef s) {
      hash.update(s);
      hash.update(llvm::StringRef("\0", 1)); // unambiguous field separator
    };
    // Anything that changes the generated code beyond the source text itself:
    // the compiler release, the exact compiler binary (development builds
    // share a release string), the LLVM it embeds (bitcode format), and the
    // runtime object layout / calling convention.
    add("paykan-import-cache");
    add(kVersion);
    add(compilerBuildId());
    add(LLVM_VERSION_STRING);
    add(std::to_string(kPaykanABIVersion));
    add((*source)->getBuffer());

    bool complete = true;
    for (auto *imp : modCtx.Root->getImports()) {
      for (auto &m : imp->getModules()) {
        std::string depPath =
            resolveImportFile(ProjectRoot, imp->isSystem(), imp->modulePath(m));
        const sema::SemaContext *depCtx =
            depPath.empty() ? nullptr : lookupImportContext(depPath);
        std::string depKey = depCtx ? importCacheKey(depPath, *depCtx) : "";
        if (depKey.empty()) {
          complete = false;
          break;
        }
        add(depKey);
      }
      if (!complete)
        break;
    }
    if (complete)
      key = llvm::toHex(hash.final(), /*LowerCase=*/true);
  }

  ImportGraph->CacheKeys[resolved] = key;
  return key;
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
      std::string resolved =
          resolveImportFile(ProjectRoot, imp->isSystem(), modulePath);
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

      // Sema's pre-computed context for the module: the import list needed
      // for the cache key and, on a cache miss, the AST to generate from.
      const sema::SemaContext *modCtx = lookupImportContext(resolved);
      if (!modCtx)
        continue;

      // Records, in the importing context, which qualifier the module's
      // classes were imported under (method-name lookup for imported classes).
      auto recordClassQualifiers = [&] {
        for (auto &[name, _] : modCtx->ASTCtx->getClassTypes()) {
          if (name == names::kObj || name == names::kString)
            continue;
          if (auto *ct = ASTCtx.lookupClassType(name))
            Classes.ImportedClassQualifiers.try_emplace(ct, qualifier);
        }
      };

      // -- Bitcode cache check ----------------------------------------------
      // The key covers the module's source and everything it transitively
      // imports; an entry is only ever used if it was written under the very
      // same key.  Anything else (missing, truncated, foreign, or stale)
      // falls through to a full recompile, which rewrites it.
      auto cachePath = module_utils::getCachePath(resolved, ProjectRoot);
      std::string cacheKey = importCacheKey(resolved, *modCtx);
      if (!cacheKey.empty()) {
        if (auto buf = llvm::MemoryBuffer::getFile(cachePath)) {
          auto modOrErr =
              llvm::parseBitcodeFile((*buf)->getMemBufferRef(), LLVMCtx);
          if (!modOrErr) {
            llvm::consumeError(modOrErr.takeError()); // corrupt: recompile
          } else if (cachedModuleIsValid(**modOrErr, cacheKey)) {
            auto &cachedMod = *modOrErr;
            llvm::Module *defMod = cachedMod.get();
            addImportAliases(defMod, qualifier, modulePath);
            (*ImportRegistry)[resolved] = defMod;
            ImportedModules.push_back(std::move(cachedMod));

            // The cached bitcode only DEFINES this module's own functions; the
            // modules it imports still have to be loaded or generated.
            if (modCtx->Root)
              processImports(modCtx->Root);
            recordClassQualifiers();
            continue;
          }
        }
      }

      // -- Full codegen using the pre-computed SemaContext ------------------
      auto &importedSemaCtx = *modCtx;

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
      importCG.ImportGraph = ImportGraph;
      if (!importCG.run(importedSemaCtx.Root)) {
        // Carrying on would link the importer against a module that was never
        // generated (unresolved symbols or a crash at run time).
        reportInternalError("code generation failed for imported module '" +
                            resolved + "'");
        continue;
      }

      auto impMod = importCG.takeModule();
      llvm::Module *defMod = impMod.get();

      // Declare externals in this module + add qualifier aliases in the
      // imported module that defines the functions.
      addImportAliases(defMod, qualifier, modulePath);
      (*ImportRegistry)[resolved] = defMod;

      // -- Write bitcode to cache ------------------------------------------
      // Stamp the ABI version and cache key so a future compiler with a
      // different object layout, or a later edit anywhere in this module's
      // import graph, refuses this entry instead of linking incompatible code.
      if (!cacheKey.empty()) {
        stampCachedModule(*impMod, cacheKey);
        writeCacheFile(*impMod, cachePath);
      }

      ImportedModules.push_back(std::move(impMod));
      for (auto &mm : importCG.takeImportedModules())
        ImportedModules.push_back(std::move(mm));

      recordClassQualifiers();
    } // for each module in imp->getModules()
  }
}

} // namespace codegen
} // namespace paykan
