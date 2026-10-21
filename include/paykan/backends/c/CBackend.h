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

#include "paykan/backends/c/CBuild.h"
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

/// The emitter as a CSource (emitC, emitModuleC).
CSource emitterSource();

/// Emit C, compile and link it into @p outputPath (paykan/backends/c/
/// CBuild.h, with the emitter).  @p keepC, when non-empty, is where the C
/// source is also written.
bool buildExecutable(const pir::Program &program, const std::string &outputPath,
                     const Toolchain &tc, std::ostream &errs,
                     const std::string &keepC = "");

/// Build into a temporary directory and run, with the emitter (CBuild.h).
int buildAndRun(const pir::Program &program,
                const std::vector<std::string> &args, bool trackHeap,
                const Toolchain &tc, std::ostream &errs);

} // namespace paykan::backend_c
