// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Command-line options of the paykan driver.

#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace paykan::driver {

/// Everything the driver reads from the command line.
struct Options {
  /// The source file to compile (the first positional argument).
  std::string InputFilename;
  /// Arguments forwarded to the Paykan program: everything after the source
  /// file, verbatim, including arguments that look like options.
  std::vector<std::string> ProgramArgs;

  bool DumpAST = false;       // --dump-ast
  bool TraceParsing = false;  // --trace-parser
  bool TraceScanning = false; // --trace-scanner
  bool EmitLLVM = false;      // --emit-llvm
  bool CheckOnly = false;     // --check-only
  bool TrackHeap = false;     // --track-heap
  unsigned OptLevel = 0;      // -O<n>

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
/// and `--` ends option processing.  The first positional argument is the
/// source file; everything after it belongs to the program.
ParseResult parseCommandLine(int argc, const char *const *argv);

/// Print the usage text.
void printUsage(std::ostream &os, const char *argv0);

} // namespace paykan::driver
