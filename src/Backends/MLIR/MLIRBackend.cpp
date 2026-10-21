// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The mlir backend plugin's two backends (docs/design/mlir-backend.md): PIR
// imported into the `pir` MLIR dialect (Import/), lowered once for both
// targets (Lowering/PIRToStandard.cpp), then
//
// * `mlir`: to the LLVM dialect and LLVM IR (ToLLVM/), which runs in
//   process with the JIT or is built into an executable
//   (paykan_llvm_codegen); its source output (--emit-source) is the
//   program's LLVM IR as `run` executes it;
// * `mlir-c`: to EmitC and C (ToEmitC/), built and run with the system C
//   compiler (paykan/backends/c/CBuild.h); its source output is the C.
//
// Either prints the program at one stage of the pipeline with
// --emit-mlir=<stage>: `pir`, `scf`, `llvm` or `emitc`.

#include "Import/PIRImport.h"
#include "Names.h"
#include "NativeBuild.h"
#include "Pipeline.h"
#include "paykan/Backend.h"
#include "paykan/backends/c/CBuild.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/raw_os_ostream.h>

#include <filesystem>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>

namespace paykan::backend::mlir_backend {

namespace {

namespace MB = paykan::mlir_backend;

/// The program at stage @p stageName of the pipeline.
Status printStage(const Input &in, const std::string &stageName,
                  std::ostream &out) {
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

/// The default output path of `build`: the input's stem, in the current
/// directory (what the other backends do).
std::string defaultOutput(const Input &in) {
  std::string output = std::filesystem::path(in.InputFilename).stem().string();
  return output.empty() ? "a.out" : output;
}

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
  /// The program at stage @p stageName, or its optimised LLVM IR when that
  /// is empty.
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
    return printStage(in, stageName, out);
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
};

/// The C path: the program in C through EmitC, built with the system C
/// compiler as the c backend builds its own C.
class MLIRCBackend : public Backend {
public:
  std::string_view name() const override { return "mlir-c"; }

  Capabilities capabilities() const override {
    Capabilities c;
    c.EmitSource = true;
    c.EmitObject = false;
    c.EmitExecutable = true;
    c.Run = true;
    c.SourceExtension = ".c";
    return c;
  }

  std::string describe() const override {
    return std::string("MLIR ") + LLVM_VERSION_STRING + " EmitC";
  }

  Status emit(const Input &in, EmitKind kind, const EmitOptions &opts,
              std::ostream &out) override {
    if (!in.Program || in.Program->Modules.empty())
      return Status::error("the mlir-c backend needs the PIR program");
    switch (kind) {
    case EmitKind::Source: {
      if (!opts.SourceStage.empty())
        return printStage(in, opts.SourceStage, out);
      llvm::raw_os_ostream os(out);
      return MB::emitProgramC(*in.Program, os);
    }
    case EmitKind::Executable: {
      std::ostringstream errs;
      if (!backend_c::buildExecutable(
              *in.Program, source(),
              opts.OutputPath.empty() ? defaultOutput(in) : opts.OutputPath,
              toolchain(in), errs))
        return Status::error(errs.str());
      return Status::ok();
    }
    case EmitKind::Object:
      break;
    }
    return Status::error("the mlir-c backend cannot emit object files");
  }

  StatusOr<int> run(const Input &in, std::span<const std::string> args,
                    const RunOptions &opts) override {
    if (!in.Program || in.Program->Modules.empty())
      return Status::error("the mlir-c backend needs the PIR program");
    std::ostringstream errs;
    std::vector<std::string> argv(args.begin(), args.end());
    int rc = backend_c::buildAndRun(*in.Program, source(), argv, opts.TrackHeap,
                                    toolchain(in), errs);
    if (rc < 0)
      return Status::error(errs.str());
    return rc;
  }

private:
  /// The C of the whole program, or of one module, through EmitC.
  static backend_c::CSource source() { return MB::cSource(); }

  /// The c backend's toolchain: -O<n>, and the object cache beside the
  /// bitcode cache.
  static backend_c::Toolchain toolchain(const Input &in) {
    backend_c::Toolchain tc;
    tc.ExtraObjects = in.Objects;
    tc.ExtraFlags.push_back("-O" +
                            std::to_string(in.OptLevel > 3 ? 3u : in.OptLevel));
    tc.CacheDir =
        (std::filesystem::path(in.ProjectRoot) / names::kCacheDir).string();
    return tc;
  }
};

std::unique_ptr<Backend> createMLIRBackend() {
  return std::make_unique<MLIRBackend>();
}

std::unique_ptr<Backend> createMLIRCBackend() {
  return std::make_unique<MLIRCBackend>();
}

} // namespace

} // namespace paykan::backend::mlir_backend

PAYKAN_REGISTER_BACKEND(mlir, "mlir",
                        &paykan::backend::mlir_backend::createMLIRBackend);
PAYKAN_REGISTER_BACKEND(mlir_c, "mlir-c",
                        &paykan::backend::mlir_backend::createMLIRCBackend);
