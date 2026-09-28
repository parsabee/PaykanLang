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

/// Build the bitcode cache path for a resolved source file.
/// Layout: <cwd>/.paykan_cache/<path-relative-to-projectRoot>.bc
/// If the resolved path is not under projectRoot, uses the filename only.
inline llvm::SmallString<256> getCachePath(llvm::StringRef resolvedPath,
                                           llvm::StringRef projectRoot) {
  // Current working directory. On failure `cwd` is left empty, which yields a
  // cache path relative to the process root — acceptable for a best-effort
  // cache location, so the error code is intentionally ignored.
  llvm::SmallString<256> cwd;
  (void)llvm::sys::fs::current_path(cwd);

  llvm::SmallString<256> cachePath(cwd);
  llvm::sys::path::append(cachePath, names::kCacheDir);

  // Canonicalize projectRoot so prefix stripping works with resolved paths.
  llvm::SmallString<256> canonRoot;
  if (llvm::sys::fs::real_path(projectRoot, canonRoot))
    canonRoot = projectRoot; // fallback

  // Compute relative portion: strip projectRoot prefix.
  llvm::StringRef rel = resolvedPath;
  if (rel.starts_with(canonRoot)) {
    rel = rel.drop_front(canonRoot.size());
    // Strip leading separator.
    if (!rel.empty() && llvm::sys::path::is_separator(rel.front()))
      rel = rel.drop_front(1);
  }

  // Append the relative source path, replacing .pkn with .bc.
  for (auto comp = llvm::sys::path::begin(rel), end = llvm::sys::path::end(rel);
       comp != end; ++comp) {
    llvm::sys::path::append(cachePath, *comp);
  }
  llvm::sys::path::replace_extension(cachePath, ".bc");
  return cachePath;
}

/// Return true if the source file is newer than the cache file,
/// or if the cache file does not exist.
inline bool isSourceNewer(llvm::StringRef sourcePath,
                          llvm::StringRef cachePath) {
  llvm::sys::fs::file_status srcStat, cacheStat;
  if (llvm::sys::fs::status(sourcePath, srcStat))
    return true; // can't stat source — treat as newer
  if (llvm::sys::fs::status(cachePath, cacheStat))
    return true; // cache doesn't exist
  return srcStat.getLastModificationTime() >
         cacheStat.getLastModificationTime();
}

} // namespace module_utils
} // namespace paykan
