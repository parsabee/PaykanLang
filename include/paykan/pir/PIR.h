// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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
  Box, // PaykanShared*: a strong reference (may be null)
  Obj, // raw PaykanObject* (any runtime or user class)
  Ptr, // any other pointer: vtable address, literal data, kind descriptor
  // Inline optionals (#96): a bool presence flag, then the payload at its
  // natural alignment, held and passed by value (docs/pir.md §1).
  OptI64,
  OptF64,
  OptBool,
  OptChar,
  // A run of bytes held in memory, never in a value: the type of a `local`
  // or a field that stores an aggregate (a fixed-size array), whose size and
  // alignment the local or field gives (docs/pir.md §1).
  Mem,
  // A weak reference (PaykanWeak*, may be null): the type of a `weak`
  // local or field (docs/pir.md §1).  Made from a box by `weak.new`, read
  // as a box by `weak.load`, dropped by `weak.release`.
  Weak,
};

const char *typeName(Type t);

/// The alignments a `mem` slot may have.
constexpr bool isMemAlign(int64_t a) {
  return a == 1 || a == 2 || a == 4 || a == 8;
}

/// True for an inline optional type (opt_i64 opt_f64 opt_bool opt_char).
constexpr bool isOptional(Type t) {
  return t == Type::OptI64 || t == Type::OptF64 || t == Type::OptBool ||
         t == Type::OptChar;
}

/// The payload type of the optional @p t (Void when @p t is not one).
constexpr Type optionalPayload(Type t) {
  switch (t) {
  case Type::OptI64:
    return Type::I64;
  case Type::OptF64:
    return Type::F64;
  case Type::OptBool:
    return Type::Bool;
  case Type::OptChar:
    return Type::Char;
  default:
    return Type::Void;
  }
}

/// The optional of the payload type @p t (Void when @p t has none: only
/// i64, f64, bool and char do).
constexpr Type optionalOf(Type t) {
  switch (t) {
  case Type::I64:
    return Type::OptI64;
  case Type::F64:
    return Type::OptF64;
  case Type::Bool:
    return Type::OptBool;
  case Type::Char:
    return Type::OptChar;
  default:
    return Type::Void;
  }
}

/// The bytes a value of type @p t takes in memory (a field, an array
/// element, a native function's PaykanType).  An optional is its presence
/// flag (a bool) and then its payload, at the payload's alignment.
constexpr int64_t storeSize(Type t) {
  switch (t) {
  case Type::Void:
    return 0;
  case Type::Bool:
  case Type::Char:
    return 1;
  case Type::OptBool:
  case Type::OptChar:
    return 2;
  case Type::OptI64:
  case Type::OptF64:
    return 16;
  default:
    return 8;
  }
}

/// The alignment of a value of type @p t in memory: an optional's is its
/// payload's.
constexpr int64_t storeAlign(Type t) {
  const Type payload = isOptional(t) ? optionalPayload(t) : t;
  return payload == Type::Void ? 1 : storeSize(payload);
}

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

/// A native C function a `native fn` calls (#198) is a module-less extern
/// named kNativePrefix + its C symbol (`@$c.paykan_io_write`).  Unlike a
/// runtime extern its signature is not the runtime's: the backends declare
/// it from its PIR signature.
inline constexpr std::string_view kNativePrefix = "$c.";

/// The PIR name of the native C symbol @p symbol.
inline std::string nativeName(std::string_view symbol) {
  std::string name(kNativePrefix);
  name += symbol;
  return name;
}

/// True when @p name is a native extern's PIR name.
inline bool isNativeName(std::string_view name) {
  return name.size() > kNativePrefix.size() && name.starts_with(kNativePrefix);
}

