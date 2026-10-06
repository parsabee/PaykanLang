// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The host table of the C plugin interface: what paykan offers a plugin
// (paykan/plugin_api.h, docs/plugins/plugin-api.md).  Every function here is
// called from plugin code through a C function pointer, so none of them may
// let anything unwind (the core is built without exceptions anyway).

#include "PluginHost.h"

#include "paykan/Registry.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace paykan::plugin::host {

namespace {

/// The text of a (pointer, size) buffer; a null pointer is "".
std::string_view text(const char *data, size_t size) {
  return data ? std::string_view(data, size) : std::string_view();
}

const char *levelName(uint32_t level) {
  switch (level) {
  case PAYKAN_DIAG_ERROR:
    return "error";
  case PAYKAN_DIAG_WARNING:
    return "warning";
  default:
    return "note";
  }
}

void diagnostic(PaykanSession *s, uint32_t level, const char *file,
                uint32_t line, uint32_t column, const char *message,
                size_t messageSize) {
  if (!s)
    return;
  if (s->Diag) {
    // A frontend's: the built-in frontends' format, source snippet and all.
    // A diagnostic about another file than the one being parsed has no
    // snippet: its position goes into the message.
    std::string msg(text(message, messageSize));
    ast::SourceLocation loc(line, column, line, column);
    if (file && *file && file != s->InputFile) {
      msg = std::string(file) + (line ? ":" + std::to_string(line) : "") +
            (line && column ? ":" + std::to_string(column) : "") + ": " + msg;
      loc = ast::SourceLocation();
    }
    if (level == PAYKAN_DIAG_ERROR) {
      ++s->DiagErrors;
      s->Diag->error(loc, msg);
    } else if (level == PAYKAN_DIAG_WARNING) {
      s->Diag->warning(loc, msg);
    } else {
      s->Diag->note(loc, msg);
    }
    return;
  }
  std::string d =
      formatDiagnostic(*s, level, file, line, column, message, messageSize);
  if (level == PAYKAN_DIAG_ERROR)
    s->Errors.push_back(std::move(d));
  else
    std::cerr << d << "\n";
}

int writeOutput(PaykanSession *s, const void *data, size_t size) {
  if (!s || !s->Out || (!data && size))
    return PAYKAN_ERROR;
  s->Out->write(static_cast<const char *>(data),
                static_cast<std::streamsize>(size));
  return *s->Out ? PAYKAN_OK : PAYKAN_ERROR;
}

void *allocate(size_t size) { return std::malloc(size); }

void deallocate(void *ptr) { std::free(ptr); }

bool debugLogging() {
  const char *env = std::getenv("PAYKAN_PLUGIN_DEBUG");
  return env && env[0] && std::string_view(env) != "0";
}

void log(uint32_t level, const char *message, size_t messageSize) {
  if (level == PAYKAN_LOG_DEBUG && !debugLogging())
    return;
  std::cerr << "paykan: plugin: "
            << (level == PAYKAN_LOG_WARNING ? "warning: " : "")
            << text(message, messageSize) << "\n";
}

void error(PaykanSession *s, const std::string &msg) {
  diagnostic(s, PAYKAN_DIAG_ERROR, nullptr, 0, 0, msg.data(), msg.size());
}

/// The text of @p errs without its trailing newlines.
std::string message(const std::ostringstream &errs) {
  std::string msg = errs.str();
  while (!msg.empty() && msg.back() == '\n')
    msg.pop_back();
  return msg;
}

/// The session's toolchain, resolved once; null (with a diagnostic) when
/// the runtime can't be found.
const toolchain::Toolchain *resolved(PaykanSession *s) {
  if (!s)
    return nullptr;
  if (!s->ToolchainResolved) {
    s->ToolchainResolved = true;
    s->Toolchain.ExtraFlags.push_back(
        "-O" + std::to_string(s->OptLevel > 3 ? 3u : s->OptLevel));
    std::ostringstream errs;
    s->ToolchainOk = toolchain::resolveToolchain(s->Toolchain, errs);
    if (!s->ToolchainOk)
      error(s, message(errs));
  }
  return s->ToolchainOk ? &s->Toolchain : nullptr;
}

const char *runtimeLibrary(PaykanSession *s) {
  const toolchain::Toolchain *tc = resolved(s);
  return tc ? tc->RuntimeLib.c_str() : nullptr;
}

const char *runtimeIncludeDir(PaykanSession *s) {
  const toolchain::Toolchain *tc = resolved(s);
  return tc ? tc->RuntimeIncludeDir.c_str() : nullptr;
}

int linkExecutable(PaykanSession *s, const char *const *inputs,
                   size_t numInputs, const char *outputPath) {
  const toolchain::Toolchain *tc = resolved(s);
  if (!tc)
    return PAYKAN_ERROR;
  if (!outputPath || !*outputPath || (!inputs && numInputs)) {
    error(s, "link_executable: no output path or no inputs");
    return PAYKAN_ERROR;
  }
  // C inputs include Runtime.h; the flag is harmless for objects.
  std::vector<std::string> args{"-I" + tc->RuntimeIncludeDir};
  for (size_t i = 0; i < numInputs; ++i) {
    if (!inputs[i]) {
      error(s, "link_executable: input " + std::to_string(i) + " is null");
      return PAYKAN_ERROR;
    }
    args.emplace_back(inputs[i]);
  }
  std::ostringstream errs;
  if (!toolchain::linkExecutable(args, outputPath, *tc, errs)) {
    error(s, message(errs));
    return PAYKAN_ERROR;
  }
  return PAYKAN_OK;
}

int runExecutable(PaykanSession *s, const char *path, const char *const *args,
                  size_t numArgs, uint32_t trackHeap, int *exitCode) {
  if (!s || !path || !*path || !exitCode || (!args && numArgs)) {
    error(s, "run_executable: no path, no exit-code slot or no arguments");
    return PAYKAN_ERROR;
  }
  std::vector<std::string> rest;
  for (size_t i = 1; i < numArgs; ++i)
    rest.emplace_back(args[i] ? args[i] : "");
  std::string argv0 = numArgs && args[0] ? args[0] : "";
  // The runtime reads these (src/Runtime): heap statistics at exit, and a
  // program started with argc == 0.
  std::vector<std::pair<std::string, std::string>> env;
  if (trackHeap)
    env.emplace_back("PAYKAN_TRACK_HEAP", "1");
  if (!numArgs)
    env.emplace_back("PAYKAN_NO_ARGS", "1");
  std::cout.flush();
  std::cerr.flush();
  std::ostringstream errs;
  int rc = toolchain::spawn(path, rest, numArgs ? &argv0 : nullptr, env, errs);
  if (rc < 0) {
    std::string msg = message(errs);
    error(s, msg.empty() ? std::string("cannot run '") + path + "'" : msg);
    return PAYKAN_ERROR;
  }
  *exitCode = rc;
  return PAYKAN_OK;
}

const char *tempDir(PaykanSession *s) {
  if (!s)
    return nullptr;
  if (!s->Temp)
    s->Temp = std::make_unique<toolchain::TempDir>();
  if (s->Temp->Path.empty()) {
    error(s, "cannot create a temporary directory");
    return nullptr;
  }
  return s->Temp->Path.c_str();
}

/// The version string, with static storage.
const std::string &toolchainVersionString() {
  static const std::string v(toolchainVersion());
  return v;
}

} // namespace

std::string formatDiagnostic(const PaykanSession &s, uint32_t level,
                             const char *file, uint32_t line, uint32_t column,
                             const char *message, size_t messageSize) {
  std::string where = file && *file ? std::string(file) : s.InputFile;
  std::string out;
  if (where.empty() || (!line && (!file || !*file))) {
    // Nothing points into a file: name the plugin.
    out = s.Owner.empty() ? std::string("plugin") : s.Owner;
  } else {
    out = where;
    if (line) {
      out += ":" + std::to_string(line);
      if (column)
        out += ":" + std::to_string(column);
    }
  }
  out += ": ";
  out += levelName(level);
  out += ": ";
  out += text(message, messageSize);
  return out;
}

const PaykanHost *hostTable() {
  static const PaykanHost table = {
      sizeof(PaykanHost),
      PAYKAN_PLUGIN_API_VERSION,
      toolchainVersionString().c_str(),
      &diagnostic,
      &writeOutput,
      &allocate,
      &deallocate,
      &log,
      &runtimeLibrary,
      &runtimeIncludeDir,
      &linkExecutable,
      &runExecutable,
      &tempDir,
  };
  return &table;
}

} // namespace paykan::plugin::host
