// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// PIR round trips over the whole corpus: every program under samples/ that
// passes Sema is lowered, verified and printed; the text is parsed back,
// verified again and re-printed, and the two texts must be identical
// (docs/pir.md: the printed form is a faithful serialization).  The binary
// form (docs/design/pkm.md §5) must hold the same two invariants, and its
// symbol index must slice every function out of the blob.

#include "TestUtils.h"
#include "paykan/lowering/Lowering.h"
#include "paykan/pir/Binary.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Printer.h"
#include "paykan/pir/Verifier.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
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

/// A module holding only @p f, for printing it.
pir::Module oneFunction(pir::Function f) {
  pir::Module m;
  m.Name = "f";
  m.Functions.push_back(std::move(f));
  return m;
}

/// Binary round trip of every module of a verified program (§5.8: the
/// printed text and the re-encoded bytes are identical, the decoded module
/// verifies) and a per-function decode through the index.  Adds the text and
/// binary sizes to @p text / @p bin; returns "" on success.
std::string binaryRoundTrip(const pir::Program &program, size_t &text,
                            size_t &bin) {
  for (const pir::Module &m : program.Modules) {
    std::string printed = pir::toString(m);
    std::vector<uint8_t> b = pir::binary::encode(m);
    text += printed.size();
    bin += b.size();
    auto decoded = pir::binary::decode(b);
    if (!decoded)
      return m.Name + ": " + decoded.status().message();
    auto errors = pir::verify(*decoded);
    if (!errors.empty())
      return m.Name + ": decoded module: " + pir::formatErrors(errors);
    if (pir::toString(*decoded) != printed)
      return m.Name + ": decoded module prints differently";
    if (pir::binary::encode(*decoded) != b)
      return m.Name + ": re-encoded bytes differ";
    auto info = pir::binary::inspect(b);
    if (!info)
      return m.Name + ": inspect: " + info.status().message();
    std::vector<pir::binary::SymbolEntry> index = pir::binary::index(b);
    pir::Module &dm = decoded.value();
    size_t functions = 0;
    for (const pir::binary::SymbolEntry &e : index) {
      if (e.Kind > 1) // functions and extern functions only
        continue;
      ++functions;
      auto f = pir::binary::decodeFunction(b, *info, e.Offset, e.Length);
      if (!f)
        return m.Name + ": @" + e.Name + ": " + f.status().message();
      auto whole = std::find_if(
          dm.Functions.begin(), dm.Functions.end(),
          [&](const pir::Function &g) { return g.Name == e.Name; });
      if (whole == dm.Functions.end())
        return m.Name + ": @" + e.Name + " is not in the decoded module";
      if (pir::toString(oneFunction(std::move(*f))) !=
          pir::toString(oneFunction(std::move(*whole))))
        return m.Name + ": @" + e.Name + " decodes differently alone";
    }
    if (functions != m.Functions.size())
      return m.Name + ": the index names " + std::to_string(functions) +
             " functions, the module has " + std::to_string(m.Functions.size());
  }
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

TEST(PIRBinary, Corpus) {
  size_t checked = 0, text = 0, bin = 0;
  for (const std::string &path : corpus()) {
    pir::Program program;
    std::string error;
    if (!lowerFile(path, program, error))
      continue;
    ++checked;
    if (error.empty() && pir::verify(program).empty())
      error = binaryRoundTrip(program, text, bin);
    EXPECT_TRUE(error.empty()) << path << ": " << error;
  }
  EXPECT_GT(checked, 100u);
  ASSERT_GT(bin, 0u);
  std::cout << "[ binary PIR ] " << checked << " programs: " << text
            << " bytes of text, " << bin << " bytes binary (" << std::fixed
            << std::setprecision(2) << double(text) / double(bin)
            << "x smaller)\n";
  // The design's measured ratio is ~3.3x; a drop below 2x means the codec
  // wastes bytes.
  EXPECT_GT(double(text) / double(bin), 2.0);
}
