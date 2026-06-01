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
    {kPaykanFileNew,      reinterpret_cast<void *>(&PaykanFile_new)},
    {kPaykanFileOpen,     reinterpret_cast<void *>(&PaykanFile_open)},
    {kPaykanFileDestroy,  reinterpret_cast<void *>(&PaykanFile_destroy)},
    {kPaykanFileToString, reinterpret_cast<void *>(&PaykanFile_toString)},
    {kPaykanFileEquals,   reinterpret_cast<void *>(&PaykanFile_equals)},
    {kPaykanFileWrite,    reinterpret_cast<void *>(&PaykanFile_write)},
    {kPaykanFileReadln,   reinterpret_cast<void *>(&PaykanFile_readln)},
    {kPaykanFileVtable,   reinterpret_cast<void *>(&PaykanFile_vtable)},
    // match dispatch looks up "File_vtable" (ClassName + "_vtable"); alias it.
    {"File_vtable",       reinterpret_cast<void *>(&PaykanFile_vtable)},

    {kPaykanErrorNew,      reinterpret_cast<void *>(&PaykanError_new)},
    {kPaykanErrorDestroy,  reinterpret_cast<void *>(&PaykanError_destroy)},
    {kPaykanErrorToString, reinterpret_cast<void *>(&PaykanError_toString)},
    {kPaykanErrorEquals,   reinterpret_cast<void *>(&PaykanError_equals)},
    {kPaykanErrorVtable,   reinterpret_cast<void *>(&PaykanError_vtable)},
    // match dispatch looks up "Error_vtable"; alias it.
    {"Error_vtable",       reinterpret_cast<void *>(&PaykanError_vtable)},

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
    {kPaykanStringAt,        reinterpret_cast<void *>(&PaykanString_at)},

    {kPaykanPrint,           reinterpret_cast<void *>(&Paykan_print)},
    {kPaykanPrintln,         reinterpret_cast<void *>(&Paykan_println)},
    {kPaykanErrPrint,        reinterpret_cast<void *>(&Paykan_printerr)},
    {kPaykanErrPrintln,      reinterpret_cast<void *>(&Paykan_printerrln)},

    {kPaykanSharedNew,       reinterpret_cast<void *>(&PaykanShared_new)},
    {kPaykanSharedGet,       reinterpret_cast<void *>(&PaykanShared_get)},
    {kPaykanRetain,          reinterpret_cast<void *>(&Paykan_retain)},
    {kPaykanRelease,         reinterpret_cast<void *>(&Paykan_release)},

    {kPaykanObjectVtable,    reinterpret_cast<void *>(&PaykanObject_vtable)},
    {kPaykanStringVtable,    reinterpret_cast<void *>(&PaykanString_vtable)},
    // match dispatch looks up "Str_vtable" (ClassName + "_vtable"); alias it.
    {"Str_vtable",           reinterpret_cast<void *>(&PaykanString_vtable)},

    {kPaykanArrayNew,        reinterpret_cast<void *>(&PaykanArray_new)},
    {kPaykanArrayNewFromData,reinterpret_cast<void *>(&PaykanArray_new_from_data)},
    {kPaykanArrayNewObj,     reinterpret_cast<void *>(&PaykanArray_new_obj)},
    {kPaykanArrayDestroy,    reinterpret_cast<void *>(&PaykanArray_destroy)},
    {kPaykanArrayDestroyObj, reinterpret_cast<void *>(&PaykanArray_destroy_obj)},
    {kPaykanArrayGet,        reinterpret_cast<void *>(&PaykanArray_get)},
    {kPaykanArraySet,        reinterpret_cast<void *>(&PaykanArray_set)},
    {kPaykanArraySetObj,     reinterpret_cast<void *>(&PaykanArray_set_obj)},
    {kPaykanArrayLength,     reinterpret_cast<void *>(&PaykanArray_length)},
    {kPaykanArrayPush,       reinterpret_cast<void *>(&PaykanArray_push)},
    {kPaykanArrayPushObj,    reinterpret_cast<void *>(&PaykanArray_push_obj)},
    {kPaykanArrayPop,        reinterpret_cast<void *>(&PaykanArray_pop)},
    {kPaykanArrayPopObj,     reinterpret_cast<void *>(&PaykanArray_pop_obj)},
    {kPaykanArrayToString,   reinterpret_cast<void *>(&PaykanArray_toString)},
    {kPaykanArrayEquals,     reinterpret_cast<void *>(&PaykanArray_equals)},
    {kPaykanArrayVtable,     reinterpret_cast<void *>(&PaykanArray_vtable)},
    {kPaykanArrayObjVtable,  reinterpret_cast<void *>(&PaykanArray_obj_vtable)},
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
