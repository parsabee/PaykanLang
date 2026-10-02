// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// C backend: compiling and running the emitted C with the system compiler.

#include "paykan/backends/c/CBackend.h"

#include "Version.h"

#include "CNames.h"
#include "Platform.h"
#include "ToolchainNames.h"

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace paykan::backend_c {

namespace fs = std::filesystem;

// The process / runtime / link helpers are shared with the llvm backend
// (paykan/backends/Toolchain.h).
using toolchain::spawn;
using toolchain::TempDir;
namespace platform = toolchain::platform;
namespace tcnames = toolchain::tcnames;

namespace {

std::string readFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return std::string(1, '\0'); // never equal to real content
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

/// A temporary name next to @p path, unique to this process and call, so
/// concurrent builds sharing a cache never write the same file.
std::string tempSibling(const std::string &path) {
  static std::atomic<unsigned> counter{0};
  return path + ".tmp" + std::to_string(platform::processId()) + "-" +
         std::to_string(counter++);
}

/// Move @p tmp over @p path (atomic within a directory); removes @p tmp on
/// failure.
bool renameOver(const std::string &tmp, const std::string &path) {
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (!ec)
    return true;
  fs::remove(tmp, ec);
  return false;
}

/// Write @p text to @p path through a temporary file in the same directory
/// and a rename, so a concurrent or interrupted compile never observes a
/// partially written entry.
bool writeFileAtomically(const std::string &path, const std::string &text) {
  std::string tmp = tempSibling(path);
  {
    std::ofstream out(tmp, std::ios::binary);
    out << text;
    if (!out) {
      std::error_code ec;
      fs::remove(tmp, ec);
      return false;
    }
  }
  return renameOver(tmp, path);
}

/// 64-bit FNV-1a of @p data, as 16 hex digits.
std::string fnv1a(const std::string &data) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (unsigned char c : data) {
    h ^= c;
    h *= 0x100000001b3ULL;
  }
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016" PRIx64, h);
  return buf;
}

/// The cache entry base path (without extension) of a module.
std::string cacheEntryBase(const Toolchain &tc, const std::string &moduleName) {
  std::error_code ec;
  fs::path root =
      tc.ProjectRoot.empty() ? fs::current_path(ec) : fs::path(tc.ProjectRoot);
  root = fs::weakly_canonical(root, ec);
  fs::path mod = fs::weakly_canonical(moduleName, ec);
  fs::path rel = mod.lexically_relative(root);
  if (rel.empty() || rel.native().rfind("..", 0) == 0)
    rel = mod.relative_path(); // outside the project: mirror the full path
  return (fs::path(tc.CacheDir) / rel).string();
}

} // namespace

