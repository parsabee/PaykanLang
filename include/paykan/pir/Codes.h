// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// The one table of PIR opcodes and types: each enumerator's mnemonic (what
// the text printer writes and the parser reads) and its wire code (what the
// binary codec writes, docs/design/pkm.md §5.4-§5.5).  Wire codes are fixed
// by this table and never by the enumerator's value, so the enum can be
// reordered without changing a single stored blob; codes not listed here are
// reserved and a reader rejects them.

#pragma once

#include "paykan/pir/PIR.h"

#include <cstddef>
#include <cstdint>
#include <iterator>

namespace paykan::pir {

struct OpcodeEntry {
  Opcode Id;
  uint8_t Code;
  const char *Name;
};

inline constexpr OpcodeEntry kOpcodeTable[] = {
    {Opcode::Add, 0, "add"},
    {Opcode::Sub, 1, "sub"},
    {Opcode::Mul, 2, "mul"},
    {Opcode::Div, 3, "div"},
    {Opcode::Rem, 4, "rem"},
    {Opcode::Neg, 5, "neg"},
    {Opcode::Not, 6, "not"},
    {Opcode::Cmp, 7, "cmp"},
    {Opcode::Select, 8, "select"},
    {Opcode::IToF, 9, "itof"},
    {Opcode::FToI, 10, "ftoi"},
    {Opcode::Cast, 11, "cast"},
    {Opcode::Call, 12, "call"},
    {Opcode::VCall, 13, "vcall"},
    {Opcode::Retain, 14, "retain"},
    {Opcode::Release, 15, "release"},
    {Opcode::Box, 16, "box"},
    {Opcode::Unbox, 17, "unbox"},
    {Opcode::New, 18, "new"},
    {Opcode::Free, 19, "free"},
    {Opcode::FieldLoad, 20, "field.load"},
    {Opcode::FieldStore, 21, "field.store"},
    {Opcode::VTableLoad, 22, "vtable.load"},
    {Opcode::VTableAddr, 23, "vtable.addr"},
    {Opcode::Load, 24, "load"},
    {Opcode::Store, 25, "store"},
    {Opcode::Some, 26, "some"},
    {Opcode::None, 27, "none"},
    {Opcode::IsSome, 28, "is_some"},
    {Opcode::Unwrap, 29, "unwrap"},
    {Opcode::WeakLoad, 30, "weak.load"},
    {Opcode::WeakNew, 31, "weak.new"},
    // 32-33 reserved (#170 global.load/global.store).
    {Opcode::LocalAddr, 34, "local.addr"},
    {Opcode::FieldAddr, 35, "field.addr"},
    {Opcode::PtrLoad, 36, "ptr.load"},
    {Opcode::PtrStore, 37, "ptr.store"},
    {Opcode::PtrAdd, 38, "ptr.add"},
    {Opcode::MemCopy, 39, "mem.copy"},
    {Opcode::MemZero, 40, "mem.zero"},
    // Sized numeric types (docs/design/sized-types.md).
    {Opcode::Wrap, 41, "wrap"},
    {Opcode::Mask, 42, "mask"},
    {Opcode::UDiv, 43, "udiv"},
    {Opcode::URem, 44, "urem"},
    {Opcode::UCmp, 45, "ucmp"},
    {Opcode::UToF, 46, "utof"},
    {Opcode::FToU, 47, "ftou"},
    {Opcode::FRound32, 48, "fround32"},
    {Opcode::WeakRelease, 49, "weak.release"},
    // Function values (docs/design/function-types.md).
    {Opcode::FnAddr, 50, "fn.addr"},
    {Opcode::ICall, 51, "icall"},
    // 52-255 reserved.
};

struct TypeEntry {
  Type Id;
  uint8_t Code;
  const char *Name;
};

inline constexpr TypeEntry kTypeTable[] = {
    {Type::Void, 0, "void"},         {Type::I64, 1, "i64"},
    {Type::F64, 2, "f64"},           {Type::Bool, 3, "bool"},
    {Type::Char, 4, "char"},         {Type::Box, 5, "box"},
    {Type::Obj, 6, "obj"},           {Type::Ptr, 7, "ptr"},
    {Type::OptI64, 8, "opt_i64"},    {Type::OptF64, 9, "opt_f64"},
    {Type::OptBool, 10, "opt_bool"}, {Type::OptChar, 11, "opt_char"},
    {Type::Weak, 16, "weak"},        {Type::Mem, 17, "mem"},
    // 12-15 reserved (#96 further inline optionals), 18-255 reserved.
};

namespace detail {

/// True when no two rows share an enumerator, a code or a name.
template <typename T, size_t N> constexpr bool rowsUnique(const T (&table)[N]) {
  for (size_t i = 0; i < N; ++i)
    for (size_t j = i + 1; j < N; ++j) {
      bool sameName = true;
      for (size_t k = 0; sameName; ++k) {
        sameName = table[i].Name[k] == table[j].Name[k];
        if (table[i].Name[k] == '\0')
          break;
      }
      if (table[i].Id == table[j].Id || table[i].Code == table[j].Code ||
          sameName)
        return false;
    }
  return true;
}

} // namespace detail

// The enumerators are dense from 0, so a complete table has exactly one row
// per enumerator up to the last one.  Adding an enumerator without a row
// fails here or in the exhaustive switch of tests/PIR/BinaryTests.cpp.
static_assert(std::size(kOpcodeTable) == static_cast<size_t>(Opcode::ICall) + 1,
              "every Opcode needs a row in kOpcodeTable");
static_assert(std::size(kTypeTable) == static_cast<size_t>(Type::Weak) + 1,
              "every Type needs a row in kTypeTable");
static_assert(detail::rowsUnique(kOpcodeTable), "duplicate opcode row");
static_assert(detail::rowsUnique(kTypeTable), "duplicate type row");

/// The wire code / mnemonic of an enumerator (every enumerator has a row).
constexpr const OpcodeEntry &opcodeEntry(Opcode op) {
  for (const OpcodeEntry &e : kOpcodeTable)
    if (e.Id == op)
      return e;
  return kOpcodeTable[0]; // unreachable: the static_asserts above
}
constexpr const TypeEntry &typeEntry(Type t) {
  for (const TypeEntry &e : kTypeTable)
    if (e.Id == t)
      return e;
  return kTypeTable[0];
}

/// The enumerator with wire code @p code; false for a reserved code.
constexpr bool opcodeFromCode(uint8_t code, Opcode &out) {
  for (const OpcodeEntry &e : kOpcodeTable)
    if (e.Code == code) {
      out = e.Id;
      return true;
    }
  return false;
}
constexpr bool typeFromCode(uint8_t code, Type &out) {
  for (const TypeEntry &e : kTypeTable)
    if (e.Code == code) {
      out = e.Id;
      return true;
    }
  return false;
}

/// The enumerator with mnemonic @p name; false for an unknown name.
constexpr bool opcodeFromName(std::string_view name, Opcode &out) {
  for (const OpcodeEntry &e : kOpcodeTable)
    if (name == e.Name) {
      out = e.Id;
      return true;
    }
  return false;
}
constexpr bool typeFromName(std::string_view name, Type &out) {
  for (const TypeEntry &e : kTypeTable)
    if (name == e.Name) {
      out = e.Id;
      return true;
    }
  return false;
}

} // namespace paykan::pir
