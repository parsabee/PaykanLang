// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The `llvm` backend plugin: LLVM IR generation, -O<n> through PassBuilder,
// --emit-llvm, and in-process execution with the ORC JIT.
//
// This wraps the legacy AST code generator (src/CodeGen) and the JIT
// (src/JIT) unchanged.  It therefore consumes the typed AST, not PIR
// (consumesPIR() == false); the PIR-based translation replaces it later and
// the AST path is deleted then (see the migration note in paykan/Backend.h).

#include "CodeGen.h"
#include "JIT.h"
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

  bool consumesPIR() const override { return false; }

  Status emit(const Input &in, EmitKind kind, const EmitOptions &,
              std::ostream &out) override {
    if (kind != EmitKind::Source)
      return Status::error("the llvm backend only emits LLVM IR source");
    auto ctx = std::make_unique<llvm::LLVMContext>();
    std::unique_ptr<codegen::CodeGen> cg;
    if (Status s = compile(in, *ctx, cg); !s)
      return s;
    llvm::raw_os_ostream os(out);
    cg->getModule().print(os, nullptr);
    return Status::ok();
  }

  StatusOr<int> run(const Input &in, std::span<const std::string> args,
                    const RunOptions &opts) override {
    auto ctx = std::make_unique<llvm::LLVMContext>();
    std::unique_ptr<codegen::CodeGen> cg;
    if (Status s = compile(in, *ctx, cg); !s)
      return s;
    if (!cg->linkImportedModules())
      return Status::error("failed to link imported module");
    auto module = cg->takeModule();
    cg.reset();

    // Select the tracking allocator before any program allocation happens,
    // so that every block is allocated and freed by the same back-end.
    if (opts.TrackHeap) {
      Paykan_heap_set_tracking(1);
      Paykan_heap_reset();
    }
    std::vector<std::string> progArgs(args.begin(), args.end());
    auto result = jit::runModule(std::move(module), std::move(ctx), progArgs);
    if (opts.TrackHeap)
      Paykan_heap_dump();
    if (!result)
      return Status::error("JIT error: " + llvm::toString(result.takeError()));
    return *result;
  }

private:
  /// Generate and optimise the module for @p in.
  static Status compile(const Input &in, llvm::LLVMContext &ctx,
                        std::unique_ptr<codegen::CodeGen> &cg) {
    if (!in.Sema || !in.TU)
      return Status::error("the llvm backend needs the typed AST");
    cg = std::make_unique<codegen::CodeGen>(*in.Sema, ctx, in.InputFilename,
                                            in.ProjectRoot);
    if (!cg->run(in.TU))
      return Status::error(
          "code generation failed (module verification error)");
    cg->optimize(in.OptLevel);
#ifndef NDEBUG
    std::string errMsg;
    if (!cg->verify(errMsg))
      return Status::error("LLVM IR verification failed:\n" + errMsg);
#endif
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
