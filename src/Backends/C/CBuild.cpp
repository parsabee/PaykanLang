// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// C backend: compiling and running the emitted C with the system compiler.

#include "paykan/backends/c/CBackend.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace paykan::backend_c {

namespace fs = std::filesystem;

// The process / runtime / link helpers are shared with the llvm backend
// (paykan/backends/Toolchain.h).
using toolchain::spawn;
using toolchain::TempDir;

namespace {

std::string readFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return std::string(1, '\0'); // never equal to real content
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

/// Write @p text to @p path through a temporary file in the same directory
/// and a rename, so a concurrent or interrupted compile never observes a
/// partially written entry.
bool writeFileAtomically(const std::string &path, const std::string &text) {
  std::string tmp = path + ".tmp" + std::to_string(getpid());
  {
    std::ofstream out(tmp, std::ios::binary);
    out << text;
    if (!out)
      return false;
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec)
    fs::remove(tmp, ec);
  return !ec;
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

  std::vector<std::string> compileFlags = {"-std=c11", "-w",
                                           "-I" + tc.RuntimeIncludeDir};
  for (const auto &f : tc.ExtraFlags)
    compileFlags.push_back(f);
  std::string flagsId = tc.CC;
  for (const auto &f : compileFlags)
    flagsId += " " + f;
  flagsId += "\n";

  // One translation unit per module; the object is reused from the cache
  // while the module's generated C and the compiler flags are unchanged.
  std::vector<std::string> objects;
  for (size_t mi = 0; mi < program.Modules.size(); ++mi) {
    std::ostringstream src;
    if (!emitModuleC(program, mi, src, errs))
      return false;
    std::string text = src.str();

    std::string cPath, oPath;
    bool cached = false;
    if (!tc.CacheDir.empty()) {
      std::string base = cacheEntryBase(tc, program.Modules[mi].Name);
      std::error_code dirErr;
      fs::create_directories(fs::path(base).parent_path(), dirErr);
      cPath = base + ".c";
      oPath = base + ".o";
      std::error_code existsErr; // set for a missing entry: not a failure
      cached = !dirErr && fs::exists(oPath, existsErr) &&
               readFile(cPath) == text && readFile(base + ".flags") == flagsId;
      if (!cached && (dirErr || !writeFileAtomically(cPath, text) ||
                      !writeFileAtomically(base + ".flags", flagsId))) {
        // Unwritable cache: build this module in the temporary directory.
        cPath.clear();
      }
    }
    if (cPath.empty()) {
      cPath = tmp.Path + "/module" + std::to_string(mi) + ".c";
      oPath = tmp.Path + "/module" + std::to_string(mi) + ".o";
      std::ofstream out(cPath, std::ios::binary);
      out << text;
      if (!out) {
        errs << "cannot write '" << cPath << "'\n";
        return false;
      }
    }
    if (!cached) {
      std::vector<std::string> args = compileFlags;
      args.push_back("-c");
      args.push_back(cPath);
      args.push_back("-o");
      args.push_back(oPath);
      int rc = spawn(tc.CC, args, &tc.CC, {}, errs);
      if (rc != 0) {
        errs << "C compilation of '" << program.Modules[mi].Name << "' failed ("
             << tc.CC << " exited with " << rc << ")\n";
        return false;
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
    env.emplace_back("PAYKAN_TRACK_HEAP", "1");
  if (args.empty())
    env.emplace_back("PAYKAN_NO_ARGS", "1");
  std::fflush(stdout);
  std::fflush(stderr);
  return spawn(exe, progArgs, argv0, env, errs);
}

} // namespace paykan::backend_c
