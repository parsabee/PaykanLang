// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Plugin registry shared by frontends and backends.
//
// A plugin implements one of the compiler's interfaces (frontend::Frontend,
// backend::Backend) and is registered under a name.  There are two kinds:
//   - Built-in plugins, linked into paykan.  Registration is static: the
//     library defines one plugin::Registration object (see
//     PAYKAN_REGISTER_FRONTEND and PAYKAN_REGISTER_BACKEND), and the build
//     links the library into the executable whole, so the object's
//     constructor runs before main().
//   - Loaded plugins: shared libraries with the C interface of
//     paykan/plugin_api.h, found and checked at startup by the plugin loader
//     (paykan/PluginLoader.h), which registers an adapter for each of their
//     frontends/backends with the file it came from (Entry::Path).
// The driver then lists and selects plugins by name (--frontend=,
// --backend=), the same way for both.
//
// Nothing here depends on any plugin: the registry is part of the standard
// C++ core and never includes a plugin header.

#pragma once

#include "paykan/PluginCompat.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace paykan::plugin {

// -- Compatibility (#103) ----------------------------------------------------
//
// Every plugin records the PaykanLang version it was BUILT WITH
// (PAYKAN_PLUGIN_BUILD_VERSION of the headers it was compiled against; the
// PAYKAN_REGISTER_* macros pass it in).  This paykan accepts a plugin only if
// that version is on its compatibility list (cmake/PluginCompat.cmake).
// Versions match as exact strings, pre-release label included.  An
// incompatible plugin stays registered, so it is listed with the reason, but
// it can't be selected and is never instantiated.  The built-in plugins are
// built with this paykan's own version, which is always on the list.

/// The version of the running toolchain ("0.1.0-alpha").
std::string_view toolchainVersion();

/// The plugin build versions the running toolchain accepts.
std::span<const char *const> compatibleBuildVersions();

/// True if a plugin built with @p buildVersion may be used (exact match
/// against compatibleBuildVersions()).  A null version is never compatible.
bool isCompatibleBuildVersion(const char *buildVersion);

/// Why a plugin built with @p buildVersion is not compatible:
/// "built with PaykanLang 0.0.9; this paykan 0.1.0-alpha accepts 0.1.0-alpha".
std::string incompatibilityReason(const char *buildVersion);

// -- Registration ------------------------------------------------------------

/// What a plugin registers: plain data, filled in by the PAYKAN_REGISTER_*
/// macros of the interface headers.
template <typename Interface> struct PluginInfo {
  /// The name users select the plugin by.
  const char *Name;
  /// Creates a fresh plugin instance.
  std::unique_ptr<Interface> (*Create)();
  /// The PaykanLang version the plugin was built with
  /// (PAYKAN_PLUGIN_BUILD_VERSION where it registers).
  const char *BuildVersion;
};

/// The registry of every plugin implementing @p Interface, keyed by name.
/// Entries are kept sorted by name, so listings are deterministic whatever
/// the static-initialisation order of the plugin libraries.
template <typename Interface> class Registry {
public:
  /// Creates a fresh plugin instance.
  using Factory = std::function<std::unique_ptr<Interface>()>;

  struct Entry {
    std::string Name;
    Factory Create;
    /// The PaykanLang version the plugin was built with ("" if it gave none).
    std::string BuildVersion;
    /// Whether this paykan accepts that version.
    bool Compatible = false;
    /// Why an incompatible plugin is not accepted (incompatibilityReason()
    /// for a build version not on the list), else "".
    std::string Incompatibility;
    /// The file a loaded plugin came from; "" for a built-in one.
    std::string Path;
    /// The description a loaded plugin gives in its descriptor ("" if none).
    std::string Description;
    /// Non-empty when a second plugin was registered under the same name:
    /// "provided by both <first> and <second>".  Such a name can't be
    /// selected until one of the two is removed.
    std::string Conflict;
  };

  /// The one registry for this interface.
  static Registry &get() {
    static Registry instance;
    return instance;
  }

  /// Register @p info, checking its build version.  An incompatible plugin
  /// is registered too, marked as such.  Returns false (and keeps the first
  /// registration) if the name is already taken.
  bool add(const PluginInfo<Interface> &info) {
    Entry e;
    e.Name = info.Name ? info.Name : "";
    if (info.Create)
      e.Create = info.Create;
    e.BuildVersion = info.BuildVersion ? info.BuildVersion : "";
    e.Compatible = isCompatibleBuildVersion(info.BuildVersion);
    if (!e.Compatible)
      e.Incompatibility = incompatibilityReason(info.BuildVersion);
    return add(std::move(e));
  }

  /// Register a complete entry (the plugin loader's path).  Returns false
  /// (and keeps the first registration) if the name is already taken.
  bool add(Entry e) {
    if (find(e.Name))
      return false;
    auto pos = std::lower_bound(
        Entries.begin(), Entries.end(), e,
        [](const Entry &a, const Entry &b) { return a.Name < b.Name; });
    Entries.insert(pos, std::move(e));
    return true;
  }

  /// The entry registered under @p name, compatible or not, or nullptr.
  const Entry *find(std::string_view name) const {
    for (const Entry &e : Entries)
      if (e.Name == name)
        return &e;
    return nullptr;
  }

  /// Record that a second plugin, from @p otherPath, also provides
  /// @p name: the name becomes ambiguous (Entry::Conflict) and can't be
  /// selected.  Returns false if nothing is registered under @p name.
  bool markConflict(std::string_view name, const std::string &otherPath) {
    for (Entry &e : Entries) {
      if (e.Name != name)
        continue;
      if (e.Conflict.empty())
        e.Conflict = "provided by both " + describePath(e.Path) + " and " +
                     describePath(otherPath);
      else
        e.Conflict += " and " + describePath(otherPath);
      return true;
    }
    return false;
  }

  /// "the built-in plugin" for "", else @p path.
  static std::string describePath(const std::string &path) {
    return path.empty() ? std::string("the built-in plugin") : path;
  }

  /// A new instance of the plugin registered under @p name, or nullptr if
  /// there is none, it is incompatible (its build version is checked again
  /// here, at selection) or its name is ambiguous.
  std::unique_ptr<Interface> create(std::string_view name) const {
    const Entry *e = find(name);
    if (!e || !e->Compatible || !e->Conflict.empty() || !e->Create ||
        !isCompatibleBuildVersion(e->BuildVersion.c_str()))
      return nullptr;
    return e->Create();
  }

  /// Every registered plugin, compatible or not, sorted by name.
  const std::vector<Entry> &entries() const { return Entries; }

  /// The registered names, compatible or not, sorted.
  std::vector<std::string> names() const {
    std::vector<std::string> out;
    out.reserve(Entries.size());
    for (const Entry &e : Entries)
      out.push_back(e.Name);
    return out;
  }

private:
  Registry() = default;
  std::vector<Entry> Entries;
};

/// Registers a plugin from a static initialiser.  Use through the
/// PAYKAN_REGISTER_* macros of the interface headers.
template <typename Interface> struct Registration {
  explicit Registration(const PluginInfo<Interface> &info) {
    Registry<Interface>::get().add(info);
  }
};

} // namespace paykan::plugin
