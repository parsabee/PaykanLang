// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The host side of the C plugin interface (paykan/plugin_api.h), private
// to src/PluginHost: the host table paykan hands every plugin, the session
// each callback runs in, and the adapter that turns a C backend into a
// backend::Backend.

#pragma once

#include "paykan/Backend.h"
#include "paykan/backends/Toolchain.h"
#include "paykan/plugin_api.h"

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
  /// "backend 'name'", for messages.
  std::string Owner;
  /// The error diagnostics reported so far, formatted: the failure message
  /// of the call.
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
/// loading.
inline constexpr size_t kMinPluginSize = sizeof(PaykanPlugin);
inline constexpr size_t kMinBackendSize = sizeof(PaykanBackend);

/// A backend::Backend that forwards to the C backend @p b of a loaded
/// plugin (which must stay loaded: plugins are never unloaded).
std::unique_ptr<backend::Backend> makeBackendAdapter(const PaykanBackend *b);

/// "<file>:<line>:<col>: error: <message>" (the parts that are known), or
/// "<owner>: error: <message>" without a file.
std::string formatDiagnostic(const PaykanSession &s, uint32_t level,
                             const char *file, uint32_t line, uint32_t column,
                             const char *message, size_t messageSize);

} // namespace paykan::plugin::host
