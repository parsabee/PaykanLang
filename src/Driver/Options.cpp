// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0

#include "Options.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>
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
    {"emit-ast", &Options::EmitAST,
     "Print the parsed AST in the AST interchange format and exit"},
    {"emit-source", &Options::EmitSource,
     "Write the backend's source output (C, LLVM IR, ...) to stdout"},
    {"emit-pir", &Options::EmitPIR,
     "Print the program's PIR (the backend-neutral IR) and exit"},
    {"emit-pkm", &Options::EmitPkm,
     "Write the module's .pkm file (-o <file>, or next to the input) and "
     "exit"},
    {"list-frontends", &Options::ListFrontends,
     "List the available frontends and exit"},
    {"list-backends", &Options::ListBackends,
     "List the available backends and exit"},
    {"no-plugins", &Options::NoPlugins,
     "Don't search the plugin directories (--plugin files still load)"},
    {"rebuild-modules", &Options::RebuildModules,
     "Ignore the .pkm module cache entries (they are rewritten)"},
    {"no-module-cache", &Options::NoModuleCache,
     "Never read or write the .pkm module cache"},
    {"verbose", &Options::Verbose,
     "Report how each imported module was resolved (on stderr)"},
    {"stats", &Options::Stats,
     "Report what the PIR passes did (on stderr), after the last one ran"},
    {"track-heap", &Options::TrackHeap,
     "Track runtime heap allocations and dump statistics (incl. leaks) at "
     "exit"},
    {"trace-parser", &Options::TraceParsing,
     "Enable the frontend's parser debug traces, if it has them"},
    {"trace-scanner", &Options::TraceScanning,
     "Enable the frontend's scanner debug traces, if it has them"},
};

