// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: types named by an imported module's exported signatures —
// module-local classes/enums/arrays in method and function signatures,
// transitive imports, and the diagnostics for unresolvable exported types.

#include "TestUtils.h"
#include <gtest/gtest.h>
#include <sstream>

using namespace paykan::test;

// ─── helpers ────────────────────────────────────────────────────────────────

static std::string writeFile(const std::string &dir, const std::string &relPath,
                             const std::string &content) {
  auto full = std::filesystem::path(dir) / relPath;
  std::filesystem::create_directories(full.parent_path());
  std::ofstream ofs(full);
  ofs << content;
  return full.string();
}

struct SemaFileResult {
  bool Ok;
  std::string Diagnostics;
};

static SemaFileResult semaCheckFile(const std::string &filePath,
                                    const std::string &projectRoot) {
  paykan::parser::ParserDriver drv(paykan::test::testFrontend());
  if (drv.parseFile(filePath) != 0)
    return {false, "parse error"};
  std::ostringstream os;
  paykan::sema::DiagEngine diagEngine(os);
  diagEngine.setSourceInfo(drv.getCurrentFile(), &drv.getSourceLines());
  paykan::sema::Sema sema(drv.getASTContext(), diagEngine, projectRoot,
                          drv.getFrontendName());
  bool ok = sema.run(drv.getRoot()).Ok;
  return {ok, os.str()};
}

// ─── OK: exported signatures that name other types of the SAME module ────────
//
// Exported classes are reconstructed from an unordered registry, so a method
// whose return/parameter type is another class of the module used to resolve
// only if that class happened to be rebuilt first (otherwise it silently
// became void/Obj).  Every type position must resolve regardless of order.

// Several classes referencing each other in every direction so that no
// single reconstruction order satisfies all of them by luck.
static const char *const kShapesModule =
    "enum Kind { Round, Square }\n"
    "class Zeta { v: int; fn __init__() { self.v = 7; } }\n"
    "class Alpha {\n"
    "  kind: Kind;\n"
    "  fn __init__() { self.kind = Kind::Round; }\n"
    "  fn make() -> Zeta { return Zeta(); }\n"
    "  fn take(z: Zeta) -> int { return z.v; }\n"
    "  fn kindOf() -> Kind { return self.kind; }\n"
    "  fn many() -> Zeta[] { return [Zeta(), Zeta()]; }\n"
    "  fn count(zs: Zeta[]) -> int { return zs.len(); }\n"
    "}\n"
    "class Mid {\n"
    "  a: Alpha;\n"
    "  fn __init__() { self.a = Alpha(); }\n"
    "  fn alpha() -> Alpha { return self.a; }\n"
    "}\n"
    "fn mk() -> Zeta { return Zeta(); }\n";

