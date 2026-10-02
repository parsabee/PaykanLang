// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen tests: import / module loading across file boundaries.

#include "CodeGenTestUtils.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace paykan::test;

// Helper: write a file into a temporary multi-file project directory.
static std::string writeTempFile(const std::string &dir,
                                 const std::string &relPath,
                                 const std::string &content) {
  auto full = std::filesystem::path(dir) / relPath;
  std::filesystem::create_directories(full.parent_path());
  std::ofstream ofs(full);
  ofs << content;
  return full.string();
}

// Helper: read a whole file (binary) into a string.
static std::string slurp(const std::filesystem::path &p) {
  std::ifstream ifs(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(ifs)),
                     std::istreambuf_iterator<char>());
}

// Helper: compile and run @p mainPath in a forked child process, optionally
// from a different working directory, and return the program's exit code (-1
// if compilation failed).  Each run must start from a pristine in-process
// state: Sema keeps a process-global cache of analysed modules keyed by file
// path, so a second in-process compile of the same project would reuse the
// first analysis and never exercise the on-disk bitcode cache under test.
static int runProjectInChild(const std::string &mainPath,
                             const std::string &cwd = "") {
  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid == 0) {
    if (!cwd.empty() && chdir(cwd.c_str()) != 0)
      _exit(254);
    auto r = compileAndRunFile(mainPath);
    if (!r.CompileOk) {
      fprintf(stderr, "child compile failed: %s\n", r.StdErr.c_str());
      _exit(255);
    }
    _exit(r.ExitCode & 0xff);
  }
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
    return -1;
  int code = WEXITSTATUS(status);
  return code >= 254 ? -1 : code;
}

// ============================================================================
// Imports
// ============================================================================

TEST(Module, ImportBasic) {
  auto tmpDir = paykan::test::tempDir() / "pkn_import_test";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);

  writeTempFile(tmpDir.string(), "helper.pkn", R"(
fn add(a: int, b: int) -> int { return a + b; }
)");

  auto mainPath = writeTempFile(tmpDir.string(), "main.pkn", R"(
import helper;
fn main() -> int { return helper::add(10, 22); }
)");

  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 32);

  std::filesystem::remove_all(tmpDir);
}

TEST(Module, ImportNested) {
  auto tmpDir = paykan::test::tempDir() / "pkn_import_nested";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);

  writeTempFile(tmpDir.string(), "math/arith.pkn", R"(
fn mul(a: int, b: int) -> int { return a * b; }
)");

  auto mainPath = writeTempFile(tmpDir.string(), "main.pkn", R"(
import math::arith;
fn main() -> int { return arith::mul(6, 7); }
)");

  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 42);

  std::filesystem::remove_all(tmpDir);
}

TEST(Module, ImportTransitive) {
  auto tmpDir = paykan::test::tempDir() / "pkn_import_trans";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);

  writeTempFile(tmpDir.string(), "base.pkn", R"(
fn double(x: int) -> int { return x + x; }
)");

  writeTempFile(tmpDir.string(), "mid.pkn", R"(
import base;
fn quadruple(x: int) -> int { return base::double(base::double(x)); }
)");

  auto mainPath = writeTempFile(tmpDir.string(), "main.pkn", R"(
import mid;
fn main() -> int { return mid::quadruple(5); }
)");

  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 20);

  std::filesystem::remove_all(tmpDir);
}

// A module function whose param type is a ClassType (Str) must be callable
// across module boundaries with correct type remapping end-to-end.
TEST(Module, ImportClassTypeRemap) {
  auto tmpDir = paykan::test::tempDir() / "pkn_cg_classremap";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);

  writeTempFile(tmpDir.string(), "strmod.pkn", R"(
fn check(s: Str) -> int { return 7; }
)");

  auto mainPath = writeTempFile(tmpDir.string(), "main.pkn", R"(
import strmod;
fn main() -> int {
  s: Str = "hello";
  return strmod::check(s);
}
)");

  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 7);

  std::filesystem::remove_all(tmpDir);
}

