// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: module imports — all syntactic forms, OK and error cases.

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
  unsigned ErrorCount = 0;
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
  auto ctx = sema.run(drv.getRoot());
  return {ctx.Ok, os.str(), ctx.ErrorCount};
}

// ─── OK: import mod; ────────────────────────────────────────────────────────

TEST(Module, BareImportOk) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_bare").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_nshort").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_nfull").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_alias").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_alias_full").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_sel").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_sel_alias").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_sel_mix").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_inh_s").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_inh_f").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_inh_a").string();
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
  auto tmp = paykan::test::tempDir() / "pkn_ms_circ";
  std::filesystem::remove_all(tmp);
  std::filesystem::create_directories(tmp);
  std::ofstream(tmp / "a.pkn") << "import b;\nfn fa() -> int { return 1; }\n";
  std::ofstream(tmp / "b.pkn") << "import a;\nfn fb() -> int { return 2; }\n";

  paykan::parser::ParserDriver drv(paykan::test::testFrontend());
  ASSERT_EQ(drv.parseFile((tmp / "a.pkn").string()), 0);
  std::ostringstream os;
  paykan::sema::DiagEngine diagEngine(os);
  diagEngine.setSourceInfo(drv.getCurrentFile(), &drv.getSourceLines());
  paykan::sema::Sema sema(drv.getASTContext(), diagEngine, tmp.string(),
                          drv.getFrontendName());
  EXPECT_FALSE(sema.run(drv.getRoot()).Ok);
  EXPECT_NE(os.str().find("circular"), std::string::npos);
  std::filesystem::remove_all(tmp);
}

// ─── OK: ClassType remapping across import ───────────────────────────────────

TEST(Module, ClassTypeRemapOk) {
  auto tmp = paykan::test::tempDir() / "pkn_ms_classremap";
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
  auto tmp = paykan::test::tempDir() / "pkn_ms_classid";
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_notfound").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_wrongq").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_undefn").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_wrongarg").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_inh_badq").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_sel_miss").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_qt_param").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_qt_var").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_qt_ret").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_qt_field").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_qt_arr").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_qt_alias").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_qt_unknown").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_alias_unk").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_enum_basic").string();
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

// Match arms use bare variant names, also for an imported enum (#79).
TEST(Module, EnumQualifiedMatchArmNamesTheBareVariant) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_enum_qual_arm").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "pal/color.pkn", kColorModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import pal::color;
fn main() -> int {
  c: color::Color = color::Color::Green;
  match c { color::Color::Green { return 1; } color::Color::Pink { } _ { } }
  return 0;
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("error: 'color::Color::Green' is not a valid "
                               "match arm; use the bare variant name 'Green'"),
            std::string::npos)
      << r.Diagnostics;
  // Not a variant at all: the usual message.
  EXPECT_NE(r.Diagnostics.find("error: 'color::Color::Pink' is not a variant "
                               "of enum '"),
            std::string::npos)
      << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, EnumFullPathQualifierOk) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_enum_full").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_enum_alias").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_enum_fn").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_enum_field").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_enum_badvar").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_local_vs_imp").string();
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
  auto tmp = (paykan::test::tempDir() / "pkn_ms_fn_vs_impcls").string();
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

// ─── #70: same-named functions of two modules; one qualifier, one module ────

