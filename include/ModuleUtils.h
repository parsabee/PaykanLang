// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Where a module's source file is: the path an `import` names, under the
// project root, or under the standard library for a system import.

#pragma once

#include "Names.h"

#include <cstdlib>
#include <filesystem>
#include <optional>
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

/// The installed standard library, <prefix>/lib/paykan/stdlib/<version>/
/// found from the executable: the driver sets it at startup.  Empty when
/// there is none to search.
inline std::filesystem::path &installedStdlibDir() {
  static std::filesystem::path dir;
  return dir;
}

/// The directory of system imports: $PAYKAN_STDLIB when it is set, else the
/// installed standard library when it exists, else <projectRoot>/stdlib.
inline std::filesystem::path stdlibDir(const std::string &projectRoot) {
  const char *env = std::getenv(names::kPaykanStdlibEnv);
  if (env && env[0])
    return env;
  std::error_code ec;
  if (const auto &installed = installedStdlibDir();
      !installed.empty() && std::filesystem::is_directory(installed, ec))
    return installed;
  return appendPath(projectRoot, names::kStdlibDir);
}

/// The file of module @p modulePath, not checked to exist: under the
/// standard library (stdlibDir) for a system import, under @p projectRoot
/// otherwise.
inline std::filesystem::path moduleFile(const std::string &projectRoot,
                                        bool isSystem,
                                        std::string_view modulePath) {
  std::filesystem::path base =
      isSystem ? stdlibDir(projectRoot) : std::filesystem::path(projectRoot);
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

/// @p path from the source root @p projectRoot ("" = the current
/// directory) when it lies under it (`lib/m.pkn`), or nothing.
inline std::optional<std::filesystem::path>
pathUnderRoot(const std::string &projectRoot,
              const std::filesystem::path &path) {
  std::error_code ec;
  auto root = std::filesystem::weakly_canonical(
      projectRoot.empty() ? std::filesystem::path(".")
                          : std::filesystem::path(projectRoot),
      ec);
  if (ec)
    return std::nullopt;
  auto abs = std::filesystem::weakly_canonical(path, ec);
  if (ec)
    return std::nullopt;
  auto rel = abs.lexically_relative(root);
  if (rel.empty() || *rel.begin() == "..")
    return std::nullopt;
  return rel;
}

/// @p path from the source root when it lies under it (pathUnderRoot), else
/// as given: how a module file records its source (its DIAG and TMPL
/// paths), the same wherever the compiler runs.
inline std::string relativeToRoot(const std::string &projectRoot,
                                  const std::filesystem::path &path) {
  std::optional<std::filesystem::path> rel = pathUnderRoot(projectRoot, path);
  return rel ? rel->generic_string() : path.string();
}

/// @p path as a diagnostic or `--verbose` names it, a path that works from
/// the directory the compiler runs in: a file under the source root is the
/// root as the main file's path gives it (@p projectRoot, "" for the
/// current directory) followed by the file's path from the root, so
/// `paykan tasks/main.pkn` names `tasks/lib/m.pkn`, and an absolute main
/// file gives absolute paths; any other file is @p path as given.
inline std::string displayPath(const std::string &projectRoot,
                               const std::filesystem::path &path) {
  std::optional<std::filesystem::path> rel = pathUnderRoot(projectRoot, path);
  return rel ? appendPath(projectRoot, *rel).generic_string() : path.string();
}

} // namespace module_utils
} // namespace paykan