// An imported class used as a qualified type `mod::Type` in a function
// parameter must compile and run end-to-end.
TEST(Module, QualifiedTypeParamRuns) {
  auto tmpDir = paykan::test::tempDir() / "pkn_cg_qt_param";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);

  writeTempFile(tmpDir.string(), "geometry/point.pkn", R"(
class Point {
  x: int; y: int;
  fn __init__(x: int, y: int) { self.x = x; self.y = y; }
  fn sum() -> int { return self.x + self.y; }
}
)");

  auto mainPath = writeTempFile(tmpDir.string(), "main.pkn", R"(
import geometry::point;
fn total(p: point::Point) -> int { return p.sum(); }
fn main() -> int { p = point::Point(15, 27); return total(p); }
)");

  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 42);

  std::filesystem::remove_all(tmpDir);
}

// An imported class used as a qualified type `mod::Type` for a class field
// must compile and run end-to-end.
TEST(Module, QualifiedTypeFieldRuns) {
  auto tmpDir = paykan::test::tempDir() / "pkn_cg_qt_field";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);

  writeTempFile(tmpDir.string(), "geometry/point.pkn", R"(
class Point {
  x: int; y: int;
  fn __init__(x: int, y: int) { self.x = x; self.y = y; }
  fn sum() -> int { return self.x + self.y; }
}
)");

  auto mainPath = writeTempFile(tmpDir.string(), "main.pkn", R"(
import geometry::point;
class Box {
  origin: point::Point;
  fn __init__() { self.origin = point::Point(20, 22); }
  fn total() -> int { return self.origin.sum(); }
}
fn main() -> int { b = Box(); return b.total(); }
)");

  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 42);

  std::filesystem::remove_all(tmpDir);
}

// End-to-end: an imported enum used as a qualified type, a variant, an argument
// to an imported function, and an imported function's return value.
TEST(Module, ImportEnum) {
  auto tmpDir = paykan::test::tempDir() / "pkn_import_enum";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);

  writeTempFile(tmpDir.string(), "pal/color.pkn", R"(
enum Color { Red, Green, Blue }
fn name(c: Color) -> Str {
  match c { Red { return "red"; } Green { return "green"; } Blue { return "blue"; } }
}
fn favorite() -> Color { return Color::Blue; }
)");

  auto mainPath = writeTempFile(tmpDir.string(), "main.pkn", R"(
import pal::color;
fn main() -> int {
  c: color::Color = color::Color::Green;
  println(color::name(c));            // green
  f: color::Color = color::favorite();
  println(color::name(f));            // blue
  if (f == color::Color::Blue) { return 0; }
  return 1;
}
)");

  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "green\nblue\n");

  std::filesystem::remove_all(tmpDir);
}

// Diamond import: main imports both `widget` and `color`, and `widget` itself
// imports `color`.  The shared `color` module must be code-generated exactly
// once, or linking fails with "symbol multiply defined".
TEST(Module, ImportDiamond) {
  auto tmpDir = paykan::test::tempDir() / "pkn_import_diamond";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);

  writeTempFile(tmpDir.string(), "pal/leaf.pkn", R"(
fn base() -> int { return 7; }
)");
  writeTempFile(tmpDir.string(), "pal/mid.pkn", R"(
import pal::leaf;
fn bumped() -> int { return leaf::base() + 1; }
)");
  auto mainPath = writeTempFile(tmpDir.string(), "main.pkn", R"(
import pal::mid;
import pal::leaf;
fn main() -> int { return mid::bumped() + leaf::base(); }
)");

  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 15); // 8 + 7

  std::filesystem::remove_all(tmpDir);
}

// ============================================================================
// Bitcode cache
// ============================================================================