TEST(Module, SameNamedFunctionsOfTwoModulesOk) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_dup_fns").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "x.pkn", "fn tag() -> Str { return \"x\"; }\n");
  writeFile(tmp, "y.pkn", "fn tag() -> int { return 1; }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import x;
import y as r;
fn tag() -> bool { return True; }
fn main() -> int { s: Str = x::tag(); n: int = r::tag() + y::tag(); b: bool = tag(); return n; }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, OneQualifierForTwoModulesErr) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_qual_clash").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "a/util.pkn", "fn f() -> int { return 1; }\n");
  writeFile(tmp, "b/util.pkn", "fn f() -> Str { return \"b\"; }\n");
  writeFile(tmp, "util.pkn", "fn f() -> int { return 3; }\n");
  // Two last segments `util`, two equal aliases, and a qualifier spelled like
  // another import's full path.
  for (const char *imports : {"import a::util;\nimport b::util;\n",
                              "import a::util as u;\nimport b::util as u;\n",
                              "import util as v;\nimport b::util;\n"}) {
    auto main =
        writeFile(tmp, "main.pkn",
                  std::string(imports) + "fn main() -> int { return 0; }\n");
    auto r = semaCheckFile(main, tmp);
    EXPECT_FALSE(r.Ok) << imports;
    EXPECT_NE(r.Diagnostics.find("already names module"), std::string::npos)
        << imports << r.Diagnostics;
  }
  // A qualifier may name the same module twice, and aliases keep two
  // `util`s apart.
  for (const char *imports : {"import util;\nimport util as v;\n",
                              "import a::util;\nimport a::util;\n",
                              "import a::util;\nimport b::util as bu;\n",
                              "import a::util;\nimport b::util as a;\n"}) {
    auto main =
        writeFile(tmp, "main.pkn",
                  std::string(imports) + "fn main() -> int { return 0; }\n");
    auto r = semaCheckFile(main, tmp);
    EXPECT_TRUE(r.Ok) << imports << r.Diagnostics;
  }
  std::filesystem::remove_all(tmp);
}

// ─── ERR: a failed import is reported once (#119) ───────────────────────────
//
// The import's failure is the one error: uses of its qualifier are poisoned
// (silent follow-ons, like a poisoned binder's), an imported module's own
// errors are not repeated at each import on the way up, and modules are named
// by their canonical module name, files by their path from the source root.

namespace {

// Exactly one error, containing @p expected, and no absolute path.
void expectOneError(const SemaFileResult &r, const std::string &expected,
                    const std::string &root) {
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find(" error: ", r.Diagnostics.find(" error: ") + 1),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(expected), std::string::npos) << r.Diagnostics;
  // The main file's own name is printed in full; nothing else may be.
  std::string rest = r.Diagnostics;
  for (size_t at; (at = rest.find(root + "/main.pkn")) != std::string::npos;)
    rest.erase(at, root.size() + 9);
  EXPECT_EQ(rest.find(root), std::string::npos) << r.Diagnostics;
}

} // namespace

TEST(Module, MissingModuleUsesAreSilent) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_missing").string();
  std::filesystem::remove_all(tmp);
  auto main = writeFile(tmp, "main.pkn", R"(
import lib::nothere;
class Sub : nothere::Base { }
fn take(t: nothere::T) -> int { return 0; }
fn main() -> int {
  n: int = nothere::f(1);
  t: nothere::T = nothere::T();
  u = lib::nothere::g(t, n);
  c: nothere::Color = nothere::Color::Red;
  match t { nothere::T { } _ { } }
  x: Sub = Sub();
  println(Str(n + take(t)));
  return lib::nothere::h<int>(2);
}
)");
  expectOneError(semaCheckFile(main, tmp),
                 "module 'lib::nothere' not found (tried lib/nothere.pkn)",
                 tmp);
  std::filesystem::remove_all(tmp);
}

