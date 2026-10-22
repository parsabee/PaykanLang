// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// The TMPL section (docs/design/pkm.md §4.3): a module's generic classes,
// structs and functions, as the module resolved them, for an importer to
// instantiate without the module's source.
//
// The bodies are the AST interchange text (docs/plugins/ast-format.md) of a
// unit that holds the templates and their `impl` blocks, every name the
// module declares or imports spelled `$<canonical module>::name`
// (Sema::exportTemplates); this library only frames them and never parses
// them.  Sema turns the blob into its ModuleTemplates and back.

#pragma once

#include "paykan/Status.h"
#include "paykan/pkm/Format.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace paykan::pkm {

/// The TMPL blob's version: the release's, like every format.
inline constexpr uint32_t kTmplVersion = kVersionNumber;

struct Templates {
  /// Which template an `impl` block of the unit belongs to.
  enum class OwnerKind : uint8_t { Class = 1, Struct = 2 };
  struct Impl {
    OwnerKind Kind = OwnerKind::Class;
    /// Index among the unit's generic classes (or structs), in order.
    uint32_t Owner = 0;
    bool operator==(const Impl &) const = default;
  };
  /// The template module's source as diagnostics name it (relative to its
  /// source root), for errors inside an instantiated body.
  std::string File;
  /// The other modules the bodies name (their canonical names), in the
  /// order the module met them; the importer registers each under its
  /// hidden qualifier.
  std::vector<std::string> Uses;
  /// One per `impl` block of the unit, in the unit's order.
  std::vector<Impl> Impls;
  /// The interchange text of the unit.
  std::string Ast;
  bool operator==(const Templates &) const = default;
};

std::vector<uint8_t> writeTemplates(const Templates &t);
StatusOr<Templates> readTemplates(std::span<const uint8_t> bytes);

} // namespace paykan::pkm
