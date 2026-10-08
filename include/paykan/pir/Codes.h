// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
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
    // 26-29 reserved (#96 some/none/is_some/unwrap), 30-31 (#99 weak.load/
    // weak.store), 32-33 (#170 global.load/global.store).
    {Opcode::LocalAddr, 34, "local.addr"},
    {Opcode::FieldAddr, 35, "field.addr"},
    {Opcode::PtrLoad, 36, "ptr.load"},
    {Opcode::PtrStore, 37, "ptr.store"},
    // 38-255 reserved.
};

struct TypeEntry {
  Type Id;
  uint8_t Code;
  const char *Name;
};

inline constexpr TypeEntry kTypeTable[] = {
    {Type::Void, 0, "void"}, {Type::I64, 1, "i64"},   {Type::F64, 2, "f64"},
    {Type::Bool, 3, "bool"}, {Type::Char, 4, "char"}, {Type::Box, 5, "box"},
    {Type::Obj, 6, "obj"},   {Type::Ptr, 7, "ptr"},
    // 8-15 reserved (#96 inline optionals), 16 (#99 weak), 17-255 reserved.
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
static_assert(std::size(kOpcodeTable) ==
                  static_cast<size_t>(Opcode::PtrStore) + 1,
              "every Opcode needs a row in kOpcodeTable");
static_assert(std::size(kTypeTable) == static_cast<size_t>(Type::Ptr) + 1,
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
