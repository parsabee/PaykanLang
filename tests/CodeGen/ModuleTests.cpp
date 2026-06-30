// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen tests: import / module loading across file boundaries.

#include "TestUtils.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

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

// ============================================================================
// Imports
// ============================================================================

TEST(Module, ImportBasic) {
  auto tmpDir = std::filesystem::temp_directory_path() / "pkn_import_test";
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
  auto tmpDir = std::filesystem::temp_directory_path() / "pkn_import_nested";
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
  auto tmpDir = std::filesystem::temp_directory_path() / "pkn_import_trans";
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
  auto tmpDir = std::filesystem::temp_directory_path() / "pkn_cg_classremap";
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
  auto tmpDir = std::filesystem::temp_directory_path() / "pkn_cg_qt_param";
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
  auto tmpDir = std::filesystem::temp_directory_path() / "pkn_cg_qt_field";
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
  auto tmpDir = std::filesystem::temp_directory_path() / "pkn_import_enum";
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
  auto tmpDir = std::filesystem::temp_directory_path() / "pkn_import_diamond";
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

// Diamond import where the shared module is reached under DIFFERENT qualifiers:
// `mid` imports it as `leaf`, `main` imports it aliased as `lf`.  Both
// qualifier aliases must resolve against the single generated module.
TEST(Module, ImportDiamondDifferentQualifier) {
  auto tmpDir = std::filesystem::temp_directory_path() / "pkn_import_diamond_q";
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
