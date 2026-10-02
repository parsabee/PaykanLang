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

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

namespace paykan::toolchain {

namespace fs = std::filesystem;

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
