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
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/TargetProcess/RegisterEHFrames.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>

namespace paykan::jit {

// -- Symbol table of the runtime functions and globals the JIT resolves

namespace {

struct RuntimeSymbol {
  const char *Name;
  void *Addr;
};

using namespace paykan::names;

// clang-format off
const RuntimeSymbol kRuntimeSymbols[] = {


    {kPaykanObjectNew,       reinterpret_cast<void *>(&PaykanObject_new)},
    {kPaykanObjectDestroy,   reinterpret_cast<void *>(&PaykanObject_destroy)},
    {kPaykanObjectToString,  reinterpret_cast<void *>(&PaykanObject_toString)},
    {kPaykanObjectEquals,    reinterpret_cast<void *>(&PaykanObject_equals)},
    {kPaykanObjectNone,      reinterpret_cast<void *>(&PaykanObject_None)},

    {kPaykanStringNew,       reinterpret_cast<void *>(&PaykanString_new)},
    {kPaykanStringFromInt,   reinterpret_cast<void *>(&PaykanString_from_int)},
    {kPaykanStringFromUint,  reinterpret_cast<void *>(&PaykanString_from_uint)},
    {kPaykanStringParseUint, reinterpret_cast<void *>(&PaykanString_parse_uint)},
    {kPaykanStringFromFloat, reinterpret_cast<void *>(&PaykanString_from_float)},
    {kPaykanStringFromBool,  reinterpret_cast<void *>(&PaykanString_from_bool)},
    {kPaykanStringFromChar,  reinterpret_cast<void *>(&PaykanString_from_char)},
    {kPaykanStringParseInt,  reinterpret_cast<void *>(&PaykanString_parse_int)},
    {kPaykanStringParseFloat, reinterpret_cast<void *>(&PaykanString_parse_float)},
    {kPaykanStringParseBool, reinterpret_cast<void *>(&PaykanString_parse_bool)},
    {kPaykanStringCharAt,    reinterpret_cast<void *>(&PaykanString_char_at)},
    {kPaykanStringToString,  reinterpret_cast<void *>(&PaykanString_toString)},
    {kPaykanStringDestroy,   reinterpret_cast<void *>(&PaykanString_destroy)},
    {kPaykanStringEquals,    reinterpret_cast<void *>(&PaykanString_equals)},
    {kPaykanStringLength,    reinterpret_cast<void *>(&PaykanString_length)},
    {kPaykanStringConcat,    reinterpret_cast<void *>(&PaykanString_concat)},
    {kPaykanStringAt,        reinterpret_cast<void *>(&PaykanString_at)},
    {kPaykanIntHash,         reinterpret_cast<void *>(&PaykanInt_hash)},
    {kPaykanStringHash,      reinterpret_cast<void *>(&PaykanString_hash)},

    {kPaykanPrint,           reinterpret_cast<void *>(&Paykan_print)},
    {kPaykanPrintln,         reinterpret_cast<void *>(&Paykan_println)},
    {kPaykanErrPrint,        reinterpret_cast<void *>(&Paykan_printerr)},
    {kPaykanErrPrintln,      reinterpret_cast<void *>(&Paykan_printerrln)},

    {kPaykanSharedNew,       reinterpret_cast<void *>(&PaykanShared_new)},
    {kPaykanSharedGet,       reinterpret_cast<void *>(&PaykanShared_get)},
    {kPaykanDispose,         reinterpret_cast<void *>(&Paykan_dispose)},
    {kPaykanWeakNew,         reinterpret_cast<void *>(&Paykan_weak_new)},
    {kPaykanWeakLoad,        reinterpret_cast<void *>(&Paykan_weak_load)},
    {kPaykanWeakRelease,     reinterpret_cast<void *>(&Paykan_weak_release)},

    {kPaykanPanicDivByZero,  reinterpret_cast<void *>(&Paykan_panic_div_by_zero)},
    {kPaykanPanicDivOverflow, reinterpret_cast<void *>(&Paykan_panic_div_overflow)},
    {kPaykanPanicFloatToInt, reinterpret_cast<void *>(&Paykan_panic_float_to_int)},
    {kPaykanPanicIntToChar,  reinterpret_cast<void *>(&Paykan_panic_int_to_char)},
    {kPaykanPanicFloatToType, reinterpret_cast<void *>(&Paykan_panic_float_to_type)},
    {kPaykanRoundF16,        reinterpret_cast<void *>(&Paykan_round_f16)},
    {kPaykanPanicIndex,      reinterpret_cast<void *>(&Paykan_panic_index)},
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

    {kPaykanTupleToString,   reinterpret_cast<void *>(&PaykanTuple_toString)},
    {kPaykanTupleEquals,     reinterpret_cast<void *>(&PaykanTuple_equals)},
    {kPaykanStructToString,  reinterpret_cast<void *>(&PaykanStruct_toString)},
    {kPaykanStructEquals,    reinterpret_cast<void *>(&PaykanStruct_equals)},

    // For the C code of generic `native fn`s.
    {kPaykanTypeCopy,        reinterpret_cast<void *>(&PaykanType_copy)},
    {kPaykanTypeDrop,        reinterpret_cast<void *>(&PaykanType_drop)},
    {kPaykanTypeIsUnset,     reinterpret_cast<void *>(&PaykanType_isUnset)},

    // Reached through `native fn`s (the standard library's memory::Buffer).
    {kPaykanBufferNew,       reinterpret_cast<void *>(&PaykanBuffer_new)},
    {kPaykanBufferLen,       reinterpret_cast<void *>(&PaykanBuffer_len)},
    {kPaykanBufferGet,       reinterpret_cast<void *>(&PaykanBuffer_get)},
    {kPaykanBufferCopy,      reinterpret_cast<void *>(&PaykanBuffer_copy)},
    {kPaykanBufferSet,       reinterpret_cast<void *>(&PaykanBuffer_set)},
    {kPaykanBufferReserve,   reinterpret_cast<void *>(&PaykanBuffer_reserve)},
    {kPaykanBufferGrow,      reinterpret_cast<void *>(&PaykanBuffer_grow)},
    {kPaykanBufferShrink,    reinterpret_cast<void *>(&PaykanBuffer_shrink)},
    {kPaykanBufferIsSet,     reinterpret_cast<void *>(&PaykanBuffer_isSet)},
    {kPaykanBufferTryGrow,   reinterpret_cast<void *>(&PaykanBuffer_tryGrow)},
    {kPaykanBufferTryReserve, reinterpret_cast<void *>(&PaykanBuffer_tryReserve)},
    {kPaykanPanicMessage,    reinterpret_cast<void *>(&Paykan_panic_message)},
    {kPaykanPanicValue,      reinterpret_cast<void *>(&Paykan_panic_value)},
    // Reached through `native fn`s (the standard library's ::io), with the
    // print functions above.
    {kPaykanIoOpen,          reinterpret_cast<void *>(&Paykan_io_open)},
    {kPaykanIoWrite,         reinterpret_cast<void *>(&Paykan_io_write)},
    {kPaykanIoReadln,        reinterpret_cast<void *>(&Paykan_io_readln)},
    {kPaykanIoReadBytes,     reinterpret_cast<void *>(&Paykan_io_readbytes)},
    {kPaykanIoRead,          reinterpret_cast<void *>(&Paykan_io_read)},
    {kPaykanIoStdin,         reinterpret_cast<void *>(&Paykan_io_stdin)},
    // The standard library's ::error.
    {kPaykanErrorCode,       reinterpret_cast<void *>(&Paykan_error_code)},
    {kPaykanErrorText,       reinterpret_cast<void *>(&Paykan_error_text)},
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

// -- Runtime panics
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

// -- Public API

llvm::Error setHostLayout(llvm::Module &module) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  // What LLJITBuilder uses when given no target machine builder, so the
  // module's layout is the one the JIT checks it against.
  auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
  if (!jtmb)
    return jtmb.takeError();
  auto layout = jtmb->getDefaultDataLayoutForTarget();
  if (!layout)
    return layout.takeError();
  module.setTargetTriple(jtmb->getTargetTriple().str());
  module.setDataLayout(*layout);
  return llvm::Error::success();
}

llvm::Expected<int> runModule(std::unique_ptr<llvm::Module> module,
                              std::unique_ptr<llvm::LLVMContext> ctx,
                              const std::vector<std::string> &args,
                              const std::vector<std::string> &nativeObjects) {
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
             "JIT symbol table is missing a symbol the lowering requires");
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

