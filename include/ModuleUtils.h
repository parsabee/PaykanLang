// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Where a module's source file is: the path an `import` names, under the
// project root, or under the standard library for a system import.

#pragma once

#include "Names.h"

#include <cstdlib>
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

/// "a::b::c" as the relative path a/b/c.pkn (empty components skipped).
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

/// The file of module @p modulePath, not checked to exist: under
/// $PAYKAN_STDLIB (or <projectRoot>/stdlib) for a system import, under
/// @p projectRoot otherwise.
inline std::filesystem::path moduleFile(const std::string &projectRoot,
                                        bool isSystem,
                                        std::string_view modulePath) {
  std::filesystem::path base = projectRoot;
  if (isSystem) {
    const char *env = std::getenv(names::kPaykanStdlibEnv);
    base = env && env[0] ? std::filesystem::path(env)
                         : appendPath(base, names::kStdlibDir);
  }
  return appendPath(base, modulePathToRelative(modulePath));
}

/// The canonical path of @p path, or "" when it does not resolve.
inline std::string realPath(const std::filesystem::path &path) {
  std::error_code ec;
  auto resolved = std::filesystem::canonical(path, ec);
  if (ec)
    return "";
  return resolved.string();
}

} // namespace module_utils
} // namespace paykan