// main -> mid -> base.  Editing base's class layout must invalidate mid's
// cached bitcode too: mid's code addresses Point's fields by slot index, so a
// stale mid.bc would read the wrong slots (and, without dependency tracking,
// nothing in mid's own source or mtime changes).
TEST(Module, CacheInvalidatesOnTransitiveLayoutChange) {
  if (testBackend() != "llvm")
    GTEST_SKIP() << "the bitcode import cache belongs to the LLVM backend";
  auto tmpDir = paykan::test::tempDir() / "pkn_cache_trans";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);

  writeTempFile(tmpDir.string(), "base.pkn", R"(
class Point {
  x: int; y: int;
  fn __init__(x: int, y: int) { self.x = x; self.y = y; }
  fn sum() -> int { return self.x + self.y; }
}
fn make(a: int, b: int) -> Point { return Point(a, b); }
)");
  writeTempFile(tmpDir.string(), "mid.pkn", R"(
import base;
fn total() -> int { p: base::Point = base::make(15, 27); return p.x + p.y + p.sum(); }
)");
  auto mainPath = writeTempFile(tmpDir.string(), "main.pkn", R"(
import mid;
fn main() -> int { return mid::total(); }
)");

  auto midBc = tmpDir / ".paykan_cache" / "mid.bc";
  auto baseBc = tmpDir / ".paykan_cache" / "base.bc";

  // Cold run populates the cache for both imported modules (never for main).
  EXPECT_EQ(runProjectInChild(mainPath), 84);
  ASSERT_TRUE(std::filesystem::exists(midBc));
  ASSERT_TRUE(std::filesystem::exists(baseBc));
  EXPECT_FALSE(std::filesystem::exists(tmpDir / ".paykan_cache" / "main.bc"));
  auto midBefore = slurp(midBc);

  // Warm run: everything served from the cache, same result.
  EXPECT_EQ(runProjectInChild(mainPath), 84);
  EXPECT_EQ(slurp(midBc), midBefore);

  // Insert a field before x/y in the transitive dependency only.
  writeTempFile(tmpDir.string(), "base.pkn", R"(
class Point {
  tag: int; x: int; y: int;
  fn __init__(x: int, y: int) { self.tag = 100; self.x = x; self.y = y; }
  fn sum() -> int { return self.x + self.y; }
}
fn make(a: int, b: int) -> Point { return Point(a, b); }
)");

  EXPECT_EQ(runProjectInChild(mainPath), 84);
  EXPECT_NE(slurp(midBc), midBefore) << "mid.bc was not regenerated";

  // And the regenerated entries are reused as-is.
  auto midAfter = slurp(midBc);
  EXPECT_EQ(runProjectInChild(mainPath), 84);
  EXPECT_EQ(slurp(midBc), midAfter);

  std::filesystem::remove_all(tmpDir);
}

// The cache lives under the project root (the main file's directory), not
// under whatever directory the compiler happens to be launched from.
TEST(Module, CacheLivesUnderProjectRoot) {
  if (testBackend() != "llvm")
    GTEST_SKIP() << "the bitcode import cache belongs to the LLVM backend";
  auto tmpDir = paykan::test::tempDir() / "pkn_cache_root";
  auto cwdA = paykan::test::tempDir() / "pkn_cache_cwd_a";
  auto cwdB = paykan::test::tempDir() / "pkn_cache_cwd_b";
  for (auto &d : {tmpDir, cwdA, cwdB}) {
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
  }

  writeTempFile(tmpDir.string(), "helper.pkn", R"(
fn add(a: int, b: int) -> int { return a + b; }
)");
  auto mainPath = writeTempFile(tmpDir.string(), "main.pkn", R"(
import helper;
fn main() -> int { return helper::add(10, 22); }
)");

  auto helperBc = tmpDir / ".paykan_cache" / "helper.bc";

  EXPECT_EQ(runProjectInChild(mainPath, cwdA.string()), 32);
  ASSERT_TRUE(std::filesystem::exists(helperBc));
  EXPECT_FALSE(std::filesystem::exists(cwdA / ".paykan_cache"));
  auto stamp = std::filesystem::last_write_time(helperBc);

  // A run from another directory finds the same entry and leaves it alone.
  EXPECT_EQ(runProjectInChild(mainPath, cwdB.string()), 32);
  EXPECT_FALSE(std::filesystem::exists(cwdB / ".paykan_cache"));
  EXPECT_EQ(std::filesystem::last_write_time(helperBc), stamp)
      << "helper.bc was rewritten on a warm run";

  for (auto &d : {tmpDir, cwdA, cwdB})
    std::filesystem::remove_all(d);
}

