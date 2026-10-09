// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// PIR — the Paykan intermediate representation.  See docs/pir.md for the
// specification; this header is the in-memory form shared by the lowering
// (producer), the printer/parser/verifier and every backend (consumers).
//
// Standard C++ only: this header is part of the barebones core.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace paykan::pir {

// -- Types

enum class Type : uint8_t {
  Void,
  I64,
  F64,
  Bool,
  Char,
  Box, // PaykanShared* (may be null)
  Obj, // raw PaykanObject* (any runtime or user class)
  Ptr, // any other pointer: vtable address, literal data, kind descriptor
};

const char *typeName(Type t);

/// A function signature: parameter types and return type.
struct Signature {
  std::vector<Type> Params;
  Type Ret = Type::Void;

  bool operator==(const Signature &o) const {
    return Params == o.Params && Ret == o.Ret;
  }
};

// -- Runtime symbols

/// A runtime extern (`extern fn` without a module, `extern obj`, `extern
/// vtable`) has the PIR name kRuntimePrefix + its C symbol
/// (`@$rt.PaykanString_new`), a name no Paykan identifier can spell.  So the
/// runtime and the program never share a PIR name: a user function may be
/// called `PaykanString_new` and stays an ordinary program symbol.  The
/// backends link a runtime extern by runtimeSymbol(), its C name.
inline constexpr std::string_view kRuntimePrefix = "$rt.";

/// The PIR name of the runtime C symbol @p symbol.
inline std::string runtimeName(std::string_view symbol) {
  std::string name(kRuntimePrefix);
  name += symbol;
  return name;
}

/// True when @p name is a runtime extern's PIR name.
inline bool isRuntimeName(std::string_view name) {
  return name.size() > kRuntimePrefix.size() &&
         name.starts_with(kRuntimePrefix);
}

/// The C symbol of the runtime extern named @p name (@p name itself when it
/// is not a runtime name, which the verifier rejects for a runtime extern).
inline std::string runtimeSymbol(std::string_view name) {
  return std::string(isRuntimeName(name) ? name.substr(kRuntimePrefix.size())
                                         : name);
}

// -- Values and operands

/// An SSA value defined by a parameter or an instruction.  Ids are unique per
/// function and are what operands refer to; Name is for readability only (the
/// printer emits `%Name.Id`, or `%Id` when Name is empty).
using ValueId = uint32_t;
inline constexpr ValueId kNoValue = 0; // ids start at 1

struct Value {
  ValueId Id = kNoValue;
  Type Ty = Type::Void;
  std::string Name;
};

/// A reference to a module-level symbol by name (`@sym`).  Which kind of item
/// it names (function, class, cstr/data/bytes global, extern object/vtable) is
/// resolved against the module by the verifier and the backends.
struct SymbolRef {
  std::string Name;
};

/// Instruction operand: an SSA value, a constant or a symbol address.
struct Operand {
  struct Null {
    Type Ty; // Box or Obj
  };
  std::variant<ValueId,  // %v
               int64_t,  // i64 constant
               double,   // f64 constant
               bool,     // bool constant
               char,     // char constant
               Null,     // null box / obj
               SymbolRef // @sym address (ptr for globals, obj for externs)
               >
      V;

  static Operand value(ValueId id) { return {id}; }
  static Operand value(const Value &v) { return {v.Id}; }
  static Operand i64(int64_t x) { return {x}; }
  static Operand f64(double x) { return {x}; }
  static Operand boolean(bool x) { return {x}; }
  static Operand chr(char x) { return {x}; }
  static Operand null(Type t) { return {Null{t}}; }
  static Operand symbol(std::string name) {
    return {SymbolRef{std::move(name)}};
  }

  bool isValue() const { return std::holds_alternative<ValueId>(V); }
  ValueId valueId() const { return std::get<ValueId>(V); }
};

// -- Instructions

enum class Opcode : uint8_t {
  // arithmetic / logic (operands of one type)
  Add,
  Sub,
  Mul,
  Div,
  Rem,
  Neg,
  Not,
  Cmp,    // Pred, two operands -> Bool
  Select, // cond, a, b
  IToF,   // i64 -> f64 (numeric)
  FToI,   // f64 -> i64 (numeric, toward zero; operand must be in range)
  Cast,   // reinterpret / resize to CastTo (see docs/pir.md)
  // calls
  Call,  // Callee = @fn, Args
  VCall, // receiver (obj) = Args[0], ClassName + Slot, Sig; Args[1..] = args
  // ARC and objects
  Retain,     // box
  Release,    // box
  Box,        // obj -> box   (PaykanShared_new)
  Unbox,      // box -> obj   (PaykanShared_get)
  New,        // ClassName -> obj
  Free,       // obj
  FieldLoad,  // obj, ClassName.Field -> field type
  FieldStore, // obj, ClassName.Field, value
  VTableLoad, // obj -> ptr
  VTableAddr, // ClassName or @extern vtable -> ptr
  // locals
  Load,  // Local -> type
  Store, // Local, value
  // addresses of scalar slots (`inout` parameters, docs/pir.md §6)
  LocalAddr, // Local -> ptr
  FieldAddr, // obj, ClassName.Field -> ptr
  PtrLoad,   // ptr -> Result.Ty (i64 f64 bool char)
  PtrStore,  // ptr, value
};

