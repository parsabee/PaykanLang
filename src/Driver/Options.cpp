// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "Options.h"

#include <cstdlib>
#include <ostream>
#include <string_view>

namespace paykan::driver {

namespace {

struct Flag {
  const char *Name;
  bool Options::*Field;
  const char *Help;
};

// Boolean options, in the order --help lists them.
constexpr Flag kFlags[] = {
    {"check-only", &Options::CheckOnly,
     "Run parsing and semantic analysis only (no codegen)"},
    {"dump-ast", &Options::DumpAST, "Print the AST in tree form"},
    {"dump-tokens", &Options::DumpTokens,
     "Print the token stream (frontends that support it)"},
    {"emit-source", &Options::EmitSource,
     "Write the backend's source output (C, LLVM IR, ...) to stdout"},
    {"emit-pir", &Options::EmitPIR,
     "Print the program's PIR (the backend-neutral IR) and exit"},
    {"list-frontends", &Options::ListFrontends,
     "List the available frontends and exit"},
    {"list-backends", &Options::ListBackends,
     "List the available backends and exit"},
    {"track-heap", &Options::TrackHeap,
     "Track runtime heap allocations and dump statistics (incl. leaks) at "
     "exit"},
    {"trace-parser", &Options::TraceParsing,
     "Enable the frontend's parser debug traces, if it has them"},
    {"trace-scanner", &Options::TraceScanning,
     "Enable the frontend's scanner debug traces, if it has them"},
};

/// Strip one or two leading dashes; returns false if @p arg is not an option
/// (no dash, a bare "-", or "--").
bool optionName(std::string_view arg, std::string_view &name) {
  if (arg.size() < 2 || arg[0] != '-')
    return false;
  name = arg.substr(arg[1] == '-' ? 2 : 1);
  return !name.empty();
}

/// The highest -O level (`--help` and the README document 0..3).
constexpr unsigned kMaxOptLevel = 3;

/// Parse the digits of an -O level.  Returns false on anything but a
/// non-negative integer up to kMaxOptLevel (`-O4` .. `-O999` are rejected).
bool parseLevel(std::string_view text, unsigned &level) {
  if (text.empty() || text.size() > 3)
    return false;
  unsigned v = 0;
  for (char c : text) {
    if (c < '0' || c > '9')
      return false;
    v = v * 10 + static_cast<unsigned>(c - '0');
  }
  if (v > kMaxOptLevel)
    return false;
  level = v;
  return true;
}

/// `--<key>=<value>` or `--<key> <value>`.  Returns true when @p name is this
/// option; @p value is then set, or @p error on a missing value.
bool valueOption(std::string_view name, const char *key, int &i, int argc,
                 const char *const *argv, std::string &value,
                 std::string &error) {
  std::string_view k(key);
  if (name != k &&
      !(name.size() > k.size() && name.starts_with(k) && name[k.size()] == '='))
    return false;
  std::string_view v = name.substr(k.size());
  if (!v.empty())
    v.remove_prefix(1); // '='
  else if (i + 1 < argc)
    v = argv[++i];
  if (v.empty())
    error = std::string("option '--") + key + "' requires a value";
  value = std::string(v);
  return true;
}

} // namespace

ParseResult parseCommandLine(int argc, const char *const *argv) {
  ParseResult r;
  Options &o = r.Opts;
  bool optionsEnded = false;
  bool sawCommand = false;
  bool emitLLVM = false;
  bool emitC = false;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];

    // For `run`, everything after the source file belongs to the program.
    // `build` has no program arguments, so its options may follow the file.
    if (!o.InputFilename.empty() && o.Cmd != Command::Build) {
      o.ProgramArgs.emplace_back(arg);
      continue;
    }

