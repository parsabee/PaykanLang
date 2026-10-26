// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// The system toolchain shared by the backends that produce native programs:
// finding the C compiler / linker and the Paykan runtime, spawning them, and
// linking objects against libpaykan_runtime.a.  The mlir backend links the
// objects it emits with it; a backend plugin that emits C can compile its C
// with it too.
//
// Standard C++ plus POSIX, every OS call behind the portability layer in
// src/Backends/Toolchain/Platform.h (Linux and macOS side by side).

#pragma once

#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace paykan::toolchain {

/// Where the C compiler and the runtime are.
struct Toolchain {
  /// C compiler program; empty means `$CC`'s first word (its other words go
  /// to the front of ExtraFlags), then `cc` (in a coverage build, the C
  /// compiler that built the runtime).
  std::string CC;
  /// Extra flags, right after CC in every compile (e.g. "-O2").
  std::vector<std::string> ExtraFlags;
  /// Object files or archives linked into every executable: the objects
  /// defining a program's native functions (--object, #198).
  std::vector<std::string> ExtraObjects;
  /// Path of libpaykan_runtime.a and of the directory holding Runtime.h.
  /// Empty: $PAYKAN_RUNTIME_DIR, then the runtime recorded with
  /// setPackageRuntime(), then the install layout around the executable
  /// (`<prefix>/bin` -> `<prefix>/lib`, `<prefix>/include/paykan`), then the
  /// build tree's runtime (only for a binary inside that build tree), then
  /// the install location configured at build time.
  std::string RuntimeLib;
  std::string RuntimeIncludeDir;
};

/// Resolve the defaults of @p tc (compiler and runtime paths).  Returns false
/// with a message when the runtime cannot be found.  When the runtime is the
/// build tree's (or installed) one, the sanitizer and coverage flags the
/// compiler was built with are appended to tc.ExtraFlags: that runtime
/// archive is instrumented, so programs linked against it need them too.
bool resolveToolchain(Toolchain &tc, std::ostream &errs);

/// Record the runtime of the installed Paykan package this program was
/// built against: resolveToolchain() tries it right after
/// $PAYKAN_RUNTIME_DIR.  A driver made with `paykan_add_driver`
/// (PaykanConfig.cmake) calls it at startup with the package's
/// PAYKAN_RUNTIME_LIBRARY and PAYKAN_RUNTIME_INCLUDE_DIR, so an out-of-tree
/// driver finds the runtime wherever its own executable lives.
void setPackageRuntime(std::string lib, std::string includeDir);

/// Directory of the running executable, or "".
std::string executableDir();

/// Run @p path with @p argv0 and then @p args as the child's argv and
/// @p extraEnv added to the environment.  Returns the exit status, 128 +
/// signal, or -1 with a message on @p errs when it could not be run.
int spawn(const std::string &path, const std::vector<std::string> &args,
          const std::string &argv0,
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