TEST(Module, MissingModuleDoesNotSilenceOtherNames) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_missing_other").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib/m.pkn", "fn f() -> int { return 1; }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import lib::nothere;
import lib::m;
fn main() -> int { return nothere::f() + m::g() + nowhere::h(); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("'nothere::f'"), std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("call to undeclared function 'm::g'"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("call to undeclared function 'nowhere::h'"),
            std::string::npos)
      << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, MissingSystemModuleIsOneError) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_missing_sys").string();
  std::filesystem::remove_all(tmp);
  auto main = writeFile(tmp, "main.pkn", R"(
import ::nosuchstd;
fn main() -> int { return nosuchstd::f(); }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("system module '::nosuchstd' not found"),
            std::string::npos)
      << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, CircularImportIsOneError) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_circ_one").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib/a.pkn", "import lib::b;\nfn fa() -> int { return 1; }\n");
  writeFile(tmp, "lib/b.pkn",
            "import lib::c;\nfn fb() -> int { return c::fc(); }\n");
  writeFile(tmp, "lib/c.pkn",
            "import lib::a;\nfn fc() -> int { return a::fa(); }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import lib::a;
fn main() -> int { return a::fa() + a::missing(); }
)");
  auto r = semaCheckFile(main, tmp);
  expectOneError(r, "circular import of module 'lib::a'", tmp);
  // The error is located in the module that closes the cycle; each import on
  // the way there adds a note, not an error.
  EXPECT_NE(r.Diagnostics.find("lib/c.pkn:1:1: error:"), std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("note: in module 'lib::b' imported here"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("note: in module 'lib::a' imported here"),
            std::string::npos)
      << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, ErrorsInAnImportedModuleAreNotRepeated) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_bad_module").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib/bad.pkn", "fn f() -> int { return \"no\"; }\n");
  writeFile(tmp, "lib/mid.pkn",
            "import lib::bad;\nfn g() -> int { return bad::f(); }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import lib::mid;
fn main() -> int { return mid::g(); }
)");
  expectOneError(semaCheckFile(main, tmp), "lib/bad.pkn:1:", tmp);
  // A syntax error in a module, likewise.
  writeFile(tmp, "lib/bad.pkn", "fn f() -> int { return 1 }\n");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("failed to parse"), std::string::npos)
      << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(Module, ModuleThatIsADirectoryErr) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_dir_module").string();
  std::filesystem::remove_all(tmp);
  std::filesystem::create_directories(std::filesystem::path(tmp) / "lib" /
                                      "m.pkn");
  auto main = writeFile(tmp, "main.pkn", R"(
import lib::m;
fn main() -> int { return m::f(); }
)");
  expectOneError(semaCheckFile(main, tmp),
                 "module 'lib::m': 'lib/m.pkn' is a directory, not a source "
                 "file",
                 tmp);
  std::filesystem::remove_all(tmp);
}

TEST(Module, TypeClashNamesModulesCanonically) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_clash_names").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "data/list.pkn",
            "class Node { v: int; fn __init__() { self.v = 1; } }\n");
  writeFile(tmp, "data/tree.pkn",
            "class Node { l: int; fn __init__() { self.l = 0; } }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import data::list;
import data::tree as t;
fn main() -> int { return 0; }
)");
  expectOneError(semaCheckFile(main, tmp),
                 "import of module 'data::tree': class 'Node' conflicts with "
                 "a type of the same name declared by module 'data::list'",
                 tmp);
  std::filesystem::remove_all(tmp);
}

// -- A failed module is reported once per compilation (#132) -----------------
//
// However many import paths reach a module that fails -- not found, or with
// errors of its own -- its failure is reported where it happens, once; every
// later import of it adds a note.  The ModuleCache outlives a compilation,
// but the set of failed modules does not: the next compilation reports the
// failure again.

namespace {

size_t countOf(const std::string &text, const std::string &what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos;
       at = text.find(what, at + 1))
    ++n;
  return n;
}

} // namespace

TEST(Module, FailedImportReachedTwiceIsReportedOnce) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_failed_twice").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib/a.pkn",
            "import lib::gone; fn f() -> int { return 1; }\n");
  auto main = writeFile(tmp, "main.pkn",
                        "import lib::a; import lib::a as x;\n"
                        "fn main() -> int { return a::f() + x::f(); }\n");
  for (int compilation = 0; compilation < 2; ++compilation) {
    auto r = semaCheckFile(main, tmp);
    expectOneError(r, "lib/a.pkn:1:1: error: module 'lib::gone' not found",
                   tmp);
    EXPECT_EQ(countOf(r.Diagnostics, "not found"), 1u) << r.Diagnostics;
    EXPECT_NE(r.Diagnostics.find("main.pkn:1:16: note: module 'lib::a' "
                                 "failed to load; its errors are reported "
                                 "above"),
              std::string::npos)
        << r.Diagnostics;
  }
  std::filesystem::remove_all(tmp);
}

