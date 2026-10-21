// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// PIR -> the `pir` MLIR dialect: one op per PIR item, statement and
// instruction (Dialect/PIR.td).

#pragma once

#include "paykan/Status.h"
#include "paykan/pir/PIR.h"

#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/MLIRContext.h>
#include <mlir/IR/OwningOpRef.h>

#include <cstddef>

namespace paykan::mlir_backend {

/// Load the dialects the import produces (pir, arith) into @p ctx.
void loadImportDialects(mlir::MLIRContext &ctx);

/// Module @p index of the verified @p program as a `builtin.module` named
/// after it, holding its globals, classes and functions.
StatusOr<mlir::OwningOpRef<mlir::ModuleOp>>
importModule(const pir::Program &program, size_t index, mlir::MLIRContext &ctx);

/// @p module alone (the program's main module when @p isMain).
StatusOr<mlir::OwningOpRef<mlir::ModuleOp>>
importModule(const pir::Module &module, bool isMain, mlir::MLIRContext &ctx);

/// Every module of @p program (main module first), each a nested module of
/// the result.
StatusOr<mlir::OwningOpRef<mlir::ModuleOp>>
importProgram(const pir::Program &program, mlir::MLIRContext &ctx);

} // namespace paykan::mlir_backend
