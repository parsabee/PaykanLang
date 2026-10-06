// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
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
/// written by the prototype.
enum class DeclTag : uint8_t { Enum = 1, Class = 2, Func = 5 };

/// A MODS entry (§3.3): index 0 is the module itself (hashes zero), the
/// rest its direct dependencies sorted by canonical name.
struct ModuleRef {
  std::string Canonical;
  bool System = false;
  support::Hash256 IfaceHash{};
  support::Hash256 TmplHash{};
  bool operator==(const ModuleRef &) const = default;
};

/// ModuleInfo::FunctionInfo.
struct FuncRec {
  std::string Name;
  std::string ReturnTypeName;
  std::vector<std::string> ParamTypeNames;
  bool operator==(const FuncRec &) const = default;
};

/// ModuleInfo::ClassInfo (OriginPath is a host path and is never written;
/// OriginModule is the canonical name).
struct ClassRec {
  struct Field {
    std::string FieldName;
    std::string TypeName;
    bool operator==(const Field &) const = default;
  };
  struct Method {
    std::string Name;
    std::string ReturnTypeName;
    std::vector<std::string> ParamTypeNames;
    uint8_t Flags = 0; ///< ast::MethodDecl flags (Private bit)
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

/// ModuleInfo::EnumInfo.
struct EnumRec {
  std::string Name;
  std::vector<std::string> Variants;
  bool IsLocal = true;
  std::string OriginModule;
  bool operator==(const EnumRec &) const = default;
};

struct Interface {
  std::string Module; ///< canonical name (MODS[0])
  bool System = false;
  std::vector<ModuleRef> Mods; ///< direct dependencies (MODS[1..])
  std::vector<FuncRec> Functions;
  std::vector<ClassRec> Classes;
  std::vector<EnumRec> Enums;
  std::string DisplayFile; ///< DIAG: project-relative; "" writes no DIAG
  bool operator==(const Interface &) const = default;
};

/// Deterministic: dependencies and records are sorted by name, the string
/// table is in first-use order.
std::vector<uint8_t> writeInterface(const Interface &iface);

/// The checks of §3.7 that apply to the prototype records: magic/major,
/// parts in bounds, aligned and non-overlapping, STRS valid, MODS[0] equal
/// to @p expectedModule, dependencies sorted and unique, records sorted by
/// (tag, name) with unique names per kind, every string index in range.
StatusOr<Interface> readInterface(std::span<const uint8_t> bytes,
                                  std::string_view expectedModule);

} // namespace paykan::pkm
