// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The C path of the mlir backend (docs/design/mlir-backend.md): the shared
// lowering's output in EmitC, printed as C11 by MLIR's C emitter.

#pragma once

#include "paykan/Status.h"

#include <llvm/Support/raw_ostream.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/MLIRContext.h>
#include <mlir/Pass/PassManager.h>

namespace paykan::mlir_backend {

/// Load the dialects the C path produces (emitc).
void loadEmitCDialects(mlir::MLIRContext &ctx);

/// Add the passes from the shared lowering's dialects to EmitC: one C
/// translation unit of the operated-on module (one PIR module, or a program
/// of nested ones), with its includes and declarations and, when it holds
/// the program's main module, C's main().
void addEmitCPasses(mlir::PassManager &pm);

/// Print @p module, in EmitC (addEmitCPasses), as C11 on @p os.
Status translateToC(mlir::ModuleOp module, llvm::raw_ostream &os);

} // namespace paykan::mlir_backend
