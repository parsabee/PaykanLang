// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// The IFACE section (docs/design/pkm.md §3): what Sema needs to type-check
// an importer, as a self-delimiting blob with the part table of §3.2.
//
// Prototype: the framing (magic, versions, parts, record encoding, STRS,
// MODS, DIAG) is the design's; the DECLS records carry the string-typed
// declarations of today's Sema::ModuleInfo instead of the structured type
// table (TYPES and INST are written empty).  Phase A3 replaces the record
// payloads, not the framing.  The Interface struct mirrors ModuleInfo field
// for field so the integrator's conversion is mechanical; this library never
// includes Sema.

#pragma once

#include "paykan/Status.h"
#include "paykan/pkm/Format.h"
#include "paykan/support/Sha256.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace paykan::pkm {

/// IFACE blob flags (§3.2).
inline constexpr uint32_t kIfaceHasTmpl = 1 << 0;
inline constexpr uint32_t kIfaceSystem = 1 << 1;

/// Part tags (§3.2).
enum class PartTag : uint32_t {
  Strs = 1,
  Mods = 2,
  Types = 3,
  Decls = 4,
  Inst = 5,
  Diag = 7
};

/// DECLS record tags (§3.5); TRAIT, IMPL, CONST and TEMPLATE_* are not
/// written by the prototype.  STRUCT (9) is the prototype's record for a
/// struct type (docs/language/11-structs.md): a name, fields and methods,
/// like a CLASS without a superclass or a vtable.
enum class DeclTag : uint8_t { Enum = 1, Class = 2, Func = 5, Struct = 9 };

/// A MODS entry (§3.3): index 0 is the module itself (hashes zero), the
/// rest its direct dependencies sorted by canonical name.
struct ModuleRef {
  std::string Canonical;
  bool System = false;
  support::Hash256 IfaceHash{};
  support::Hash256 TmplHash{};
  bool operator==(const ModuleRef &) const = default;
};

/// A parameter's mode (ast::ParamMode's values).
inline constexpr uint8_t kModeValue = 0;
inline constexpr uint8_t kModeView = 1;
inline constexpr uint8_t kModeInout = 2;

/// A method record's flags byte (ast::MethodDecl's flags).
inline constexpr uint8_t kMethodPrivate = 1 << 0;
inline constexpr uint8_t kMethodView = 1 << 1; ///< a `view fn`
inline constexpr uint8_t kMethodImpl = 1 << 2; ///< an `impl` block's method
/// A pure virtual method: a class's vtable slot with no definition (§3.5's
/// PURE bit).  Never an `impl` method's, a struct's, an initializer's or
/// `destroy`'s (readInterface rejects it there).
inline constexpr uint8_t kMethodPure = 1 << 4;
inline constexpr uint8_t kMethodKnownFlags =
    kMethodPrivate | kMethodView | kMethodImpl | kMethodPure;

/// The modes of a signature's parameters (ast::ParamModes): one per
/// parameter with its name, or none when every parameter is by value, and
/// how the result is returned (a copy, or a `view` / `inout` borrow).
/// They are appended to the FUNC and CLASS records.
struct ModeRecs {
  std::vector<uint8_t> Modes;
  std::vector<std::string> Names;
  uint8_t Result = kModeValue;
  bool operator==(const ModeRecs &) const = default;
};

/// A FUNC record's flags byte, after its result mode (§3.5): bit 0 for a
/// `native fn`, whose body is C (docs/language/03-functions.md, "Native
/// Functions"): it keeps nothing it is lent and changes only what its
/// `inout` arguments name.
inline constexpr uint8_t kFuncNative = 1 << 0;
inline constexpr uint8_t kFuncKnownFlags = kFuncNative;

/// ModuleInfo::FunctionInfo.
struct FuncRec {
  std::string Name;
  std::string ReturnTypeName;
  std::vector<std::string> ParamTypeNames;
  ModeRecs Modes;
  bool Native = false; ///< kFuncNative
  bool operator==(const FuncRec &) const = default;
};

/// ModuleInfo::ClassInfo (OriginPath is a host path and is never written;
/// OriginModule is the canonical name).
struct ClassRec {
  struct Field {
    std::string FieldName;
    std::string TypeName;
    bool Weak = false; ///< a class field declared `weak` (never a struct's)
    bool operator==(const Field &) const = default;
  };
  struct Method {
    std::string Name;
    std::string ReturnTypeName;
    std::vector<std::string> ParamTypeNames;
    uint8_t Flags = 0; ///< kMethod* bits
    ModeRecs Modes;
    bool operator==(const Method &) const = default;
  };
  std::string Name;
  std::string SuperClassName; ///< "" -> implicit Object root
  bool IsLocal = true;
  std::string OriginModule;
  std::vector<Field> Fields;
  std::vector<Method> Methods;
  bool operator==(const ClassRec &) const = default;
};

/// ModuleInfo::StructInfo: the fields in declaration order and the methods
/// (`__init__` first, then the `impl` methods), encoded as a ClassRec's.
struct StructRec {
  std::string Name;
  bool IsLocal = true;
  std::string OriginModule;
  std::vector<ClassRec::Field> Fields;
  std::vector<ClassRec::Method> Methods;
  bool operator==(const StructRec &) const = default;
};

/// ModuleInfo::EnumInfo.
struct EnumRec {
  std::string Name;
  std::vector<std::string> Variants;
  bool IsLocal = true;
  std::string OriginModule;
  /// The variants that have fields, for a tagged union: each one's fields
  /// are the STRUCT record `Name::variant`.  Written after the variants
  /// only when there are any.
  std::vector<std::string> PayloadVariants;
  bool operator==(const EnumRec &) const = default;
};

struct Interface {
  std::string Module; ///< canonical name (MODS[0])
  bool System = false;
  std::vector<ModuleRef> Mods; ///< direct dependencies (MODS[1..])
  std::vector<FuncRec> Functions;
  std::vector<ClassRec> Classes;
  std::vector<EnumRec> Enums;
  std::vector<StructRec> Structs;
  std::string DisplayFile; ///< DIAG: project-relative; "" writes no DIAG
  bool operator==(const Interface &) const = default;
};

/// Deterministic: dependencies and records are sorted by name, the string
/// table is in first-use order.
std::vector<uint8_t> writeInterface(const Interface &iface);

/// The checks of §3.7 that apply to the prototype records: magic/major,
/// parts in bounds, aligned and non-overlapping, STRS valid, MODS[0] equal
/// to @p expectedModule, dependencies sorted and unique, records sorted by
/// (tag, name) with unique names per kind, every string index in range,
/// function and method flags known, and kMethodPure only on a class's
/// virtual method (not `__init__`, `___init__` or `destroy`).
StatusOr<Interface> readInterface(std::span<const uint8_t> bytes,
                                  std::string_view expectedModule);

} // namespace paykan::pkm
