// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: module imports — all syntactic forms, OK and error cases.

#include "TestUtils.h"
#include <gtest/gtest.h>

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
  paykan::parser::ParserDriver drv;
  if (drv.parseFile(filePath) != 0)
    return {false, "parse error"};
  std::string diag;
  llvm::raw_string_ostream os(diag);
  paykan::sema::DiagEngine diagEngine(os);
  diagEngine.setSourceInfo(drv.getCurrentFile(), &drv.getSourceLines());
  paykan::sema::Sema sema(drv.getASTContext(), diagEngine, projectRoot);
  bool ok = sema.run(drv.getRoot()).Ok;
  return {ok, diag};
}

// ─── OK: import mod; ────────────────────────────────────────────────────────

TEST(Module, BareImportOk) {
  auto tmp = (std::filesystem::temp_directory_path() / "pkn_ms_bare").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "math.pkn",
            "fn add(a: int, b: int) -> int { return a + b; }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import math;
fn main() -> int { return math::add(1, 2); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// ─── OK: import path::mod; — short + full qualifier both valid ───────────────

TEST(Module, NestedShortQualifierOk) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_nshort").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "math/arith.pkn",
            "fn mul(a: int, b: int) -> int { return a * b; }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import math::arith;
fn main() -> int { return arith::mul(3, 4); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, NestedFullQualifierOk) {
  auto tmp = (std::filesystem::temp_directory_path() / "pkn_ms_nfull").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "math/arith.pkn",
            "fn mul(a: int, b: int) -> int { return a * b; }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import math::arith;
fn main() -> int { return math::arith::mul(3, 4); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// ─── OK: import path::mod as alias; — alias + full path both valid ───────────

TEST(Module, AliasQualifierOk) {
  auto tmp = (std::filesystem::temp_directory_path() / "pkn_ms_alias").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "utils/strings.pkn",
            "fn upper(s: Str) -> int { return 0; }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import utils::strings as str;
fn main() -> int { return str::upper("hi"); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, AliasFullPathAlsoOk) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_alias_full").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "utils/strings.pkn",
            "fn upper(s: Str) -> int { return 0; }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import utils::strings as str;
fn main() -> int { return utils::strings::upper("hi"); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// ─── OK: import path::{a, b}; ────────────────────────────────────────────────

TEST(Module, SelectiveOk) {
  auto tmp = (std::filesystem::temp_directory_path() / "pkn_ms_sel").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib/foo.pkn", "fn val() -> int { return 1; }\n");
  writeFile(tmp, "lib/bar.pkn", "fn val() -> int { return 2; }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import lib::{foo, bar};
fn main() -> int { return foo::val() + bar::val(); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// ─── OK: import path::{a as x, b as y}; ─────────────────────────────────────

TEST(Module, SelectiveAliasOk) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_sel_alias").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib/foo.pkn", "fn val() -> int { return 10; }\n");
  writeFile(tmp, "lib/bar.pkn", "fn val() -> int { return 20; }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import lib::{foo as f, bar as b};
fn main() -> int { return f::val() + b::val(); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// ─── OK: import path::{a, b as y}; (mixed) ───────────────────────────────────

TEST(Module, SelectiveMixedAliasOk) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_sel_mix").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib/foo.pkn", "fn val() -> int { return 5; }\n");
  writeFile(tmp, "lib/bar.pkn", "fn val() -> int { return 7; }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import lib::{foo, bar as b};
fn main() -> int { return foo::val() + b::val(); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// ─── OK: class inheritance — short, full, alias qualifier ────────────────────

TEST(Module, ClassInheritShortQualOk) {
  auto tmp = (std::filesystem::temp_directory_path() / "pkn_ms_inh_s").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "shapes/base.pkn",
            "class Shape { fn area() -> int { return 0; } }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import shapes::base;
class Circle : base::Shape { fn area() -> int { return 1; } }
fn main() -> int { return 0; }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, ClassInheritFullQualOk) {
  auto tmp = (std::filesystem::temp_directory_path() / "pkn_ms_inh_f").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "shapes/base.pkn",
            "class Shape { fn area() -> int { return 0; } }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import shapes::base;
class Circle : shapes::base::Shape { fn area() -> int { return 1; } }
fn main() -> int { return 0; }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, ClassInheritAliasOk) {
  auto tmp = (std::filesystem::temp_directory_path() / "pkn_ms_inh_a").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "shapes/base.pkn",
            "class Shape { fn area() -> int { return 0; } }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import shapes::base as sh;
class Circle : sh::Shape { fn area() -> int { return 1; } }
fn main() -> int { return 0; }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// ─── OK: circular import detection ──────────────────────────────────────────

TEST(Module, CircularImportErr) {
  auto tmp = std::filesystem::temp_directory_path() / "pkn_ms_circ";
  std::filesystem::remove_all(tmp);
  std::filesystem::create_directories(tmp);
  std::ofstream(tmp / "a.pkn") << "import b;\nfn fa() -> int { return 1; }\n";
  std::ofstream(tmp / "b.pkn") << "import a;\nfn fb() -> int { return 2; }\n";

  paykan::parser::ParserDriver drv;
  ASSERT_EQ(drv.parseFile((tmp / "a.pkn").string()), 0);
  std::string diag;
  llvm::raw_string_ostream os(diag);
  paykan::sema::DiagEngine diagEngine(os);
  diagEngine.setSourceInfo(drv.getCurrentFile(), &drv.getSourceLines());
  paykan::sema::Sema sema(drv.getASTContext(), diagEngine, tmp.string());
  EXPECT_FALSE(sema.run(drv.getRoot()).Ok);
  EXPECT_NE(diag.find("circular"), std::string::npos);
  std::filesystem::remove_all(tmp);
}

// ─── OK: ClassType remapping across import ───────────────────────────────────

TEST(Module, ClassTypeRemapOk) {
  auto tmp = std::filesystem::temp_directory_path() / "pkn_ms_classremap";
  std::filesystem::remove_all(tmp);
  std::filesystem::create_directories(tmp);
  std::ofstream(tmp / "strmod.pkn") << "fn wrap(s: Str) -> int { return 0; }\n";
  auto mainPath = (tmp / "main.pkn").string();
  std::ofstream(mainPath) << R"(
import strmod;
fn main() -> int { s: Str = "hi"; return strmod::wrap(s); }
)";
  auto r = semaCheckFile(mainPath, tmp.string());
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// ─── OK: same module imported twice -> same ClassType pointer ────────────────

TEST(Module, ClassTypeIdentityOk) {
  auto tmp = std::filesystem::temp_directory_path() / "pkn_ms_classid";
  std::filesystem::remove_all(tmp);
  std::filesystem::create_directories(tmp);
  std::ofstream(tmp / "strutil.pkn") << "fn id(s: Str) -> int { return 0; }\n";
  std::ofstream(tmp / "modA.pkn")
      << "import strutil;\nfn useA(s: Str) -> int { return strutil::id(s); }\n";
  std::ofstream(tmp / "modB.pkn")
      << "import strutil;\nfn useB(s: Str) -> int { return strutil::id(s); }\n";
  auto mainPath = (tmp / "main.pkn").string();
  std::ofstream(mainPath) << R"(
import modA;
import modB;
fn main() -> int { s: Str = "hello"; modA::useA(s); modB::useB(s); return 0; }
)";
  auto r = semaCheckFile(mainPath, tmp.string());
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// ─── ERR cases ──────────────────────────────────────────────────────────────

TEST(Module, ModuleNotFoundErr) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_notfound").string();
  std::filesystem::remove_all(tmp);
  auto main =
      writeFile(tmp, "main.pkn",
                "import does_not_exist;\nfn main() -> int { return 0; }\n");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("not found"), std::string::npos);
  std::filesystem::remove_all(tmp);
}

TEST(Module, WrongQualifierErr) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_wrongq").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "math/arith.pkn",
            "fn add(a: int, b: int) -> int { return a + b; }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import math::arith as ar;
fn main() -> int { return arith::add(1, 2); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  std::filesystem::remove_all(tmp);
}

TEST(Module, UndeclaredFnErr) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_undefn").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "math.pkn",
            "fn add(a: int, b: int) -> int { return a + b; }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import math;
fn main() -> int { return math::multiply(2, 3); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  std::filesystem::remove_all(tmp);
}

TEST(Module, WrongArgTypeErr) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_wrongarg").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "math.pkn",
            "fn add(a: int, b: int) -> int { return a + b; }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import math;
fn main() -> int { return math::add(1.0, 2); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  std::filesystem::remove_all(tmp);
}

TEST(Module, ClassInheritBadQualifierErr) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_inh_badq").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "shapes/base.pkn", "class Shape {}\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import shapes::base;
class Circle : shapes::Shape {}
fn main() -> int { return 0; }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  std::filesystem::remove_all(tmp);
}

TEST(Module, SelectiveMissingErr) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_sel_miss").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib/foo.pkn", "fn val() -> int { return 1; }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import lib::{foo, bar};
fn main() -> int { return foo::val(); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  std::filesystem::remove_all(tmp);
}

// ─── OK: qualified type `mod::Type` in every type position ───────────────────

// Shared module body declaring a class that is used as a typed annotation.
static const char *const kPointModule =
    "class Point {\n"
    "  x: int; y: int;\n"
    "  fn __init__(x: int, y: int) { self.x = x; self.y = y; }\n"
    "  fn getX() -> int { return self.x; }\n"
    "}\n";

TEST(Module, QualifiedTypeParamOk) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_qt_param").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "geometry/point.pkn", kPointModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import geometry::point;
fn show(p: point::Point) -> int { return p.getX(); }
fn main() -> int { p = point::Point(3, 4); return show(p); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, QualifiedTypeVarDeclOk) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_qt_var").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "geometry/point.pkn", kPointModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import geometry::point;
fn main() -> int { p: point::Point = point::Point(3, 4); return p.getX(); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, QualifiedTypeReturnOk) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_qt_ret").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "geometry/point.pkn", kPointModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import geometry::point;
fn make() -> point::Point { return point::Point(1, 2); }
fn main() -> int { p = make(); return p.getX(); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, QualifiedTypeFieldOk) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_qt_field").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "geometry/point.pkn", kPointModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import geometry::point;
class Box {
  origin: point::Point;
  fn __init__() { self.origin = point::Point(0, 0); }
  fn x() -> int { return self.origin.getX(); }
}
fn main() -> int { b = Box(); return b.x(); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, QualifiedTypeArrayOk) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_qt_arr").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "geometry/point.pkn", kPointModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import geometry::point;
fn first(ps: point::Point[]) -> int { return ps[0].getX(); }
fn main() -> int { ps: point::Point[] = [point::Point(7, 8)]; return first(ps); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, QualifiedTypeAliasOk) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_qt_alias").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "geometry/point.pkn", kPointModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import geometry::point as geo;
fn show(p: geo::Point) -> int { return p.getX(); }
fn main() -> int { return show(geo::Point(3, 4)); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// ─── ERR: unknown qualified type ─────────────────────────────────────────────

TEST(Module, QualifiedTypeUnknownErr) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_qt_unknown").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "geometry/point.pkn", kPointModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import geometry::point;
fn show(p: point::Nope) -> int { return 0; }
fn main() -> int { return 0; }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("unknown class type"), std::string::npos)
      << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, AliasUnknownCallErr) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_alias_unk").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "utils/strings.pkn", "fn trim(s: Str) -> int { return 0; }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import utils::strings as str;
fn main() -> int { return strings::trim("x"); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  std::filesystem::remove_all(tmp);
}

// ─── OK: imported enums — qualified type + variants in every position
// ─────────

static const char *const kColorModule =
    "enum Color { Red, Green, Blue }\n"
    "fn name(c: Color) -> Str {\n"
    "  match c { Red { return \"red\"; } Green { return \"green\"; }\n"
    "            Blue { return \"blue\"; } }\n"
    "}\n"
    "fn favorite() -> Color { return Color::Blue; }\n";

TEST(Module, EnumQualifiedTypeAndVariantOk) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_enum_basic").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "pal/color.pkn", kColorModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import pal::color;
fn main() -> int {
  c: color::Color = color::Color::Green;
  if (c == color::Color::Green) { return 0; }
  return 1;
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, EnumFullPathQualifierOk) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_enum_full").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "pal/color.pkn", kColorModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import pal::color;
fn main() -> int {
  c: pal::color::Color = pal::color::Color::Blue;
  return 0;
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, EnumAliasQualifierOk) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_enum_alias").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "pal/color.pkn", kColorModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import pal::color as C;
fn main() -> int {
  c: C::Color = C::Color::Red;
  println(C::name(c));
  return 0;
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, EnumParamAndReturnOk) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_enum_fn").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "pal/color.pkn", kColorModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import pal::color;
fn main() -> int {
  f: color::Color = color::favorite();
  println(color::name(f));
  return 0;
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// Imported enum as a class field type: the field type (resolved by bare name
// during class reconstruction) and the constructor argument (resolved by
// qualified name) must be the SAME enum type, or this fails to type-check.
TEST(Module, EnumAsClassFieldTypeIdentityOk) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_enum_field").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "pal/color.pkn", kColorModule);
  writeFile(tmp, "pal/widget.pkn", R"(
import pal::color;
class Widget {
  tint: color::Color;
  fn __init__(t: color::Color) { self.tint = t; }
  fn tintName() -> Str { return color::name(self.tint); }
}
fn makeBlue() -> Widget { return Widget(color::Color::Blue); }
)");
  auto main = writeFile(tmp, "main.pkn", R"(
import pal::widget;
fn main() -> int {
  w: widget::Widget = widget::makeBlue();
  println(w.tintName());
  return 0;
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// ─── ERR: unknown variant on an imported enum still diagnosed
// ─────────────────

TEST(Module, EnumUnknownVariantErr) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_enum_badvar").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "pal/color.pkn", kColorModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import pal::color;
fn main() -> int {
  c: color::Color = color::Color::Purple;
  return 0;
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("has no variant 'Purple'"), std::string::npos)
      << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// ─── OK: a local function never shadows an imported one ─────────────────────
//
// Imported functions are only ever reachable through their qualifier, so a
// local `add` and `math::add` coexist and each call resolves to its own.

TEST(Module, LocalFunctionCoexistsWithImportedQualifiedName) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_local_vs_imp").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "math.pkn",
            "fn add(a: int, b: int) -> int { return a + b; }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import math;
fn add(a: int) -> int { return a + 1; }
fn main() -> int { return add(1) + math::add(1, 2); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// ─── ERR: an imported module's classes share the local class namespace ──────

TEST(Module, LocalFunctionNamedLikeImportedClassErr) {
  auto tmp =
      (std::filesystem::temp_directory_path() / "pkn_ms_fn_vs_impcls").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "shapes.pkn", "class Box { fn __init__() {} }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import shapes;
fn Box() -> int { return 0; }
fn main() -> int { return Box(); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'Box' is already declared as a class"),
            std::string::npos)
      << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}