/// The PIR passes the driver runs (docs/pir.md §12), which --pir-skip may
/// name.
constexpr std::string_view kPassNames[] = {"dead-code", "moves-at-last-use",
                                           "lending", "rc-elimination"};

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
  bool sawVerb = false;
  bool emitLLVM = false;
  bool emitC = false;
  bool emitMLIR = false;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];

    // For `run`, everything after the source file belongs to the program.
    // `build`, `pkm` and `--emit-pkm` have no program arguments, so their
    // options may follow the file.
    if (!o.InputFilename.empty() && o.Cmd == Command::Run && !o.EmitPkm) {
      o.ProgramArgs.emplace_back(arg);
      continue;
    }

    std::string_view name;
    if (optionsEnded || !optionName(arg, name)) {
      if (!optionsEnded && arg == "--") {
        optionsEnded = true;
        continue;
      }
      if (!sawCommand && (arg == "run" || arg == "build" || arg == "pkm")) {
        o.Cmd = arg == "build" ? Command::Build
                : arg == "pkm" ? Command::Pkm
                               : Command::Run;
        sawCommand = true;
        continue;
      }
      sawCommand = true;
      if (o.Cmd == Command::Pkm && !sawVerb) {
        sawVerb = true;
        if (arg == "dump") {
          o.Verb = PkmVerb::Dump;
        } else if (arg == "check") {
          o.Verb = PkmVerb::Check;
        } else if (arg == "extract") {
          o.Verb = PkmVerb::Extract;
        } else {
          r.Error = "unknown pkm command '" + std::string(arg) +
                    "' (expected `dump`, `check` or `extract`)";
          return r;
        }
        continue;
      }
      if (!o.InputFilename.empty()) {
        r.Error = "unexpected argument '" + std::string(arg) + "' (`" +
                  (o.Cmd == Command::Pkm ? "pkm" : "build") +
                  "` takes one file and no program arguments)";
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

    if (std::string stage;
        valueOption(name, "emit-mlir", i, argc, argv, stage, r.Error)) {
      if (!r.Error.empty())
        return r;
      emitMLIR = true;
      o.EmitStage = std::move(stage);
      continue;
    }
    if (std::string plugin;
        valueOption(name, "plugin", i, argc, argv, plugin, r.Error)) {
      if (!r.Error.empty())
        return r;
      o.Plugins.push_back(std::move(plugin));
      continue;
    }
    // `pkm extract --object`: what to extract (a `pkm` command links no
    // native objects).
    if (o.Cmd == Command::Pkm && name == "object") {
      o.PkmObject = true;
      continue;
    }
    if (std::string list;
        valueOption(name, "object", i, argc, argv, list, r.Error)) {
      if (!r.Error.empty())
        return r;
      for (size_t start = 0; start <= list.size();) {
        size_t comma = std::min(list.find(',', start), list.size());
        if (comma > start)
          o.Objects.push_back(list.substr(start, comma - start));
        start = comma + 1;
      }
      continue;
    }
    if (std::string list;
        valueOption(name, "pir-skip", i, argc, argv, list, r.Error)) {
      if (!r.Error.empty())
        return r;
      for (size_t start = 0; start <= list.size();) {
        size_t comma = std::min(list.find(',', start), list.size());
        std::string pass = list.substr(start, comma - start);
        if (std::find(std::begin(kPassNames), std::end(kPassNames), pass) ==
            std::end(kPassNames)) {
          r.Error = "--pir-skip: no PIR pass is named '" + pass +
                    "' (dead-code, moves-at-last-use, lending, "
                    "rc-elimination)";
          return r;
        }
        o.SkipPasses.push_back(std::move(pass));
        start = comma + 1;
      }
      continue;
    }
    if (std::string dir;
        valueOption(name, "module-path", i, argc, argv, dir, r.Error)) {
      if (!r.Error.empty())
        return r;
      o.ModulePath.push_back(std::move(dir));
      continue;
    }
    if (valueOption(name, "frontend", i, argc, argv, o.Frontend, r.Error) ||
        valueOption(name, "backend", i, argc, argv, o.Backend, r.Error) ||
        valueOption(name, "section", i, argc, argv, o.PkmSection, r.Error) ||
        valueOption(name, "module-name", i, argc, argv, o.ModuleName,
                    r.Error) ||
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

  // --emit-llvm, --emit-c and --emit-mlir=<stage> are the mlir backend's
  // source outputs (its EmitOptions::SourceStage: "", "c", <stage>).
  int emits = static_cast<int>(emitLLVM) + static_cast<int>(emitC) +
              static_cast<int>(emitMLIR);
  if (emits > 1) {
    r.Error = "--emit-llvm, --emit-c and --emit-mlir exclude each other";
    return r;
  }
  if (emits == 1) {
    const char *flag =
        emitLLVM ? "--emit-llvm" : (emitC ? "--emit-c" : "--emit-mlir");
    if (!o.Backend.empty() && o.Backend != "mlir") {
      r.Error = std::string(flag) +
                " needs the mlir backend, but --backend=" + o.Backend +
                " was given";
      return r;
    }
    o.Backend = "mlir";
    o.EmitSource = true;
    if (emitC)
      o.EmitStage = "c";
  }
  if (!o.SkipPasses.empty()) {
    if (o.EmitPkm) {
      r.Error = "--pir-skip cannot be used with --emit-pkm: a .pkm keeps "
                "the code every PIR pass made";
      return r;
    }
    o.NoModuleCache = true;
  }
  if (!o.ModuleName.empty()) {
    if (!o.EmitPkm) {
      r.Error = "--module-name names the module --emit-pkm writes";
      return r;
    }
    // `a::b`, or `::a` for a module of the standard library: identifiers
    // separated by `::`.
    std::string_view rest(o.ModuleName);
    if (rest.starts_with("::"))
      rest.remove_prefix(2);
    bool valid = !rest.empty();
    for (size_t start = 0; valid && start <= rest.size();) {
      size_t sep = std::min(rest.find("::", start), rest.size());
      std::string_view part = rest.substr(start, sep - start);
      valid =
          !part.empty() && !std::isdigit(static_cast<unsigned char>(part[0]));
      for (char c : part)
        valid =
            valid && (std::isalnum(static_cast<unsigned char>(c)) || c == '_');
      start = sep + 2;
    }
    if (!valid) {
      r.Error = "--module-name: '" + o.ModuleName +
                "' is not a module name (identifiers separated by '::')";
      return r;
    }
  }
  bool infoOnly =
      o.ShowHelp || o.ShowVersion || o.ListFrontends || o.ListBackends;
  if (!infoOnly && o.InputFilename.empty())
    r.Error = o.Cmd == Command::Pkm
                  ? (sawVerb ? "no module file specified"
                             : "no pkm command specified (`dump`, `check` or "
                               "`extract`)")
                  : "no source file specified";
  return r;
}

void printUsage(std::ostream &os, const char *argv0) {
  os << "OVERVIEW: Paykan language compiler\n\n"
     << "USAGE: " << argv0
     << " [options] [run] <source-file> [program arguments]\n"
     << "       " << argv0 << " [options] build <source-file> -o <output>\n"
     << "       " << argv0
     << " pkm dump <file.pkm> [--section=manifest|sections|iface|symidx|"
        "code|obj|payloads]\n"
     << "       " << argv0 << " pkm check <file.pkm>\n"
     << "       " << argv0
     << " pkm extract --object <file.pkm> [-o <file.o>]\n\n"
     << "OPTIONS:\n";
  for (const Flag &f : kFlags)
    os << "  --" << f.Name
       << std::string(16 - std::string_view(f.Name).size(), ' ') << "- "
       << f.Help << "\n";
  os << "  --emit-llvm       - Emit the program's LLVM IR to stdout "
        "(--emit-source)\n"
     << "  --emit-c          - Emit the program's C to stdout\n"
     << "  --emit-mlir=<stage> - Emit MLIR at a stage of the mlir backend to "
        "stdout\n"
     << "                      (pir: the imported PIR dialect; scf: after "
        "the shared\n"
     << "                      lowering; llvm: the LLVM dialect; emitc: "
        "EmitC, the C)\n"
     << "  --frontend=<name> - Parse with the named frontend "
        "(--list-frontends)\n"
     << "  --backend=<name>  - Generate code with the named backend "
        "(--list-backends)\n"
     << "  --plugin=<file>   - Load a plugin library (repeatable; see "
        "--version for\n"
     << "                      the plugin directories)\n"
     << "  --module-path=<dir> - Look for prebuilt .pkm modules there "
        "(repeatable;\n"
     << "                      then $PAYKAN_MODULE_PATH)\n"
     << "  --object=<files>  - Link these objects or archives "
        "(comma-separated;\n"
     << "                      repeatable): they define the native "
        "functions\n"
     << "  -o <file>         - Output file of `build` or `--emit-pkm`\n"
     << "  --module-name=<name> - The module `--emit-pkm` writes "
        "(`a::b`, or `::name`\n"
     << "                      for a module of the standard library)\n"
     << "  -O<n>             - Optimization level (0-3, default "
     << kDefaultOptLevel << "; -O0 for debugging)\n"
     << "  --version, -v     - Print the version and exit\n"
     << "  --help, -h        - Print this help and exit\n";
}

} // namespace paykan::driver
