// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Running and building a program's LLVM module (see NativeBuild.h).

#include "NativeBuild.h"

#include "JIT.h"
#include "ModuleCache.h"
#include "Names.h"
#include "Runtime.h"
#include "paykan/backends/Toolchain.h"

#include <llvm/ADT/StringMap.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/SubtargetFeature.h>

#include <optional>
#include <sstream>

namespace paykan::llvm_codegen {

namespace {

/// The name the Paykan `main` is renamed to; '.' is not valid in a Paykan
/// identifier, so it cannot clash with a program symbol.
constexpr const char *kPaykanMainName = "paykan.main";

} // namespace

Status addEntryPoint(llvm::Module &module) {
  llvm::Function *paykanMain = module.getFunction("main");
  if (!paykanMain || paykanMain->isDeclaration())
    return Status::error("the main module defines no 'main' function");
  if (paykanMain->arg_size() != 0 && paykanMain->arg_size() != 2)
    return Status::error("'main' takes no parameter or a slice of strings");
  paykanMain->setName(kPaykanMainName);
  paykanMain->setLinkage(llvm::GlobalValue::InternalLinkage);
  bool takesArgs = paykanMain->arg_size() == 2;

  llvm::LLVMContext &ctx = module.getContext();
  llvm::IRBuilder<> b(ctx);
  auto *i32 = b.getInt32Ty();
  auto *i64 = b.getInt64Ty();
  auto *ptr = b.getPtrTy();
  auto *voidTy = b.getVoidTy();
  auto fn = [&](const char *name, llvm::Type *ret,
                llvm::ArrayRef<llvm::Type *> params) {
    return module.getOrInsertFunction(
        name, llvm::FunctionType::get(ret, params, false));
  };

  auto *entryTy = llvm::FunctionType::get(i32, {i32, ptr}, false);
  auto *entry = llvm::Function::Create(
      entryTy, llvm::GlobalValue::ExternalLinkage, "main", module);
  llvm::Argument *argc = entry->getArg(0);
  llvm::Argument *argv = entry->getArg(1);
  argc->setName("argc");
  argv->setName("argv");

  auto *bb = llvm::BasicBlock::Create(ctx, "entry", entry);
  b.SetInsertPoint(bb);
  // PAYKAN_TRACK_HEAP: select the tracking allocator before the program
  // allocates anything (the C path's main() does the same).
  llvm::Value *trackEnv = b.CreateCall(
      fn("getenv", ptr, {ptr}), {b.CreateGlobalString("PAYKAN_TRACK_HEAP")});
  llvm::Value *track = b.CreateIsNotNull(trackEnv, "track");
  auto *trackOn = llvm::BasicBlock::Create(ctx, "track.on", entry);
  auto *start = llvm::BasicBlock::Create(ctx, "start", entry);
  b.CreateCondBr(track, trackOn, start);
  b.SetInsertPoint(trackOn);
  b.CreateCall(fn("Paykan_heap_set_tracking", voidTy, {i32}), {b.getInt32(1)});
  b.CreateCall(fn(names::kPaykanHeapReset, voidTy, {}));
  b.CreateBr(start);
  b.SetInsertPoint(start);

  llvm::SmallVector<llvm::Value *, 2> callArgs;
  llvm::Value *argCount = nullptr;
  if (takesArgs) {
    // args: view Str[] -- a slice of the arguments' strings
    // (Paykan_args_new, which also honours PAYKAN_NO_ARGS).
    llvm::Value *countSlot = b.CreateAlloca(i64, nullptr, "args.len.addr");
    llvm::Value *args =
        b.CreateCall(fn(names::kPaykanArgsNew, ptr, {i32, ptr, ptr}),
                     {argc, argv, countSlot}, "args");
    argCount = b.CreateLoad(i64, countSlot, "args.len");
    callArgs.push_back(args);
    callArgs.push_back(argCount);
  }
  llvm::Value *rc = b.CreateCall(paykanMain, callArgs);
  if (takesArgs)
    b.CreateCall(fn(names::kPaykanArgsFree, voidTy, {ptr, i64}),
                 {callArgs[0], argCount});

  auto *dump = llvm::BasicBlock::Create(ctx, "track.dump", entry);
  auto *exit = llvm::BasicBlock::Create(ctx, "exit", entry);
  b.CreateCondBr(track, dump, exit);
  b.SetInsertPoint(dump);
  b.CreateCall(fn("Paykan_heap_dump", voidTy, {}));
  b.CreateBr(exit);
  b.SetInsertPoint(exit);
  llvm::Type *retTy = paykanMain->getReturnType();
  if (retTy->isIntegerTy())
    b.CreateRet(b.CreateSExtOrTrunc(rc, i32));
  else
    b.CreateRet(b.getInt32(0));

  std::string err;
  llvm::raw_string_ostream os(err);
  if (llvm::verifyFunction(*entry, &os))
    return Status::error("invalid entry point: " + os.str());
  return Status::ok();
}

StatusOr<std::unique_ptr<llvm::TargetMachine>>
createHostTargetMachine(unsigned optLevel) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
  std::string err;
  const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, err);
  if (!target)
    return Status::error("no LLVM target for '" + triple.str() + "': " + err);

  llvm::SubtargetFeatures features;
  for (const auto &f : llvm::sys::getHostCPUFeatures())
    features.AddFeature(f.first(), f.second);

  static const llvm::CodeGenOptLevel levels[] = {
      llvm::CodeGenOptLevel::None,
      llvm::CodeGenOptLevel::Less,
      llvm::CodeGenOptLevel::Default,
      llvm::CodeGenOptLevel::Aggressive,
  };
  llvm::TargetOptions options;
  std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
      triple, llvm::sys::getHostCPUName(), features.getString(), options,
      llvm::Reloc::PIC_, std::nullopt, levels[optLevel < 4 ? optLevel : 3]));
  if (!tm)
    return Status::error("cannot create an LLVM target machine for '" +
                         triple.str() + "'");
  return tm;
}

