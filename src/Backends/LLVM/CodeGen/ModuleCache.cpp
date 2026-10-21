// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// A program's LLVM modules: translated by a backend (ModuleCache.h), served
// from and written to the per-project bitcode cache, linked and optimised.

#include "ModuleCache.h"

#include "ModuleName.h"
#include "Names.h"
#include "Runtime.h"
#include "Version.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/raw_ostream.h>

namespace paykan::llvm_codegen {

/// Link @p parts (main module first) into one module named @p name.
StatusOr<std::unique_ptr<llvm::Module>>
linkModules(const pir::Program &program,
            std::vector<std::unique_ptr<llvm::Module>> parts,
            const std::string &name) {
  std::unique_ptr<llvm::Module> main = std::move(parts.front());
  main->setModuleIdentifier(name);
  for (size_t i = 1; i < parts.size(); ++i)
    if (llvm::Linker::linkModules(*main, std::move(parts[i])))
      return Status::error("failed to link module '" + program.Modules[i].Name +
                           "'");
  return std::move(main);
}

// -- Bitcode cache
//
// An imported module's LLVM IR depends only on what the backend translates
// it from (the text of the part of its PIR the program uses, which spells
// out every layout and signature it uses from other modules) and on the
// compiler that translates it, so that text is the cache key.  Entries live
// under <projectRoot>/.paykan_cache/<module name as a path><extension>,
// stamped with the key, which names the backend too; anything that does not
// parse or carries another key is recompiled and rewritten.

namespace {

/// The runtime ABI version (Runtime.h), stamped into every cached module so
/// entries from an older compiler are never linked.
constexpr uint64_t kPaykanABIVersion = kRuntimeABIVersion;
constexpr const char *kABIVersionFlag = "paykan.abi.version";
constexpr const char *kCacheKeyMD = "paykan.cache.key";

/// Identity of the running compiler binary (size and modification time of
/// the executable), so a development build with different codegen but the
/// same version string never serves entries written by the previous build.
llvm::StringRef compilerBuildId() {
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

std::string cacheKey(llvm::StringRef text, llvm::StringRef backend) {
  llvm::SHA256 hasher;
  hasher.update(text);
  hasher.update("|backend ");
  hasher.update(backend);
  hasher.update("|paykan ");
  hasher.update(kVersion);
  hasher.update("|llvm ");
  hasher.update(LLVM_VERSION_STRING);
  hasher.update("|build ");
  hasher.update(compilerBuildId());
  hasher.update("|abi ");
  hasher.update(std::to_string(kPaykanABIVersion));
  return llvm::toHex(hasher.final());
}

/// <projectRoot>/.paykan_cache/<module name as a path><extension>
/// (module_name::cacheRelativePath: `geometry::shapes` is geometry/shapes.bc).
/// An empty project root means the current directory.
std::string cachePath(const std::string &moduleName,
                      const std::string &projectRoot,
                      llvm::StringRef extension) {
  llvm::SmallString<256> path(projectRoot);
  llvm::sys::path::append(path, names::kCacheDir);
  llvm::sys::path::append(
      path, module_name::cacheRelativePath(moduleName).generic_string());
  path += extension;
  return std::string(path);
}

bool cachedModuleIsValid(const llvm::Module &mod, llvm::StringRef key) {
  auto *flag = llvm::mdconst::extract_or_null<llvm::ConstantInt>(
      mod.getModuleFlag(kABIVersionFlag));
  if (!flag || flag->getZExtValue() != kPaykanABIVersion)
    return false;
  auto *nmd = mod.getNamedMetadata(kCacheKeyMD);
  if (!nmd || nmd->getNumOperands() != 1 ||
      nmd->getOperand(0)->getNumOperands() != 1)
    return false;
  auto *md = llvm::dyn_cast<llvm::MDString>(nmd->getOperand(0)->getOperand(0));
  return md && md->getString() == key;
}

void stampCachedModule(llvm::Module &mod, llvm::StringRef key) {
  if (!mod.getModuleFlag(kABIVersionFlag))
    mod.addModuleFlag(llvm::Module::Error, kABIVersionFlag, kPaykanABIVersion);
  auto *nmd = mod.getOrInsertNamedMetadata(kCacheKeyMD);
  nmd->clearOperands();
  auto &ctx = mod.getContext();
  nmd->addOperand(llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, key)}));
}

