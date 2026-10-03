// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "JIT.h"
#include "Names.h"
#include "Runtime.h"

#include <cassert>
#include <csignal>
#include <cstdlib>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>

#include <llvm/ExecutionEngine/Orc/EPCEHFrameRegistrar.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/TargetProcess/RegisterEHFrames.h>
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
  void *Addr;
};

using namespace paykan::names;

// clang-format off
const RuntimeSymbol kRuntimeSymbols[] = {
    {kPaykanFileNew,      reinterpret_cast<void *>(&PaykanFile_new)},
    {kPaykanFileOpen,     reinterpret_cast<void *>(&PaykanFile_open)},
    {kPaykanFileDestroy,  reinterpret_cast<void *>(&PaykanFile_destroy)},
    {kPaykanFileToString, reinterpret_cast<void *>(&PaykanFile_toString)},
    {kPaykanFileEquals,   reinterpret_cast<void *>(&PaykanFile_equals)},
    {kPaykanFileWrite,     reinterpret_cast<void *>(&PaykanFile_write)},
    {kPaykanFileReadln,    reinterpret_cast<void *>(&PaykanFile_readln)},
    {kPaykanFileReadBytes, reinterpret_cast<void *>(&PaykanFile_readbytes)},
    {kPaykanFileRead,      reinterpret_cast<void *>(&PaykanFile_read)},
    {kPaykanFileVtable,    reinterpret_cast<void *>(&PaykanFile_vtable)},
    {kPaykanFileStdin,     reinterpret_cast<void *>(&PaykanFile_Stdin)},

    {kPaykanErrorNew,      reinterpret_cast<void *>(&PaykanError_new)},
    {kPaykanErrorDestroy,  reinterpret_cast<void *>(&PaykanError_destroy)},
    {kPaykanErrorToString, reinterpret_cast<void *>(&PaykanError_toString)},
    {kPaykanErrorEquals,   reinterpret_cast<void *>(&PaykanError_equals)},
    {kPaykanErrorVtable,   reinterpret_cast<void *>(&PaykanError_vtable)},

    {kPaykanObjectNew,       reinterpret_cast<void *>(&PaykanObject_new)},
    {kPaykanObjectDestroy,   reinterpret_cast<void *>(&PaykanObject_destroy)},
    {kPaykanObjectToString,  reinterpret_cast<void *>(&PaykanObject_toString)},
    {kPaykanObjectEquals,    reinterpret_cast<void *>(&PaykanObject_equals)},
    {kPaykanObjectNone,      reinterpret_cast<void *>(&PaykanObject_None)},

    {kPaykanIntNew,          reinterpret_cast<void *>(&PaykanInt_new)},
    {kPaykanIntValue,        reinterpret_cast<void *>(&PaykanInt_value)},
    {kPaykanIntFromStr,      reinterpret_cast<void *>(&PaykanInt_from_str)},
    {kPaykanIntVtable,       reinterpret_cast<void *>(&PaykanInt_vtable)},

    {kPaykanFloatNew,        reinterpret_cast<void *>(&PaykanFloat_new)},
    {kPaykanFloatValue,      reinterpret_cast<void *>(&PaykanFloat_value)},
    {kPaykanFloatFromStr,    reinterpret_cast<void *>(&PaykanFloat_from_str)},
    {kPaykanFloatVtable,     reinterpret_cast<void *>(&PaykanFloat_vtable)},

    {kPaykanBoolNew,         reinterpret_cast<void *>(&PaykanBool_new)},
    {kPaykanBoolValue,       reinterpret_cast<void *>(&PaykanBool_value)},
    {kPaykanBoolFromStr,     reinterpret_cast<void *>(&PaykanBool_from_str)},
    {kPaykanBoolVtable,      reinterpret_cast<void *>(&PaykanBool_vtable)},

    {kPaykanCharNew,         reinterpret_cast<void *>(&PaykanChar_new)},
    {kPaykanCharValue,       reinterpret_cast<void *>(&PaykanChar_value)},
    {kPaykanCharVtable,      reinterpret_cast<void *>(&PaykanChar_vtable)},

    {kPaykanStringNew,       reinterpret_cast<void *>(&PaykanString_new)},
    {kPaykanStringFromInt,   reinterpret_cast<void *>(&PaykanString_from_int)},
    {kPaykanStringFromFloat, reinterpret_cast<void *>(&PaykanString_from_float)},
    {kPaykanStringFromBool,  reinterpret_cast<void *>(&PaykanString_from_bool)},
    {kPaykanStringFromChar,  reinterpret_cast<void *>(&PaykanString_from_char)},
    {kPaykanStringCharAt,    reinterpret_cast<void *>(&PaykanString_char_at)},
    {kPaykanStringToString,  reinterpret_cast<void *>(&PaykanString_toString)},
    {kPaykanStringDestroy,   reinterpret_cast<void *>(&PaykanString_destroy)},
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

    {kPaykanPanicDivByZero,  reinterpret_cast<void *>(&Paykan_panic_div_by_zero)},
    {kPaykanPanicDivOverflow, reinterpret_cast<void *>(&Paykan_panic_div_overflow)},
    {kPaykanPanicFloatToInt, reinterpret_cast<void *>(&Paykan_panic_float_to_int)},
    {kPaykanPanicIntToChar,  reinterpret_cast<void *>(&Paykan_panic_int_to_char)},
    {kPaykanRetain,          reinterpret_cast<void *>(&Paykan_retain)},
    {kPaykanRelease,         reinterpret_cast<void *>(&Paykan_release)},

    {kPaykanMalloc,          reinterpret_cast<void *>(&Paykan_malloc)},
    {kPaykanRealloc,         reinterpret_cast<void *>(&Paykan_realloc)},
    {kPaykanFree,            reinterpret_cast<void *>(&Paykan_free)},
    {kPaykanHeapReset,       reinterpret_cast<void *>(&Paykan_heap_reset)},
    {kPaykanHeapLiveBlocks,  reinterpret_cast<void *>(&Paykan_heap_live_blocks)},
    {kPaykanHeapLiveBytes,   reinterpret_cast<void *>(&Paykan_heap_live_bytes)},

    {kPaykanObjectVtable,    reinterpret_cast<void *>(&PaykanObject_vtable)},
    {kPaykanStringVtable,    reinterpret_cast<void *>(&PaykanString_vtable)},

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

    {kPaykanTupleNew,        reinterpret_cast<void *>(&PaykanTuple_new)},
    {kPaykanTupleSet,        reinterpret_cast<void *>(&PaykanTuple_set)},
    {kPaykanTupleSetObj,     reinterpret_cast<void *>(&PaykanTuple_set_obj)},
    {kPaykanTupleGet,        reinterpret_cast<void *>(&PaykanTuple_get)},
    {kPaykanTupleDestroy,    reinterpret_cast<void *>(&PaykanTuple_destroy)},
    {kPaykanTupleToString,   reinterpret_cast<void *>(&PaykanTuple_toString)},
    {kPaykanTupleEquals,     reinterpret_cast<void *>(&PaykanTuple_equals)},
    {kPaykanTupleVtable,     reinterpret_cast<void *>(&PaykanTuple_vtable)},
};

