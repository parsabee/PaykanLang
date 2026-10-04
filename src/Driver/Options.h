// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Command-line options of the paykan driver.

#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace paykan::driver {

/// What the driver is asked to do with the program.
enum class Command {
  Run,   // `paykan [run] file.pkn args...`: compile and execute (default)
  Build, // `paykan build file.pkn -o out`: compile to an executable
};

/// The optimisation level of `paykan build` and `paykan run` (and --emit-*)
/// without -O<n> (#102).  -O2 for `run` too: over the samples corpus it adds
/// about 5 ms (llvm JIT) and 30 ms (c, cold cache) per program to -O1's
/// compile time, and the code it runs is up to 2.5x faster.
inline constexpr unsigned kDefaultOptLevel = 2;

/// Everything the driver reads from the command line.
struct Options {
  Command Cmd = Command::Run;

  /// The source file to compile (the first positional argument after the
  /// optional command word).
  std::string InputFilename;
  /// Arguments forwarded to the Paykan program: everything after the source
  /// file, verbatim, including arguments that look like options.
  std::vector<std::string> ProgramArgs;

  /// --frontend=<name>; "" selects the build's default.
  std::string Frontend;
  bool ListFrontends = false; // --list-frontends

  /// --backend=<name>; "" selects the build's default.
  std::string Backend;
  bool ListBackends = false; // --list-backends

  /// --plugin=<file> (repeatable): plugin libraries to load, in order,
  /// before the plugin directories are searched (paykan/PluginLoader.h).
  std::vector<std::string> Plugins;
  /// --no-plugins: don't search the plugin directories ($PAYKAN_PLUGIN_PATH,
  /// the user and system directories); --plugin files are still loaded.
  bool NoPlugins = false;

  /// -o <file>: the output of `build`.
  std::string OutputPath;

  bool DumpAST = false;       // --dump-ast
  bool EmitAST = false;       // --emit-ast: the AST interchange format
  bool DumpTokens = false;    // --dump-tokens
  bool TraceParsing = false;  // --trace-parser
  bool TraceScanning = false; // --trace-scanner
  bool CheckOnly = false;     // --check-only
  bool TrackHeap = false;     // --track-heap
  /// -O<n>; unset means the default (optLevel()).
  std::optional<unsigned> OptLevel;

  /// The optimisation level the backend gets: -O<n> when given, else
  /// kDefaultOptLevel.
  unsigned optLevel() const { return OptLevel.value_or(kDefaultOptLevel); }

  /// --emit-source: write the backend's source output (C, LLVM IR, ...) to
  /// stdout and stop.  --emit-llvm is --backend=llvm --emit-source.
  bool EmitSource = false;
  /// --emit-pir: print the verified PIR of the whole program and stop.
  bool EmitPIR = false;

  bool ShowHelp = false;    // --help / -h
  bool ShowVersion = false; // --version / -v
};

/// Outcome of parsing the command line.
struct ParseResult {
  Options Opts;
  /// Non-empty when the command line was rejected; the driver prints it and
  /// exits with failure.
  std::string Error;
};

/// Parse argv.  Options take one or two leading dashes (`--dump-ast` and
/// `-dump-ast` are the same), `-O<n>` and `-O=<n>` set the optimisation level,
/// `--frontend=<name>` / `--backend=<name>` (or with a space) select the
/// plugins, `-o <file>` names the output, and `--` ends option processing.
/// The first positional argument may be a command word (`run`, `build`); the
/// next is the source file, and everything after it belongs to the program.
ParseResult parseCommandLine(int argc, const char *const *argv);

/// Print the usage text.
void printUsage(std::ostream &os, const char *argv0);

} // namespace paykan::driver
