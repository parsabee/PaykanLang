// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Canonical module names (#102).  A module is identified everywhere past
// Sema -- PIR `module` clauses, C banners and symbol prefixes, LLVM symbol
// names, cache entries -- by a name that depends only on the program's
// sources, never on the directory they were compiled in:
//
//   import geometry::shapes;   ->  geometry::shapes
//   import ::io;  (system)     ->  ::io
//   paykan ../demo/zoo.pkn     ->  zoo           (the main module)
//
// File paths are kept for reading files and for diagnostics only.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace paykan::module_name {

/// The qualifier separator of module paths (names::kQualSep).
inline constexpr std::string_view kSep = "::";

/// The cache directory (under .paykan_cache) of system modules, whose names
/// start with "::".  Not a Paykan identifier, so no user module's entries
/// can land there.
inline constexpr const char *kSystemCacheDir = "@system";

/// The non-empty "::"-separated components of @p path, in order.
inline std::vector<std::string_view> components(std::string_view path) {
  std::vector<std::string_view> out;
  while (true) {
    size_t sep = path.find(kSep);
    if (sep != 0 && !path.empty())
      out.push_back(path.substr(0, sep));
    if (sep == std::string_view::npos)
      return out;
    path.remove_prefix(sep + kSep.size());
  }
}

/// The canonical name of the module an import names: the module path with
/// empty components dropped (`a::::b` reads a/b.pkn, so it is `a::b`), and
/// a leading "::" for a system import, which keeps `import ::io` apart from
/// a project module `io`.
inline std::string canonicalImportName(std::string_view modulePath,
                                       bool isSystem) {
  std::string out;
  for (std::string_view c : components(modulePath)) {
    if (!out.empty())
      out += kSep;
    out += c;
  }
  return isSystem ? std::string(kSep) + out : out;
}

/// The canonical name of the main module: the stem of its file name, the
/// same whatever directory the compiler is run from (`zoo.pkn`,
/// `../zoo.pkn` and `/abs/zoo.pkn` are all `zoo`).  A ':' (legal in a file
/// name, never in a module path) becomes '_' so the name has no "::" and
/// cannot be mistaken for an import's; an empty stem is `main`.
inline std::string mainModuleName(const std::string &inputPath) {
  std::string stem = std::filesystem::path(inputPath).stem().string();
  for (char &c : stem)
    if (c == ':')
      c = '_';
  return stem.empty() ? std::string("main") : stem;
}

/// The cache entry of module @p name, relative to the cache directory and
/// without an extension: one directory per "::"-separated component
/// (`geometry::shapes` -> geometry/shapes), system modules under
/// kSystemCacheDir.  Callers append the extension (the last component may
/// contain '.', so it must not be "replaced").
inline std::filesystem::path cacheRelativePath(std::string_view name) {
  std::filesystem::path rel;
  if (name.starts_with(kSep))
    rel = kSystemCacheDir;
  for (std::string_view c : components(name))
    rel /= std::string(c);
  return rel;
}

} // namespace paykan::module_name
