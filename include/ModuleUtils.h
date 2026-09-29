// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#pragma once

#include "AST.h"
#include "ASTContext.h" // remapType() calls into ASTContext members

#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>

namespace paykan {
namespace module_utils {

/// Split a module path on "::" and build a platform-native relative path
/// using llvm::sys::path::append, then tack on the ".pkn" extension.
inline llvm::SmallString<128> modulePathToRelative(llvm::StringRef modulePath) {
  llvm::SmallString<128> relPath;
  llvm::SmallVector<llvm::StringRef, 4> components;
  modulePath.split(components, "::", /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  for (auto &c : components)
    llvm::sys::path::append(relPath, c);
  relPath += ".pkn";
  return relPath;
}

/// Resolve a path to a real (canonical) path, or return empty on failure.
inline std::string realPath(const llvm::Twine &path) {
  llvm::SmallString<256> resolved;
  if (llvm::sys::fs::real_path(path, resolved))
    return ""; // real_path returns non-zero on failure
  return std::string(resolved);
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

// These depend on Names.h, so include after the basic utilities.
#include "Names.h"

namespace paykan {
namespace module_utils {

/// Build the bitcode cache path for a resolved (canonical) source file.
///
/// Layout: <projectRoot>/.paykan_cache/<path-relative-to-projectRoot>.bc
///
/// The cache is anchored to the project root (the main file's directory, the
/// same root imports are resolved against), never to the current working
/// directory, so every invocation of a project shares one cache wherever it
/// is launched from.  A module resolved from outside the project root (e.g.
/// a stdlib module located through PAYKAN_STDLIB) mirrors its full path,
/// minus the root directory, under the same cache directory.  An empty
/// projectRoot means "the current directory" (the driver passes "" for a bare
/// `paykan main.pkn`), which yields a relative `.paykan_cache/...` path.
inline llvm::SmallString<256> getCachePath(llvm::StringRef resolvedPath,
                                           llvm::StringRef projectRoot) {
  llvm::SmallString<256> cachePath(projectRoot);
  llvm::sys::path::append(cachePath, names::kCacheDir);

  // Canonicalize projectRoot so prefix stripping works with resolved paths.
  llvm::SmallString<256> canonRoot;
  if (llvm::sys::fs::real_path(projectRoot.empty() ? "." : projectRoot,
                               canonRoot))
    canonRoot = projectRoot; // fallback

  // Compute the portion relative to the project root; the match must end on a
  // path-component boundary so "<root>2/x.pkn" is not mistaken for "<root>".
  llvm::StringRef rel = resolvedPath;
  if (!canonRoot.empty() && rel.starts_with(canonRoot) &&
      (rel.size() == canonRoot.size() ||
       llvm::sys::path::is_separator(rel[canonRoot.size()]) ||
       llvm::sys::path::is_separator(canonRoot.back()))) {
    rel = rel.drop_front(canonRoot.size());
    while (!rel.empty() && llvm::sys::path::is_separator(rel.front()))
      rel = rel.drop_front(1);
  } else {
    // Outside the project: mirror the absolute path without its root ("/" or
    // "C:\") so it stays inside the cache directory.
    rel = llvm::sys::path::relative_path(rel);
  }

  // Append the relative source path, replacing .pkn with .bc.
  for (auto comp = llvm::sys::path::begin(rel), end = llvm::sys::path::end(rel);
       comp != end; ++comp) {
    llvm::sys::path::append(cachePath, *comp);
  }
  llvm::sys::path::replace_extension(cachePath, ".bc");
  return cachePath;
}

} // namespace module_utils
} // namespace paykan
