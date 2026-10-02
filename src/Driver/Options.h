// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Command-line options of the paykan driver.

#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace paykan::driver {

/// What the driver is asked to do with the program.
enum class Command {
  Run,   // `paykan [run] file.pkn args...`: compile and execute (default)
  Build, // `paykan build file.pkn -o out`: compile to an executable
};

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

  /// -o <file>: the output of `build`.
  std::string OutputPath;

  bool DumpAST = false;       // --dump-ast
  bool DumpTokens = false;    // --dump-tokens
  bool TraceParsing = false;  // --trace-parser
  bool TraceScanning = false; // --trace-scanner
  bool CheckOnly = false;     // --check-only
  bool TrackHeap = false;     // --track-heap
  unsigned OptLevel = 0;      // -O<n>

  /// --emit-source: write the backend's source output (C, LLVM IR, ...) to
  /// stdout and stop.  --emit-llvm is --backend=llvm --emit-source.
  bool EmitSource = false;

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
