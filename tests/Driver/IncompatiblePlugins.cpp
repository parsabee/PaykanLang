// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Test-only plugins for the paykan_incompatible_plugins driver
// (tests/CMakeLists.txt): a frontend and a backend that claim to be built
// with PaykanLang 0.0.9, a version this toolchain does not accept, as if
// compiled against the installed headers of another release.  The driver
// must list them as incompatible, refuse to select them (exit 2), and never
// instantiate them: their factories abort the process.

#include "paykan/Backend.h"
#include "paykan/Frontend.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {

[[noreturn]] void instantiated(const char *what) {
  std::fprintf(stderr, "test plugin: instantiated the incompatible %s\n", what);
  std::abort();
}

std::unique_ptr<paykan::frontend::Frontend> createOldFrontend() {
  instantiated("frontend");
}

std::unique_ptr<paykan::backend::Backend> createOldBackend() {
  instantiated("backend");
}

} // namespace

// What the registration macros record is PAYKAN_PLUGIN_BUILD_VERSION of the
// headers at the point of registration; a plugin compiled against
// PaykanLang 0.0.9's headers would see this.
#undef PAYKAN_PLUGIN_BUILD_VERSION
#define PAYKAN_PLUGIN_BUILD_VERSION "0.0.9"

PAYKAN_REGISTER_FRONTEND(old_frontend, "old-frontend", &createOldFrontend);
PAYKAN_REGISTER_BACKEND(old_backend, "old-backend", &createOldBackend);
