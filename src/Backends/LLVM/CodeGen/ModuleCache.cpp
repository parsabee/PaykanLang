// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/raw_ostream.h>

#include <array>
#include <ostream>

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
// under <projectRoot>/.paykan_cache/<module name as a path><extension>:
// a header that names the key and the SHA-256 of the bitcode, then the
// bitcode.  An entry is parsed only when its header is whole, names the
// key and matches the bitcode (LLVM's bitcode reader is not hardened
// against damaged input), and used only when the module it holds
// verifies; anything else is translated and written again.

namespace {

/// The first bytes of a cache entry; the last four number the layout.
constexpr llvm::StringLiteral kEntryMagic("PKNBC001");
/// The key is a SHA-256 in hex.
constexpr size_t kKeySize = 64;
constexpr size_t kHashSize = 32;
constexpr size_t kHeaderSize = kEntryMagic.size() + kKeySize + kHashSize;

/// The runtime ABI version (Runtime.h): part of every key, so entries from
/// an older compiler are never linked.
constexpr uint64_t kPaykanABIVersion = kRuntimeABIVersion;

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

std::array<uint8_t, kHashSize> bitcodeHash(llvm::StringRef bitcode) {
  return llvm::SHA256::hash(llvm::arrayRefFromStringRef(bitcode));
}

/// <module name as a path><extension> under the cache directory
/// (module_name::cacheRelativePath: `geometry::shapes` is
/// geometry/shapes.bc).
std::string cacheRelative(const std::string &moduleName,
                          llvm::StringRef extension) {
  llvm::SmallString<128> path(names::kCacheDir);
  llvm::sys::path::append(
      path, module_name::cacheRelativePath(moduleName).generic_string());
  path += extension;
  return std::string(path);
}

/// <projectRoot>/<cacheRelative>; an empty project root means the current
/// directory.  --verbose names an entry by it, the path from where the
/// compiler runs.
std::string cachePath(const std::string &projectRoot,
                      const std::string &relative) {
  llvm::SmallString<256> path(projectRoot);
  llvm::sys::path::append(path, relative);
  return std::string(path);
}

/// Write @p mod, translated for @p key, atomically (temporary file +
/// rename) to @p path; best effort, a failure just leaves the entry absent.
void writeCacheFile(const llvm::Module &mod, llvm::StringRef key,
                    llvm::StringRef path) {
  auto parentDir = llvm::sys::path::parent_path(path);
  if (!parentDir.empty() && llvm::sys::fs::create_directories(parentDir))
    return;
  std::string bitcode;
  {
    llvm::raw_string_ostream os(bitcode);
    // With its use-list order, so a module read back is the module written
    // (the optimiser's output, `--emit-source`, does not depend on whether
    // a module came from the cache).
    llvm::WriteBitcodeToFile(mod, os, /*ShouldPreserveUseListOrder=*/true);
  }
  std::string entry = encodeCacheEntry(key, bitcode);
  int fd = -1;
  llvm::SmallString<256> tmpPath;
  if (llvm::sys::fs::createUniqueFile(path + ".%%%%%%%%.tmp", fd, tmpPath))
    return;
  bool written;
  {
    llvm::raw_fd_ostream out(fd, /*shouldClose=*/true);
    out << entry;
    out.close();
    written = !out.has_error();
    out.clear_error();
  }
  if (!written || llvm::sys::fs::rename(tmpPath, path))
    (void)llvm::sys::fs::remove(tmpPath);
}

/// The cached translation at @p path, or why it is not used (Why: "" when
/// there is no entry).
struct Cached {
  std::unique_ptr<llvm::Module> Module;
  std::string Why;
};

/// The entry at @p path when it holds a translation under @p key that
/// verifies.
Cached loadCached(const std::string &path, llvm::StringRef key,
                  llvm::LLVMContext &ctx) {
  auto buf = llvm::MemoryBuffer::getFile(path);
  if (!buf)
    return {nullptr, ""};
  StatusOr<CacheEntry> entry = readCacheEntry((*buf)->getBuffer());
  if (!entry)
    return {nullptr, "is corrupt (" + entry.status().message() + ")"};
  if ((*entry).Key != key)
    return {nullptr, "is stale (it holds another translation)"};
  auto mod = llvm::parseBitcodeFile(
      llvm::MemoryBufferRef((*entry).Bitcode, path), ctx);
  if (!mod)
    return {nullptr, "is corrupt (its bitcode does not read: " +
                         llvm::toString(mod.takeError()) + ")"};
  if (std::string why = verifyModuleText(**mod); !why.empty())
    return {nullptr, "is corrupt (its module does not verify: " + why + ")"};
  return {std::move(*mod), ""};
}

} // namespace

