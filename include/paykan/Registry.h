// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Plugin registry shared by frontends and backends.
//
// A plugin is a library that implements one of the compiler's interfaces
// (frontend::Frontend, backend::Backend) and registers a factory under a
// name.  Registration is static: a plugin library defines one
// plugin::Registration object (see PAYKAN_REGISTER_FRONTEND and
// PAYKAN_REGISTER_BACKEND), and the build links the library into the
// executable whole, so the object's constructor runs before main().  The
// driver then lists and selects plugins by name (--frontend=, --backend=).
//
// Nothing here depends on any plugin: the registry is part of the standard
// C++ core and never includes a plugin header.

#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace paykan::plugin {

/// The registry of every plugin implementing @p Interface, keyed by name.
/// Entries are kept sorted by name, so listings are deterministic whatever
/// the static-initialisation order of the plugin libraries.
template <typename Interface> class Registry {
public:
  /// Creates a fresh plugin instance.
  using Factory = std::unique_ptr<Interface> (*)();

  struct Entry {
    std::string Name;
    Factory Create;
  };

  /// The one registry for this interface.
  static Registry &get() {
    static Registry instance;
    return instance;
  }

  /// Register @p factory under @p name.  Returns false (and keeps the first
  /// registration) if the name is already taken.
  bool add(std::string_view name, Factory factory) {
    if (find(name))
      return false;
    Entry e{std::string(name), factory};
    auto pos = std::lower_bound(
        Entries.begin(), Entries.end(), e,
        [](const Entry &a, const Entry &b) { return a.Name < b.Name; });
    Entries.insert(pos, std::move(e));
    return true;
  }

  /// The entry registered under @p name, or nullptr.
  const Entry *find(std::string_view name) const {
    for (const Entry &e : Entries)
      if (e.Name == name)
        return &e;
    return nullptr;
  }

  /// A new instance of the plugin registered under @p name, or nullptr if
  /// there is none.
  std::unique_ptr<Interface> create(std::string_view name) const {
    const Entry *e = find(name);
    return e ? e->Create() : nullptr;
  }

  /// Every registered plugin, sorted by name.
  const std::vector<Entry> &entries() const { return Entries; }

  /// The registered names, sorted.
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
  Registration(std::string_view name,
               typename Registry<Interface>::Factory factory) {
    Registry<Interface>::get().add(name, factory);
  }
};

} // namespace paykan::plugin
