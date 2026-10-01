// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The `llvm` backend plugin: PIR -> LLVM IR (PIRToLLVM.cpp), -O<n> through
// PassBuilder, --emit-llvm, and in-process execution with the ORC JIT
// (src/JIT).  Imported modules are served from the bitcode cache under
// <project root>/.paykan_cache when their PIR is unchanged.

#include "JIT.h"
#include "PIRToLLVM.h"
#include "Runtime.h"
#include "paykan/Backend.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/raw_os_ostream.h>

#include <memory>
#include <ostream>
#include <string>

namespace paykan::backend::llvm_backend {

namespace {

class LLVMBackend : public Backend {
public:
  std::string_view name() const override { return "llvm"; }

  Capabilities capabilities() const override {
    Capabilities c;
    c.EmitSource = true;
    c.Run = true;
    c.SourceExtension = ".ll";
    return c;
  }

  std::string describe() const override {
    return std::string("LLVM ") + LLVM_VERSION_STRING;
  }

  Status emit(const Input &in, EmitKind kind, const EmitOptions &,
              std::ostream &out) override {
    if (kind != EmitKind::Source)
      return Status::error("the llvm backend only emits LLVM IR source");
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto module = compile(in, *ctx);
    if (!module)
      return module.status();
    llvm::raw_os_ostream os(out);
    (*module)->print(os, nullptr);
    return Status::ok();
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
  /// Translate (through the cache) and optimise the program.
  static StatusOr<std::unique_ptr<llvm::Module>>
  compile(const Input &in, llvm::LLVMContext &ctx) {
    if (!in.Program)
      return Status::error("the llvm backend needs the PIR program");
    auto module =
        compileProgram(*in.Program, ctx, in.InputFilename, in.ProjectRoot);
    if (!module)
      return module.status();
    optimizeModule(**module, in.OptLevel);
    return module;
  }
};

std::unique_ptr<Backend> createLLVMBackend() {
  return std::make_unique<LLVMBackend>();
}

} // namespace

} // namespace paykan::backend::llvm_backend

PAYKAN_REGISTER_BACKEND(llvm, "llvm",
                        &paykan::backend::llvm_backend::createLLVMBackend);
