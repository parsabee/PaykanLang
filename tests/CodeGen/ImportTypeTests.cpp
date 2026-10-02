// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen tests: imported signatures that name other types of the imported
// module (classes, enums, arrays of classes) and transitively-reached classes.

#include "CodeGenTestUtils.h"
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

// A method of an imported class whose return type is ANOTHER class of the
// same module: the call site must see the real return type (it used to come
// back as `void` whenever the classes were rebuilt in the wrong order).
TEST(Module, MethodReturnsModuleLocalClass) {
  auto tmpDir = paykan::test::tempDir() / "pkn_cg_method_ret";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);

  writeTempFile(tmpDir.string(), "lib.pkn", R"(
class A { v: int; fn __init__() { self.v = 7; } }
class B { fn __init__() {} fn make() -> A { return A(); } }
fn mk() -> A { return A(); }
)");
  auto mainPath = writeTempFile(tmpDir.string(), "main.pkn", R"(
import lib;
fn main() -> int { b: lib::B = lib::B(); a: lib::A = b.make(); return a.v; }
)");

  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 7);

  std::filesystem::remove_all(tmpDir);
}

// Method signatures mentioning enums and arrays of a module-local class, plus
// a class-typed field, all resolved through one import.  (The array is
// consumed by a free function: user-defined METHODS with array-typed
// parameters crash at runtime even in a single file — a separate, pre-existing
// CodeGen ABI bug, not an import issue.)
TEST(Module, MethodSignaturesWithEnumAndArrayOfLocalClass) {
  auto tmpDir = paykan::test::tempDir() / "pkn_cg_method_sig";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);

  writeTempFile(tmpDir.string(), "lib.pkn", R"(
enum Kind { Small, Big }
class Item { v: int; fn __init__(v: int) { self.v = v; } }
class Bag {
  kind: Kind;
  first: Item;
  fn __init__() { self.kind = Kind::Big; self.first = Item(10); }
  fn items() -> Item[] { return [Item(1), Item(2), Item(3)]; }
  fn kindOf() -> Kind { return self.kind; }
}
fn total(xs: Item[]) -> int {
  s: int = 0; i: int = 0;
  while (i < xs.len()) { s = s + xs[i].v; i = i + 1; }
  return s;
}
)");
  auto mainPath = writeTempFile(tmpDir.string(), "main.pkn", R"(
import lib;
fn main() -> int {
  b: lib::Bag = lib::Bag();
  xs: lib::Item[] = b.items();
  k: lib::Kind = b.kindOf();
  bonus: int = 0;
  if (k == lib::Kind::Big) { bonus = 100; }
  return lib::total(xs) + b.first.v + bonus;   // 6 + 10 + 100
}
)");

  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 116);

  std::filesystem::remove_all(tmpDir);
}

// Transitive: main imports only `mid`; mid's class returns a value of leaf's
// class.  The value flows through (and its methods dispatch correctly) even
// though main never imports `leaf`.
TEST(Module, TransitiveClassFlowsThroughSignature) {
  auto tmpDir = paykan::test::tempDir() / "pkn_cg_trans_class";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);

  writeTempFile(tmpDir.string(), "leaf.pkn", R"(
class Thing { v: int; fn __init__(v: int) { self.v = v; } fn twice() -> int { return self.v + self.v; } }
)");
  writeTempFile(tmpDir.string(), "mid.pkn", R"(
import leaf;
class Wrap {
  t: leaf::Thing;
  fn __init__() { self.t = leaf::Thing(21); }
  fn get() -> leaf::Thing { return self.t; }
}
)");
  auto mainPath = writeTempFile(tmpDir.string(), "main.pkn", R"(
import mid;
fn main() -> int { w: mid::Wrap = mid::Wrap(); t = w.get(); return t.twice(); }
)");

  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 42);

  std::filesystem::remove_all(tmpDir);
}
