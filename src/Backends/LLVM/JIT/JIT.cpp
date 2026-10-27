// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0

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

#include <llvm/BinaryFormat/Magic.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
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
    {kPaykanStringParseF32,  reinterpret_cast<void *>(&PaykanString_parse_f32)},
    {kPaykanStringParseF16,  reinterpret_cast<void *>(&PaykanString_parse_f16)},
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
    {kPaykanPanicToChar,     reinterpret_cast<void *>(&Paykan_panic_to_char)},
    {kPaykanPanicFloatToType, reinterpret_cast<void *>(&Paykan_panic_float_to_type)},
    {kPaykanRoundF16,        reinterpret_cast<void *>(&Paykan_round_f16)},
    {kPaykanIntToF32,        reinterpret_cast<void *>(&Paykan_int_to_f32)},
    {kPaykanUintToF32,       reinterpret_cast<void *>(&Paykan_uint_to_f32)},
    {kPaykanPanicIndex,      reinterpret_cast<void *>(&Paykan_panic_index)},
    {kPaykanPanicForStep,    reinterpret_cast<void *>(&Paykan_panic_for_step)},
    {kPaykanPanicPureVirtual, reinterpret_cast<void *>(&Paykan_panic_pure_virtual)},
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
    {kPaykanEnumToString,    reinterpret_cast<void *>(&PaykanEnum_toString)},
    {kPaykanEnumEquals,      reinterpret_cast<void *>(&PaykanEnum_equals)},

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
    {kPaykanUnreachableView, reinterpret_cast<void *>(&Paykan_unreachable_view)},
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
    // ... and ::collections's string functions.
    {kPaykanStringSlice,     reinterpret_cast<void *>(&PaykanString_slice)},
    {kPaykanStringFind,      reinterpret_cast<void *>(&PaykanString_find)},
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

} // anonymous namespace

// -- Runtime panics
//
// A runtime panic (division by zero, an index out of bounds, ...) prints its
// message and calls abort(), which a program run as its own process ends
// with: its exit status is 128 + SIGABRT (134).  The JIT runs it in this
// process, where the abort would kill the compiler itself.  While main
// runs, SIGABRT is turned into that same exit status, so `paykan run` ends
// as the built executable does.  Like abort(), _Exit flushes
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
  module.setTargetTriple(jtmb->getTargetTriple());
  module.setDataLayout(*layout);
  return llvm::Error::success();
}

llvm::Expected<int> runModule(std::unique_ptr<llvm::Module> module,
                              std::unique_ptr<llvm::LLVMContext> ctx,
                              const std::vector<std::string> &args,
                              const std::vector<std::string> &nativeObjects,
                              llvm::CodeGenOptLevel codeGen) {
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

  // Build the JIT, for the host (as setHostLayout() laid the module out)
  // and generating code at @p codeGen: at -O0, where the module is not
  // optimised, the fast instruction selector, as a native -O0 build's.
  // Its default object linking layer registers the code's EH frames
  // itself: the in-process executor hands JITLink the addresses of the
  // registration functions, which are linked into this binary, so nothing
  // is looked up by name in the running process (LLVM 17 needed them
  // passed explicitly here on Linux).
  auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
  if (!jtmb)
    return jtmb.takeError();
  jtmb->setCodeGenOptLevel(codeGen);
  auto jitOrErr = llvm::orc::LLJITBuilder()
                      .setJITTargetMachineBuilder(std::move(*jtmb))
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

  // The program's native C (#198): its objects and archives are linked like
  // the module, their runtime calls resolve against the table above, and
  // their libc calls against this process.  An archive's members load as a
  // linker loads them, each when it defines a symbol still undefined; the
  // archives are searched before the process, as a linker searches them
  // before the C library.
  for (const std::string &path : nativeObjects) {
    auto buf = llvm::MemoryBuffer::getFile(path);
    if (!buf)
      return llvm::createStringError(buf.getError(), "cannot read '%s': %s",
                                     path.c_str(),
                                     buf.getError().message().c_str());
    auto cannotLoad = [&path](llvm::Error err) {
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "cannot load '%s': %s", path.c_str(),
                                     llvm::toString(std::move(err)).c_str());
    };
    if (llvm::identify_magic((*buf)->getBuffer()) ==
        llvm::file_magic::archive) {
      auto archive = llvm::orc::StaticLibraryDefinitionGenerator::Create(
          jit->getObjLinkingLayer(), std::move(*buf));
      if (!archive)
        return cannotLoad(archive.takeError());
      mainDylib.addGenerator(std::move(*archive));
    } else if (auto err = jit->addObjectFile(std::move(*buf))) {
      return cannotLoad(std::move(err));
    }
  }
  if (!nativeObjects.empty()) {
    auto libc = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        jit->getDataLayout().getGlobalPrefix());
    if (!libc)
      return libc.takeError();
    mainDylib.addGenerator(std::move(*libc));
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
