// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The `mlir` backend plugin: PIR imported into the `pir` MLIR dialect
// (Import/), then lowered through MLIR (docs/design/mlir-backend.md).  Its
// source output is the program at one stage of that pipeline, chosen with
// --emit-mlir=<stage>: `pir`, the imported dialect.

#include "Import/PIRImport.h"
#include "paykan/Backend.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/Support/raw_os_ostream.h>
#include <mlir/IR/MLIRContext.h>

#include <memory>
#include <ostream>
#include <string>

namespace paykan::backend::mlir_backend {

namespace {

/// The stages --emit-mlir prints.
constexpr const char *kStagePIR = "pir";

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
    if (stage != kStagePIR)
      return Status::error("unknown MLIR stage '" + stage +
                           "' (expected: pir)");
    ::mlir::MLIRContext ctx;
    auto mod = paykan::mlir_backend::importProgram(*in.Program, ctx);
    if (!mod)
      return mod.status();
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
