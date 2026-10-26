// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0

#include "paykan/pir/PIR.h"

#include "Names.h"
#include "paykan/pir/Codes.h"

namespace paykan::pir {

const char *typeName(Type t) { return typeEntry(t).Name; }

// -- The runtime's C functions (docs/pir.md §8)

namespace {

struct RuntimeSig {
  const char *Name;
  Signature Sig;
};

const Type kObj = Type::Obj;
const Type kBox = Type::Box;
const Type kI64 = Type::I64;
const Type kF64 = Type::F64;
const Type kChr = Type::Char;
const Type kPtr = Type::Ptr;
const Type kWeak = Type::Weak;
const Type kVoid = Type::Void;

using namespace names;

const RuntimeSig kRuntimeSigs[] = {
    {kPaykanRetain, {{kBox}, kVoid}},
    {kPaykanRelease, {{kBox}, kVoid}},
    {kPaykanSharedNew, {{kObj}, kBox}},
    {kPaykanSharedGet, {{kBox}, kObj}},
    {kPaykanPanicDivByZero, {{}, kVoid}},
    {kPaykanPanicDivOverflow, {{}, kVoid}},
    {kPaykanPanicFloatToInt, {{kF64}, kVoid}},
    {kPaykanPanicFloatToType, {{kF64, kPtr, kI64}, kVoid}},
    {kPaykanRoundF16, {{kF64}, kF64}},
    {kPaykanIntToF32, {{kI64}, kF64}},
    {kPaykanUintToF32, {{kI64}, kF64}},
    {kPaykanPanicIntToChar, {{kI64}, kVoid}},
    {kPaykanPanicToChar, {{kI64, kI64, kPtr, kI64}, kVoid}},
    {kPaykanPanicIndex, {{kI64, kI64}, kVoid}},
    {kPaykanPanicForStep, {{}, kVoid}},
    {kPaykanPanicPureVirtual, {{kPtr, kI64, kPtr, kI64}, kVoid}},
    {kPaykanStringNew, {{kPtr, kI64}, kObj}},
    {kPaykanStringDestroy, {{kObj}, kVoid}},
    {kPaykanDispose, {{kObj}, kVoid}},
    {kPaykanStringConcat, {{kObj, kObj}, kObj}},
    {kPaykanStringCharAt, {{kObj, kI64}, kChr}},
    {kPaykanStringFromInt, {{kI64}, kObj}},
    {kPaykanStringFromUint, {{kI64}, kObj}},
    {kPaykanStringParseUint, {{kObj, kPtr}, kI64}},
    {kPaykanStringFromFloat, {{kF64}, kObj}},
    {kPaykanStringFromBool, {{kI64}, kObj}},
    {kPaykanStringFromChar, {{kChr}, kObj}},
    {kPaykanStringParseInt, {{kObj, kPtr}, kI64}},
    {kPaykanStringParseFloat, {{kObj, kPtr}, kI64}},
    {kPaykanStringParseBool, {{kObj, kPtr}, kI64}},
    {kPaykanStringParseF32, {{kObj, kPtr}, kI64}},
    {kPaykanStringParseF16, {{kObj, kPtr}, kI64}},
    // The vtable `equals`s spell out their slot's ownership mask
    // (docs/pir.md §3, §8).
    {kPaykanStringEquals, {{kObj, kBox, kI64}, kI64}},
    {kPaykanStringToString, {{kObj}, kBox}},
    {kPaykanStringLength, {{kObj}, kI64}},
    {kPaykanIntHash, {{kI64}, kI64}},
    {kPaykanStringHash, {{kObj}, kI64}},
    {kPaykanTupleToString, {{kPtr, kPtr}, kObj}},
    {kPaykanTupleEquals, {{kPtr, kPtr, kPtr}, kI64}},
    {kPaykanStructToString, {{kPtr, kPtr}, kObj}},
    {kPaykanStructEquals, {{kPtr, kPtr, kPtr}, kI64}},
    {kPaykanEnumToString, {{kPtr, kPtr}, kObj}},
    {kPaykanEnumEquals, {{kPtr, kPtr, kPtr}, kI64}},
    {kPaykanObjectDestroy, {{kObj}, kVoid}},
    {kPaykanObjectToString, {{kObj}, kBox}},
    {kPaykanObjectEquals, {{kObj, kBox, kI64}, kI64}},
    // What the backends call for `new`, `free`, `weak.*` and an f64's
    // `rem` (src/Backends/MLIR/Lowering/PIRToStandard.cpp).
    {kPaykanMalloc, {{kI64}, kPtr}},
    {kPaykanFree, {{kPtr}, kVoid}},
    {kPaykanWeakNew, {{kBox}, kWeak}},
    {kPaykanWeakLoad, {{kWeak}, kBox}},
    {kPaykanWeakRelease, {{kWeak}, kVoid}},
    {"fmod", {{kF64, kF64}, kF64}},
    // What an executable's entry code calls (C's main() on the C path,
    // src/Backends/LLVM/CodeGen/NativeBuild.cpp's main), and what the C
    // path calls itself for a retain, a release, a `mem.copy` and a `cast`
    // between f64 and i64 (src/Backends/MLIR/ToEmitC/ToEmitC.cpp), in PIR
    // types (a `size_t` an i64).
    {kPaykanArgsFree, {{kPtr, kI64}, kVoid}},
    {kPaykanHeapReset, {{}, kVoid}},
    {kPaykanHeapDump, {{}, kVoid}},
    {"getenv", {{kPtr}, kPtr}},
    {kPaykanRetainInline, {{kBox}, kVoid}},
    {kPaykanReleaseInline, {{kBox}, kVoid}},
    {"memmove", {{kPtr, kPtr, kI64}, kPtr}},
    {"memcpy", {{kPtr, kPtr, kI64}, kPtr}},
};

/// What that code calls with a C `int`, which no PIR type is, so they have
/// no signature in PIR types: the entry code's Paykan_args_new and
/// Paykan_heap_set_tracking, and the memset of the C path's `mem.zero`.
const char *const kIntTakingSymbols[] = {
    kPaykanArgsNew, // (int argc, char **argv, int64_t *count)
    kPaykanHeapSetTracking,
    "memset",
};

} // namespace

const Signature *runtimeSignature(std::string_view symbol) {
  for (const RuntimeSig &rs : kRuntimeSigs)
    if (symbol == rs.Name)
      return &rs.Sig;
  return nullptr;
}

bool isRuntimeSymbol(std::string_view symbol) {
  for (const char *s : kIntTakingSymbols)
    if (symbol == s)
      return true;
  return runtimeSignature(symbol) != nullptr;
}

const Function *Module::findFunction(const std::string &name) const {
  for (const Function &f : Functions)
    if (f.Name == name)
      return &f;
  return nullptr;
}

const Class *Module::findClass(const std::string &name) const {
  for (const Class &c : Classes)
    if (c.Name == name)
      return &c;
  return nullptr;
}

bool lendsType(const Signature &sig) {
  for (Type t : sig.Params)
    if (t == Type::Box)
      return true;
  return false;
}

bool lends(const Function &f) {
  return !(f.IsExtern && f.Module.empty()) && lendsType(f.Sig);
}

size_t boxIndex(const Signature &sig, size_t i) {
  size_t k = 0;
  for (size_t p = 0; p < i && p < sig.Params.size(); ++p)
    if (sig.Params[p] == Type::Box)
      ++k;
  return k;
}

Signature machineSignature(const Signature &sig, bool lend) {
  Signature m = sig;
  if (lend)
    m.Params.push_back(Type::I64);
  return m;
}

bool sameConstant(const Operand &a, const Operand &b) {
  if (a.V.index() != b.V.index())
    return false;
  if (const auto *x = std::get_if<int64_t>(&a.V))
    return *x == std::get<int64_t>(b.V);
  if (const auto *x = std::get_if<bool>(&a.V))
    return *x == std::get<bool>(b.V);
  if (const auto *x = std::get_if<char>(&a.V))
    return *x == std::get<char>(b.V);
  return false;
}

bool CaseSet::insert(const Operand &c) {
  if (const auto *x = std::get_if<int64_t>(&c.V))
    return I64s.insert(*x).second;
  if (const auto *x = std::get_if<char>(&c.V)) {
    const auto i = static_cast<unsigned char>(*x);
    const bool fresh = !Chars.test(i);
    Chars.set(i);
    return fresh;
  }
  if (const auto *x = std::get_if<bool>(&c.V)) {
    const size_t i = *x ? 1 : 0;
    const bool fresh = !Bools.test(i);
    Bools.set(i);
    return fresh;
  }
  return true; // no other operand is the same constant as another
}

} // namespace paykan::pir
