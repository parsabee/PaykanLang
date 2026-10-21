// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Running and building a program's linked LLVM module, for the backends
// that produce one (llvm, mlir): in process with the JIT (`run`), or ahead
// of time as a native object with the C entry point an executable needs,
// linked against the runtime with the system toolchain (`build`).

#pragma once

#include "paykan/Status.h"

#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace paykan::llvm_codegen {

/// Turn the program's `main` into the C entry point an executable needs.
/// The Paykan `main` (`() -> i64`, or `(ptr, i64) -> i64` for its `args:
/// view Str[]` slice) is renamed and made internal, and a `int main(int argc,
/// char **argv)` is added that does what the C path's main() does
/// (ToEmitC/): honour PAYKAN_TRACK_HEAP (tracking allocator + heap dump at
/// exit), pass argv as the slice (Paykan_args_new, which honours
/// PAYKAN_NO_ARGS), and return the program's result as the exit status.
Status addEntryPoint(llvm::Module &module);

/// A TargetMachine for the host (default triple, host CPU and features,
/// position-independent code) generating code at -O<optLevel>.
StatusOr<std::unique_ptr<llvm::TargetMachine>>
createHostTargetMachine(unsigned optLevel);

/// Write @p module as a native object file to @p path.  The module's triple
/// and data layout should be @p tm's.
Status writeObjectFile(llvm::Module &module, llvm::TargetMachine &tm,
                       const std::string &path);

/// Put @p module under the host's layout, which the JIT runs it with, and
/// optimise it at -O@p optLevel.
Status prepareForJIT(llvm::Module &module, unsigned optLevel);

/// Run the module prepared by prepareForJIT() in process: @p args are the
/// program's arguments (args[0] the script path), @p objects define its
/// native functions; with @p trackHeap the runtime's tracking allocator
/// counts its blocks and the statistics are printed at exit.
StatusOr<int> runInProcess(std::unique_ptr<llvm::Module> module,
                           std::unique_ptr<llvm::LLVMContext> ctx,
                           std::span<const std::string> args,
                           const std::vector<std::string> &objects,
                           bool trackHeap);

/// The program as a native object for the host at @p path: @p module (not
/// yet optimised) with the C entry point, optimised at -O@p optLevel.
Status emitHostObject(llvm::Module &module, unsigned optLevel,
                      const std::string &path);

/// `build`: the object @p emitObject writes to the path it is given, linked
/// with @p objects against the runtime into the executable @p output.
Status buildHostExecutable(
    const std::function<Status(const std::string &)> &emitObject,
    const std::vector<std::string> &objects, const std::string &output);

} // namespace paykan::llvm_codegen