    std::string_view name;
    if (optionsEnded || !optionName(arg, name)) {
      if (!optionsEnded && arg == "--") {
        optionsEnded = true;
        continue;
      }
      if (!sawCommand && (arg == "run" || arg == "build")) {
        o.Cmd = arg == "build" ? Command::Build : Command::Run;
        sawCommand = true;
        continue;
      }
      sawCommand = true;
      if (!o.InputFilename.empty()) {
        r.Error = "unexpected argument '" + std::string(arg) +
                  "' (`build` takes one source file and no program "
                  "arguments)";
        return r;
      }
      o.InputFilename = std::string(arg);
      continue;
    }

    if (name == "help" || name == "h") {
      o.ShowHelp = true;
      continue;
    }
    if (name == "version" || name == "v") {
      o.ShowVersion = true;
      continue;
    }
    if (name == "emit-llvm") {
      emitLLVM = true;
      continue;
    }
    if (name == "emit-c") {
      emitC = true;
      continue;
    }

    if (valueOption(name, "frontend", i, argc, argv, o.Frontend, r.Error) ||
        valueOption(name, "backend", i, argc, argv, o.Backend, r.Error) ||
        valueOption(name, "o", i, argc, argv, o.OutputPath, r.Error)) {
      if (!r.Error.empty())
        return r;
      continue;
    }

    // -O<n>, -O=<n>, and -O <n>.
    if (name.size() >= 1 && name[0] == 'O') {
      std::string_view level = name.substr(1);
      if (!level.empty() && level[0] == '=')
        level.remove_prefix(1);
      if (level.empty()) {
        if (i + 1 >= argc) {
          r.Error = "option '-O' requires a value";
          return r;
        }
        level = argv[++i];
      }
      unsigned value = 0;
      if (!parseLevel(level, value)) {
        r.Error = "invalid optimization level '" + std::string(level) +
                  "' (expected -O0 .. -O3)";
        return r;
      }
      o.OptLevel = value;
      continue;
    }

    bool matched = false;
    for (const Flag &f : kFlags) {
      if (name == f.Name) {
        o.*f.Field = true;
        matched = true;
        break;
      }
    }
    if (!matched) {
      r.Error = "unknown command line argument '" + std::string(arg) + "'";
      return r;
    }
  }

  if (emitLLVM) {
    if (!o.Backend.empty() && o.Backend != "llvm") {
      r.Error =
          "--emit-llvm needs the llvm backend, but --backend=" + o.Backend +
          " was given";
      return r;
    }
    o.Backend = "llvm";
    o.EmitSource = true;
  }
  if (emitC) {
    if (!o.Backend.empty() && o.Backend != "c") {
      r.Error = "--emit-c needs the c backend, but --backend=" + o.Backend +
                " was given";
      return r;
    }
    o.Backend = "c";
    o.EmitSource = true;
  }
  bool infoOnly =
      o.ShowHelp || o.ShowVersion || o.ListFrontends || o.ListBackends;
  if (!infoOnly && o.InputFilename.empty())
    r.Error = "no source file specified";
  return r;
}

void printUsage(std::ostream &os, const char *argv0) {
  os << "OVERVIEW: Paykan language compiler\n\n"
     << "USAGE: " << argv0
     << " [options] [run] <source-file> [program arguments]\n"
     << "       " << argv0 << " [options] build <source-file> -o <output>\n\n"
     << "OPTIONS:\n";
  for (const Flag &f : kFlags)
    os << "  --" << f.Name
       << std::string(16 - std::string_view(f.Name).size(), ' ') << "- "
       << f.Help << "\n";
  os << "  --emit-llvm       - Emit LLVM IR to stdout (--backend=llvm "
        "--emit-source)\n"
     << "  --emit-c          - Emit C to stdout (--backend=c --emit-source)\n"
     << "  --frontend=<name> - Parse with the named frontend "
        "(--list-frontends)\n"
     << "  --backend=<name>  - Generate code with the named backend "
        "(--list-backends)\n"
     << "  -o <file>         - Output file of `build`\n"
     << "  -O<n>             - Optimization level (0-3, default "
     << kDefaultOptLevel << "; -O0 for debugging)\n"
     << "  --version, -v     - Print the version and exit\n"
     << "  --help, -h        - Print this help and exit\n";
}

} // namespace paykan::driver
