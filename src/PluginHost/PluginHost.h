// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The host side of the C plugin interface (paykan/plugin_api.h), private
// to src/PluginHost: the host table paykan hands every plugin, the session
// each callback runs in, and the adapters that turn a C frontend into a
// frontend::Frontend and a C backend into a backend::Backend.

#pragma once

#include "DiagEngine.h"
#include "paykan/Backend.h"
#include "paykan/Frontend.h"
#include "paykan/backends/Toolchain.h"
#include "paykan/plugin_api.h"

#include <cstddef>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

/// One call into a plugin callback (opaque to plugins: plugin_api.h only
/// declares it).
struct PaykanSession {
  /// Where host->write_output goes.
  std::ostream *Out = nullptr;
  /// What diagnostics without a file name refer to.
  std::string InputFile;
  /// "backend 'name'" / "frontend 'name'", for messages.
  std::string Owner;
  /// A frontend's diagnostics go here (with the source snippet), counted in
  /// DiagErrors.  Null for a backend.
  paykan::sema::DiagEngine *Diag = nullptr;
  unsigned DiagErrors = 0;
  /// A backend's error diagnostics so far, formatted: the failure message of
  /// the call.
  std::vector<std::string> Errors;
  /// -O<n> of the program, for link_executable's compile of C inputs.
  unsigned OptLevel = 0;
  /// The toolchain, resolved on first use.
  bool ToolchainResolved = false;
  bool ToolchainOk = false;
  paykan::toolchain::Toolchain Toolchain;
  /// host->temp_dir, created on first use and removed with the session.
  std::unique_ptr<paykan::toolchain::TempDir> Temp;
};

namespace paykan::plugin::host {

/// The table every plugin is initialised with (static storage).
const PaykanHost *hostTable();

/// The smallest descriptors this paykan reads: the structs as plugin API
/// version 1 first defined them.  Fields appended later must not raise
/// these, so plugins built against the first version of the header keep
/// loading: PaykanPlugin's frontend fields came after its backends, and are
/// read only when struct_size covers them (pluginHasFrontends).
inline constexpr size_t kMinPluginSize = offsetof(PaykanPlugin, num_frontends);
inline constexpr size_t kMinBackendSize = sizeof(PaykanBackend);
inline constexpr size_t kMinFrontendSize = sizeof(PaykanFrontend);

/// Whether @p p's descriptor has the frontend fields.
inline bool pluginHasFrontends(const PaykanPlugin &p) {
  return p.struct_size >=
         offsetof(PaykanPlugin, frontends) + sizeof(const PaykanFrontend *);
}

/// A frontend::Frontend that forwards to the C frontend @p f of a loaded
/// plugin whose descriptor is @p plugin (both stay loaded).
std::unique_ptr<frontend::Frontend>
makeFrontendAdapter(const PaykanPlugin *plugin, const PaykanFrontend *f);

/// A backend::Backend that forwards to the C backend @p b of a loaded
/// plugin (which must stay loaded: plugins are never unloaded).
std::unique_ptr<backend::Backend> makeBackendAdapter(const PaykanBackend *b);

/// "<file>:<line>:<col>: error: <message>" (the parts that are known), or
/// "<owner>: error: <message>" without a file.
std::string formatDiagnostic(const PaykanSession &s, uint32_t level,
                             const char *file, uint32_t line, uint32_t column,
                             const char *message, size_t messageSize);

} // namespace paykan::plugin::host
