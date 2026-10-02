// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The C backend: translates a PIR program into C11 against the runtime's
// Runtime.h, and builds / runs it with the system C compiler.
//
// Standard C++ only, plus the POSIX calls behind the backend's portability
// layer (src/Backends/C/Platform.h: Linux and macOS).

#pragma once

#include "paykan/pir/PIR.h"

#include <ostream>
#include <string>
#include <vector>

namespace paykan::backend_c {

/// Write the whole program as one C translation unit to @p out.  Returns
/// false (with a message on @p errs) when the program references something
/// no module defines.
bool emitC(const pir::Program &program, std::ostream &out, std::ostream &errs);

/// Write only module @p module (an index into program.Modules) as a
/// translation unit of its own: what it defines plus declarations of what it
/// imports.  `build` / `run` compile one such unit per module and cache the
/// objects.
bool emitModuleC(const pir::Program &program, size_t module, std::ostream &out,
                 std::ostream &errs);

/// Where the C compiler and the runtime are.
struct Toolchain {
  /// C compiler command; empty means `$CC`, then `cc`.
  std::string CC;
  /// Extra flags appended to every compile (e.g. "-O2").
  std::vector<std::string> ExtraFlags;
  /// Path of libpaykan_runtime.a and of the directory holding Runtime.h.
  /// Empty: the paths baked in at build time (then the install layout next
  /// to the executable, then $PAYKAN_RUNTIME_DIR).
  std::string RuntimeLib;
  std::string RuntimeIncludeDir;
  /// Object cache: one `.c` / `.o` pair per module under this directory
  /// (`<project root>/.paykan_cache`), reused while the module's generated
  /// C and the compiler flags are unchanged.  Empty: no cache, everything
  /// is compiled into the temporary build directory.
  std::string CacheDir;
  /// The project root cache entries are named relative to.
  std::string ProjectRoot;
};

/// Resolve the defaults of @p tc (compiler and runtime paths).  Returns false
/// with a message when the runtime cannot be found.
bool resolveToolchain(Toolchain &tc, std::ostream &errs);

/// Emit C, compile and link it into @p outputPath.  @p keepC, when
/// non-empty, is where the C source is also written.
bool buildExecutable(const pir::Program &program, const std::string &outputPath,
                     const Toolchain &tc, std::ostream &errs,
                     const std::string &keepC = "");

/// Build into a temporary directory and run.  @p args[0] is the script path
/// (becomes the program's args[0]); @p trackHeap selects the tracking
/// allocator in the child (its stats are printed to stderr at exit).  Returns
/// the program's exit status, 128 + signal when it died from a signal, or -1
/// when it could not be built or started.
int buildAndRun(const pir::Program &program,
                const std::vector<std::string> &args, bool trackHeap,
                const Toolchain &tc, std::ostream &errs);

} // namespace paykan::backend_c
