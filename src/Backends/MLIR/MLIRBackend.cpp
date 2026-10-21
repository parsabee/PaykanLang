// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The `mlir` backend plugin: PIR imported into the `pir` MLIR dialect
// (Import/), then lowered through MLIR (docs/design/mlir-backend.md).  Its
// source output is the program at one stage of that pipeline, chosen with
// --emit-mlir=<stage>: `pir`, the imported dialect, or `scf`, after the
// shared lowering (Lowering/PIRToStandard.cpp).

#include "Import/PIRImport.h"
#include "Lowering/PIRToStandard.h"
#include "paykan/Backend.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Support/raw_ostream.h>
#include <mlir/IR/Diagnostics.h>
#include <mlir/IR/MLIRContext.h>
#include <mlir/Pass/PassManager.h>

#include <memory>
#include <ostream>
#include <string>

namespace paykan::backend::mlir_backend {

namespace {

/// The stages --emit-mlir prints.
constexpr const char *kStagePIR = "pir";
constexpr const char *kStageSCF = "scf";

/// Run the pipeline up to @p stage on @p mod.
Status lowerTo(::mlir::ModuleOp mod, const std::string &stage) {
  if (stage == kStagePIR)
    return Status::ok();
  std::string diagnostics;
  ::mlir::ScopedDiagnosticHandler handler(
      mod.getContext(), [&](::mlir::Diagnostic &d) {
        llvm::raw_string_ostream os(diagnostics);
        os << d << "\n";
        return ::mlir::success();
      });
  ::mlir::PassManager pm(mod.getContext());
  pm.addPass(paykan::mlir_backend::createPIRToStandardPass());
  if (::mlir::failed(pm.run(mod)))
    return Status::error("the MLIR pipeline failed:\n" + diagnostics);
  return Status::ok();
}

class MLIRBackend : public Backend {
public:
  std::string_view name() const override { return "mlir"; }

  Capabilities capabilities() const override {
    Capabilities c;
    c.EmitSource = true;
    c.SourceExtension = ".mlir";
    return c;
  }

  std::string describe() const override {
    return std::string("MLIR ") + LLVM_VERSION_STRING;
  }

  Status emit(const Input &in, EmitKind kind, const EmitOptions &opts,
              std::ostream &out) override {
    if (kind != EmitKind::Source)
      return Status::error("the mlir backend has only source output so far");
    if (!in.Program || in.Program->Modules.empty())
      return Status::error("the mlir backend needs the PIR program");
    const std::string &stage =
        opts.SourceStage.empty() ? kStagePIR : opts.SourceStage;
    if (stage != kStagePIR && stage != kStageSCF)
      return Status::error("unknown MLIR stage '" + stage +
                           "' (expected: pir, scf)");
    ::mlir::MLIRContext ctx;
    auto mod = paykan::mlir_backend::importProgram(*in.Program, ctx);
    if (!mod)
      return mod.status();
    if (Status s = lowerTo((*mod).get(), stage); !s)
      return s;
    llvm::raw_os_ostream os(out);
    (*mod)->print(os);
    return Status::ok();
  }
};

std::unique_ptr<Backend> createMLIRBackend() {
  return std::make_unique<MLIRBackend>();
}

} // namespace

} // namespace paykan::backend::mlir_backend

PAYKAN_REGISTER_BACKEND(mlir, "mlir",
                        &paykan::backend::mlir_backend::createMLIRBackend);
