// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Thin wrapper around LLVM ORC LLJIT for Paykan JIT execution.

#ifndef PAYKAN_JIT_H
#define PAYKAN_JIT_H

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/Error.h>

#include <memory>

namespace paykan::jit {

/// Run the "main" function inside the given module via ORC JIT.
/// The module is consumed (moved into the JIT).
/// Returns the exit code from main(), or an Error on failure.
llvm::Expected<int> runModule(std::unique_ptr<llvm::Module> module,
                              std::unique_ptr<llvm::LLVMContext> ctx);

} // namespace paykan::jit

#endif // PAYKAN_JIT_H
