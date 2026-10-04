// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The plugin loader (paykan/PluginLoader.h): discovery, the check before
// any plugin code but the entry point runs, and registration.

#include "paykan/PluginLoader.h"

#include "Platform.h"
#include "PluginHost.h"

#include "paykan/Backend.h"
#include "paykan/Registry.h"
#include "paykan/backends/Toolchain.h"
#include "paykan/plugin_api.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef PAYKAN_PLUGIN_VERSION_DIR
#error "PAYKAN_PLUGIN_VERSION_DIR must be defined by the build"
#endif
#ifndef PAYKAN_PLUGIN_DIR_FROM_BINDIR
#error "PAYKAN_PLUGIN_DIR_FROM_BINDIR must be defined by the build"
#endif

namespace paykan::plugin {

namespace fs = std::filesystem;

namespace {

/// The plugin API versions this paykan reads.
constexpr uint32_t kSupportedApiVersions[] = {PAYKAN_PLUGIN_API_VERSION};

std::string supportedApiVersions() {
  std::string out;
  for (uint32_t v : kSupportedApiVersions) {
    if (!out.empty())
      out += ", ";
    out += std::to_string(v);
  }
  return out;
}

bool supportsApiVersion(uint32_t v) {
  return std::find(std::begin(kSupportedApiVersions),
                   std::end(kSupportedApiVersions),
                   v) != std::end(kSupportedApiVersions);
}

bool endsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() &&
         s.substr(s.size() - suffix.size()) == suffix;
}

/// The plugin files in @p dir, sorted by name (none if it doesn't exist).
std::vector<std::string> pluginFilesIn(const std::string &dir) {
  std::vector<std::string> out;
  std::error_code ec;
  fs::directory_iterator it(dir, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    const fs::path &p = it->path();
    std::error_code fileEc;
    if (endsWith(p.filename().string(), pluginFileSuffix()) &&
        fs::is_regular_file(p, fileEc)) // follows links
      out.push_back(p.string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

/// The file's identity for "loaded once": its canonical path, or the
/// absolute one when it doesn't resolve.
std::string identity(const std::string &path) {
  std::error_code ec;
  fs::path canon = fs::canonical(path, ec);
  if (!ec)
    return canon.string();
  return fs::absolute(path, ec).lexically_normal().string();
}

/// The @p i-th backend of @p p.  The array's stride is the struct_size of
/// its elements (all equal: one struct, compiled once), which is larger than
/// this paykan's sizeof(PaykanBackend) when the plugin was built against a
/// newer header of the same plugin API (fields are only ever appended).
const PaykanBackend &backendAt(const PaykanPlugin &p, size_t i) {
  const auto *base = reinterpret_cast<const unsigned char *>(p.backends);
  return *reinterpret_cast<const PaykanBackend *>(
      base + i * size_t(p.backends[0].struct_size));
}

/// Check the backend descriptors of @p p; "" when they are well-formed.
std::string checkBackends(const PaykanPlugin &p) {
  if (p.num_backends && !p.backends)
    return "the descriptor lists " + std::to_string(p.num_backends) +
           " backends but no array";
  if (!p.num_backends)
    return "it provides no backend";
  // The first element's size is the array's stride: check it first.
  if (uint32_t size = p.backends[0].struct_size; size < host::kMinBackendSize)
    return "backend #1: its descriptor is " + std::to_string(size) +
           " bytes, plugin API " + std::to_string(PAYKAN_PLUGIN_API_VERSION) +
           " needs " + std::to_string(host::kMinBackendSize);
  std::set<std::string_view> seen;
  for (size_t i = 0; i < p.num_backends; ++i) {
    std::string which = "backend #" + std::to_string(i + 1);
    const PaykanBackend &b = backendAt(p, i);
    if (b.struct_size != p.backends[0].struct_size)
      return which + ": its descriptor is " + std::to_string(b.struct_size) +
             " bytes, the first one " +
             std::to_string(p.backends[0].struct_size);
    if (!b.name || !*b.name)
      return which + " has no name";
    which = "backend '" + std::string(b.name) + "'";
    if (!seen.insert(b.name).second)
      return which + " is listed twice";
    if (!b.emit)
      return which + " has no emit callback";
    if ((b.capabilities & PAYKAN_BACKEND_RUN) && !b.run)
      return which + " can run programs but has no run callback";
  }
  return "";
}

/// A library that stays loaded for the life of the process.
struct LoadedLibrary {
  void *Handle = nullptr;
  const PaykanPlugin *Plugin = nullptr;
};

std::vector<LoadedLibrary> &loadedLibraries() {
  static std::vector<LoadedLibrary> libs;
  return libs;
}

/// Load, check and register one file.  Fills @p f.
void loadFile(PluginFile &f) {
  std::error_code ec;
  if (!fs::exists(f.Path, ec)) {
    f.Error = "no such file";
    return;
  }
  // 1. Load: the platform's dynamic loader runs the library's global
  //    constructors.  Nothing else in it runs before the checks below.
  std::string loadError;
  void *handle = toolchain::platform::loadLibrary(f.Path, loadError);
  if (!handle) {
    f.Error = "cannot load it: " + loadError;
    return;
  }
  // 2. The entry point.  (A function pointer can't be cast from void *
  //    portably; copy the bits, as POSIX specifies for dlsym.)
  void *sym =
      toolchain::platform::librarySymbol(handle, PAYKAN_PLUGIN_ENTRY_POINT);
  if (!sym) {
    f.Error = "no " PAYKAN_PLUGIN_ENTRY_POINT
              " entry point (not a PaykanLang plugin)";
    return;
  }
  PaykanPluginInitFn init = nullptr;
  static_assert(sizeof(init) == sizeof(sym));
  std::memcpy(&init, &sym, sizeof(init));
  // 3. The descriptor, checked before anything else in the library is
  //    called: the plugin API version, its layout, then the build version.
  const PaykanPlugin *p = init(host::hostTable());
  if (!p) {
    f.Error = PAYKAN_PLUGIN_ENTRY_POINT " returned no plugin descriptor";
    return;
  }
  constexpr size_t kApiVersionEnd =
      offsetof(PaykanPlugin, api_version) + sizeof(uint32_t);
  if (p->struct_size < kApiVersionEnd) {
    f.Error = "its descriptor is " + std::to_string(p->struct_size) +
              " bytes, too small to hold a plugin API version";
    return;
  }
  if (!supportsApiVersion(p->api_version)) {
    f.Error = "built for plugin API " + std::to_string(p->api_version) +
              "; this paykan supports plugin API " + supportedApiVersions();
    return;
  }
  if (p->struct_size < host::kMinPluginSize) {
    f.Error = "its descriptor is " + std::to_string(p->struct_size) +
              " bytes, plugin API " + std::to_string(p->api_version) +
              " needs " + std::to_string(host::kMinPluginSize);
    return;
  }
  if (std::string why = checkBackends(*p); !why.empty()) {
    f.Error = "invalid descriptor: " + why;
    return;
  }
  f.Name = p->name ? p->name : "";
  f.Version = p->version ? p->version : "";
  loadedLibraries().push_back({handle, p});

  // 4. Register.  An incompatible plugin's backends are registered without
  //    a factory: listed with the reason, never callable.
  bool compatible = isCompatibleBuildVersion(p->build_version);
  std::string reason =
      compatible ? "" : incompatibilityReason(p->build_version);
  auto &registry = backend::Registry::get();
  for (size_t i = 0; i < p->num_backends; ++i) {
    const PaykanBackend *b = &backendAt(*p, i);
    f.Provides.push_back(std::string("backend ") + b->name);
    if (registry.find(b->name)) {
      registry.markConflict(b->name, f.Path);
      continue;
    }
    backend::Registry::Entry e;
    e.Name = b->name;
    if (compatible)
      e.Create = [b] { return host::makeBackendAdapter(b); };
    e.BuildVersion = p->build_version ? p->build_version : "";
    e.Compatible = compatible;
    e.Incompatibility = reason;
    e.Path = f.Path;
    e.Description = b->description ? b->description : "";
    registry.add(std::move(e));
  }
}

} // namespace

const char *pluginFileSuffix() {
  return toolchain::platform::sharedLibrarySuffix();
}

std::string userPluginDir(const std::string &homeDir) {
  if (homeDir.empty())
    return "";
  return (fs::path(homeDir) / ".paykan" / "plugins" / PAYKAN_PLUGIN_VERSION_DIR)
      .string();
}

std::string systemPluginDir(const std::string &executableDir) {
  if (executableDir.empty())
    return "";
  return (fs::path(executableDir) / PAYKAN_PLUGIN_DIR_FROM_BINDIR)
      .lexically_normal()
      .string();
}

DiscoveryOptions discoveryOptionsFromEnvironment() {
  DiscoveryOptions o;
  if (const char *no = std::getenv("PAYKAN_NO_PLUGINS"))
    o.DisableDiscovery = no[0] && std::string_view(no) != "0";
  if (const char *path = std::getenv("PAYKAN_PLUGIN_PATH"))
    o.PluginPath = path;
  if (const char *home = std::getenv("HOME"))
    o.HomeDir = home;
  o.ExecutableDir = toolchain::executableDir();
  return o;
}

std::vector<std::string> searchDirectories(const DiscoveryOptions &opts) {
  std::vector<std::string> dirs;
  if (opts.DisableDiscovery)
    return dirs;
  // $PAYKAN_PLUGIN_PATH: an empty entry is skipped (it does NOT mean the
  // current directory, unlike $PATH).
  size_t start = 0;
  const std::string &env = opts.PluginPath;
  while (start <= env.size() && !env.empty()) {
    size_t colon = env.find(':', start);
    std::string dir = env.substr(
        start, colon == std::string::npos ? std::string::npos : colon - start);
    if (!dir.empty())
      dirs.push_back(dir);
    if (colon == std::string::npos)
      break;
    start = colon + 1;
  }
  if (std::string user = userPluginDir(opts.HomeDir); !user.empty())
    dirs.push_back(user);
  if (std::string sys = systemPluginDir(opts.ExecutableDir); !sys.empty())
    dirs.push_back(sys);
  return dirs;
}

const LoadReport &loadPlugins(const DiscoveryOptions &opts) {
  static LoadReport report;
  static bool done = false;
  if (done)
    return report;
  done = true;

  report.DiscoveryDisabled = opts.DisableDiscovery;
  report.SearchDirs = searchDirectories(opts);
  std::set<std::string> seen;
  auto consider = [&](const std::string &path, bool isExplicit) {
    if (!seen.insert(identity(path)).second)
      return;
    PluginFile f;
    std::error_code ec;
    fs::path abs = fs::absolute(path, ec);
    f.Path = ec ? path : abs.lexically_normal().string();
    f.Explicit = isExplicit;
    loadFile(f);
    report.Files.push_back(std::move(f));
  };
  for (const std::string &file : opts.ExplicitFiles)
    consider(file, true);
  for (const std::string &dir : report.SearchDirs)
    for (const std::string &file : pluginFilesIn(dir))
      consider(file, false);
  return report;
}

} // namespace paykan::plugin
