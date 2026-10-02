// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Ahead-of-time compilation in the `llvm` backend (see NativeBuild.h).

#include "NativeBuild.h"

#include "Names.h"

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

namespace paykan::backend::llvm_backend {

namespace {

/// The name the Paykan `main` is renamed to; '.' is not valid in a Paykan
/// identifier, so it cannot clash with a program symbol.
constexpr const char *kPaykanMainName = "paykan.main";

} // namespace

Status addEntryPoint(llvm::Module &module) {
  llvm::Function *paykanMain = module.getFunction("main");
  if (!paykanMain || paykanMain->isDeclaration())
    return Status::error("the main module defines no 'main' function");
  if (paykanMain->arg_size() > 1)
    return Status::error("'main' takes at most one parameter");
  paykanMain->setName(kPaykanMainName);
  paykanMain->setLinkage(llvm::GlobalValue::InternalLinkage);
  bool takesArgs = paykanMain->arg_size() == 1;

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
  // allocates anything (the C backend's main does the same).
  llvm::Value *trackEnv = b.CreateCall(
      fn("getenv", ptr, {ptr}), {b.CreateGlobalStringPtr("PAYKAN_TRACK_HEAP")});
  llvm::Value *track = b.CreateIsNotNull(trackEnv, "track");
  auto *trackOn = llvm::BasicBlock::Create(ctx, "track.on", entry);
  auto *start = llvm::BasicBlock::Create(ctx, "start", entry);
  b.CreateCondBr(track, trackOn, start);
  b.SetInsertPoint(trackOn);
  b.CreateCall(fn("Paykan_heap_set_tracking", voidTy, {i32}), {b.getInt32(1)});
  b.CreateCall(fn(names::kPaykanHeapReset, voidTy, {}));
  b.CreateBr(start);
  b.SetInsertPoint(start);

  llvm::SmallVector<llvm::Value *, 1> callArgs;
  if (takesArgs) {
    // args: Str[] -- one string per argv entry (none under PAYKAN_NO_ARGS:
    // the kernel always supplies an argv[0], so a caller that wants
    // argc == 0 says so).
    llvm::Value *arr = b.CreateCall(fn(names::kPaykanArrayNewObj, ptr, {i64}),
                                    {b.getInt64(0)}, "args");
    llvm::Value *noArgs = b.CreateIsNotNull(b.CreateCall(
        fn("getenv", ptr, {ptr}), {b.CreateGlobalStringPtr("PAYKAN_NO_ARGS")}));
    llvm::Value *count = b.CreateSelect(noArgs, b.getInt32(0), argc, "count");
    auto *head = llvm::BasicBlock::Create(ctx, "args.head", entry);
    auto *body = llvm::BasicBlock::Create(ctx, "args.body", entry);
    auto *done = llvm::BasicBlock::Create(ctx, "args.done", entry);
    llvm::BasicBlock *pre = b.GetInsertBlock();
    b.CreateBr(head);
    b.SetInsertPoint(head);
    llvm::PHINode *i = b.CreatePHI(i32, 2, "i");
    i->addIncoming(b.getInt32(0), pre);
    b.CreateCondBr(b.CreateICmpSLT(i, count), body, done);
    b.SetInsertPoint(body);
    llvm::Value *cstr =
        b.CreateLoad(ptr, b.CreateGEP(ptr, argv, b.CreateSExt(i, i64)));
    llvm::Value *len = b.CreateCall(fn("strlen", i64, {ptr}), {cstr});
    llvm::Value *str =
        b.CreateCall(fn(names::kPaykanStringNew, ptr, {ptr, i64}), {cstr, len});
    llvm::Value *box =
        b.CreateCall(fn(names::kPaykanSharedNew, ptr, {ptr}), {str});
    b.CreateCall(fn(names::kPaykanArrayPushObj, voidTy, {ptr, ptr}),
                 {arr, box});
    b.CreateCall(fn(names::kPaykanRelease, voidTy, {ptr}), {box});
    i->addIncoming(b.CreateAdd(i, b.getInt32(1)), body);
    b.CreateBr(head);
    b.SetInsertPoint(done);
    // Ownership of the boxed array passes to main; its scope cleanup
    // releases it.
    callArgs.push_back(
        b.CreateCall(fn(names::kPaykanSharedNew, ptr, {ptr}), {arr}));
  }
  llvm::Value *rc = b.CreateCall(paykanMain, callArgs);

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
  std::string triple = llvm::sys::getDefaultTargetTriple();
  std::string err;
  const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, err);
  if (!target)
    return Status::error("no LLVM target for '" + triple + "': " + err);

  llvm::SubtargetFeatures features;
  llvm::StringMap<bool> hostFeatures;
  if (llvm::sys::getHostCPUFeatures(hostFeatures))
    for (const auto &f : hostFeatures)
      features.AddFeature(f.first(), f.second);

  static const llvm::CodeGenOpt::Level levels[] = {
      llvm::CodeGenOpt::None,
      llvm::CodeGenOpt::Less,
      llvm::CodeGenOpt::Default,
      llvm::CodeGenOpt::Aggressive,
  };
  llvm::TargetOptions options;
  std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
      triple, llvm::sys::getHostCPUName(), features.getString(), options,
      llvm::Reloc::PIC_, std::nullopt, levels[optLevel < 4 ? optLevel : 3]));
  if (!tm)
    return Status::error("cannot create an LLVM target machine for '" + triple +
                         "'");
  return tm;
}

Status writeObjectFile(llvm::Module &module, llvm::TargetMachine &tm,
                       const std::string &path) {
  std::error_code ec;
  llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_None);
  if (ec)
    return Status::error("cannot write '" + path + "': " + ec.message());
  llvm::legacy::PassManager pm;
  if (tm.addPassesToEmitFile(pm, out, nullptr, llvm::CGFT_ObjectFile))
    return Status::error("the LLVM target cannot emit object files");
  pm.run(module);
  out.close();
  if (out.has_error()) {
    out.clear_error();
    return Status::error("cannot write '" + path + "'");
  }
  return Status::ok();
}

} // namespace paykan::backend::llvm_backend
