// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Building C translations of a PIR program with the system C compiler: one
// translation unit per module, each object cached while its C and the
// toolchain are unchanged, linked with the runtime.  Shared by the backends
// that translate PIR to C (the c backend's emitter, the mlir backend's
// EmitC path, docs/design/mlir-backend.md).
//
// Standard C++ only, plus the POSIX calls behind the shared toolchain's
// portability layer (paykan/backends/Toolchain.h, src/Backends/Toolchain/
// Platform.h: Linux and macOS).

#pragma once

#include "paykan/backends/Toolchain.h"
#include "paykan/pir/PIR.h"

#include <functional>
#include <ostream>
#include <string>
#include <vector>

namespace paykan::backend_c {

/// Where the C compiler and the runtime are, and their lookup: shared with
/// the llvm backend (paykan/backends/Toolchain.h).
using toolchain::resolveToolchain;
using toolchain::Toolchain;

/// A translation of PIR to C: each writes C on its first stream, or returns
/// false with a message on the second.
struct CSource {
  /// The whole program as one translation unit.
  std::function<bool(const pir::Program &, std::ostream &, std::ostream &)>
      Program;
  /// Only module @p module (an index into program.Modules), as a
  /// translation unit of its own: what it defines plus declarations of what
  /// it imports.
  std::function<bool(const pir::Program &, size_t module, std::ostream &,
                     std::ostream &)>
      Module;
};

/// Translate with @p source, compile and link into @p outputPath.
/// @p keepC, when non-empty, is where the whole program's C is also
/// written.
bool buildExecutable(const pir::Program &program, const CSource &source,
                     const std::string &outputPath, const Toolchain &tc,
                     std::ostream &errs, const std::string &keepC = "");

/// Build into a temporary directory and run.  @p args[0] is the script path
/// (becomes the program's args[0]); @p trackHeap selects the tracking
/// allocator in the child (its stats are printed to stderr at exit).  Returns
/// the program's exit status, 128 + signal when it died from a signal, or -1
/// when it could not be built or started.
int buildAndRun(const pir::Program &program, const CSource &source,
                const std::vector<std::string> &args, bool trackHeap,
                const Toolchain &tc, std::ostream &errs);

} // namespace paykan::backend_c
