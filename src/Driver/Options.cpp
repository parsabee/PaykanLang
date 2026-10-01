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
    {"dump-ast", &Options::DumpAST, "Print the AST in tree form"},
    {"check-only", &Options::CheckOnly,
     "Run parsing and semantic analysis only (no codegen)"},
    {"emit-llvm", &Options::EmitLLVM, "Emit LLVM IR to stdout"},
    {"track-heap", &Options::TrackHeap,
     "Track runtime heap allocations and dump statistics (incl. leaks) at "
     "exit"},
    {"trace-parser", &Options::TraceParsing,
     "Enable Bison parser debug traces"},
    {"trace-scanner", &Options::TraceScanning,
     "Enable Flex scanner debug traces"},
};

/// Strip one or two leading dashes; returns false if @p arg is not an option
/// (no dash, a bare "-", or "--").
bool optionName(std::string_view arg, std::string_view &name) {
  if (arg.size() < 2 || arg[0] != '-')
    return false;
  name = arg.substr(arg[1] == '-' ? 2 : 1);
  return !name.empty();
}

/// Parse the digits of an -O level.  Returns false on anything but a small
/// non-negative integer.
bool parseLevel(std::string_view text, unsigned &level) {
  if (text.empty() || text.size() > 3)
    return false;
  unsigned v = 0;
  for (char c : text) {
    if (c < '0' || c > '9')
      return false;
    v = v * 10 + static_cast<unsigned>(c - '0');
  }
  level = v;
  return true;
}

} // namespace

ParseResult parseCommandLine(int argc, const char *const *argv) {
  ParseResult r;
  Options &o = r.Opts;
  bool optionsEnded = false;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];

    // Everything after the source file belongs to the program.
    if (!o.InputFilename.empty()) {
      o.ProgramArgs.emplace_back(arg);
      continue;
    }

    std::string_view name;
    if (optionsEnded || !optionName(arg, name)) {
      if (!optionsEnded && arg == "--") {
        optionsEnded = true;
        continue;
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
      if (!parseLevel(level, o.OptLevel)) {
        r.Error = "invalid optimization level '" + std::string(level) +
                  "' (expected -O0 .. -O3)";
        return r;
      }
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

  if (!o.ShowHelp && !o.ShowVersion && o.InputFilename.empty())
    r.Error = "no source file specified";
  return r;
}

void printUsage(std::ostream &os, const char *argv0) {
  os << "OVERVIEW: Paykan language compiler\n\n"
     << "USAGE: " << argv0 << " [options] <source-file> [program arguments]\n\n"
     << "OPTIONS:\n";
  for (const Flag &f : kFlags)
    os << "  --" << f.Name << std::string(16 - std::string_view(f.Name).size(),
                                          ' ')
       << "- " << f.Help << "\n";
  os << "  -O<n>             - Optimization level (0-3)\n"
     << "  --version, -v     - Print the version and exit\n"
     << "  --help, -h        - Print this help and exit\n";
}

} // namespace paykan::driver