bool buildExecutable(const pir::Program &program, const std::string &outputPath,
                     const Toolchain &tcIn, std::ostream &errs,
                     const std::string &keepC) {
  Toolchain tc = tcIn;
  if (!resolveToolchain(tc, errs))
    return false;

  if (!keepC.empty()) {
    std::ostringstream src;
    if (!emitC(program, src, errs))
      return false;
    std::ofstream out(keepC, std::ios::binary);
    out << src.str();
    if (!out) {
      errs << "cannot write '" << keepC << "'\n";
      return false;
    }
  }

  TempDir tmp;
  if (tmp.Path.empty()) {
    errs << "cannot create a temporary directory\n";
    return false;
  }

  std::vector<std::string> compileFlags = {
      tcnames::kFlagStd, tcnames::kFlagNoWarnings,
      tcnames::kFlagInclude + tc.RuntimeIncludeDir};
  for (const auto &f : tc.ExtraFlags)
    compileFlags.push_back(f);
  // Everything besides the module's C that its object depends on: the C
  // compiler and flags, the runtime header the C includes (its struct
  // layouts and prototypes are the runtime ABI), and this compiler's
  // version.  Stored, with a hash of the module's C, next to each cached
  // object (`.key`).
  std::string cacheKey = "cc:";
  cacheKey += tc.CC;
  for (const auto &f : compileFlags) {
    cacheKey += ' ';
    cacheKey += f;
  }
  cacheKey += "\nruntime.h:";
  cacheKey +=
      fnv1a(readFile(tc.RuntimeIncludeDir + "/" + tcnames::kRuntimeHeader));
  cacheKey += "\npaykan:";
  cacheKey += kVersion;
  cacheKey += '\n';

  // One translation unit per module; the object is reused from the cache
  // while the module's generated C and the cache key are unchanged.
  std::vector<std::string> objects;
  for (size_t mi = 0; mi < program.Modules.size(); ++mi) {
    std::ostringstream src;
    if (!emitModuleC(program, mi, src, errs))
      return false;
    std::string text = src.str();

    // An entry is valid when its `.key` (the cache key plus a hash of the
    // module's C) matches.  Every file of an entry is replaced by a rename,
    // and a rebuild drops the old `.key` first and writes the new one only
    // after the new object, so a concurrent build of the same module (a
    // shared import) never links a partial or mismatched object.
    std::string moduleKey = cacheKey + "c:" + fnv1a(text) + '\n';
    std::string cPath, oPath, keyPath, oTmp;
    bool cached = false;
    if (!tc.CacheDir.empty()) {
      std::string base = cacheEntryBase(tc, program.Modules[mi].Name);
      std::error_code dirErr;
      fs::create_directories(fs::path(base).parent_path(), dirErr);
      cPath = base + tcnames::kCExt;
      oPath = base + tcnames::kObjExt;
      keyPath = base + ".key";
      std::error_code existsErr; // set for a missing entry: not a failure
      cached = !dirErr && readFile(keyPath) == moduleKey &&
               readFile(cPath) == text && fs::exists(oPath, existsErr);
      if (!cached) {
        std::error_code rmErr; // a missing key is not a failure
        if (!dirErr)
          fs::remove(keyPath, rmErr);
        if (dirErr || rmErr || !writeFileAtomically(cPath, text)) {
          // Unwritable cache: build this module in the temporary directory.
          cPath.clear();
        } else {
          oTmp = tempSibling(oPath);
        }
      }
    }
    if (cPath.empty()) {
      cPath = tmp.Path + "/module" + std::to_string(mi) + tcnames::kCExt;
      oPath = tmp.Path + "/module" + std::to_string(mi) + tcnames::kObjExt;
      std::ofstream out(cPath, std::ios::binary);
      out << text;
      if (!out) {
        errs << "cannot write '" << cPath << "'\n";
        return false;
      }
    }
    if (!cached) {
      std::vector<std::string> args = compileFlags;
      args.push_back(tcnames::kFlagCompileOnly);
      args.push_back(cPath);
      args.push_back(tcnames::kFlagOutput);
      args.push_back(oTmp.empty() ? oPath : oTmp);
      int rc = spawn(tc.CC, args, &tc.CC, {}, errs);
      if (rc != 0) {
        if (!oTmp.empty()) {
          std::error_code ec;
          fs::remove(oTmp, ec);
        }
        errs << "C compilation of '" << program.Modules[mi].Name << "' failed ("
             << tc.CC << " exited with " << rc << ")\n";
        return false;
      }
      if (!oTmp.empty()) {
        if (!renameOver(oTmp, oPath)) {
          errs << "cannot write '" << oPath << "'\n";
          return false;
        }
        // Best effort: without a key the entry is rebuilt next time.
        writeFileAtomically(keyPath, moduleKey);
      }
    }
    objects.push_back(oPath);
  }

  return toolchain::linkExecutable(objects, outputPath, tc, errs);
}

int buildAndRun(const pir::Program &program,
                const std::vector<std::string> &args, bool trackHeap,
                const Toolchain &tc, std::ostream &errs) {
  TempDir tmp;
  if (tmp.Path.empty()) {
    errs << "cannot create a temporary directory\n";
    return -1;
  }
  std::string exe = tmp.Path + "/program";
  if (!buildExecutable(program, exe, tc, errs))
    return -1;
  // args[0] (the script path) becomes the program's argv[0]; an empty list
  // runs the program with argc == 0, exactly what the JIT path does.
  std::vector<std::string> progArgs(args.begin() + (args.empty() ? 0 : 1),
                                    args.end());
  const std::string *argv0 = args.empty() ? nullptr : &args[0];
  std::vector<std::pair<std::string, std::string>> env;
  if (trackHeap)
    env.emplace_back(cnames::kEnvTrackHeap, cnames::kEnvOn);
  if (args.empty())
    env.emplace_back(cnames::kEnvNoArgs, cnames::kEnvOn);
  std::fflush(stdout);
  std::fflush(stderr);
  return spawn(exe, progArgs, argv0, env, errs);
}

} // namespace paykan::backend_c
