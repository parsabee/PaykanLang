// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Part of the test support for out-of-tree frontends (installed in
// share/paykan/frontend-tests; paykan_add_frontend_tests links it into each
// suite): loads the plugin files named in $PAYKAN_TEST_PLUGINS
// (`:`-separated) with the same loader and checks as the installed `paykan`
// (paykan/PluginLoader.h), before any test runs.  The frontend under test
// then comes from the plugin module itself, through its C interface and the
// AST interchange format, exactly as `paykan --plugin=<file>` uses it.  A
// file the loader rejects fails the whole suite (exit status 2).

#include "paykan/PluginLoader.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

bool loadTestPlugins() {
  const char *env = std::getenv("PAYKAN_TEST_PLUGINS");
  if (!env || !*env)
    return true;
  paykan::plugin::DiscoveryOptions opts;
  opts.DisableDiscovery = true; // exactly the named files
  std::string list = env;
  for (size_t start = 0; start <= list.size();) {
    size_t colon = list.find(':', start);
    std::string file = list.substr(
        start, colon == std::string::npos ? std::string::npos : colon - start);
    if (!file.empty())
      opts.ExplicitFiles.push_back(file);
    if (colon == std::string::npos)
      break;
    start = colon + 1;
  }
  const auto &report = paykan::plugin::loadPlugins(opts);
  for (const auto &f : report.Files)
    if (!f.Error.empty()) {
      std::fprintf(stderr, "cannot load plugin '%s': %s\n", f.Path.c_str(),
                   f.Error.c_str());
      std::exit(2);
    }
  return true;
}

// Before main(): the suites look the frontend up in the registry.
[[maybe_unused]] const bool loaded = loadTestPlugins();

} // namespace
