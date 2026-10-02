// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The C backend's portability layer (see Platform.h).  This is the only file
// of the backend that includes operating-system headers.

#include "Platform.h"

// -- Platform headers -------------------------------------------------------
// POSIX (every supported platform): process spawning, mkdtemp, getpid and
// access.
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
// macOS: _NSGetExecutablePath (realpath is in <cstdlib>).
#include <mach-o/dyld.h>
#elif defined(__linux__)
// Linux: the executable is the /proc/self/exe symlink, read with
// std::filesystem::read_symlink (<filesystem>); no further header.
#else
// Other POSIX systems: no executable-path query; executablePath() falls back
// to the $PATH search.
#endif

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace paykan::backend_c::platform {

namespace fs = std::filesystem;

namespace {

/// The OS's own answer for the running executable's path, or "".
std::string osExecutablePath() {
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
  return "";
#elif defined(__linux__)
  std::error_code ec;
  fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  return ec ? "" : exe.string();
#else
  return ""; // no OS query: the caller searches $PATH
#endif
}

/// The canonical path of the first executable @p name on $PATH, or "".
/// An empty $PATH entry means the current directory.
std::string searchPath(const std::string &name) {
  const char *pathEnv = std::getenv("PATH");
  if (!pathEnv)
    return "";
  std::string dirs = pathEnv;
  size_t start = 0;
  while (start <= dirs.size()) {
    size_t colon = dirs.find(':', start);
    std::string dir = dirs.substr(
        start, colon == std::string::npos ? std::string::npos : colon - start);
    fs::path cand = fs::path(dir.empty() ? "." : dir) / name;
    std::error_code ec;
    if (access(cand.c_str(), X_OK) == 0)
      return fs::canonical(cand, ec).string();
    if (colon == std::string::npos)
      break;
    start = colon + 1;
  }
  return "";
}

} // namespace

std::string executablePath() {
  std::string path = osExecutablePath();
  return path.empty() ? searchPath("paykan") : path;
}

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

std::string makeTempDir(const std::string &prefix) {
  std::string tmpl = (fs::temp_directory_path() / (prefix + "XXXXXX")).string();
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  return mkdtemp(buf.data()) ? std::string(buf.data()) : std::string();
}

unsigned long processId() { return static_cast<unsigned long>(getpid()); }

} // namespace paykan::backend_c::platform
