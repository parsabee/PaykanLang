// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The mlir backend plugin (docs/design/mlir-backend.md): PIR imported into
// the `pir` MLIR dialect (Import/), lowered once for both targets
// (Lowering/PIRToStandard.cpp), then
//
// * to the LLVM dialect and LLVM IR (ToLLVM/), which `run` runs in process
//   with the JIT and `build` builds into an object or an executable
//   (paykan_llvm_codegen); and
// * to EmitC and C (ToEmitC/), the program's C.
//
// Its source output is chosen with EmitOptions::SourceStage: the program's
// LLVM IR as `run` executes it (the default: --emit-source, --emit-llvm),
// its C (`c`: --emit-c), or the program at one stage of the pipeline
// (--emit-mlir=<stage>: `pir`, `scf`, `llvm` or `emitc`).

#include "Import/PIRImport.h"
#include "NativeBuild.h"
#include "Pipeline.h"
#include "paykan/Backend.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/raw_os_ostream.h>

#include <filesystem>
#include <memory>
#include <ostream>
#include <string>

namespace paykan::backend::mlir_backend {

namespace {

namespace MB = paykan::mlir_backend;

/// The source output that is the program's C rather than a pipeline stage.
constexpr const char *kCStage = "c";

class MLIRBackend : public Backend {
public:
  std::string_view name() const override { return "mlir"; }

  Capabilities capabilities() const override {
    Capabilities c;
    c.EmitSource = true;
    c.EmitObject = true;
    c.EmitExecutable = true;
    c.Run = true;
    c.SourceExtension = ".ll";
    return c;
  }

  std::string describe() const override {
    return std::string("MLIR ") + LLVM_VERSION_STRING;
  }

  Status emit(const Input &in, EmitKind kind, const EmitOptions &opts,
              std::ostream &out) override {
    if (!in.Program || in.Program->Modules.empty())
      return Status::error("the mlir backend needs the PIR program");
    switch (kind) {
    case EmitKind::Source:
      return emitSource(in, opts.SourceStage, out);
    case EmitKind::Object: {
      auto ctx = std::make_unique<llvm::LLVMContext>();
      return emitObject(in, *ctx,
                        opts.OutputPath.empty() ? defaultOutput(in) + ".o"
                                                : opts.OutputPath);
    }
    case EmitKind::Executable: {
      auto ctx = std::make_unique<llvm::LLVMContext>();
      return llvm_codegen::buildHostExecutable(
          [&](const std::string &object) {
            return emitObject(in, *ctx, object);
          },
          in.Objects,
          opts.OutputPath.empty() ? defaultOutput(in) : opts.OutputPath);
    }
    }
    return Status::error("unknown emit kind");
  }

  StatusOr<int> run(const Input &in, std::span<const std::string> args,
                    const RunOptions &opts) override {
    if (!in.Program || in.Program->Modules.empty())
      return Status::error("the mlir backend needs the PIR program");
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto module = translate(in, *ctx);
    if (!module)
      return module.status();
    if (Status s = llvm_codegen::prepareForJIT(**module, in.OptLevel); !s)
      return s;
    return llvm_codegen::runInProcess(std::move(*module), std::move(ctx), args,
                                      in.Objects, opts.TrackHeap);
  }

private:
  /// The source output @p stageName names (see the top of the file).
  static Status emitSource(const Input &in, const std::string &stageName,
                           std::ostream &out) {
    if (stageName.empty()) {
      llvm::LLVMContext ctx;
      auto module = translate(in, ctx);
      if (!module)
        return module.status();
      if (Status s = llvm_codegen::prepareForJIT(**module, in.OptLevel); !s)
        return s;
      llvm::raw_os_ostream os(out);
      (*module)->print(os, nullptr);
      return Status::ok();
    }
    if (stageName == kCStage) {
      llvm::raw_os_ostream os(out);
      return MB::emitProgramC(*in.Program, os);
    }
    MB::Stage stage = MB::Stage::PIR;
    if (!MB::parseStage(stageName, stage))
      return Status::error("unknown MLIR stage '" + stageName +
                           "' (expected: pir, scf, llvm, emitc)");
    auto ctx = MB::createContext();
    auto mod = MB::importProgram(*in.Program, *ctx);
    if (!mod)
      return mod.status();
    if (Status s = MB::lowerTo((*mod).get(), stage); !s)
      return s;
    llvm::raw_os_ostream os(out);
    (*mod)->print(os);
    return Status::ok();
  }

  /// The program in LLVM IR (through the cache), not yet optimised.  The
  /// linked module is named after the main module, not the input path, so
  /// the output does not depend on the directory the compiler ran in.
  static StatusOr<std::unique_ptr<llvm::Module>>
  translate(const Input &in, llvm::LLVMContext &ctx) {
    return MB::compileProgram(*in.Program, ctx,
                              in.Program->Modules.front().Name, in.ProjectRoot);
  }

  static Status emitObject(const Input &in, llvm::LLVMContext &ctx,
                           const std::string &path) {
    auto module = translate(in, ctx);
    if (!module)
      return module.status();
    return llvm_codegen::emitHostObject(**module, in.OptLevel, path);
  }

  /// The default output path of `build`: the input's stem, in the current
  /// directory.
  static std::string defaultOutput(const Input &in) {
    std::string output =
        std::filesystem::path(in.InputFilename).stem().string();
    return output.empty() ? "a.out" : output;
  }
};

std::unique_ptr<Backend> createMLIRBackend() {
  return std::make_unique<MLIRBackend>();
}

} // namespace

} // namespace paykan::backend::mlir_backend

PAYKAN_REGISTER_BACKEND(mlir, "mlir",
                        &paykan::backend::mlir_backend::createMLIRBackend);