/// The C symbol of a module-less extern, runtime or native.
inline std::string externCSymbol(std::string_view name) {
  if (isNativeName(name))
    return std::string(name.substr(kNativePrefix.size()));
  return runtimeSymbol(name);
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

/// Whether @p a and @p b are the same i64, char or bool constant (two cases
/// of a switch that are one).
bool sameConstant(const Operand &a, const Operand &b);

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
  Retain,      // box
  Release,     // box
  Box,         // obj -> box   (PaykanShared_new)
  Unbox,       // box -> obj   (PaykanShared_get)
  New,         // ClassName -> obj
  Free,        // obj
  FieldLoad,   // obj, ClassName.Field -> field type
  FieldStore,  // obj, ClassName.Field, value
  VTableLoad,  // obj -> ptr
  VTableAddr,  // ClassName or @extern vtable -> ptr
  VTableStore, // obj, ClassName: the object's vtable becomes the class's
  // locals
  Load,  // Local -> type
  Store, // Local, value
  // addresses of slots (`inout` parameters, `mem` slots, docs/pir.md §6)
  LocalAddr, // Local -> ptr
  FieldAddr, // obj, ClassName.Field -> ptr
  PtrLoad,   // ptr -> Result.Ty (i64 f64 bool char box, an optional)
  PtrStore,  // ptr, value
  PtrAdd,    // ptr, i64 byte offset -> ptr
  MemCopy,   // dst ptr, src ptr, i64 byte count (the two may overlap)
  MemZero,   // ptr, i64 byte count
  // inline optionals (docs/pir.md §6)
  Some,   // payload -> its optional
  None,   // -> Result.Ty (an optional), absent
  IsSome, // optional -> bool
  Unwrap, // optional -> payload (zero for an absent one)
  // sized numeric types (docs/design/sized-types.md): every one is held in
  // an i64 or f64, in canonical form, and these restore the form
  Wrap,     // i64, Slot = 8/16/32 -> i64: the low Slot bits, sign-extended
  Mask,     // i64, Slot = 8/16/32 -> i64: the low Slot bits, zero-extended
  UDiv,     // i64, i64 -> i64: unsigned division (operand b nonzero)
  URem,     // i64, i64 -> i64: unsigned remainder (operand b nonzero)
  UCmp,     // Pred, two i64 -> bool: unsigned comparison
  UToF,     // i64 -> f64: the unsigned reading of the bits, rounded
  FToU,     // f64 -> i64: toward zero, as uint64 bits (operand in [0, 2^64))
  FRound32, // f64 -> f64: rounded to the nearest f32
  // weak references (docs/pir.md §6)
  WeakNew,     // box -> weak   (Paykan_weak_new: the weak count +1)
  WeakLoad,    // weak -> box   (Paykan_weak_load: a +1 box, or null)
  WeakRelease, // weak          (Paykan_weak_release)
  // function values (docs/pir.md §6)
  FnAddr, // Callee = @fn -> ptr: the function's address
  ICall,  // callee (ptr) = Args[0], Sig; Args[1..] = args
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
  CmpPred Pred = CmpPred::Eq; // Cmp / UCmp
  Type CastTo = Type::Void;   // Cast
  std::string Callee;         // Call / FnAddr: function symbol
  std::string ClassName;      // VCall / New / Field* / VTable{Addr,Store}
  std::string Field;          // FieldLoad / FieldStore / FieldAddr
  uint32_t Slot = 0;          // VCall; Wrap / Mask: the bit width
  Signature Sig;              // VCall: the slot's; ICall: the callee's
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

/// `switch %v { case c { ... } ... default { ... } }`: runs the block of the
/// case whose constant equals the value, else the default's (when there is
/// one).  The value is an i64, char or bool, every case constant one of
/// that type, no two of them equal.  (A match on floats is an if chain.)
struct Switch {
  struct Case {
    Operand Value;
    std::unique_ptr<Block> Body;
  };
  Operand Value;
  std::vector<Case> Cases;
  std::unique_ptr<Block> Default; // may be null
};

struct Break {};
struct Continue {};
struct Return {
  std::optional<Operand> Value;
};
struct Unreachable {};

using Stmt = std::variant<Instr, If, While, Break, Continue, Return,
                          Unreachable, Switch>;

struct Block {
  std::vector<Stmt> Stmts;
};

// -- Functions

struct Local {
  std::string Name;
  Type Ty;
  /// A `mem` local's bytes and alignment (zero for any other type).
  int64_t Size = 0;
  int64_t Align = 0;
  /// A `view` parameter's or variable's slot (`local %v: box view`): it
  /// names a value another variable holds, so no pass hands what it holds
  /// on (docs/pir.md §3, §12).
  bool View = false;
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
  /// An instantiation of a template (`Box<int>.get`, `$lib::f<int>`): every
  /// module that uses it may hold a copy, a program keeps one, and it links
  /// under a name that does not depend on its module.  An extern of one is
  /// shared too.
  bool Shared = false;
  /// For a compiled declaration (an extern naming its own module: the
  /// function is in the module's compiled code, docs/pir.md §4): the symbols
  /// of this module its compiled body references, which a program must keep
  /// while it keeps the function.
  std::vector<std::string> Uses;
  /// Next free ValueId for the builder / parser (ids are dense from 1).
  ValueId NextValueId = 1;
};

// -- Classes

struct Field {
  std::string Name;
  Type Ty; // I64 F64 Bool Char Box Weak, an inline optional, or Mem
  /// A `mem` field's bytes and alignment (zero for any other type).
  int64_t Size = 0;
  int64_t Align = 0;
};

struct VTableEntry {
  std::string Slot;   // method name (slot 0 is "destroy")
  std::string Target; // function symbol, or empty for a null slot
  Signature Sig;
};

struct Class {
  std::string Name;
  std::string Super; // "" for the root (Obj)
  /// All fields, ancestors first, in layout order.
  std::vector<Field> Fields;
  std::vector<VTableEntry> VTable;
  /// Declared in another PIR module (layout known, vtable defined there).
  /// Naming its own module, a compiled class: defined in this module's
  /// compiled code, its vtable still described (what the compiled vtable
  /// holds).
  bool IsExtern = false;
  std::string Module;
  /// An instantiation of a generic class (see Function::Shared).
  bool Shared = false;
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

  /// Whether @p f / @p c is a compiled declaration: an extern naming this
  /// module, whose definition is in the module's compiled code rather than
  /// in its PIR (a module loaded from a `.pkm` with an OBJ section).
  bool isCompiled(const Function &f) const {
    return f.IsExtern && f.Module == Name;
  }
  bool isCompiled(const Class &c) const {
    return c.IsExtern && c.Module == Name;
  }
  /// Whether this module defines @p f / @p c: in its PIR or compiled.
  bool defines(const Function &f) const { return !f.IsExtern || isCompiled(f); }
  bool defines(const Class &c) const { return !c.IsExtern || isCompiled(c); }
};

struct Program {
  /// Main module first; every module exactly once.
  std::vector<Module> Modules;
};

} // namespace paykan::pir
