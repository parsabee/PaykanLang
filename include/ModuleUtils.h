// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#pragma once

#include "AST.h"
#include "ASTContext.h" // remapType() calls into ASTContext members

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace paykan {
namespace module_utils {

/// Append @p component to @p base the way a module path is joined: an empty
/// base yields the bare component (so an empty project root means "the
/// current directory"), and the component is always treated as relative.
inline std::filesystem::path
appendPath(const std::filesystem::path &base,
           const std::filesystem::path &component) {
  if (base.empty())
    return component;
  return base / component.relative_path();
}

/// Split a module path on "::" and build a platform-native relative path,
/// then tack on the ".pkn" extension.  Empty components are skipped.
inline std::string modulePathToRelative(std::string_view modulePath) {
  std::filesystem::path relPath;
  size_t pos = 0;
  while (pos <= modulePath.size()) {
    size_t sep = modulePath.find("::", pos);
    std::string_view c = modulePath.substr(pos, sep == std::string_view::npos
                                                    ? std::string_view::npos
                                                    : sep - pos);
    if (!c.empty())
      relPath /= c;
    if (sep == std::string_view::npos)
      break;
    pos = sep + 2;
  }
  return relPath.string() + ".pkn";
}

/// Resolve a path to a real (canonical) path, or return empty on failure.
inline std::string realPath(const std::filesystem::path &path) {
  std::error_code ec;
  auto resolved = std::filesystem::canonical(path, ec);
  if (ec)
    return "";
  return resolved.string();
}

/// Remap a Type* from a foreign ASTContext to the equivalent in ours.
inline ast::Type *remapType(ast::Type *ty, ast::ASTContext &ctx) {
  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(ty)) {
    return ctx.getBuiltinType(bt->getTypeKind());
  }
  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty)) {
    // All class types (including Obj and Str) are always registered in
    // the ASTContext that owns them.  Look up by name for identity; fall back
    // to Object only when the type has not been exported yet.
    if (auto *found = ctx.lookupClassType(ct->getName()))
      return found;
    return ctx.getObjTy(); // fallback: type not exported — treat as Obj
  }
  return ty;
}

} // namespace module_utils
} // namespace paykan
