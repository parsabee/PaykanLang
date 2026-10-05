// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The compiler's portability layer (see Platform.h).  This is the only file
// of the compiler that includes operating-system headers.

#include "Platform.h"

// -- Platform headers -------------------------------------------------------
// POSIX (every supported platform): process spawning, mkdtemp, getpid and
// access, and the dynamic loader (dlopen / dlsym; in libc on current glibc
// and on macOS, libdl on older glibc: CMAKE_DL_LIBS).
#if !defined(_WIN32)
#include <dlfcn.h>
#endif
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
// macOS: _NSGetExecutablePath (realpath is in <cstdlib>).
#include <mach-o/dyld.h>
#elif defined(__linux__)
// Linux: the executable is the /proc/self/exe symlink, read with
// std::filesystem::read_symlink (<filesystem>).  <elf.h>: the ELF headers
// a plugin file is checked against before dlopen (checkElfFile below).
#include <elf.h>
#else
// Other POSIX systems: no executable-path query; executablePath() falls back
// to the $PATH search.
#endif

#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace paykan::toolchain::platform {

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

#if defined(_WIN32)
// The Windows stub: plugins are not supported there yet.  The
// implementation will be LoadLibraryW (with the path converted to UTF-16),
// GetProcAddress and FormatMessageW for the error.  (The rest of this file
// is POSIX-only too, so Windows is not a build target yet.)
void *loadLibrary(const std::string &path, std::string &error) {
  error = "cannot load '" + path +
          "': loading plugins is not supported on Windows yet";
  return nullptr;
}

void *librarySymbol(void *, const char *) { return nullptr; }

const char *sharedLibrarySuffix() { return ".dll"; }
#else
#if defined(__linux__)
/// Why the ELF file @p f (@p size bytes) can't be handed to dlopen, or "".
template <typename Ehdr, typename Phdr>
std::string checkElfSegments(std::FILE *f, uint64_t size) {
  Ehdr eh;
  if (std::fseek(f, 0, SEEK_SET) != 0 || std::fread(&eh, sizeof eh, 1, f) != 1)
    return "the file is truncated (it ends inside its ELF header)";
  if (eh.e_phentsize != sizeof(Phdr))
    return ""; // unusual; dlopen judges it
  if (uint64_t(eh.e_phoff) + uint64_t(eh.e_phnum) * sizeof(Phdr) > size)
    return "the file is truncated (it ends inside its program headers)";
  for (uint64_t i = 0; i < eh.e_phnum; ++i) {
    Phdr ph;
    if (std::fseek(f, static_cast<long>(eh.e_phoff + i * sizeof(Phdr)),
                   SEEK_SET) != 0 ||
        std::fread(&ph, sizeof ph, 1, f) != 1)
      return "the file is truncated (it ends inside its program headers)";
    uint64_t end = uint64_t(ph.p_offset) + uint64_t(ph.p_filesz);
    if (ph.p_type == PT_LOAD && end > size)
      return "the file is truncated or corrupt (a loadable segment ends at "
             "byte " +
             std::to_string(end) + ", the file has " + std::to_string(size) +
             ")";
  }
  return "";
}

/// dlopen maps a library's loadable segments without checking that the file
/// holds them, and the first access to a page past the end of the file
/// raises SIGBUS: a truncated plugin (an interrupted copy, download or
/// install) would kill paykan, in every command, before it could report
/// the file as rejected.  So check the headers against the file's size
/// first.  "" when the file is complete or isn't a native ELF file (dlopen
/// then reports what is wrong with it).
std::string checkElfFile(const std::string &path) {
  std::error_code ec;
  uint64_t size = std::filesystem::file_size(path, ec);
  if (ec)
    return "";
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (!f)
    return "";
  unsigned char ident[EI_NIDENT];
  std::string why;
  constexpr unsigned char kNativeData =
      std::endian::native == std::endian::little ? ELFDATA2LSB : ELFDATA2MSB;
  if (std::fread(ident, 1, EI_NIDENT, f) == EI_NIDENT &&
      std::memcmp(ident, ELFMAG, SELFMAG) == 0 &&
      ident[EI_DATA] == kNativeData) {
    if (ident[EI_CLASS] == ELFCLASS64)
      why = checkElfSegments<Elf64_Ehdr, Elf64_Phdr>(f, size);
    else if (ident[EI_CLASS] == ELFCLASS32)
      why = checkElfSegments<Elf32_Ehdr, Elf32_Phdr>(f, size);
  }
  std::fclose(f);
  return why;
}
#endif

void *loadLibrary(const std::string &path, std::string &error) {
#if defined(__linux__)
  if (std::string why = checkElfFile(path); !why.empty()) {
    error = path + ": " + why;
    return nullptr;
  }
#endif
  dlerror(); // clear a stale message
  void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const char *msg = dlerror();
    error = msg ? msg : "cannot load the library";
  }
  return handle;
}

void *librarySymbol(void *library, const char *name) {
  return library ? dlsym(library, name) : nullptr;
}

const char *sharedLibrarySuffix() {
#if defined(__APPLE__)
  return ".dylib";
#else
  return ".so";
#endif
}
#endif // _WIN32

} // namespace paykan::toolchain::platform
