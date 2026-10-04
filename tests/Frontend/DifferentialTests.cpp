// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Differential check of the enabled frontends: every .pkn file under
// samples/ must be accepted or rejected by all of them
// alike and, when accepted, yield the same printed AST (docs/grammar.md).
// With a single frontend built the test only checks that the corpus parses
// without crashing.

#include "ASTPrinter.h"
#include "ParserDriver.h"
#include "paykan/Frontend.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#ifndef PAYKAN_SAMPLES_DIR
#error "PAYKAN_SAMPLES_DIR must be defined via CMake compile definition"
#endif

namespace {

std::vector<std::string> corpus() {
  std::vector<std::string> files;
  for (const auto &e :
       std::filesystem::recursive_directory_iterator(PAYKAN_SAMPLES_DIR))
    if (e.is_regular_file() && e.path().extension() == ".pkn")
      files.push_back(e.path().string());
  std::sort(files.begin(), files.end());
  return files;
}

struct Outcome {
  bool Ok;
  std::string Dump;
  std::string Diags;
};

Outcome parseWith(const std::string &frontend, const std::string &path) {
  paykan::parser::ParserDriver driver(frontend);
  std::ostringstream diagOS;
  paykan::sema::DiagEngine diag(diagOS);
  diag.setSourceInfo(path, &driver.getSourceLines());
  driver.setDiagEngine(&diag);
  Outcome o{driver.parseFile(path) == 0, "", ""};
  if (o.Ok) {
    std::ostringstream os;
    paykan::ast::ASTPrinter printer(os);
    printer.visit(driver.getRoot());
    o.Dump = os.str();
  }
  o.Diags = diagOS.str();
  return o;
}

} // namespace

TEST(Differential, CorpusIsNotEmpty) { EXPECT_GE(corpus().size(), 100u); }

TEST(Differential, EveryFrontendAgreesOnEverySample) {
  auto names = paykan::frontend::Registry::get().names();
  ASSERT_FALSE(names.empty());
  size_t accepted = 0, rejected = 0;
  for (const std::string &path : corpus()) {
    Outcome ref = parseWith(names[0], path);
    (ref.Ok ? accepted : rejected)++;
    for (size_t i = 1; i < names.size(); ++i) {
      Outcome other = parseWith(names[i], path);
      EXPECT_EQ(ref.Ok, other.Ok)
          << path << ": '" << names[0] << "' and '" << names[i]
          << "' disagree on accepting it\n"
          << names[0] << ": " << ref.Diags << names[i] << ": " << other.Diags;
      if (ref.Ok && other.Ok) {
        EXPECT_EQ(ref.Dump, other.Dump)
            << path << ": '" << names[0] << "' and '" << names[i]
            << "' build different ASTs";
      }
    }
  }
  // The corpus has both kinds of inputs, so both directions are exercised.
  EXPECT_GT(accepted, 0u);
  EXPECT_GT(rejected, 0u);
}