  // The program's native C (#198): its objects are linked like the module,
  // their runtime calls resolve against the table above, and their libc
  // calls against this process.
  if (!nativeObjects.empty()) {
    auto libc = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        jit->getDataLayout().getGlobalPrefix());
    if (!libc)
      return libc.takeError();
    mainDylib.addGenerator(std::move(*libc));
  }
  for (const std::string &path : nativeObjects) {
    auto buf = llvm::MemoryBuffer::getFile(path);
    if (!buf)
      return llvm::createStringError(buf.getError(), "cannot read '%s'",
                                     path.c_str());
    if (auto err = jit->addObjectFile(std::move(*buf)))
      return std::move(err);
  }

  // Look up main and call it.
  auto mainAddr = jit->lookup("main");
  if (!mainAddr)
    return mainAddr.takeError();

  AbortAsExitStatus panicsExit;

  if (mainParamCount == 2) {
    // main(args: view Str[]): a slice of the arguments' strings.
    std::vector<PaykanShared *> argv;
    argv.reserve(args.size());
    for (const auto &s : args)
      argv.push_back(PaykanShared_new((PaykanObject *)PaykanString_new(
          s.c_str(), static_cast<int64_t>(s.size()))));
    int rc = mainAddr->toPtr<int (*)(void *, int64_t)>()(
        argv.data(), static_cast<int64_t>(argv.size()));
    for (PaykanShared *arg : argv)
      Paykan_release(arg);
    return rc;
  }

  return mainAddr->toPtr<int (*)()>()();
}

} // namespace paykan::jit
