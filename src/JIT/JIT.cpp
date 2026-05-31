// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "JIT.h"
#include "Names.h"
#include "Runtime.h"

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>

namespace paykan::jit {

// ---------------------------------------------------------------------------
// Symbol table of runtime functions the JIT must be able to resolve.
// ---------------------------------------------------------------------------

namespace {

struct RuntimeSymbol {
  const char *Name;
  void       *Addr;
};

using namespace paykan::names;

// clang-format off
const RuntimeSymbol kRuntimeSymbols[] = {
    {kPaykanObjectNew,       reinterpret_cast<void *>(&PaykanObject_new)},
    {kPaykanObjectToString,  reinterpret_cast<void *>(&PaykanObject_toString)},
    {kPaykanObjectEquals,    reinterpret_cast<void *>(&PaykanObject_equals)},

    {kPaykanStringNew,       reinterpret_cast<void *>(&PaykanString_new)},
    {kPaykanStringFromInt,   reinterpret_cast<void *>(&PaykanString_from_int)},
    {kPaykanStringFromFloat, reinterpret_cast<void *>(&PaykanString_from_float)},
    {kPaykanStringFromBool,  reinterpret_cast<void *>(&PaykanString_from_bool)},
    {kPaykanStringToString,  reinterpret_cast<void *>(&PaykanString_toString)},
    {kPaykanStringEquals,    reinterpret_cast<void *>(&PaykanString_equals)},
    {kPaykanStringLength,    reinterpret_cast<void *>(&PaykanString_length)},
    {kPaykanStringConcat,    reinterpret_cast<void *>(&PaykanString_concat)},

    {kPaykanOut,             reinterpret_cast<void *>(&Paykan_out)},
    {kPaykanErr,             reinterpret_cast<void *>(&Paykan_err)},

    {kPaykanSharedNew,       reinterpret_cast<void *>(&PaykanShared_new)},
    {kPaykanSharedGet,       reinterpret_cast<void *>(&PaykanShared_get)},
    {kPaykanRetain,          reinterpret_cast<void *>(&Paykan_retain)},
    {kPaykanRelease,         reinterpret_cast<void *>(&Paykan_release)},

    {kPaykanObjectVtable,    reinterpret_cast<void *>(&PaykanObject_vtable)},
    {kPaykanStringVtable,    reinterpret_cast<void *>(&PaykanString_vtable)},
};
// clang-format on

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

llvm::Expected<int> runModule(std::unique_ptr<llvm::Module> module,
                              std::unique_ptr<llvm::LLVMContext> ctx) {
  // Initialise native target (idempotent).
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();

  // Build the JIT.
  auto jitOrErr = llvm::orc::LLJITBuilder().create();
  if (!jitOrErr)
    return jitOrErr.takeError();
  auto &jit = *jitOrErr;

  // Register runtime symbols so the JIT-compiled code can call them.
  auto &mainDylib = jit->getMainJITDylib();
  llvm::orc::SymbolMap runtimeSyms;
  for (const auto &sym : kRuntimeSymbols) {
    runtimeSyms[jit->mangleAndIntern(sym.Name)] = {
        llvm::orc::ExecutorAddr::fromPtr(sym.Addr),
        llvm::JITSymbolFlags::Exported};
  }
  if (auto err = mainDylib.define(
          llvm::orc::absoluteSymbols(std::move(runtimeSyms))))
    return std::move(err);

  // Add the module.
  auto tsm = llvm::orc::ThreadSafeModule(std::move(module), std::move(ctx));
  if (auto err = jit->addIRModule(std::move(tsm)))
    return std::move(err);

  // Look up main and call it.
  auto mainAddr = jit->lookup("main");
  if (!mainAddr)
    return mainAddr.takeError();

  auto *mainFn = mainAddr->toPtr<int (*)()>();
  return mainFn();
}

} // namespace paykan::jit
