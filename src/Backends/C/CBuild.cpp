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

#ifndef PAYKAN_RUNTIME_LIB_PATH
#define PAYKAN_RUNTIME_LIB_PATH ""
#endif
#ifndef PAYKAN_RUNTIME_INCLUDE_DIR
#define PAYKAN_RUNTIME_INCLUDE_DIR ""
#endif
#ifndef PAYKAN_SANITIZER_FLAGS
#define PAYKAN_SANITIZER_FLAGS ""
#endif

namespace paykan::backend_c {

namespace fs = std::filesystem;

namespace {

/// Directory of the running executable, or "".
std::string executableDir() {
  std::error_code ec;
  fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (ec)
    return "";
  return exe.parent_path().string();
}

/// Run @p path with @p args after @p argv0 as the child's argv (no argv at
/// all when @p argv0 is null, i.e. argc == 0) and @p extraEnv added to the
/// environment.  Returns the exit status, 128 + signal, or -1.
int spawn(const std::string &path, const std::vector<std::string> &args,
          const std::string *argv0,
          const std::vector<std::pair<std::string, std::string>> &extraEnv,
          std::ostream &errs) {
  pid_t pid = fork();
  if (pid < 0) {
    errs << "fork failed: " << std::strerror(errno) << "\n";
    return -1;
  }
  if (pid == 0) {
    for (const auto &[k, v] : extraEnv)
      setenv(k.c_str(), v.c_str(), 1);
    std::vector<char *> cargv;
    if (argv0)
      cargv.push_back(const_cast<char *>(argv0->c_str()));
    for (const auto &a : args)
      cargv.push_back(const_cast<char *>(a.c_str()));
    cargv.push_back(nullptr);
    execvp(path.c_str(), cargv.data());
    std::fprintf(stderr, "exec of '%s' failed: %s\n", path.c_str(),
                 std::strerror(errno));
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      errs << "waitpid failed: " << std::strerror(errno) << "\n";
      return -1;
    }
  }
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return -1;
}

struct TempDir {
  std::string Path;
  TempDir() {
    std::string tmpl = (fs::temp_directory_path() / "paykan-c-XXXXXX").string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (mkdtemp(buf.data()))
      Path = buf.data();
  }
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
    const char *cc = std::getenv("CC");
    tc.CC = (cc && cc[0]) ? cc : "cc";
  }
  // The build tree's runtime archive is sanitizer-instrumented when the
  // compiler was built with a sanitizer: programs linked against it need
  // the same flags (compile and link).
  if (tc.RuntimeLib.empty()) {
    std::string flags = PAYKAN_SANITIZER_FLAGS;
    size_t start = 0;
    while (start < flags.size()) {
      size_t sp = flags.find(' ', start);
      std::string f = flags.substr(
          start, sp == std::string::npos ? std::string::npos : sp - start);
      if (!f.empty())
        tc.ExtraFlags.push_back(f);
      if (sp == std::string::npos)
        break;
      start = sp + 1;
    }
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
    // 3. the install layout next to the executable
    std::string exeDir = executableDir();
    if (!exeDir.empty()) {
      fs::path prefix = fs::path(exeDir).parent_path();
      candidates.emplace_back((prefix / "lib" / "libpaykan_runtime.a").string(),
                              (prefix / "include" / "paykan").string());
    }
    for (const auto &[lib, inc] : candidates) {
      if (exists(lib) && exists(inc + "/Runtime.h")) {
        if (tc.RuntimeLib.empty())
          tc.RuntimeLib = lib;
        if (tc.RuntimeIncludeDir.empty())
          tc.RuntimeIncludeDir = inc;
        break;
      }
    }
  }
  if (!exists(tc.RuntimeLib) || !exists(tc.RuntimeIncludeDir + "/Runtime.h")) {
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

  std::vector<std::string> link = objects;
  for (const auto &f : tc.ExtraFlags)
    link.push_back(f);
  link.push_back(tc.RuntimeLib);
  link.push_back("-lm");
  link.push_back("-o");
  link.push_back(outputPath);
  int rc = spawn(tc.CC, link, &tc.CC, {}, errs);
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
    env.emplace_back("PAYKAN_TRACK_HEAP", "1");
  if (args.empty())
    env.emplace_back("PAYKAN_NO_ARGS", "1");
  std::fflush(stdout);
  std::fflush(stderr);
  return spawn(exe, progArgs, argv0, env, errs);
}

} // namespace paykan::backend_c
