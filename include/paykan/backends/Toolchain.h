// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The system toolchain shared by the backends that produce native programs:
// finding the C compiler / linker and the Paykan runtime, spawning them, and
// linking objects against libpaykan_runtime.a.  The C backend compiles its
// generated C with it; the llvm backend links the objects it emits with it.
//
// Standard C++ plus POSIX process spawning.

#pragma once

#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace paykan::toolchain {

/// Where the C compiler and the runtime are.
struct Toolchain {
  /// C compiler command; empty means `$CC`, then `cc` (in a coverage
  /// build, the C compiler that built the runtime).
  std::string CC;
  /// Extra flags appended to every compile (e.g. "-O2").
  std::vector<std::string> ExtraFlags;
  /// Path of libpaykan_runtime.a and of the directory holding Runtime.h.
  /// Empty: the build tree's runtime, then $PAYKAN_RUNTIME_DIR, then the
  /// install layout next to the executable, then the install location
  /// configured at build time.
  std::string RuntimeLib;
  std::string RuntimeIncludeDir;
  /// Object cache: one `.c` / `.o` pair per module under this directory
  /// (`<project root>/.paykan_cache`), reused while the module's generated
  /// C and its cache key (`.key`) are unchanged.  Empty: no cache, everything
  /// is compiled into the temporary build directory.
  std::string CacheDir;
  /// The project root cache entries are named relative to.
  std::string ProjectRoot;
};

/// Resolve the defaults of @p tc (compiler and runtime paths).  Returns false
/// with a message when the runtime cannot be found.  When the runtime is the
/// build tree's (or installed) one, the sanitizer and coverage flags the
/// compiler was built with are appended to tc.ExtraFlags: that runtime
/// archive is instrumented, so programs linked against it need them too.
bool resolveToolchain(Toolchain &tc, std::ostream &errs);

/// Directory of the running executable, or "".
std::string executableDir();

/// Run @p path with @p args after @p argv0 as the child's argv (no argv at
/// all when @p argv0 is null, i.e. argc == 0) and @p extraEnv added to the
/// environment.  Returns the exit status, 128 + signal, or -1.
int spawn(const std::string &path, const std::vector<std::string> &args,
          const std::string *argv0,
          const std::vector<std::pair<std::string, std::string>> &extraEnv,
          std::ostream &errs);

/// A fresh temporary directory, removed with its contents on destruction.
/// Path is empty when it could not be created.
struct TempDir {
  std::string Path;
  TempDir();
  ~TempDir();
  TempDir(const TempDir &) = delete;
  TempDir &operator=(const TempDir &) = delete;
};

/// Link @p objects with tc.ExtraFlags against the runtime (and libm) into
/// the executable @p outputPath, with tc.CC as the linker driver.  @p tc
/// must have been resolved (resolveToolchain).
bool linkExecutable(const std::vector<std::string> &objects,
                    const std::string &outputPath, const Toolchain &tc,
                    std::ostream &errs);

} // namespace paykan::toolchain
