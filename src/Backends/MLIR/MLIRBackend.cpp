// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace paykan::backend::mlir_backend {

namespace {

namespace MB = paykan::mlir_backend;

/// The source output that is the program's C rather than a pipeline stage.
constexpr const char *kCStage = "c";
/// The format of the compiled code of a module (ModuleObject::Format).
constexpr const char *kBitcodeFormat = "llvm-bc";

class MLIRBackend : public Backend {
public:
  std::string_view name() const override { return "mlir"; }

  Capabilities capabilities() const override {
    Capabilities c;
    c.EmitSource = true;
    c.EmitObject = true;
    c.EmitExecutable = true;
    c.Run = true;
    c.CompileModules = true;
    c.SourceExtension = ".ll";
    return c;
  }

  std::string describe() const override {
    return std::string("MLIR ") + LLVM_VERSION_STRING;
  }

  /// LLVM bitcode of the module's code, before optimisation (-O<n> applies
  /// to the program it is linked into), for the host.
  ModuleObject objectKind() const override {
    ModuleObject o;
    o.Backend = std::string(name());
    o.Format = kBitcodeFormat;
    o.Target = llvm::Triple::normalize(llvm::sys::getDefaultTargetTriple());
    o.Producer = describe();
    return o;
  }

  StatusOr<ModuleObject> compileModule(const pir::Module &module) override {
    llvm::LLVMContext ctx;
    auto mod = MB::compileModule(module, ctx);
    if (!mod)
      return mod.status();
    ModuleObject o = objectKind();
    llvm::SmallVector<char, 0> buffer;
    llvm::BitcodeWriter writer(buffer);
    writer.writeModule(**mod);
    writer.writeSymtab();
    writer.writeStrtab();
    o.Bytes.assign(buffer.begin(), buffer.end());
    return o;
  }

  Status emitModuleObject(const ModuleObject &code, unsigned optLevel,
                          const std::string &path) override {
    const ModuleObject kind = objectKind();
    if (!code.sameKind(kind))
      return Status::error("the code was compiled by backend '" + code.Backend +
                           "' (" + code.Producer + ", " + code.Format +
                           " for " + code.Target + "), not by this one (" +
                           kind.Producer + " for " + kind.Target + ")");
    llvm::LLVMContext ctx;
    llvm::StringRef buffer(reinterpret_cast<const char *>(code.Bytes.data()),
                           code.Bytes.size());
    auto module =
        llvm::parseBitcodeFile(llvm::MemoryBufferRef(buffer, path), ctx);
    if (!module)
      return Status::error("the compiled code does not read: " +
                           llvm::toString(module.takeError()));
    return llvm_codegen::emitHostModuleObject(**module, optLevel, path);
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
      return emitObject(in, *ctx, opts.OutputPath);
    }
    case EmitKind::Executable: {
      auto ctx = std::make_unique<llvm::LLVMContext>();
      return llvm_codegen::buildHostExecutable(
          [&](const std::string &object) {
            return emitObject(in, *ctx, object);
          },
          in.Objects, opts.OutputPath);
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
  Status emitSource(const Input &in, const std::string &stageName,
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

  /// The program in LLVM IR (through the cache), not yet optimised, with
  /// the part it uses of its modules' compiled code.  The linked module is
  /// named after the main module, not the input path, so the output does
  /// not depend on the directory the compiler ran in.
  StatusOr<std::unique_ptr<llvm::Module>> translate(const Input &in,
                                                    llvm::LLVMContext &ctx) {
    const ModuleObject kind = objectKind();
    std::vector<const ModuleObject *> compiled;
    for (const pir::Module &m : in.Program->Modules) {
      auto it = in.Compiled.find(m.Name);
      const ModuleObject *o = it == in.Compiled.end() ? nullptr : it->second;
      if (o && !o->sameKind(kind))
        return Status::error(
            "module '" + m.Name + "' was compiled by backend '" + o->Backend +
            "' (" + o->Producer + ", " + o->Format + " for " + o->Target +
            "); this backend links code it compiled itself (" + kind.Producer +
            " for " + kind.Target + "): rebuild the module from source");
      compiled.push_back(o);
    }
    return MB::compileProgram(
        *in.Program, ctx, in.Program->Modules.front().Name, in.ProjectRoot,
        [&](size_t i) -> std::span<const uint8_t> {
          return compiled[i] ? std::span<const uint8_t>(compiled[i]->Bytes)
                             : std::span<const uint8_t>();
        });
  }

  Status emitObject(const Input &in, llvm::LLVMContext &ctx,
                    const std::string &path) {
    auto module = translate(in, ctx);
    if (!module)
      return module.status();
    return llvm_codegen::emitHostObject(**module, in.OptLevel, path);
  }
};

std::unique_ptr<Backend> createMLIRBackend() {
  return std::make_unique<MLIRBackend>();
}

} // namespace

} // namespace paykan::backend::mlir_backend

PAYKAN_REGISTER_BACKEND(mlir, "mlir",
                        &paykan::backend::mlir_backend::createMLIRBackend);
