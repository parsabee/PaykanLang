// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Thin wrapper around LLVM ORC LLJIT for Paykan JIT execution.

#pragma once

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/Error.h>

#include <memory>
#include <string>
#include <vector>

namespace paykan::jit {

/// Run the "main" function inside the given module via ORC JIT.
/// The module is consumed (moved into the JIT).
/// @param args  Arguments forwarded to the Paykan main function as Str[].
///              args[0] should be the script path (like argv[0] for scripts).
/// Returns the exit code from main(), or an Error on failure.
llvm::Expected<int> runModule(std::unique_ptr<llvm::Module> module,
                              std::unique_ptr<llvm::LLVMContext> ctx,
                              const std::vector<std::string> &args = {});

} // namespace paykan::jit
