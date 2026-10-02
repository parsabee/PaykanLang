// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The system toolchain shared by the native backends (C and llvm): runtime
// lookup, process spawning and linking.

#include "paykan/backends/Toolchain.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

namespace paykan::toolchain {

namespace fs = std::filesystem;

namespace {

/// Path of the running executable, or "".  The OS's own answer where there
/// is one; otherwise the first `paykan` on $PATH.
std::string executablePath() {
#if defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size); // sets the size needed
  std::vector<char> buf(size + 1, '\0');
  if (_NSGetExecutablePath(buf.data(), &size) == 0) {
    if (char *real = realpath(buf.data(), nullptr)) {
      std::string path = real;
      std::free(real);
      return path;
    }
  }
#elif defined(__linux__)
  std::error_code ec;
  fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (!ec)
    return exe.string();
#endif
  if (const char *pathEnv = std::getenv("PATH")) {
    std::string dirs = pathEnv;
    size_t start = 0;
    while (start <= dirs.size()) {
      size_t colon = dirs.find(':', start);
      std::string dir =
          dirs.substr(start, colon == std::string::npos ? std::string::npos
                                                        : colon - start);
      fs::path cand = fs::path(dir.empty() ? "." : dir) / "paykan";
      std::error_code ec;
      if (access(cand.c_str(), X_OK) == 0)
        return fs::canonical(cand, ec).string();
      if (colon == std::string::npos)
        break;
      start = colon + 1;
    }
  }
  return "";
}

/// Append the space-separated words of @p flags to @p out.
void appendFlags(const std::string &flags, std::vector<std::string> &out) {
  std::istringstream words(flags);
  std::string f;
  while (words >> f)
    out.push_back(f);
}

} // namespace

/// Directory of the running executable, or "".
std::string executableDir() {
  std::string exe = executablePath();
  return exe.empty() ? "" : fs::path(exe).parent_path().string();
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

TempDir::TempDir() {
  std::string tmpl = (fs::temp_directory_path() / "paykan-c-XXXXXX").string();
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  if (mkdtemp(buf.data()))
    Path = buf.data();
}

TempDir::~TempDir() {
  if (!Path.empty()) {
    std::error_code ec;
    fs::remove_all(Path, ec);
  }
}

bool resolveToolchain(Toolchain &tc, std::ostream &errs) {
  if (tc.CC.empty()) {
    const char *cc = std::getenv("CC");
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
      tc.CC = "cc";
    }
  }
  // The build tree's runtime archive is sanitizer- or coverage-instrumented
  // when the compiler was built that way: programs linked against it need
  // the same flags (the C backend's compile and every backend's link).
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

bool linkExecutable(const std::vector<std::string> &objects,
                    const std::string &outputPath, const Toolchain &tc,
                    std::ostream &errs) {
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

} // namespace paykan::toolchain