// Match dispatch and generated vtable references look builtin vtables up as
// "<PaykanClassName>" + names::kVTableSuffix (e.g. "Str_vtable"); each
// runtime vtable is therefore also registered under that composed alias.
struct VTableAlias {
  const char *ClassName;
  void *Addr;
};

const VTableAlias kVTableAliases[] = {
    {kObj,      reinterpret_cast<void *>(&PaykanObject_vtable)},
    {kString,   reinterpret_cast<void *>(&PaykanString_vtable)},
    {kFile,     reinterpret_cast<void *>(&PaykanFile_vtable)},
    {kError,    reinterpret_cast<void *>(&PaykanError_vtable)},
    {kIntBox,   reinterpret_cast<void *>(&PaykanInt_vtable)},
    {kFloatBox, reinterpret_cast<void *>(&PaykanFloat_vtable)},
    {kBoolBox,  reinterpret_cast<void *>(&PaykanBool_vtable)},
    {kCharBox,  reinterpret_cast<void *>(&PaykanChar_vtable)},
    {kTuple,    reinterpret_cast<void *>(&PaykanTuple_vtable)},
};
// clang-format on

// Object linking layer for the LLJIT.
//
// LLJIT's default creates a JITLink layer whose EH-frame registration plugin
// finds llvm_orc_registerEHFrameSectionWrapper by NAME in the running process
// (dlsym).  That works on macOS, where every global symbol of an executable is
// visible to dlsym, and fails on Linux, where it is not unless the binary is
// linked with --export-dynamic: every program died with
//   JIT error: Symbols not found: [ llvm_orc_registerEHFrameSectionWrapper ]
// Hand the plugin the two functions' addresses directly -- they are linked
// into this binary -- so nothing depends on linker flags or symbol
// visibility, in the same spirit as the explicit runtime symbol table above.
llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>>
createObjectLinkingLayer(llvm::orc::ExecutionSession &es,
                         const llvm::Triple & /*triple*/) {
  auto layer = std::make_unique<llvm::orc::ObjectLinkingLayer>(es);
  layer->addPlugin(std::make_unique<llvm::orc::EHFrameRegistrationPlugin>(
      es, std::make_unique<llvm::orc::EPCEHFrameRegistrar>(
              es,
              llvm::orc::ExecutorAddr::fromPtr(
                  &llvm_orc_registerEHFrameSectionWrapper),
              llvm::orc::ExecutorAddr::fromPtr(
                  &llvm_orc_deregisterEHFrameSectionWrapper))));
  return layer;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Runtime panics
// ---------------------------------------------------------------------------
//
// A runtime panic (division by zero, an index out of bounds, ...) prints its
// message and calls abort().  The C backend runs the program as a child
// process and `paykan run` exits with 128 + SIGABRT (134); the JIT runs it
// in this process, where the abort would kill the compiler itself.  While
// main runs, SIGABRT is turned into that same exit status, so both backends
// end `paykan run` alike (docs/c-backend.md).  Like abort(), _Exit flushes
// no stdio buffer; the runtime's panic routine (Paykan_runtime_panic) flushes
// every output stream itself before printing the message.  A built
// executable still aborts with SIGABRT.

namespace {

extern "C" void exitOnAbort(int sig) { std::_Exit(128 + sig); }

class AbortAsExitStatus {
public:
  AbortAsExitStatus() : Prev(std::signal(SIGABRT, exitOnAbort)) {}
  ~AbortAsExitStatus() { std::signal(SIGABRT, Prev); }
  AbortAsExitStatus(const AbortAsExitStatus &) = delete;
  AbortAsExitStatus &operator=(const AbortAsExitStatus &) = delete;

private:
  void (*Prev)(int);
};

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

llvm::Expected<int> runModule(std::unique_ptr<llvm::Module> module,
                              std::unique_ptr<llvm::LLVMContext> ctx,
                              const std::vector<std::string> &args) {
  // Inspect main's param count before the module is consumed by the JIT.
  unsigned mainParamCount = 0;
  if (auto *mainIRFn = module->getFunction("main"))
    mainParamCount = mainIRFn->arg_size();

  // Take ownership of the module and its context together, before any early
  // return.  ThreadSafeModule destroys the module before the context; left in
  // the by-value parameters, their destruction order would follow the
  // unspecified argument construction order, and a context destroyed first
  // leaves ~Module() touching freed memory.
  auto tsm = llvm::orc::ThreadSafeModule(std::move(module), std::move(ctx));

  // Composed "<ClassName>_vtable" alias names (stable std::strings so the
  // symbol map and the debug drift-check below can both reference them).
  std::vector<std::string> vtableAliasNames;
  vtableAliasNames.reserve(std::size(kVTableAliases));
  for (const auto &alias : kVTableAliases)
    vtableAliasNames.push_back(std::string(alias.ClassName) +
                               names::kVTableSuffix);

#ifndef NDEBUG
  {
    std::unordered_set<std::string_view> registered;
    for (const auto &sym : kRuntimeSymbols)
      registered.insert(sym.Name);
    for (const auto &name : vtableAliasNames)
      registered.insert(name);
    for (const char *name : names::kCodeGenRequiredSymbols)
      assert(registered.count(name) &&
             "JIT symbol table is missing a symbol required by CodeGen");
  }
#endif

  // Initialise native target (idempotent).
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();

  // Build the JIT.
  auto jitOrErr = llvm::orc::LLJITBuilder()
                      .setObjectLinkingLayerCreator(createObjectLinkingLayer)
                      .create();
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
  for (size_t i = 0; i < std::size(kVTableAliases); ++i) {
    runtimeSyms[jit->mangleAndIntern(vtableAliasNames[i])] = {
        llvm::orc::ExecutorAddr::fromPtr(kVTableAliases[i].Addr),
        llvm::JITSymbolFlags::Exported};
  }
  if (auto err =
          mainDylib.define(llvm::orc::absoluteSymbols(std::move(runtimeSyms))))
    return std::move(err);

  // Add the module.
  if (auto err = jit->addIRModule(std::move(tsm)))
    return std::move(err);

  // Look up main and call it.
  auto mainAddr = jit->lookup("main");
  if (!mainAddr)
    return mainAddr.takeError();

  AbortAsExitStatus panicsExit;

  if (mainParamCount == 1) {
    // main(args: Str[]) — build a PaykanArray<Str> from the args vector.
    PaykanArray *arr = PaykanArray_new_obj(0);
    for (const auto &s : args) {
      PaykanString *ps = PaykanString_new(s.c_str(), (int64_t)s.size());
      PaykanShared *shared = PaykanShared_new((PaykanObject *)ps);
      PaykanArray_push_obj(arr, shared);
      Paykan_release(shared); // array retained; drop our ref
    }
    PaykanShared *argsShared = PaykanShared_new((PaykanObject *)arr);
    // Transfer ownership to main — its scope cleanup releases argsShared.
    return mainAddr->toPtr<int (*)(void *)>()(argsShared);
  }

  return mainAddr->toPtr<int (*)()>()();
}

} // namespace paykan::jit
