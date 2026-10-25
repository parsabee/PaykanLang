// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// PIR round trips over the whole corpus: every program under samples/ that
// passes Sema is lowered, verified and printed; the text is parsed back,
// verified again and re-printed, and the two texts must be identical
// (docs/pir.md: the printed form is a faithful serialization).  The binary
// form (docs/design/pkm.md §5) must hold the same two invariants, and its
// symbol index must slice every function out of the blob.  And the code
// passes (docs/pir.md §12), the moves at last use and the reference-count
// elimination, on every program, the standard library's tests' too: what
// each makes verifies and round-trips, and counts no more references than
// before.

#include "Lowering/Holds.h"
#include "ModuleName.h"
#include "TestUtils.h"
#include "paykan/lowering/Lowering.h"
#include "paykan/pir/Binary.h"
#include "paykan/pir/MoveAtLastUse.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Pass.h"
#include "paykan/pir/Printer.h"
#include "paykan/pir/RCElimination.h"
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
#ifndef PAYKAN_STDLIB_TESTS_DIR
#error "PAYKAN_STDLIB_TESTS_DIR must be defined via CMake compile definition"
#endif

using namespace paykan;

namespace {

/// The `.pkn` files under @p dir, sorted.
std::vector<std::string> programsIn(const std::string &dir) {
  std::vector<std::string> files;
  for (const auto &e : std::filesystem::recursive_directory_iterator(dir))
    if (e.is_regular_file() && e.path().extension() == ".pkn")
      files.push_back(e.path().string());
  std::sort(files.begin(), files.end());
  return files;
}

std::vector<std::string> corpus() { return programsIn(PAYKAN_SAMPLES_DIR); }

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
  if (!lowering::lowerProgram(ctx, driver.getRoot(),
                              module_name::mainModuleName(path), root, program,
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

/// The `retain` and `release` instructions of a program's bodies.
struct ReferenceCounting {
  size_t Retains = 0;
  size_t Releases = 0;

  void count(const pir::Block &b) {
    for (const pir::Stmt &s : b.Stmts) {
      if (const auto *in = std::get_if<pir::Instr>(&s)) {
        Retains += in->Op == pir::Opcode::Retain;
        Releases += in->Op == pir::Opcode::Release;
      } else if (const auto *x = std::get_if<pir::If>(&s)) {
        count(*x->Then);
        if (x->Else)
          count(*x->Else);
      } else if (const auto *x = std::get_if<pir::While>(&s)) {
        count(*x->CondBlock);
        count(*x->Body);
      } else if (const auto *x = std::get_if<pir::Switch>(&s)) {
        for (const pir::Switch::Case &c : x->Cases)
          count(*c.Body);
        if (x->Default)
          count(*x->Default);
      }
    }
  }
  explicit ReferenceCounting(const pir::Program &p) {
    for (const pir::Module &m : p.Modules)
      for (const pir::Function &f : m.Functions)
        count(f.Body);
  }
};

/// @p program through the moves-at-last-use pass, verified after it; what
/// it did is added to @p stats.
std::string moved(pir::Program &program, pir::Statistics &stats) {
  pir::PassManager passes(/*verifyEach=*/true);
  passes.add(pir::createMovesAtLastUsePass());
  paykan::Status s = passes.run(program, stats);
  return s ? "" : s.message();
}

/// @p program through the reference-count elimination pass, verified after
/// it; what it did is added to @p stats.
std::string eliminated(pir::Program &program, pir::Statistics &stats) {
  pir::PassManager passes(/*verifyEach=*/true);
  passes.add(pir::createRCEliminationPass());
  paykan::Status s = passes.run(program, stats);
  return s ? "" : s.message();
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

TEST(PIRMoves, EveryCorpusProgramCountsNoMoreReferencesAndFewerWhereOneMoves) {
  size_t checked = 0, fewer = 0;
  size_t retains = 0, retainsAfter = 0, releases = 0, releasesAfter = 0;
  uint64_t transfers = 0;
  // The samples, and the standard library's tests, whose programs hold
  // the copies of the library's generic code they instantiate.
  std::vector<std::string> programs = corpus();
  for (const std::string &path : programsIn(PAYKAN_STDLIB_TESTS_DIR))
    programs.push_back(path);
  for (const std::string &path : programs) {
    pir::Program program;
    std::string error;
    if (!lowerFile(path, program, error))
      continue;
    ++checked;
    ASSERT_TRUE(error.empty()) << path << ": " << error;
    const ReferenceCounting before(program);
    pir::Statistics stats;
    error = moved(program, stats);
    ASSERT_TRUE(error.empty()) << path << ": " << error;
    const ReferenceCounting after(program);
    const uint64_t moves = stats.get("moves-at-last-use", "transfers");
    // A move removes the copy's retains; what else goes is the releases of
    // slots the moves left null.  Nothing is ever added.
    EXPECT_LE(after.Retains, before.Retains) << path;
    EXPECT_LE(after.Releases, before.Releases) << path;
    EXPECT_EQ(before.Retains - after.Retains,
              stats.get("moves-at-last-use", "retains removed"))
        << path;
    EXPECT_EQ(before.Releases - after.Releases,
              stats.get("moves-at-last-use", "releases removed"))
        << path;
    if (moves > 0) {
      EXPECT_LT(after.Retains, before.Retains) << path;
      ++fewer;
    }
    // The samples written to hand variables on at their last use: fewer
    // of both, whatever the pass counted.
    if (path.find("_moves_") != std::string::npos) {
      EXPECT_LT(after.Retains, before.Retains) << path;
      EXPECT_LT(after.Releases, before.Releases) << path;
    }
    EXPECT_EQ(roundTrip(program), "") << path;
    // On its own output (a module's code is moved before its `.pkm` is
    // written, and again in the program that imports it), it changes
    // nothing more.
    const std::string once = pir::toString(program);
    pir::Statistics again;
    error = moved(program, again);
    EXPECT_TRUE(error.empty()) << path << ": " << error;
    EXPECT_EQ(pir::toString(program), once) << path;
    retains += before.Retains;
    retainsAfter += after.Retains;
    releases += before.Releases;
    releasesAfter += after.Releases;
    transfers += moves;
  }
  EXPECT_GT(checked, 100u);
  // Many programs hand some variable on at its last use (the others hold
  // no references, or keep reading them).
  EXPECT_GT(fewer, checked / 3) << fewer << " of " << checked;
  std::cout << "[ moves ] " << checked << " programs, " << fewer
            << " with moves: " << transfers << " transfers; retains " << retains
            << " -> " << retainsAfter << ", releases " << releases << " -> "
            << releasesAfter << "\n";
}

TEST(PIRRCElimination, EveryCorpusProgramCountsNoMoreReferencesAfterThePass) {
  size_t checked = 0, fewer = 0, held = 0;
  size_t retains = 0, retainsAfter = 0, releases = 0, releasesAfter = 0;
  std::vector<std::string> programs = corpus();
  for (const std::string &path : programsIn(PAYKAN_STDLIB_TESTS_DIR))
    programs.push_back(path);
  for (const std::string &path : programs) {
    pir::Program program;
    std::string error;
    if (!lowerFile(path, program, error))
      continue;
    ++checked;
    ASSERT_TRUE(error.empty()) << path << ": " << error;
    // After the moves, as the driver runs it.
    pir::Statistics stats;
    error = moved(program, stats);
    ASSERT_TRUE(error.empty()) << path << ": " << error;
    const ReferenceCounting before(program);
    const test::Holds holds(program);
    error = eliminated(program, stats);
    ASSERT_TRUE(error.empty()) << path << ": " << error;
    const ReferenceCounting after(program);
    // Nothing is added; a retain goes with a release on each path out of
    // its range, so at least as many releases go as retains.
    ASSERT_LE(after.Retains, before.Retains) << path;
    ASSERT_LE(after.Releases, before.Releases) << path;
    const uint64_t gone = stats.get("rc-elimination", "retains removed");
    EXPECT_EQ(before.Retains - after.Retains, gone) << path;
    EXPECT_EQ(before.Releases - after.Releases,
              stats.get("rc-elimination", "releases removed"))
        << path;
    EXPECT_LE(before.Retains - after.Retains, before.Releases - after.Releases)
        << path;
    if (gone > 0)
      ++fewer;
    // Every hold keeps a retain: a read in use across a call that may let
    // its field or element go (the field-load hazard).
    const test::Holds left(program);
    for (const test::Holds::Read &r : holds.Held) {
      ++held;
      EXPECT_TRUE(left.retained(r))
          << path << ": @" << r.Function << " %" << r.Value;
    }
    EXPECT_EQ(roundTrip(program), "") << path;
    // The code passes again (a module's code goes through them before its
    // `.pkm` is written, and again in the program that imports it) change
    // nothing more.
    const std::string once = pir::toString(program);
    pir::Statistics again;
    error = moved(program, again);
    EXPECT_TRUE(error.empty()) << path << ": " << error;
    error = eliminated(program, again);
    EXPECT_TRUE(error.empty()) << path << ": " << error;
    EXPECT_EQ(pir::toString(program), once) << path;
    retains += before.Retains;
    retainsAfter += after.Retains;
    releases += before.Releases;
    releasesAfter += after.Releases;
  }
  EXPECT_GT(checked, 100u);
  // A match on a field whose arms call nothing, a binding inside its
  // subject's range, a read of a variable nothing reassigns: in a fair
  // share of the programs, and of the standard library's code they copy.
  EXPECT_GT(fewer, checked / 10) << fewer << " of " << checked;
  EXPECT_GT(held, 100u);
  std::cout << "[ rc-elimination ] " << checked << " programs, " << fewer
            << " with pairs removed, " << held << " holds kept: retains "
            << retains << " -> " << retainsAfter << ", releases " << releases
            << " -> " << releasesAfter << "\n";
}