/// Write @p mod atomically (temporary file + rename) to @p path; best effort,
/// a failure just leaves the entry absent.
void writeCacheFile(const llvm::Module &mod, llvm::StringRef path) {
  auto parentDir = llvm::sys::path::parent_path(path);
  if (!parentDir.empty() && llvm::sys::fs::create_directories(parentDir))
    return;
  int fd = -1;
  llvm::SmallString<256> tmpPath;
  if (llvm::sys::fs::createUniqueFile(path + ".%%%%%%%%.tmp", fd, tmpPath))
    return;
  bool written;
  {
    llvm::raw_fd_ostream out(fd, /*shouldClose=*/true);
    // With its use-list order, so a module read back is the module written
    // (the optimiser's output, `--emit-source`, does not depend on whether
    // a module came from the cache).
    llvm::WriteBitcodeToFile(mod, out, /*ShouldPreserveUseListOrder=*/true);
    out.close();
    written = !out.has_error();
    out.clear_error();
  }
  if (!written || llvm::sys::fs::rename(tmpPath, path))
    (void)llvm::sys::fs::remove(tmpPath);
}

/// A valid cached translation under @p key, or null.
std::unique_ptr<llvm::Module> loadCached(const std::string &path,
                                         llvm::StringRef key,
                                         llvm::LLVMContext &ctx) {
  auto buf = llvm::MemoryBuffer::getFile(path);
  if (!buf)
    return nullptr;
  auto mod = llvm::parseBitcodeFile((*buf)->getMemBufferRef(), ctx);
  if (!mod) {
    llvm::consumeError(mod.takeError());
    return nullptr;
  }
  if (!cachedModuleIsValid(**mod, key))
    return nullptr;
  return std::move(*mod);
}

} // namespace

StatusOr<std::unique_ptr<llvm::Module>>
compileProgram(const pir::Program &program, llvm::LLVMContext &ctx,
               const std::string &moduleName, const std::string &projectRoot,
               const ModuleCache &cache, const ModuleText &text,
               const TranslateModule &translate) {
  if (program.Modules.empty())
    return Status::error("empty PIR program");
  std::vector<std::unique_ptr<llvm::Module>> parts;
  for (size_t i = 0; i < program.Modules.size(); ++i) {
    if (i > 0) {
      const pir::Module &module = program.Modules[i];
      std::string key = cacheKey(text(i), cache.Backend);
      std::string path = cachePath(module.Name, projectRoot, cache.Extension);
      if (auto cached = loadCached(path, key, ctx)) {
        parts.push_back(std::move(cached));
        continue;
      }
      auto part = translate(i);
      if (!part)
        return part.status();
      stampCachedModule(**part, key);
      writeCacheFile(**part, path);
      parts.push_back(std::move(*part));
      continue;
    }
    auto part = translate(i);
    if (!part)
      return part.status();
    parts.push_back(std::move(*part));
  }
  return linkModules(program, std::move(parts), moduleName);
}

void optimizeModule(llvm::Module &module, unsigned level) {
  static const llvm::OptimizationLevel levels[] = {
      llvm::OptimizationLevel::O0,
      llvm::OptimizationLevel::O1,
      llvm::OptimizationLevel::O2,
      llvm::OptimizationLevel::O3,
  };
  llvm::OptimizationLevel lvl = levels[level < 4 ? level : 3];
  if (lvl == llvm::OptimizationLevel::O0)
    return;

  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;

  llvm::PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(lvl);
  MPM.run(module, MAM);
}

} // namespace paykan::llvm_codegen