// A damaged cache entry — truncated, garbage, or empty — must be ignored and
// regenerated, never crash the compiler or produce a wrong program.  The
// garbage case damages only the LEAF module while its importer's entry stays
// valid, so the leaf has to be regenerated from underneath a cache hit.
TEST(Module, CorruptCacheEntryIsRegenerated) {
  if (testBackend() != "llvm")
    GTEST_SKIP() << "the bitcode import cache belongs to the LLVM backend";
  auto tmpDir = paykan::test::tempDir() / "pkn_cache_corrupt";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);

  writeTempFile(tmpDir.string(), "base.pkn", R"(
fn double(x: int) -> int { return x + x; }
)");
  writeTempFile(tmpDir.string(), "mid.pkn", R"(
import base;
fn quadruple(x: int) -> int { return base::double(base::double(x)); }
)");
  auto mainPath = writeTempFile(tmpDir.string(), "main.pkn", R"(
import mid;
fn main() -> int { return mid::quadruple(5); }
)");

  auto midBc = tmpDir / ".paykan_cache" / "mid.bc";
  auto baseBc = tmpDir / ".paykan_cache" / "base.bc";

  EXPECT_EQ(runProjectInChild(mainPath), 20);
  ASSERT_TRUE(std::filesystem::exists(midBc));
  ASSERT_TRUE(std::filesystem::exists(baseBc));
  ASSERT_GT(std::filesystem::file_size(midBc), 16u);

  // Runs the project against a damaged @p entry: the program must still be
  // right, the entry must have been replaced, and the replacement must be a
  // valid entry (a further run is a cache hit that leaves it untouched).
  auto checkRepaired = [&](const std::filesystem::path &entry) {
    auto damaged = slurp(entry);
    EXPECT_EQ(runProjectInChild(mainPath), 20);
    EXPECT_NE(slurp(entry), damaged) << entry << " was not regenerated";
    auto stamp = std::filesystem::last_write_time(entry);
    EXPECT_EQ(runProjectInChild(mainPath), 20);
    EXPECT_EQ(std::filesystem::last_write_time(entry), stamp)
        << entry << " was regenerated again although it was just rewritten";
  };

  // Truncated importer entry.
  std::filesystem::resize_file(midBc, std::filesystem::file_size(midBc) / 2);
  checkRepaired(midBc);

  // Garbage leaf entry underneath a valid importer entry.
  {
    std::ofstream ofs(baseBc, std::ios::binary | std::ios::trunc);
    ofs << "this is not bitcode";
  }
  checkRepaired(baseBc);

  // Empty entry.
  std::filesystem::resize_file(midBc, 0);
  checkRepaired(midBc);

  std::filesystem::remove_all(tmpDir);
}

// Diamond import where the shared module is reached under DIFFERENT qualifiers:
// `mid` imports it as `leaf`, `main` imports it aliased as `lf`.  Both
// qualifier aliases must resolve against the single generated module.
TEST(Module, ImportDiamondDifferentQualifier) {
  auto tmpDir = paykan::test::tempDir() / "pkn_import_diamond_q";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);

  writeTempFile(tmpDir.string(), "pal/leaf.pkn", R"(
fn base() -> int { return 7; }
)");
  writeTempFile(tmpDir.string(), "pal/mid.pkn", R"(
import pal::leaf;
fn bumped() -> int { return leaf::base() + 1; }
)");
  auto mainPath = writeTempFile(tmpDir.string(), "main.pkn", R"(
import pal::mid;
import pal::leaf as lf;
fn main() -> int { return mid::bumped() + lf::base(); }
)");

  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 15); // 8 + 7

  std::filesystem::remove_all(tmpDir);
}
