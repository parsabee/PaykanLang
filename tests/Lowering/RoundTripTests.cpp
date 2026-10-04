// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// PIR text round trip over the whole corpus: every program under samples/
// that passes Sema is lowered, verified and printed; the
// text is parsed back, verified again and re-printed, and the two texts must
// be identical (docs/pir.md: the printed form is a faithful serialization).

#include "TestUtils.h"
#include "paykan/lowering/Lowering.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Printer.h"
#include "paykan/pir/Verifier.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#ifndef PAYKAN_SAMPLES_DIR
#error "PAYKAN_SAMPLES_DIR must be defined via CMake compile definition"
#endif

using namespace paykan;

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

/// Lower @p path the way the driver does (project root = its directory).
/// Returns false (and leaves @p program empty) when the file is not a
/// program: it has no `main` (an imported module) or does not get through
/// the frontend and Sema (an `err_` sample).
bool lowerFile(const std::string &path, pir::Program &program,
               std::string &error) {
  // Each file is its own program, as in a separate driver run: Sema's
  // process-wide module cache must not carry modules over.
  sema::Sema::ModuleCache.clear();
  parser::ParserDriver driver(test::testFrontend());
  std::ostringstream diagOS;
  sema::DiagEngine diag(diagOS);
  diag.setSourceInfo(path, &driver.getSourceLines());
  driver.setDiagEngine(&diag);
  if (driver.parseFile(path) != 0)
    return false;
  bool hasMain = false;
  for (auto *fn : driver.getRoot()->getFuncDecls())
    hasMain = hasMain || fn->getName() == "main";
  if (!hasMain)
    return false;
  std::string root = std::filesystem::path(path).parent_path().string();
  sema::Sema sema(driver.getASTContext(), diag, root, driver.getFrontendName());
  auto ctx = sema.run(driver.getRoot());
  if (!ctx)
    return false;
  std::ostringstream errs;
  if (!lowering::lowerProgram(ctx, driver.getRoot(), path, root, program,
                              errs)) {
    error = "lowering failed: " + errs.str();
    return true;
  }
  return true;
}

/// Round-trip one lowered program; returns "" on success, else the failure.
std::string roundTrip(const pir::Program &program) {
  auto errors = pir::verify(program);
  if (!errors.empty())
    return "in-memory module: " + pir::formatErrors(errors);
  std::string text = pir::toString(program);
  pir::ParseError perr;
  auto parsed = pir::parseProgram(text, perr);
  if (!parsed)
    return "parse error " + perr.str();
  errors = pir::verify(*parsed);
  if (!errors.empty())
    return "parsed module: " + pir::formatErrors(errors);
  if (pir::toString(*parsed) != text)
    return "re-printed text differs:\n" + text + "\n---\n" +
           pir::toString(*parsed);
  return "";
}

} // namespace

TEST(PIRRoundTrip, EveryCorpusProgramPrintsParsesAndReprintsIdentically) {
  size_t checked = 0;
  for (const std::string &path : corpus()) {
    pir::Program program;
    std::string error;
    if (!lowerFile(path, program, error))
      continue;
    ++checked;
    if (error.empty())
      error = roundTrip(program);
    EXPECT_TRUE(error.empty()) << path << ": " << error;
  }
  // The corpus has well over a hundred valid programs; a much smaller count
  // means the corpus paths or the frontend setup are broken.
  EXPECT_GT(checked, 100u);
}
