// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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
  Pkm,   // `paykan pkm dump|check file.pkm`: inspect a module file
};

/// The verb of `paykan pkm`.
enum class PkmVerb {
  Dump,    // print the file (pkm::dump) and the PIR text of its CODE
  Check,   // print the verdict against this toolchain; exit 0 when usable
  Extract, // write a part of the file on its own (--object: its code)
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
  /// Arguments forwarded to the Paykan program under `run`: everything after
  /// the source file, verbatim, including arguments that look like options.
  /// What asks for anything else (`build`, `pkm`, --check-only, --emit-*)
  /// takes none, so its options may follow the file.
  std::vector<std::string> ProgramArgs;

  /// --frontend=<name>; "" selects the build's default.
  std::string Frontend;
  bool ListFrontends = false; // --list-frontends

  /// --backend=<name>; "" selects the build's default.
  std::string Backend;
  bool ListBackends = false; // --list-backends
  /// --list-modules: print the module file of each module whose compiled
  /// code the program links (what --emit-c's C needs as objects) and stop.
  bool ListModules = false;

  /// --plugin=<file> (repeatable): plugin libraries to load, in order,
  /// before the plugin directories are searched (paykan/PluginLoader.h).
  std::vector<std::string> Plugins;
  /// --no-plugins: don't search the plugin directories ($PAYKAN_PLUGIN_PATH,
  /// the user and system directories); --plugin files are still loaded.
  bool NoPlugins = false;

  /// -o <file>: the output of `build`, `--emit-pkm`, `pkm extract` or an
  /// --emit-* option (which prints to standard output without it).
  std::string OutputPath;
  /// --module-name=<name>: the canonical name of the module `--emit-pkm`
  /// writes (`geometry::shapes`, or `::collections` for a module of the
  /// standard library), instead of the one its path gives it.
  std::string ModuleName;
  /// The mlir backend's source output (its EmitOptions::SourceStage): the
  /// stage of the MLIR pipeline --emit-mlir=<stage> names, "c" for --emit-c,
  /// empty for its LLVM IR (--emit-llvm, --emit-source).
  std::string EmitStage;

  /// `paykan pkm <verb> <file>`: the verb and the `--section=<name>` of
  /// `dump` ("" = all).
  PkmVerb Verb = PkmVerb::Dump;
  /// `pkm extract --object`: the module's compiled code as a native object.
  bool PkmObject = false;
  std::string PkmSection;

  /// --module-path=<dir> (repeatable): where prebuilt `.pkm` modules are
  /// looked for after the source tree, before $PAYKAN_MODULE_PATH.
  std::vector<std::string> ModulePath;
  /// --object=<file>[,<file>...] (repeatable): objects or archives defining
  /// the program's native functions (#198), linked by every native backend.
  std::vector<std::string> Objects;
  /// --pir-skip=<pass>[,<pass>...] (repeatable, not in --help): the PIR
  /// passes not to run, by name (docs/pir.md §12), to compare the code made
  /// without them.  It implies --no-module-cache, so no cache entry keeps
  /// code made without them, and no `.pkm` is written with it.  It leaves
  /// them out of the code this run lowers, the program's and that of the
  /// modules it compiles from source; code read from a `.pkm` (a prebuilt
  /// module's, the standard library's, and each shared instantiation such a
  /// module made, which the program shares rather than makes again) went
  /// through every pass when the `.pkm` was written, and keeps what they
  /// did.
  std::vector<std::string> SkipPasses;
  bool EmitPkm = false;        // --emit-pkm: write the module's .pkm, stop
  bool RebuildModules = false; // --rebuild-modules: ignore cache entries
  bool NoModuleCache = false;  // --no-module-cache: never read or write it
  bool Verbose = false;        // --verbose: one line per import on stderr
  bool Stats = false;          // --stats: what the PIR passes did, on stderr

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

  /// --emit-source: write the backend's source output (the mlir backend's
  /// LLVM IR, ...) to stdout and stop.  --emit-llvm, --emit-c and
  /// --emit-mlir=<stage> set it with EmitStage.
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
/// The first positional argument may be a command word (`run`, `build`,
/// `pkm`); the next is the source file (after `pkm`: the verb, then the
/// module file), and everything after it belongs to the program.
ParseResult parseCommandLine(int argc, const char *const *argv);

/// Print the usage text.
void printUsage(std::ostream &os, const char *argv0);

} // namespace paykan::driver