TEST(Module, MissingModuleImportedTwiceInOneFileIsOneError) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_missing_twice").string();
  std::filesystem::remove_all(tmp);
  auto main = writeFile(tmp, "main.pkn",
                        "import lib::gone; import lib::gone as g;\n"
                        "fn main() -> int { return gone::f() + g::f(); }\n");
  expectOneError(semaCheckFile(main, tmp), "module 'lib::gone' not found", tmp);
  std::filesystem::remove_all(tmp);
}

TEST(Module, FailedImportInADiamondIsReportedOnce) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_failed_diamond").string();
  std::filesystem::remove_all(tmp);
  // main -> a -> gone, and main -> b -> a -> gone.
  writeFile(tmp, "lib/a.pkn",
            "import lib::gone; fn f() -> int { return 1; }\n");
  writeFile(tmp, "lib/b.pkn",
            "import lib::a; fn g() -> int { return a::f(); }\n");
  auto main = writeFile(tmp, "main.pkn",
                        "import lib::a; import lib::b;\n"
                        "fn main() -> int { return a::f() + b::g(); }\n");
  auto r = semaCheckFile(main, tmp);
  expectOneError(r, "module 'lib::gone' not found", tmp);
  // The module that failed only by importing a failed one fails too, quietly.
  EXPECT_NE(r.Diagnostics.find("lib/b.pkn:1:1: note: module 'lib::a' failed "
                               "to load"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("note: in module 'lib::b' imported here"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("errors in imported module"), std::string::npos)
      << r.Diagnostics;
  // Reached only through the failed path (main -> b -> a), the same.
  writeFile(tmp, "main.pkn",
            "import lib::b; import lib::a;\n"
            "fn main() -> int { return a::f() + b::g(); }\n");
  r = semaCheckFile(main, tmp);
  expectOneError(r, "module 'lib::gone' not found", tmp);
  std::filesystem::remove_all(tmp);
}

TEST(Module, ModuleWithErrorsReachedTwiceIsReportedOnce) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_bad_twice").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib/a.pkn", "fn f() -> int { return \"x\"; }\n");
  writeFile(tmp, "lib/b.pkn",
            "import lib::a; fn g() -> int { return a::f(); }\n");
  writeFile(tmp, "lib/c.pkn",
            "import lib::a; fn h() -> int { return a::f(); }\n");
  auto main = writeFile(tmp, "main.pkn",
                        "import lib::b; import lib::c; import lib::a as x;\n"
                        "fn main() -> int { return b::g() + c::h() + x::f(); "
                        "}\n");
  auto r = semaCheckFile(main, tmp);
  expectOneError(r, "lib/a.pkn:1:17: error: return value of type 'Str'", tmp);
  // A syntax error in the module, likewise.
  writeFile(tmp, "lib/a.pkn", "fn f() -> int { return 1 }\n");
  r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_EQ(countOf(r.Diagnostics, " error: "), 1u) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// A module that imports a failed module does not enter the ModuleCache as a
// good one: a later compilation that reaches it once the failure is fixed
// analyses it afresh.
TEST(Module, ModuleFailedOnlyByItsImportsIsNotCached) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_failed_not_cached").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib/b.pkn",
            "import lib::a; fn g() -> int { return a::f(); }\n");
  auto main = writeFile(tmp, "main.pkn",
                        "import lib::a; import lib::b;\n"
                        "fn main() -> int { return b::g(); }\n");
  auto r = semaCheckFile(main, tmp);
  expectOneError(r, "module 'lib::a' not found", tmp);
  writeFile(tmp, "lib/a.pkn", "fn f() -> int { return 1; }\n");
  r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}
