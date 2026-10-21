// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The mlir backend's pipeline (docs/design/mlir-backend.md): PIR imported
// into the pir dialect, the shared lowering, then the LLVM path.

#pragma once

#include "paykan/Status.h"
#include "paykan/pir/PIR.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/MLIRContext.h>

#include <memory>
#include <string>

namespace paykan::mlir_backend {

/// The stages of the pipeline, in order: what --emit-mlir=<stage> prints.
enum class Stage {
  PIR,   // `pir`: the imported pir dialect
  SCF,   // `scf`: after the shared lowering (func, scf, arith, pir memory)
  LLVM,  // `llvm`: the LLVM dialect
  EmitC, // `emitc`: EmitC, the C translation unit (the other target)
};

/// The stage named @p name ("pir", "scf", "llvm", "emitc"), or false.
bool parseStage(const std::string &name, Stage &stage);

/// A context with every dialect the pipeline makes.
std::unique_ptr<mlir::MLIRContext> createContext();

/// Run the pipeline on @p module (one PIR module, or a program of nested
/// ones) from the imported pir dialect up to @p stage.
Status lowerTo(mlir::ModuleOp module, Stage stage);

/// The whole program as one C translation unit, on @p os.
Status emitProgramC(const pir::Program &program, llvm::raw_ostream &os);

/// The whole program in LLVM IR, one linked module named @p moduleName;
/// imported modules are served from the bitcode cache under @p projectRoot
/// (<module>.bc) when their PIR is unchanged.
StatusOr<std::unique_ptr<llvm::Module>>
compileProgram(const pir::Program &program, llvm::LLVMContext &ctx,
               const std::string &moduleName, const std::string &projectRoot);

} // namespace paykan::mlir_backend
