// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The plugin loader: finds plugin libraries at startup, checks them, and
// registers what they provide (docs/plugins/overview.md).
//
// A loaded plugin is a shared library with the C interface of
// paykan/plugin_api.h.  For every candidate file the loader
//   1. loads the library (the platform's dynamic loader: this runs its
//      global constructors, and nothing else);
//   2. looks up `paykan_plugin_init` and calls it with paykan's host table;
//   3. checks the descriptor it returns -- the plugin API version, then the
//      PaykanLang version the plugin was built with against this paykan's
//      compatibility list (#103) -- before calling anything else in the
//      library;
//   4. registers an adapter for each backend in backend::Registry, with
//      the file it came from.  An incompatible plugin's backends are
//      registered too, as incompatible, so they are listed with the reason
//      and can't be selected; a name that is already taken (by a built-in
//      plugin or an earlier file) becomes ambiguous and can't be selected
//      either.
//
// Where it looks, in this order (the first file wins a name, but a clash is
// an error when that name is selected):
//   1. the --plugin=<file> files, in command-line order;
//   2. each directory of $PAYKAN_PLUGIN_PATH (`:`-separated; empty entries
//      are skipped, never the current directory);
//   3. the user directory   ~/.paykan/plugins/<version>/;
//   4. the system directory <prefix>/lib/paykan/plugins/<version>/, found
//      relative to the running executable.
// <version> is this toolchain's version, pre-release label included.  In a
// directory, every regular file (or link to one) whose name ends in the
// platform's library suffix (.so, .dylib) is a candidate, in name order;
// directories are not searched recursively.  --no-plugins or
// PAYKAN_NO_PLUGINS=1 turns off steps 2-4; the files named with --plugin are
// still loaded.  A file reached twice (a directory listed twice, a link) is
// loaded once.

#pragma once

#include <string>
#include <vector>

namespace paykan::plugin {

/// Where the loader looks.
struct DiscoveryOptions {
  /// --plugin=<file>, in order.
  std::vector<std::string> ExplicitFiles;
  /// --no-plugins / PAYKAN_NO_PLUGINS: skip the directory search.
  bool DisableDiscovery = false;
  /// $PAYKAN_PLUGIN_PATH ("" for unset).
  std::string PluginPath;
  /// $HOME ("" for unset: no user directory).
  std::string HomeDir;
  /// The running executable's directory ("" for unknown: no system
  /// directory).
  std::string ExecutableDir;
};

/// Fill DisableDiscovery, PluginPath and HomeDir from the environment
/// (PAYKAN_NO_PLUGINS set to anything but "" or "0" disables discovery) and
/// ExecutableDir from the running executable.
DiscoveryOptions discoveryOptionsFromEnvironment();

/// What happened to one candidate file.
struct PluginFile {
  /// The file's absolute path (not resolving links).
  std::string Path;
  /// Named with --plugin.
  bool Explicit = false;
  /// Why the whole file was rejected ("" when it was loaded and its
  /// descriptor read): it could not be loaded, has no entry point, returned
  /// no descriptor, or targets another plugin API version.  A file whose
  /// build version is not accepted is not rejected here: its backends are
  /// registered as incompatible.
  std::string Error;
  /// The plugin's own name and version from its descriptor, if it gave
  /// them.
  std::string Name;
  std::string Version;
  /// "backend <name>" for everything it registered.
  std::vector<std::string> Provides;
};

/// The outcome of loadPlugins().
struct LoadReport {
  /// The directories searched, in order (empty when discovery is off).
  std::vector<std::string> SearchDirs;
  bool DiscoveryDisabled = false;
  /// Every candidate file, in load order.
  std::vector<PluginFile> Files;
};

/// The directories steps 2-4 search, in order, for @p opts (whether they
/// exist or not).
std::vector<std::string> searchDirectories(const DiscoveryOptions &opts);

/// The user directory for @p homeDir: <home>/.paykan/plugins/<version>.
std::string userPluginDir(const std::string &homeDir);

/// The system directory for @p executableDir:
/// <executable dir>/<relative path to lib>/paykan/plugins/<version>.
std::string systemPluginDir(const std::string &executableDir);

/// The platform's plugin file suffix (".so", ".dylib").
const char *pluginFileSuffix();

/// Find, check and register the plugins.  Call it once, before any plugin
/// is selected; a second call does nothing and returns the first report.
const LoadReport &loadPlugins(const DiscoveryOptions &opts);

} // namespace paykan::plugin
