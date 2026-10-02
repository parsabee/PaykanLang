// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The C backend: translates a PIR program into C11 against the runtime's
// Runtime.h, and builds / runs it with the system C compiler.
//
// Standard C++ only, plus the POSIX calls behind the shared toolchain's
// portability layer (paykan/backends/Toolchain.h, src/Backends/Toolchain/
// Platform.h: Linux and macOS).

#pragma once

#include "paykan/backends/Toolchain.h"
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

/// Where the C compiler and the runtime are, and their lookup: shared with
/// the llvm backend (paykan/backends/Toolchain.h).
using toolchain::resolveToolchain;
using toolchain::Toolchain;

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
