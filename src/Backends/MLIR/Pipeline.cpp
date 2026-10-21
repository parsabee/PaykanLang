// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The mlir backend's pipeline (see Pipeline.h).

#include "Pipeline.h"

#include "Import/PIRImport.h"
#include "Lowering/PIRToStandard.h"
#include "ModuleCache.h"
#include "ToEmitC/ToEmitC.h"
#include "ToLLVM/ToLLVM.h"

#include <llvm/Support/raw_ostream.h>
#include <mlir/IR/Diagnostics.h>
#include <mlir/Pass/PassManager.h>

namespace paykan::mlir_backend {

namespace {

/// The backend's entries in the bitcode cache: <module>.bc (a cached entry
/// of another translation, which its key names, is rebuilt).
const llvm_codegen::ModuleCache kCache{"mlir", ".bc"};

/// @p mod, lowered to EmitC, printed as C.
Status emitC(mlir::ModuleOp mod, llvm::raw_ostream &os) {
  if (Status s = lowerTo(mod, Stage::EmitC); !s)
    return s;
  return translateToC(mod, os);
}

} // namespace

bool parseStage(const std::string &name, Stage &stage) {
  if (name == "pir")
    stage = Stage::PIR;
  else if (name == "scf")
    stage = Stage::SCF;
  else if (name == "llvm")
    stage = Stage::LLVM;
  else if (name == "emitc")
    stage = Stage::EmitC;
  else
    return false;
  return true;
}

std::unique_ptr<mlir::MLIRContext> createContext() {
  auto ctx = std::make_unique<mlir::MLIRContext>();
  loadImportDialects(*ctx);
  loadStandardDialects(*ctx);
  loadLLVMDialects(*ctx);
  loadEmitCDialects(*ctx);
  return ctx;
}

Status lowerTo(mlir::ModuleOp module, Stage stage) {
  if (stage == Stage::PIR)
    return Status::ok();
  std::string diagnostics;
  mlir::ScopedDiagnosticHandler handler(
      module.getContext(), [&](mlir::Diagnostic &d) {
        llvm::raw_string_ostream os(diagnostics);
        os << d << "\n";
        return mlir::success();
      });
  mlir::PassManager pm(module.getContext());
  pm.addPass(createPIRToStandardPass());
  if (stage == Stage::LLVM)
    addLLVMDialectPasses(pm);
  else if (stage == Stage::EmitC)
    addEmitCPasses(pm);
  if (mlir::failed(pm.run(module)))
    return Status::error("the MLIR pipeline failed:\n" + diagnostics);
  return Status::ok();
}

Status emitProgramC(const pir::Program &program, llvm::raw_ostream &os) {
  auto ctx = createContext();
  auto mod = importProgram(program, *ctx);
  if (!mod)
    return mod.status();
  return emitC((*mod).get(), os);
}

StatusOr<std::unique_ptr<llvm::Module>>
compileProgram(const pir::Program &program, llvm::LLVMContext &ctx,
               const std::string &moduleName, const std::string &projectRoot) {
  auto mlirCtx = createContext();
  auto top = importProgram(program, *mlirCtx);
  if (!top)
    return top.status();
  // Each module is lowered on its own, as what is left of it, and cached
  // under that: another program's use of a module leaves another part.
  std::vector<mlir::ModuleOp> modules((*top)->getOps<mlir::ModuleOp>().begin(),
                                      (*top)->getOps<mlir::ModuleOp>().end());
  if (modules.size() != program.Modules.size())
    return Status::error("the program imported as " +
                         std::to_string(modules.size()) + " modules, not " +
                         std::to_string(program.Modules.size()));
  return llvm_codegen::compileProgram(
      program, ctx, moduleName, projectRoot, kCache,
      [&](size_t index) {
        std::string text;
        llvm::raw_string_ostream os(text);
        modules[index]->print(os);
        return text;
      },
      [&](size_t index) -> StatusOr<std::unique_ptr<llvm::Module>> {
        mlir::OwningOpRef<mlir::ModuleOp> mod(modules[index]);
        modules[index]->remove();
        if (Status s = lowerTo(mod.get(), Stage::LLVM); !s)
          return s;
        return translateToLLVMIR(mod.get(), ctx, program.Modules[index].Name);
      });
}

} // namespace paykan::mlir_backend
