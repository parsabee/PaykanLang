// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The mlir backend's LLVM path (docs/design/mlir-backend.md): the shared
// lowering's output into the LLVM dialect -- upstream conversions for func,
// scf, cf and arith, and a pattern for each remaining pir op -- then LLVM IR.

#pragma once

#include "paykan/Status.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/Pass/PassManager.h>

#include <memory>
#include <string>

namespace paykan::mlir_backend {

/// Load the dialects the LLVM path produces, and register their translation
/// to LLVM IR.
void loadLLVMDialects(mlir::MLIRContext &ctx);

/// Append to @p pm the passes taking the shared lowering's output to the
/// LLVM dialect.
void addLLVMDialectPasses(mlir::PassManager &pm);

/// One module in the LLVM dialect as an LLVM module named @p name.
StatusOr<std::unique_ptr<llvm::Module>>
translateToLLVMIR(mlir::ModuleOp module, llvm::LLVMContext &ctx,
                  const std::string &name);

} // namespace paykan::mlir_backend
