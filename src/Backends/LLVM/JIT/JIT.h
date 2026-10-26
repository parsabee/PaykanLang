// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Thin wrapper around LLVM ORC LLJIT for Paykan JIT execution.

#pragma once

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/Error.h>

#include <memory>
#include <string>
#include <vector>

namespace paykan::jit {

/// Give @p module the target triple and data layout runModule() executes it
/// with (the host's, as the JIT detects it).  Call it before optimising the
/// module: the optimiser folds sizes and field offsets under the module's
/// layout, and LLVM's default one lays out an i64 after an i8 at offset 4.
llvm::Error setHostLayout(llvm::Module &module);

/// Run the "main" function inside the given module via ORC JIT.
/// The module is consumed (moved into the JIT).
/// @param args  Arguments passed to the Paykan main function as its
///              `view Str[]` slice.
///              args[0] should be the script path (like argv[0] for scripts).
/// @param nativeObjects  Object files and archives defining the program's
///              native functions (--object, #198; position-independent),
///              linked into the JIT with the module (an archive's members
///              as a linker takes them, when they define a symbol still
///              undefined).
/// @param codeGen  How hard the JIT's code generator optimises, as a native
///              build's does at the same -O.
/// Returns the exit code from main(), or an Error on failure.
llvm::Expected<int>
runModule(std::unique_ptr<llvm::Module> module,
          std::unique_ptr<llvm::LLVMContext> ctx,
          const std::vector<std::string> &args = {},
          const std::vector<std::string> &nativeObjects = {},
          llvm::CodeGenOptLevel codeGen = llvm::CodeGenOptLevel::Default);

} // namespace paykan::jit
