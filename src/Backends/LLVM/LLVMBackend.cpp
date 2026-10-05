// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The `llvm` backend plugin: PIR -> LLVM IR (PIRToLLVM.cpp), -O<n> through
// PassBuilder, --emit-llvm, in-process execution with the ORC JIT
// (JIT/), and ahead-of-time `build`: a native object for the host
// (NativeBuild.cpp) linked against libpaykan_runtime.a with the system
// toolchain the C backend uses too (paykan/backends/Toolchain.h).  Imported
// modules are served from the bitcode cache under <project root>/.paykan_cache
// when their PIR is unchanged.

#include "JIT.h"
#include "NativeBuild.h"
#include "PIRToLLVM.h"
#include "Runtime.h"
#include "paykan/Backend.h"
#include "paykan/backends/Toolchain.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/raw_os_ostream.h>

#include <filesystem>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>

namespace paykan::backend::llvm_backend {

namespace {

class LLVMBackend : public Backend {
public:
  std::string_view name() const override { return "llvm"; }

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
    return std::string("LLVM ") + LLVM_VERSION_STRING;
  }

  Status emit(const Input &in, EmitKind kind, const EmitOptions &opts,
              std::ostream &out) override {
    auto ctx = std::make_unique<llvm::LLVMContext>();
    switch (kind) {
    case EmitKind::Source: {
      auto module = compile(in, *ctx);
      if (!module)
        return module.status();
      llvm::raw_os_ostream os(out);
      (*module)->print(os, nullptr);
      return Status::ok();
    }
    case EmitKind::Object: {
      std::string output = opts.OutputPath;
      if (output.empty())
        output = defaultOutput(in) + ".o";
      return emitObject(in, *ctx, output);
    }
    case EmitKind::Executable:
      return buildExecutable(in, *ctx,
                             opts.OutputPath.empty() ? defaultOutput(in)
                                                     : opts.OutputPath);
    }
    return Status::error("unknown emit kind");
  }

  StatusOr<int> run(const Input &in, std::span<const std::string> args,
                    const RunOptions &opts) override {
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto module = compile(in, *ctx);
    if (!module)
      return module.status();
    // Select the tracking allocator before any program allocation happens,
    // so that every block is allocated and freed by the same back-end.
    if (opts.TrackHeap) {
      Paykan_heap_set_tracking(1);
      Paykan_heap_reset();
    }
    std::vector<std::string> progArgs(args.begin(), args.end());
    auto result = jit::runModule(std::move(*module), std::move(ctx), progArgs);
    if (opts.TrackHeap)
      Paykan_heap_dump();
    if (!result)
      return Status::error("JIT error: " + llvm::toString(result.takeError()));
    return *result;
  }

private:
  /// Translate the program (through the cache) without optimising it.
  static StatusOr<std::unique_ptr<llvm::Module>>
  translate(const Input &in, llvm::LLVMContext &ctx) {
    if (!in.Program)
      return Status::error("the llvm backend needs the PIR program");
    if (in.Program->Modules.empty())
      return Status::error("empty PIR program");
    // The linked module is named after the main module's canonical name, not
    // the input path, so --emit-llvm and the objects of `build` do not depend
    // on the directory the compiler ran in (#102).
    return compileProgram(*in.Program, ctx, in.Program->Modules.front().Name,
                          in.ProjectRoot);
  }

  /// Translate (through the cache) and optimise the program.
  static StatusOr<std::unique_ptr<llvm::Module>>
  compile(const Input &in, llvm::LLVMContext &ctx) {
    auto module = translate(in, ctx);
    if (!module)
      return module.status();
    optimizeModule(**module, in.OptLevel);
    return module;
  }

  /// The default output path of `build`: the input's stem, next to the
  /// current directory (what the C backend does).
  static std::string defaultOutput(const Input &in) {
    std::string output =
        std::filesystem::path(in.InputFilename).stem().string();
    return output.empty() ? "a.out" : output;
  }

  /// The program as a native object for the host: the translated modules
  /// plus the C entry point, optimised at -O<n> like `run`.
  static Status emitObject(const Input &in, llvm::LLVMContext &ctx,
                           const std::string &path) {
    auto module = translate(in, ctx);
    if (!module)
      return module.status();
    if (Status s = addEntryPoint(**module); !s)
      return s;
    auto tm = createHostTargetMachine(in.OptLevel);
    if (!tm)
      return tm.status();
    (*module)->setTargetTriple((*tm)->getTargetTriple().str());
    (*module)->setDataLayout((*tm)->createDataLayout());
    optimizeModule(**module, in.OptLevel);
    return writeObjectFile(**module, **tm, path);
  }

  /// `build`: the object of emitObject() linked against the runtime with
  /// the system toolchain (including the sanitizer / coverage flags of a
  /// sanitizer / coverage build, whose runtime archive is instrumented).
  static Status buildExecutable(const Input &in, llvm::LLVMContext &ctx,
                                const std::string &output) {
    std::ostringstream errs;
    toolchain::Toolchain tc;
    if (!toolchain::resolveToolchain(tc, errs))
      return Status::error(errs.str());
    toolchain::TempDir tmp;
    if (tmp.Path.empty())
      return Status::error("cannot create a temporary directory");
    std::string object = tmp.Path + "/program.o";
    if (Status s = emitObject(in, ctx, object); !s)
      return s;
    if (!toolchain::linkExecutable({object}, output, tc, errs))
      return Status::error(errs.str());
    return Status::ok();
  }
};

std::unique_ptr<Backend> createLLVMBackend() {
  return std::make_unique<LLVMBackend>();
}

} // namespace

} // namespace paykan::backend::llvm_backend

PAYKAN_REGISTER_BACKEND(llvm, "llvm",
                        &paykan::backend::llvm_backend::createLLVMBackend);