std::string encodeCacheEntry(llvm::StringRef key, llvm::StringRef bitcode) {
  std::string out(kEntryMagic);
  out += key;
  auto hash = bitcodeHash(bitcode);
  out.append(hash.begin(), hash.end());
  out += bitcode;
  return out;
}

StatusOr<CacheEntry> readCacheEntry(llvm::StringRef bytes) {
  if (bytes.size() < kHeaderSize || !bytes.starts_with(kEntryMagic))
    return Status::error("not a bitcode cache entry");
  CacheEntry entry;
  entry.Key = bytes.substr(kEntryMagic.size(), kKeySize).str();
  llvm::StringRef hash = bytes.substr(kEntryMagic.size() + kKeySize, kHashSize);
  entry.Bitcode = bytes.substr(kHeaderSize);
  auto actual = bitcodeHash(entry.Bitcode);
  if (hash != llvm::StringRef(reinterpret_cast<const char *>(actual.data()),
                              actual.size()))
    return Status::error("its bitcode does not match its hash");
  return entry;
}

void dropDebugInfoVersion(llvm::Module &module) {
  llvm::NamedMDNode *flags = module.getModuleFlagsMetadata();
  if (!flags)
    return;
  std::vector<llvm::MDNode *> kept;
  for (llvm::MDNode *flag : flags->operands()) {
    auto *key = flag->getNumOperands() == 3
                    ? llvm::dyn_cast<llvm::MDString>(flag->getOperand(1))
                    : nullptr;
    if (!key || key->getString() != "Debug Info Version")
      kept.push_back(flag);
  }
  flags->clearOperands();
  for (llvm::MDNode *flag : kept)
    flags->addOperand(flag);
  if (kept.empty())
    flags->eraseFromParent();
}

std::string verifyModuleText(const llvm::Module &module) {
  std::string text;
  llvm::raw_string_ostream os(text);
  if (!llvm::verifyModule(module, &os))
    return "";
  // The first problem: the verifier prints the offending IR after it.
  llvm::StringRef first = llvm::StringRef(text).split('\n').first;
  return first.rtrim("!. ").str();
}

StatusOr<std::unique_ptr<llvm::Module>>
compileProgram(const pir::Program &program, llvm::LLVMContext &ctx,
               const std::string &moduleName, const std::string &projectRoot,
               const ModuleCache &cache, const ModuleText &text,
               const TranslateModule &translate, const CompiledCode &compiled,
               std::ostream *verbose) {
  if (program.Modules.empty())
    return Status::error("empty PIR program");
  std::vector<std::unique_ptr<llvm::Module>> parts;
  for (size_t i = 0; i < program.Modules.size(); ++i) {
    if (i > 0) {
      const pir::Module &module = program.Modules[i];
      std::string key = cacheKey(text(i), cache.Backend);
      std::string relative = cacheRelative(module.Name, cache.Extension);
      std::string path = cachePath(projectRoot, relative);
      Cached cached = loadCached(path, key, ctx);
      if (cached.Module) {
        parts.push_back(std::move(cached.Module));
        continue;
      }
      if (verbose && !cached.Why.empty())
        *verbose << "module " << module.Name << ": bitcode cache " << path
                 << " " << cached.Why << "; translating it again\n";
      auto part = translate(i);
      if (!part)
        return part.status();
      // As it is read back from the cache.
      dropDebugInfoVersion(**part);
      writeCacheFile(**part, key, path);
      parts.push_back(std::move(*part));
      continue;
    }
    auto part = translate(i);
    if (!part)
      return part.status();
    parts.push_back(std::move(*part));
  }
  auto linked = linkModules(program, std::move(parts), moduleName);
  if (!linked)
    return linked;
  // A module is linked after every module that may call it (the program
  // lists a module after the modules it imports), so what it is asked for
  // is known when it is linked.
  for (size_t i = program.Modules.size(); compiled && i-- > 1;) {
    std::span<const uint8_t> bytes = compiled(i);
    if (bytes.empty())
      continue;
    const std::string &name = program.Modules[i].Name;
    llvm::StringRef buffer(reinterpret_cast<const char *>(bytes.data()),
                           bytes.size());
    auto code =
        llvm::parseBitcodeFile(llvm::MemoryBufferRef(buffer, name), ctx);
    if (!code) {
      std::string msg = "the compiled code of module '" + name;
      msg += "' does not read: ";
      msg += llvm::toString(code.takeError());
      return Status::error(msg);
    }
    if (llvm::Linker::linkModules(**linked, std::move(*code),
                                  llvm::Linker::Flags::LinkOnlyNeeded))
      return Status::error("failed to link the compiled code of module '" +
                           name + "'");
  }
  // What the optimiser and the code generator get must verify: they stop
  // the process on a module that does not.
  if (std::string why = verifyModuleText(**linked); !why.empty())
    return Status::error(
        "internal compiler error: the program's LLVM IR does not verify: " +
        why);
  return linked;
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
