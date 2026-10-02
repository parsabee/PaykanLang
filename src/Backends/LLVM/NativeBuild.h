// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Ahead-of-time compilation in the `llvm` backend: the C entry point of an
// executable and native object emission for the host.

#pragma once

#include "paykan/Status.h"

#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>

#include <memory>
#include <string>

namespace paykan::backend::llvm_backend {

/// Turn the program's `main` into the C entry point an executable needs.
/// The Paykan `main` (`() -> i64` or `(args: Str[]) -> i64`) is renamed and
/// made internal, and a `int main(int argc, char **argv)` is added that does
/// what the C backend's generated main does: honour PAYKAN_TRACK_HEAP
/// (tracking allocator + heap dump at exit) and PAYKAN_NO_ARGS (run with an
/// empty argument list), pass argv as the `Str[]` argument, and return the
/// program's result as the exit status.
Status addEntryPoint(llvm::Module &module);

/// A TargetMachine for the host (default triple, host CPU and features,
/// position-independent code) generating code at -O<optLevel>.
StatusOr<std::unique_ptr<llvm::TargetMachine>>
createHostTargetMachine(unsigned optLevel);

/// Write @p module as a native object file to @p path.  The module's triple
/// and data layout should be @p tm's.
Status writeObjectFile(llvm::Module &module, llvm::TargetMachine &tm,
                       const std::string &path);

} // namespace paykan::backend::llvm_backend
