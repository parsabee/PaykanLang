// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// C backend: compiling and running the emitted C with the system compiler.

#include "paykan/backends/c/CBackend.h"

#include "Version.h"

#include "CNames.h"
#include "Platform.h"

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

#ifndef PAYKAN_RUNTIME_LIB_PATH
#define PAYKAN_RUNTIME_LIB_PATH ""
#endif
#ifndef PAYKAN_RUNTIME_INCLUDE_DIR
#define PAYKAN_RUNTIME_INCLUDE_DIR ""
#endif
#ifndef PAYKAN_INSTALLED_RUNTIME_LIB
#define PAYKAN_INSTALLED_RUNTIME_LIB ""
#endif
#ifndef PAYKAN_INSTALLED_RUNTIME_INCLUDE_DIR
#define PAYKAN_INSTALLED_RUNTIME_INCLUDE_DIR ""
#endif
#ifndef PAYKAN_SANITIZER_FLAGS
#define PAYKAN_SANITIZER_FLAGS ""
#endif
#ifndef PAYKAN_COVERAGE_FLAGS
#define PAYKAN_COVERAGE_FLAGS ""
#endif
#ifndef PAYKAN_DEFAULT_CC
#define PAYKAN_DEFAULT_CC ""
#endif
#ifndef PAYKAN_DEFAULT_CC_FLAGS
#define PAYKAN_DEFAULT_CC_FLAGS ""
#endif

namespace paykan::backend_c {

namespace fs = std::filesystem;

namespace {

/// Directory of the running executable, or "".
std::string executableDir() {
  std::string exe = platform::executablePath();
  return exe.empty() ? "" : fs::path(exe).parent_path().string();
}

/// Append the space-separated words of @p flags to @p out.
void appendFlags(const std::string &flags, std::vector<std::string> &out) {
  std::istringstream words(flags);
  std::string f;
  while (words >> f)
    out.push_back(f);
}

/// A fresh temporary directory, removed with its contents on destruction.
/// Path is empty when it could not be created.
struct TempDir {
  std::string Path;
  TempDir() : Path(platform::makeTempDir("paykan-c-")) {}
  ~TempDir() {
    if (!Path.empty()) {
      std::error_code ec;
      fs::remove_all(Path, ec);
    }
  }
};

} // namespace

bool resolveToolchain(Toolchain &tc, std::ostream &errs) {
  if (tc.CC.empty()) {
    const char *cc = std::getenv(cnames::kEnvCC);
    // A coverage build names the compiler that built the runtime: its
    // profile runtime matches the archive's instrumentation.  It comes with
    // the flags it needs to find the system headers and libraries (the
    // macOS SDK for a clang that has no default one).
    const char *buildCC = PAYKAN_DEFAULT_CC;
    if (cc && cc[0]) {
      tc.CC = cc;
    } else if (buildCC[0]) {
      tc.CC = buildCC;
      appendFlags(PAYKAN_DEFAULT_CC_FLAGS, tc.ExtraFlags);
    } else {
      tc.CC = cnames::kDefaultCC;
    }
  }
  // The build tree's runtime archive is sanitizer- or coverage-instrumented
  // when the compiler was built that way: programs linked against it need
  // the same flags (compile and link).
  if (tc.RuntimeLib.empty()) {
    appendFlags(PAYKAN_SANITIZER_FLAGS, tc.ExtraFlags);
    appendFlags(PAYKAN_COVERAGE_FLAGS, tc.ExtraFlags);
  }
  auto exists = [](const std::string &p) {
    std::error_code ec;
    return !p.empty() && fs::exists(p, ec);
  };
  if (tc.RuntimeLib.empty() || tc.RuntimeIncludeDir.empty()) {
    std::vector<std::pair<std::string, std::string>> candidates;
    // 1. the build tree this compiler was built in
    candidates.emplace_back(PAYKAN_RUNTIME_LIB_PATH,
                            PAYKAN_RUNTIME_INCLUDE_DIR);
    // 2. $PAYKAN_RUNTIME_DIR/{lib,include}
    if (const char *env = std::getenv("PAYKAN_RUNTIME_DIR"))
      candidates.emplace_back(std::string(env) + "/lib/libpaykan_runtime.a",
                              std::string(env) + "/include/paykan");
    // 3. the install layout next to the executable (a relocated install)
    std::string exeDir = executableDir();
    if (!exeDir.empty()) {
      fs::path prefix = fs::path(exeDir).parent_path();
      candidates.emplace_back((prefix / "lib" / "libpaykan_runtime.a").string(),
                              (prefix / "include" / "paykan").string());
    }
    // 4. the install location configured at build time
    candidates.emplace_back(PAYKAN_INSTALLED_RUNTIME_LIB,
                            PAYKAN_INSTALLED_RUNTIME_INCLUDE_DIR);
    for (const auto &[lib, inc] : candidates) {
      if (exists(lib) && exists(inc + "/" + cnames::kRuntimeH)) {
        if (tc.RuntimeLib.empty())
          tc.RuntimeLib = lib;
        if (tc.RuntimeIncludeDir.empty())
          tc.RuntimeIncludeDir = inc;
        break;
      }
    }
  }
  if (!exists(tc.RuntimeLib) ||
      !exists(tc.RuntimeIncludeDir + "/" + cnames::kRuntimeH)) {
    errs << "cannot find the Paykan runtime (libpaykan_runtime.a and "
            "Runtime.h); set PAYKAN_RUNTIME_DIR\n";
    return false;
  }
  return true;
}

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
      cnames::kFlagStd, cnames::kFlagNoWarnings,
      cnames::kFlagInclude + tc.RuntimeIncludeDir};
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
  cacheKey += fnv1a(readFile(tc.RuntimeIncludeDir + "/" + cnames::kRuntimeH));
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
      cPath = base + cnames::kCExt;
      oPath = base + cnames::kObjExt;
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
      cPath = tmp.Path + "/module" + std::to_string(mi) + cnames::kCExt;
      oPath = tmp.Path + "/module" + std::to_string(mi) + cnames::kObjExt;
      std::ofstream out(cPath, std::ios::binary);
      out << text;
      if (!out) {
        errs << "cannot write '" << cPath << "'\n";
        return false;
      }
    }
    if (!cached) {
      std::vector<std::string> args = compileFlags;
      args.push_back(cnames::kFlagCompileOnly);
      args.push_back(cPath);
      args.push_back(cnames::kFlagOutput);
      args.push_back(oTmp.empty() ? oPath : oTmp);
      int rc = platform::spawn(tc.CC, args, &tc.CC, {}, errs);
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

  std::vector<std::string> link = objects;
  for (const auto &f : tc.ExtraFlags)
    link.push_back(f);
  link.push_back(tc.RuntimeLib);
  link.push_back(cnames::kFlagLibm);
  link.push_back(cnames::kFlagOutput);
  link.push_back(outputPath);
  int rc = platform::spawn(tc.CC, link, &tc.CC, {}, errs);
  if (rc != 0) {
    errs << "linking failed (" << tc.CC << " exited with " << rc << ")\n";
    return false;
  }
  return true;
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
  return platform::spawn(exe, progArgs, argv0, env, errs);
}

} // namespace paykan::backend_c