TEST(Module, MethodReturnsModuleLocalClassOk) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_mret").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "shapes.pkn", kShapesModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import shapes;
fn main() -> int {
  a: shapes::Alpha = shapes::Alpha();
  z: shapes::Zeta = a.make();
  w: shapes::Zeta = shapes::mk();
  return a.take(z) + w.v;
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, MethodParamModuleLocalClassMismatchErr) {
  // The parameter must be typed Zeta (not silently Obj): passing an int is
  // rejected.
  auto tmp = (paykan::test::tempDir() / "pkn_ms_mparam").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "shapes.pkn", kShapesModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import shapes;
fn main() -> int { a = shapes::Alpha(); return a.take(3); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  std::filesystem::remove_all(tmp);
}

TEST(Module, MethodReturnsChainedModuleLocalClassOk) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_mchain").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "shapes.pkn", kShapesModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import shapes;
fn main() -> int {
  m: shapes::Mid = shapes::Mid();
  z: shapes::Zeta = m.alpha().make();
  return z.v;
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, EnumTypedMemberAndMethodOk) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_enum_member").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "shapes.pkn", kShapesModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import shapes;
fn main() -> int {
  a: shapes::Alpha = shapes::Alpha();
  k: shapes::Kind = a.kindOf();
  f: shapes::Kind = a.kind;
  if (k == shapes::Kind::Round && f == shapes::Kind::Round) { return 0; }
  return 1;
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, ArrayOfModuleLocalClassReturnAndParamOk) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_arr_ret").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "shapes.pkn", kShapesModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import shapes;
fn main() -> int {
  a: shapes::Alpha = shapes::Alpha();
  zs: shapes::Zeta[] = a.many();
  first: shapes::Zeta = zs[0];
  return a.count(zs) + first.v;
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// ─── Transitive imports: types flow through signatures, names do not ────────
//
// main imports only `mid`; `mid` imports `leaf`.  A value of leaf's class can
// be obtained from mid's exports and used (its methods are callable), but the
// TYPE cannot be spelled as `mid::Thing` — only `leaf::Thing` after importing
// `leaf` directly, and then it is the same type as the one mid's signatures
// mention.

static const char *const kLeafModule =
    "class Thing { v: int; fn __init__(v: int) { self.v = v; } "
    "fn val() -> int { return self.v; } }\n"
    "enum Mode { Fast, Slow }\n"
    "fn made() -> Thing { return Thing(5); }\n";

static const char *const kMidModule =
    "import leaf;\n"
    "class Wrap {\n"
    "  t: leaf::Thing;\n"
    "  mode: leaf::Mode;\n"
    "  fn __init__() { self.t = leaf::Thing(9); self.mode = leaf::Mode::Fast; "
    "}\n"
    "  fn get() -> leaf::Thing { return self.t; }\n"
    "  fn put(t: leaf::Thing) -> int { self.t = t; return t.val(); }\n"
    "  fn getMode() -> leaf::Mode { return self.mode; }\n"
    "}\n"
    "fn make() -> leaf::Thing { return leaf::made(); }\n";

TEST(Module, TransitiveTypeFlowsThroughSignaturesOk) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_trans_flow").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "leaf.pkn", kLeafModule);
  writeFile(tmp, "mid.pkn", kMidModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import mid;
fn main() -> int {
  w: mid::Wrap = mid::Wrap();
  t = w.get();            // leaf's Thing, reached through mid's signature
  u = mid::make();
  m = w.getMode();
  return t.val() + w.put(u) + u.val();
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, TransitiveTypeNotNameableThroughMiddleErr) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_trans_name").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "leaf.pkn", kLeafModule);
  writeFile(tmp, "mid.pkn", kMidModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import mid;
fn main() -> int {
  w: mid::Wrap = mid::Wrap();
  t: mid::Thing = w.get();   // names are not re-exported
  return t.val();
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("unknown class type 'mid::Thing'"),
            std::string::npos)
      << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, TransitiveFunctionNotReexportedErr) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_trans_fn").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "leaf.pkn", kLeafModule);
  writeFile(tmp, "mid.pkn", kMidModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import mid;
fn main() -> int { t = mid::made(); return t.val(); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  std::filesystem::remove_all(tmp);
}

TEST(Module, TransitiveTypeIdentityWithDirectImportOk) {
  // Importing `leaf` directly (in either order relative to `mid`) yields the
  // SAME Thing/Mode types that mid's signatures mention.
  for (const char *order :
       {"import mid;\nimport leaf;\n", "import leaf;\nimport mid;\n"}) {
    auto tmp = (paykan::test::tempDir() / "pkn_ms_trans_id").string();
    std::filesystem::remove_all(tmp);
    writeFile(tmp, "leaf.pkn", kLeafModule);
    writeFile(tmp, "mid.pkn", kMidModule);
    auto main = writeFile(tmp, "main.pkn", std::string(order) + R"(
fn main() -> int {
  w: mid::Wrap = mid::Wrap();
  t: leaf::Thing = w.get();
  m: leaf::Mode = w.getMode();
  n: int = w.put(leaf::Thing(3));
  if (m == leaf::Mode::Fast) { return t.val() + n; }
  return 0;
}
)");
    auto r = semaCheckFile(main, tmp);
    EXPECT_TRUE(r.Ok) << order << r.Diagnostics;
    std::filesystem::remove_all(tmp);
  }
}

// ─── ERR: two modules exporting different types under one name ──────────────

TEST(Module, SameClassNameFromTwoModulesErr) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_dup_class").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "list.pkn",
            "class Node { v: int; fn __init__() { self.v = 1; } }\n");
  writeFile(tmp, "tree.pkn",
            "class Node { l: int; r: int; "
            "fn __init__() { self.l = 0; self.r = 0; } }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import list;
import tree;
fn main() -> int { n = tree::Node(); return n.l; }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("import of module 'tree': class 'Node' "
                               "conflicts with a type of the same name"),
            std::string::npos)
      << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// ─── ERR: an export naming a type that nothing provides ─────────────────────
//
// Cannot arise from source (the exporting module type-checked, and every type
// its signatures mention is serialised with it), so seed the module cache
// with a hand-built entry whose function returns a type no module declares.
// The reconstruction must report it against the import site — never fall
// back to void/Obj.

TEST(Module, UnknownExportedTypeErr) {
  auto tmp = paykan::test::tempDir() / "pkn_ms_unknown_export";
  std::filesystem::remove_all(tmp);
  auto libPath =
      writeFile(tmp.string(), "lib.pkn", "fn real() -> int { return 1; }\n");
  auto key = std::filesystem::canonical(libPath).string();

  paykan::sema::Sema::ModuleInfo info;
  info.ExportedFunctions.push_back(
      {"ghost", "Ghost", {"int", "Phantom[]"}, {}});
  paykan::sema::Sema::ModuleInfo::ClassInfo ci;
  ci.Name = "Holder";
  ci.OriginModule = "lib";
  ci.Fields.push_back({"f", "Spectre"});
  ci.Methods.push_back({"m", "Wraith", {"Shade"}, 0, {}});
  info.ExportedClasses.push_back(std::move(ci));
  paykan::sema::Sema::ModuleInfo::ClassInfo orphan;
  orphan.Name = "Orphan";
  orphan.SuperClassName = "Nowhere";
  orphan.OriginModule = "lib";
  info.ExportedClasses.push_back(std::move(orphan));
  paykan::sema::Sema::ModuleCache[key] = std::move(info);

  auto main = writeFile(tmp.string(), "main.pkn", R"(
import lib;
fn main() -> int { return 0; }
)");
  auto r = semaCheckFile(main, tmp.string());
  paykan::sema::Sema::ModuleCache.erase(key);

  EXPECT_FALSE(r.Ok);
  for (const char *expected :
       {"import of module 'lib': return type of function 'ghost' has unknown "
        "type 'Ghost'",
        "import of module 'lib': parameter 2 of function 'ghost' has unknown "
        "type 'Phantom[]'",
        "import of module 'lib': field 'f' of class 'Holder' has unknown "
        "type 'Spectre'",
        "import of module 'lib': return type of method 'Holder.m' has "
        "unknown type 'Wraith'",
        "import of module 'lib': parameter 1 of method 'Holder.m' has "
        "unknown type 'Shade'",
        "import of module 'lib': superclass 'Nowhere' of class 'Orphan' is "
        "unknown"})
    EXPECT_NE(r.Diagnostics.find(expected), std::string::npos)
        << "missing: " << expected << "\n"
        << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// ─── ERR: a local class clashing with an imported type (#79) ─────────────────
//
// Only the clash is reported: the module's other classes stay declared, and a
// call by the clashing name is not reported again.

TEST(Module, LocalClassClashingWithImportReportedOnce) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_clash_class").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib.pkn", "class K { fn __init__() { } }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import lib;
class K { }
class Fine { fn __init__() { } }
fn main() -> int { k = K(); f = Fine(); return 0; }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("class 'K' conflicts with an imported type of "
                               "the same name"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("undeclared"), std::string::npos)
      << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}
