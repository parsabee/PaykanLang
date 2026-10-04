// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The plugin compatibility check of the running toolchain (#103): its own
// version and the plugin build versions it accepts, compiled into the core
// from the generated paykan/PluginCompat.h of this build.

#include "paykan/PluginCompat.h"
#include "paykan/Registry.h"

#include <cstring>

namespace paykan::plugin {

namespace {
constexpr const char *kCompatibleVersions[] = {
    PAYKAN_PLUGIN_COMPATIBLE_VERSIONS};
} // namespace

std::string_view toolchainVersion() { return PAYKAN_PLUGIN_BUILD_VERSION; }

std::span<const char *const> compatibleBuildVersions() {
  return kCompatibleVersions;
}

bool isCompatibleBuildVersion(const char *buildVersion) {
  if (!buildVersion)
    return false;
  for (const char *v : kCompatibleVersions)
    if (std::strcmp(v, buildVersion) == 0)
      return true;
  return false;
}

std::string incompatibilityReason(const char *buildVersion) {
  std::string out = "built with ";
  if (buildVersion && *buildVersion) {
    out += "PaykanLang ";
    out += buildVersion;
  } else {
    out += "an unknown PaykanLang version";
  }
  out += "; this paykan ";
  out += toolchainVersion();
  out += " accepts ";
  const char *sep = "";
  for (const char *v : kCompatibleVersions) {
    out += sep;
    out += v;
    sep = ", ";
  }
  return out;
}

} // namespace paykan::plugin