enum class CmpPred : uint8_t { Eq, Ne, Lt, Le, Gt, Ge };

/// Index of a `local` slot inside Function::Locals.
using LocalId = uint32_t;

struct Instr {
  Opcode Op;
  /// The defined value, when the instruction produces one (Ty != Void).
  Value Result;
  std::vector<Operand> Args;

  // Per-opcode extras (unused fields are left default):
  CmpPred Pred = CmpPred::Eq; // Cmp
  Type CastTo = Type::Void;   // Cast
  std::string Callee;         // Call: function symbol
  std::string ClassName;      // VCall / New / Field* / VTableAddr
  std::string Field;          // FieldLoad / FieldStore / FieldAddr
  uint32_t Slot = 0;          // VCall
  Signature Sig;              // VCall: the slot's signature
  LocalId Local = 0;          // Load / Store / LocalAddr
};

// -- Structured statements

struct Block;

struct If {
  Operand Cond;
  std::unique_ptr<Block> Then;
  std::unique_ptr<Block> Else; // may be null
};

struct While {
  /// Condition region: statements, then `Cond` is the bool value it ends with.
  std::unique_ptr<Block> CondBlock;
  Operand Cond;
  std::unique_ptr<Block> Body;
};

struct Break {};
struct Continue {};
struct Return {
  std::optional<Operand> Value;
};
struct Unreachable {};

using Stmt =
    std::variant<Instr, If, While, Break, Continue, Return, Unreachable>;

struct Block {
  std::vector<Stmt> Stmts;
};

// -- Functions

struct Local {
  std::string Name;
  Type Ty;
};

struct Function {
  std::string Name;
  Signature Sig;
  /// Parameter values (Id/Ty/Name); Sig.Params mirrors their types.
  std::vector<Value> Params;
  std::vector<Local> Locals;
  Block Body;
  /// Defined elsewhere: in the runtime (Module empty) or in the PIR module
  /// named by Module.  An extern function has no params/locals/body.
  bool IsExtern = false;
  std::string Module;
  /// For an extern from another PIR module: the function's name in its
  /// defining module, when that differs from Name (empty: the same).  Calls
  /// in this module use Name, so two modules' same-named functions (`x::tag`,
  /// `y::tag`) and a local one can be declared side by side.
  std::string Symbol;
  /// The defining module's name for this function (Symbol, or Name).
  const std::string &linkName() const { return Symbol.empty() ? Name : Symbol; }
  /// Next free ValueId for the builder / parser (ids are dense from 1).
  ValueId NextValueId = 1;
};

// -- Classes

struct Field {
  std::string Name;
  Type Ty; // I64 F64 Bool Char Box
};

struct VTableEntry {
  std::string Slot;   // method name (slot 0 is "destroy")
  std::string Target; // function symbol, or empty for an abstract (null) slot
  Signature Sig;
};

struct Class {
  std::string Name;
  std::string Super; // "" for the root (Obj)
  /// All fields, ancestors first, in layout order.
  std::vector<Field> Fields;
  std::vector<VTableEntry> VTable;
  /// Declared in another PIR module (layout known, vtable defined there).
  bool IsExtern = false;
  std::string Module;
};

// -- Module-level globals

struct CStrGlobal {
  std::string Name;
  std::string Data; // without the trailing NUL; Data.size() is the length
};

struct DataGlobal {
  std::string Name;
  std::vector<int64_t> Words;
};

struct BytesGlobal {
  std::string Name;
  std::vector<uint8_t> Bytes;
};

struct ExternGlobal {
  std::string Name;
  enum Kind : uint8_t { Object, VTable } K; // obj singleton or ptr vtable
};

struct Module {
  std::string Name; // resolved source path (identity across the program)
  std::vector<CStrGlobal> CStrs;
  std::vector<DataGlobal> Datas;
  std::vector<BytesGlobal> Bytes;
  std::vector<ExternGlobal> Externs;
  std::vector<Class> Classes;
  std::vector<Function> Functions;

  const Function *findFunction(const std::string &name) const;
  const Class *findClass(const std::string &name) const;
};

struct Program {
  /// Main module first; every module exactly once.
  std::vector<Module> Modules;
};

} // namespace paykan::pir