Status writeObjectFile(llvm::Module &module, llvm::TargetMachine &tm,
                       const std::string &path) {
  std::error_code ec;
  llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_None);
  if (ec)
    return Status::error("cannot write '" + path + "': " + ec.message());
  llvm::legacy::PassManager pm;
  if (tm.addPassesToEmitFile(pm, out, nullptr,
                             llvm::CodeGenFileType::ObjectFile))
    return Status::error("the LLVM target cannot emit object files");
  pm.run(module);
  out.close();
  if (out.has_error()) {
    out.clear_error();
    return Status::error("cannot write '" + path + "'");
  }
  return Status::ok();
}

Status prepareForJIT(llvm::Module &module, unsigned optLevel) {
  if (llvm::Error err = jit::setHostLayout(module))
    return Status::error("JIT error: " + llvm::toString(std::move(err)));
  optimizeModule(module, optLevel);
  return Status::ok();
}

StatusOr<int> runInProcess(std::unique_ptr<llvm::Module> module,
                           std::unique_ptr<llvm::LLVMContext> ctx,
                           std::span<const std::string> args,
                           const std::vector<std::string> &objects,
                           bool trackHeap) {
  // Select the tracking allocator before any program allocation happens,
  // so that every block is allocated and freed by the same back-end.
  if (trackHeap) {
    Paykan_heap_set_tracking(1);
    Paykan_heap_reset();
  }
  std::vector<std::string> progArgs(args.begin(), args.end());
  auto result =
      jit::runModule(std::move(module), std::move(ctx), progArgs, objects);
  if (trackHeap)
    Paykan_heap_dump();
  if (!result)
    return Status::error("JIT error: " + llvm::toString(result.takeError()));
  return *result;
}

Status emitHostObject(llvm::Module &module, unsigned optLevel,
                      const std::string &path) {
  if (Status s = addEntryPoint(module); !s)
    return s;
  return emitHostModuleObject(module, optLevel, path);
}

Status emitHostModuleObject(llvm::Module &module, unsigned optLevel,
                            const std::string &path) {
  auto tm = createHostTargetMachine(optLevel);
  if (!tm)
    return tm.status();
  module.setTargetTriple((*tm)->getTargetTriple());
  module.setDataLayout((*tm)->createDataLayout());
  optimizeModule(module, optLevel);
  return writeObjectFile(module, **tm, path);
}

Status buildHostExecutable(
    const std::function<Status(const std::string &)> &emitObject,
    const std::vector<std::string> &objects, const std::string &output) {
  // The system toolchain, with the sanitizer / coverage flags of a sanitizer
  // / coverage build, whose runtime archive is instrumented.
  std::ostringstream errs;
  toolchain::Toolchain tc;
  tc.ExtraObjects = objects;
  if (!toolchain::resolveToolchain(tc, errs))
    return Status::error(errs.str());
  toolchain::TempDir tmp;
  if (tmp.Path.empty())
    return Status::error("cannot create a temporary directory");
  std::string object = tmp.Path + "/program.o";
  if (Status s = emitObject(object); !s)
    return s;
  if (!toolchain::linkExecutable({object}, output, tc, errs))
    return Status::error(errs.str());
  return Status::ok();
}

} // namespace paykan::llvm_codegen
