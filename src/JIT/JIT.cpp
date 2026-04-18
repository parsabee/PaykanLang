// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "JIT.h"
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

// clang-format off
const RuntimeSymbol kRuntimeSymbols[] = {
    {"PaykanObject_new",       reinterpret_cast<void *>(&PaykanObject_new)},
    {"PaykanObject_delete",    reinterpret_cast<void *>(&PaykanObject_delete)},
    {"PaykanObject_toString",  reinterpret_cast<void *>(&PaykanObject_toString)},
    {"PaykanObject_equals",    reinterpret_cast<void *>(&PaykanObject_equals)},

    {"PaykanString_new",       reinterpret_cast<void *>(&PaykanString_new)},
    {"PaykanString_delete",    reinterpret_cast<void *>(&PaykanString_delete)},
    {"PaykanString_toString",  reinterpret_cast<void *>(&PaykanString_toString)},
    {"PaykanString_equals",    reinterpret_cast<void *>(&PaykanString_equals)},
    {"PaykanString_length",    reinterpret_cast<void *>(&PaykanString_length)},
    {"PaykanString_concat",    reinterpret_cast<void *>(&PaykanString_concat)},

    {"Paykan_out",             reinterpret_cast<void *>(&Paykan_out)},
    {"Paykan_err",             reinterpret_cast<void *>(&Paykan_err)},

    {"PaykanShared_new",       reinterpret_cast<void *>(&PaykanShared_new)},
    {"PaykanShared_get",       reinterpret_cast<void *>(&PaykanShared_get)},
    {"Paykan_retain",          reinterpret_cast<void *>(&Paykan_retain)},
    {"Paykan_release",         reinterpret_cast<void *>(&Paykan_release)},

    {"PaykanObject_vtable",    reinterpret_cast<void *>(&PaykanObject_vtable)},
    {"PaykanString_vtable",    reinterpret_cast<void *>(&PaykanString_vtable)},
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
