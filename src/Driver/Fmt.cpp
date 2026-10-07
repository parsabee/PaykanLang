// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// `paykan fmt [--write | --check] [<file.pkn> | <dir> | -]...`
//
// Without --write or --check, prints the formatted file (one file, or
// standard input for `-` or no file) to standard output.  --write rewrites
// every file that changes; --check lists the files that are not formatted
// and exits 1 if there are any.  A directory stands for the `.pkn` files
// below it.

#include "Fmt.h"

#include "paykan/format/Format.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace paykan::driver {

namespace {

namespace fs = std::filesystem;

void printFmtUsage(std::ostream &os, const char *argv0) {
  os << "USAGE: " << argv0
     << " fmt [--write | --check] [<file.pkn> | <directory> | -]...\n\n"
     << "Formats Paykan source in two columns: code on the left; trailing\n"
     << "comments and the bodies of short functions from column "
     << format::Style().RightColumn << ".\n\n"
     << "  (no option)  Print the formatted file (or standard input) to\n"
     << "               standard output\n"
     << "  --write      Rewrite every file that changes\n"
     << "  --check      List the files that are not formatted; exit 1 if any\n"
     << "  --help, -h   Print this help and exit\n";
}

std::optional<std::string> readAll(std::istream &in) {
  std::ostringstream s;
  s << in.rdbuf();
  if (in.bad())
    return std::nullopt;
  return s.str();
}

/// @p arg, or the `.pkn` files below it when it is a directory (sorted;
/// module caches skipped).
std::vector<std::string> expand(const std::string &arg) {
  std::error_code ec;
  if (!fs::is_directory(arg, ec))
    return {arg};
  std::vector<std::string> files;
  fs::recursive_directory_iterator it(arg, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    if (it->is_directory(ec) && it->path().filename() == ".paykan_cache") {
      it.disable_recursion_pending();
      continue;
    }
    if (it->is_regular_file(ec) && it->path().extension() == ".pkn")
      files.push_back(it->path().string());
  }
  std::sort(files.begin(), files.end());
  return files;
}

} // namespace

int fmtTool(int argc, const char *const *argv) {
  bool write = false;
  bool check = false;
  bool optionsEnded = false;
  std::vector<std::string> args;
  for (int i = 2; i < argc; ++i) {
    std::string_view a = argv[i];
    if (!optionsEnded && a == "--") {
      optionsEnded = true;
    } else if (!optionsEnded && (a == "--help" || a == "-h")) {
      printFmtUsage(std::cout, argv[0]);
      return EXIT_SUCCESS;
    } else if (!optionsEnded && a == "--write") {
      write = true;
    } else if (!optionsEnded && a == "--check") {
      check = true;
    } else if (!optionsEnded && a.size() > 1 && a[0] == '-') {
      std::cerr << "error: unknown fmt option '" << a << "'. Try: '" << argv[0]
                << " fmt --help'\n";
      return EXIT_FAILURE;
    } else {
      args.emplace_back(a);
    }
  }
  if (write && check) {
    std::cerr << "error: --write and --check cannot be combined\n";
    return EXIT_FAILURE;
  }

  std::vector<std::string> files;
  for (const std::string &a : args)
    for (std::string &f : expand(a))
      files.push_back(std::move(f));
  bool toStdout = !write && !check;
  if (files.empty() && !args.empty()) {
    std::cerr << "error: no .pkn files found\n";
    return EXIT_FAILURE;
  }
  if (files.empty())
    files.emplace_back("-");
  if (toStdout && files.size() > 1) {
    std::cerr << "error: printing formats one file; use --write or --check "
                 "for several\n";
    return EXIT_FAILURE;
  }

  int status = EXIT_SUCCESS;
  for (const std::string &file : files) {
    bool isStdin = file == "-";
    std::optional<std::string> source;
    if (isStdin) {
      source = readAll(std::cin);
    } else {
      std::ifstream in(file, std::ios::binary);
      if (in)
        source = readAll(in);
    }
    std::string shown = isStdin ? "<stdin>" : file;
    if (!source) {
      std::cerr << "error: cannot read '" << shown << "'\n";
      status = EXIT_FAILURE;
      continue;
    }
    format::Result r = format::format(*source);
    if (!r.Ok) {
      std::cerr << shown << ":" << r.Error.substr(0, r.Error.find(": "))
                << ": error: " << r.Error.substr(r.Error.find(": ") + 2)
                << "\n";
      status = EXIT_FAILURE;
      continue;
    }
    if (toStdout) {
      std::cout << r.Text;
      continue;
    }
    if (r.Text == *source)
      continue;
    if (check) {
      std::cout << shown << ": not formatted\n";
      status = EXIT_FAILURE;
      continue;
    }
    if (isStdin) {
      std::cout << r.Text;
      continue;
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << r.Text;
    if (!out) {
      std::cerr << "error: cannot write '" << file << "'\n";
      status = EXIT_FAILURE;
    }
  }
  return status;
}

} // namespace paykan::driver
