// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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

/// The errors and notes of @p diags, one per line without the source text
/// shown under each, every file named by its last component.
static std::string diagSummary(const std::string &diags) {
  std::istringstream in(diags);
  std::string out;
  for (std::string line; std::getline(in, line);) {
    size_t kind = line.find(": error: ");
    if (kind == std::string::npos)
      kind = line.find(": note: ");
    if (kind == std::string::npos)
      continue;
    const size_t slash = line.rfind('/', kind);
    out += (slash == std::string::npos ? line : line.substr(slash + 1)) + "\n";
  }
  return out;
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
    "class Zeta { v: int; }\n"
    "impl Zeta { fn __init__() { self.v = 7; } }\n"
    "class Alpha {\n"
    "  kind: Kind;\n"
    "  fn make() -> Zeta { return Zeta(); }\n"
    "  fn take(z: Zeta) -> int { return z.v; }\n"
    "  fn kindOf() -> Kind { return self.kind; }\n"
    "  fn many() -> Zeta[2] { return [Zeta(), Zeta()]; }\n"
    "  fn count(zs: view Zeta[]) -> int { return zs.len(); }\n"
    "}\n"
    "impl Alpha {\n"
    "  fn __init__() { self.kind = Kind::Round; }\n"
    "}\n"
    "class Mid {\n"
    "  a: Alpha;\n"
    "  fn alpha() -> Alpha { return self.a; }\n"
    "}\n"
    "impl Mid {\n"
    "  fn __init__() { self.a = Alpha(); }\n"
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
  if k == shapes::Kind::Round && f == shapes::Kind::Round { return 0; }
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
  zs: shapes::Zeta[2] = a.many();
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
    "class Thing { v: int; "
    "fn val() -> int { return self.v; } }\n"
    "impl Thing { fn __init__(v: int) { self.v = v; } }\n"
    "enum Mode { Fast, Slow }\n"
    "fn made() -> Thing { return Thing(5); }\n";

static const char *const kMidModule =
    "import leaf;\n"
    "class Wrap {\n"
    "  t: leaf::Thing;\n"
    "  mode: leaf::Mode;\n"
    "  fn get() -> leaf::Thing { return self.t; }\n"
    "  fn put(t: leaf::Thing) -> int { self.t = t; return t.val(); }\n"
    "  fn getMode() -> leaf::Mode { return self.mode; }\n"
    "}\n"
    "impl Wrap {\n"
    "  fn __init__() { self.t = leaf::Thing(9); self.mode = leaf::Mode::Fast; "
    "}\n"
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
  if m == leaf::Mode::Fast { return t.val() + n; }
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
            "class Node { v: int; }\n"
            "impl Node { fn __init__() { self.v = 1; } }\n");
  writeFile(tmp, "tree.pkn",
            "class Node { l: int; r: int; }\n"
            "impl Node { fn __init__() { self.l = 0; self.r = 0; } }\n");
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
      {"ghost", "Ghost", {"int", "Phantom[2]"}, {}});
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
        "type 'Phantom[2]'",
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

// A function whose type does not resolve is reported once, though the
// import declares it under two names (`m::ghost` and `lib::m::ghost`): the
// second report was the first one again, which stopped a debug build (the
// duplicate-diagnostic guard).
TEST(Module, UnknownExportedTypeOfANestedModuleIsReportedOnce) {
  auto tmp = paykan::test::tempDir() / "pkn_ms_unknown_nested";
  std::filesystem::remove_all(tmp);
  auto libPath =
      writeFile(tmp.string(), "lib/m.pkn", "fn real() -> int { return 1; }\n");
  auto key = std::filesystem::canonical(libPath).string();
  paykan::sema::Sema::ModuleInfo info;
  info.ExportedFunctions.push_back({"ghost", "Ghost", {}, {}});
  paykan::sema::Sema::ModuleCache[key] = std::move(info);
  auto main = writeFile(tmp.string(), "main.pkn",
                        "import lib::m;\nfn main() -> int { return 0; }\n");
  auto r = semaCheckFile(main, tmp.string());
  paykan::sema::Sema::ModuleCache.erase(key);
  EXPECT_FALSE(r.Ok);
  const std::string expected = "import of module 'lib::m': return type of "
                               "function 'ghost' has unknown type 'Ghost'";
  const size_t first = r.Diagnostics.find(expected);
  EXPECT_NE(first, std::string::npos) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find(expected, first + 1), std::string::npos)
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
  writeFile(tmp, "lib.pkn", "class K {}\nimpl K { fn __init__() { } }\n");
  auto main = writeFile(tmp, "main.pkn", R"(
import lib;
class K { }
class Fine {}
impl Fine { fn __init__() { } }
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

// ─── A module-private initializer is constructed by its module only ─────────
//
// `___init__` (docs/language/15-modules.md): an importer cannot construct the
// type, nor extend it (`__super__` into the initializer, written or the
// default one), from the module's source or its templates; the module's own
// functions and templates can.

TEST(Module, ModulePrivateInitializerIsNotTheImportersErr) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_private_init").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib.pkn", R"(
class Conn { __h: int; }
impl Conn {
  fn ___init__(h: int) { self.__h = h; }
  static fn open(h: int) -> Conn { return Conn(h); }
  view fn h() -> int { return self.__h; }
}
fn mk() -> Conn { return Conn(1); }
struct P { x: int; }
impl P { fn ___init__(x: int) { self.x = x; } }
fn mkP() -> P { return P(2); }
class Box<T> { v: T; }
impl Box<T> { fn ___init__(v: T) { self.v = v; } }
fn boxOf(v: int) -> Box<int> { return Box<int>(v); }
)");
  auto main = writeFile(tmp, "main.pkn", R"(
import lib;
class Sub : lib::Conn {}
impl Sub { fn __init__() { __super__(2); } }
class Sub2 : lib::Conn {}
fn main() -> int {
  a = lib::Conn(1);
  b = lib::Conn::open(2);
  c = lib::mk();
  p = lib::P(3);
  q = lib::mkP();
  e = lib::Box<int>(4);
  f = lib::boxOf(5);
  return a.h() + b.h() + c.h() + p.x + q.x + e.v + f.v;
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  for (const char *d :
       {"'Conn' cannot be extended here: its initializer '___init__' is "
        "private to module 'lib'",
        "'lib::Conn' cannot be constructed here: its initializer '___init__' "
        "is private to module 'lib'",
        "'lib::P' cannot be constructed here: its initializer '___init__' is "
        "private to module 'lib'",
        "cannot be constructed here: its initializer '___init__' is private "
        "to module 'lib'"})
    EXPECT_NE(r.Diagnostics.find(d), std::string::npos) << d << "\n"
                                                        << r.Diagnostics;
  // The factories, the module's function and its template's are fine.
  for (const char *ok :
       {"lib::Conn::open", "lib::mk()", "lib::mkP()", "lib::boxOf(5)"})
    EXPECT_EQ(r.Diagnostics.find(std::string("error") + ok), std::string::npos);
  std::filesystem::remove_all(tmp);
}

// ─── Conformances and traits across modules ─────────────────────────────────
//
// A type's conformance to a built-in trait (`impl Hashable for Key`) is part
// of its module's interface: an importer sees it wherever it reaches the
// type, directly, through another module or through a template body.

// The module keys of the conformance tests: a struct and a class with an
// `impl Hashable for` block, a private struct that a template body uses, and
// instantiations the module makes of its generic types.
static const char *const kKeysLibModule = R"(import ::collections;
struct Key { a: int; b: int; }
impl Hashable for Key { view fn hash() -> int { return (self.a * 31 + self.b).hash(); } }
class CK {
  v: int;
  view fn equals(other: Obj) -> bool {
    match other {
      p: CK { return p.v == self.v; }
      _ { return False; }
    }
  }
}
impl CK { fn __init__(v: int) { self.v = v; } }
impl Hashable for CK { view fn hash() -> int { return self.v.hash(); } }
struct _Priv { n: int; }
impl Hashable for _Priv { view fn hash() -> int { return self.n.hash(); } }
fn distinct<T>(x: T) -> unsigned {
  s = collections::HashSet<_Priv>(); s.add(_Priv(1)); s.add(_Priv(1)); s.add(_Priv(2));
  return s.len();
}
trait Named { view fn name() -> Str; }
struct Keyed<K: Hashable> { k: K; }
impl Hashable for Keyed<K> { view fn hash() -> int { return self.k.hash(); } }
class GBox<T> { v: T; }
impl GBox<T> { fn __init__(v: T) { self.v = v; } }
impl Hashable for GBox<T> { view fn hash() -> int { return 3; } }
impl Named for GBox<T> { view fn name() -> Str { return "box"; } }
enum Maybe<T: Hashable> { Just(v: T), Nothing }
fn mkKeyed() -> Keyed<int> { return Keyed<int>(7); }
fn mkBox() -> GBox<int> { return GBox<int>(1); }
)";

// The module mid: it only mentions keys' types, and declares a subclass of
// one, so its interface describes them (and their conformances) again.
static const char *const kKeysMidModule = R"(import lib::keys;
fn mk() -> keys::Key { return keys::Key(1, 2); }
fn mkc() -> keys::CK { return keys::CK(4); }
class MSub : keys::CK {
  view fn equals(other: Obj) -> bool { match other { p: MSub { return p.v == self.v; } _ { return False; } } }
}
impl MSub { fn __init__(v: int) { __super__(v); } }
fn mksub(v: int) -> MSub { return MSub(v); }
)";

// The module m2: it instantiates keys' generic types for types neither keys
// nor main does, so its interface describes those instantiations, with
// their conformances.
static const char *const kKeysM2Module = R"(import lib::keys;
fn mkBox() -> keys::GBox<bool> { return keys::GBox<bool>(True); }
fn mkKeyed() -> keys::Keyed<keys::Key> { return keys::Keyed<keys::Key>(keys::Key(2, 3)); }
)";

// main uses keys' types and conformances directly, through mid (also a
// subclass declared there), through a template body of keys (distinct), and
// through the instantiations of keys' generic types that keys made (mkKeyed,
// mkBox), that m2 made, and that main makes itself (`Keyed<Str>`,
// `GBox<Str>`: their `impl Trait for` blocks go with the templates); and
// they are type arguments of bounded generic enums, keys' and main's, in
// field types, which are checked once the conformances are known.
TEST(Module, HashableAcrossModulesOk) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_hashable").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib/keys.pkn", kKeysLibModule);
  writeFile(tmp, "lib/mid.pkn", kKeysMidModule);
  writeFile(tmp, "lib/m2.pkn", kKeysM2Module);
  auto main = writeFile(tmp, "main.pkn", R"(import ::io;
import ::collections;
import lib::keys;
import lib::mid;
import lib::m2;
class Sub : keys::CK {
  view fn equals(other: Obj) -> bool { match other { p: Sub { return p.v == self.v; } _ { return False; } } }
}
impl Sub { fn __init__(v: int) { __super__(v); } }
fn h<T: Hashable>(x: view T) -> int { return x.hash(); }
enum Opt<T: Hashable> { Some(v: T), No }
class Slots { m: keys::Maybe<keys::Key>; o: Opt<keys::CK>; }
impl Slots {
  fn __init__() {
    self.m = keys::Maybe<keys::Key>::Just(keys::Key(4, 0));
    self.o = Opt<keys::CK>::Some(keys::CK(5));
  }
}
fn main() -> int {
  m = collections::HashMap<keys::Key, int>();
  m.put(keys::Key(1, 2), 5);
  m.put(mid::mk(), 6);
  s = collections::HashSet<keys::CK>();
  s.add(keys::CK(3)); s.add(mid::mkc());
  t = collections::HashSet<Sub>();
  t.add(Sub(1));
  io::println(Str(h(keys::mkKeyed()) == h(7)) + " " + Str(h(keys::mkBox())));
  io::println(Str(h(mid::mk()) + h(mid::mkc())) + keys::mkBox().name());
  io::println(Str(h(keys::Keyed<Str>("a")) == h("a")) + " " +
              Str(h(keys::GBox<Str>("x"))) + keys::GBox<Str>("x").name());
  io::println(Str(keys::distinct(0)));
  a = collections::HashSet<mid::MSub>();
  a.add(mid::mksub(1)); a.add(mid::MSub(2));
  b = collections::HashSet<keys::Keyed<keys::Key>>();
  b.add(m2::mkKeyed());
  io::println(Str(a.len() + b.len()) + " " + Str(h(m2::mkBox())) + m2::mkBox().name());
  slots = Slots();
  return 0;
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// keys' conformances reach main three ways: keys itself (aliased, and by its
// full path), mid's description of Key, and lib2's template body (keys under
// its hidden qualifier).  They are one fact each, not a duplicate.
TEST(Module, ConformanceReachedTwiceIsOne) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_hashable_twice").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib/keys.pkn", kKeysLibModule);
  writeFile(tmp, "lib/mid.pkn", kKeysMidModule);
  writeFile(tmp, "lib/lib2.pkn", R"(import lib::keys;
import ::collections;
fn count<T>(x: T) -> unsigned {
  s = collections::HashSet<keys::Key>(); s.add(keys::Key(1, 2)); s.add(keys::Key(1, 2));
  return s.len();
}
)");
  auto main = writeFile(tmp, "main.pkn", R"(import lib::keys as ks;
import lib::mid;
import lib::lib2;
fn h<T: Hashable>(x: view T) -> int { return x.hash(); }
fn main() -> int {
  return h(mid::mk()) + h(ks::Key(3, 4)) + h(lib::keys::CK(5)) + int(u32(lib2::count(0)));
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// A subclass of another module's Hashable class is Hashable through it, and
// cannot repeat the conformance, as in one module.
TEST(Module, RedeclaredThroughImportedSuperclassErr) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_hashable_sub").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib/keys.pkn", kKeysLibModule);
  auto main = writeFile(tmp, "main.pkn", R"(import lib::keys;
class Sub : keys::CK {}
impl Hashable for Sub {}
fn main() -> int { return 0; }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("main.pkn:3:1: error: class 'Sub' already "
                               "implements 'Hashable' (through 'CK')"),
            std::string::npos)
      << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// An imported struct or enum named as a superclass says what it is, as a
// local one does; it was "not defined".  An imported class is a superclass.
TEST(Module, ImportedStructAsSuperclassErr) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_struct_super").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib/m.pkn",
            "struct S { x: int; }\nenum E { X }\nclass K {}\n");
  auto main = writeFile(tmp, "main.pkn", R"(import lib::m;
class A : m::S {}
class B : m::E {}
class D : m::K {}
fn main() -> int { d = D(); return 0; }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(diagSummary(r.Diagnostics),
            "main.pkn:2:1: error: superclass 'm::S' of class 'A' is a struct, "
            "not a class\n"
            "main.pkn:3:1: error: superclass 'm::E' of class 'B' is an enum, "
            "not a class\n");
  std::filesystem::remove_all(tmp);
}

// Only the type's module declares its conformances.
TEST(Module, ReimplementImportedConformanceErr) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_hashable_reimpl").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib/keys.pkn", kKeysLibModule);
  auto main = writeFile(tmp, "main.pkn", R"(import lib::keys;
impl Hashable for keys::Key { view fn hash() -> int { return 1; } }
impl Hashable for Key { view fn hash() -> int { return 1; } }
impl Hashable for keys::CK { view fn hash() -> int { return 1; } }
fn main() -> int { return 0; }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  for (const char *d :
       {"main.pkn:2:1: error: an 'impl' block for a struct of another module "
        "is not supported yet",
        "main.pkn:3:1: error: an 'impl' block for a struct of another module "
        "is not supported yet",
        "main.pkn:4:1: error: an 'impl' block for a class of another module "
        "is not supported yet"})
    EXPECT_NE(r.Diagnostics.find(d), std::string::npos) << d << "\n"
                                                        << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// A generic class's `impl Trait for` blocks go with its template: an
// importer's instantiation checks them as the module would check its own
// (`Box<float>` in gb is the same error), in gb's file, with the importer's
// request as the note, also a block of gb's own trait and an `impl Show
// for` block, whose methods main never calls.
TEST(Module, ImporterInstantiationChecksTraitBlock) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_trait_block").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "gb.pkn", R"(class Box<T> { v: T; }
impl Box<T> { fn __init__(v: T) { self.v = v; } }
impl Hashable for Box<T> { view fn hash() -> int { return self.v.hash(); } }
trait Named { view fn name() -> Str; }
class NBox<T> { v: T; }
impl NBox<T> { fn __init__(v: T) { self.v = v; } }
impl Named for NBox<T> { view fn name() -> Str { return self.v.name(); } }
class SBox<T> { v: T; }
impl SBox<T> { fn __init__(v: T) { self.v = v; } }
impl Show for SBox<T> {}
)");
  auto main = writeFile(tmp, "main.pkn", R"(import gb;
fn main() -> int { b = gb::Box<float>(1.5); return 0; }
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(diagSummary(r.Diagnostics),
            "gb.pkn:3:59: error: method call on non-class type 'float'\n"
            "main.pkn:2:24: note: in instantiation of 'Box<float>' requested "
            "here\n")
      << r.Diagnostics;
  auto named = semaCheckFile(
      writeFile(tmp, "named.pkn",
                "import gb;\nfn main() -> int { b = gb::NBox<int>(1); return "
                "0; }\n"),
      tmp);
  EXPECT_FALSE(named.Ok);
  EXPECT_EQ(diagSummary(named.Diagnostics),
            "gb.pkn:7:57: error: method call on non-class type 'int'\n"
            "named.pkn:2:24: note: in instantiation of 'NBox<int>' requested "
            "here\n")
      << named.Diagnostics;
  auto show = semaCheckFile(
      writeFile(tmp, "show.pkn",
                "import gb;\nfn main() -> int { b = gb::SBox<int>(1); return "
                "0; }\n"),
      tmp);
  EXPECT_FALSE(show.Ok);
  EXPECT_EQ(diagSummary(show.Diagnostics),
            "gb.pkn:10:1: error: class 'SBox<int>' already implements 'Show' "
            "(through 'Obj')\n"
            "show.pkn:2:24: note: in instantiation of 'SBox<int>' requested "
            "here\n")
      << show.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// A block of the module's own trait gives an importer's instantiation its
// methods, as the module's own `GBox<int>` has them, but not the trait,
// which the importer cannot name (#147): its own trait of that name is
// another trait, which GBox does not implement.  Instantiated before the
// `impl` blocks are checked (a field) and after (a call) alike.
TEST(Module, ModuleTraitBlockGivesMethodsOnly) {
  auto tmp = (paykan::test::tempDir() / "pkn_ms_module_trait_block").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "gb.pkn", R"(trait Named { view fn name() -> Str; }
class GBox<T> { v: T; }
impl GBox<T> { fn __init__(v: T) { self.v = v; } }
impl Named for GBox<T> { view fn name() -> Str { return "gbox"; } }
fn named<T: Named>(x: view T) -> Str { return x.name(); }
fn mine() -> Str { return named(GBox<int>(1)); }
)");
  auto methods = writeFile(tmp, "methods.pkn", R"(import gb;
class H { b: gb::GBox<Str>; }
impl H { fn __init__() { self.b = gb::GBox<Str>("s"); } }
fn main() -> int {
  s = gb::GBox<float>(1.5).name() + H().b.name() + gb::mine();
  return 0;
}
)");
  auto onlyMethods = semaCheckFile(methods, tmp);
  EXPECT_TRUE(onlyMethods.Ok) << onlyMethods.Diagnostics;

  auto own = writeFile(tmp, "own.pkn", R"(import gb;
trait Named { view fn name() -> Str; }
class H { b: gb::GBox<Str>; }
fn g<T: Named>(x: view T) -> Str { return x.name(); }
fn main() -> int {
  s = gb::GBox<Str>("s").name() + g(gb::GBox<Str>("s")) + g(gb::GBox<float>(1.5));
  return 0;
}
)");
  // A second variable, not an assignment: SemaFileResult is not the same
  // struct in every Sema test file.
  auto r = semaCheckFile(own, tmp);
  EXPECT_FALSE(r.Ok);
  for (const char *d :
       {"own.pkn:6:35: error: 'GBox<Str>' does not implement trait 'Named', "
        "which 'g' requires of 'T'",
        "own.pkn:6:59: error: 'GBox<float>' does not implement trait 'Named', "
        "which 'g' requires of 'T'"})
    EXPECT_NE(r.Diagnostics.find(d), std::string::npos) << d << "\n"
                                                        << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("no method"), std::string::npos)
      << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// A template of another module bounded by that module's own trait, directly
// or through a template its body uses, cannot be instantiated here (#147):
// reported where this module asks for the instantiation, once for each type
// parameter, and never satisfied by this module's trait of the same name,
// whether the template is a function, a class, a struct or an enum and the
// trait generic or not.  A chain of templates too deep for that request to
// be known (two of the module's, or one of a third module's calling one of
// it) is reported in the module's file.  A built-in bound, a non-generic
// function and the module's own code are unaffected.
TEST(Module, ForeignTraitBound) {
  auto tmp = paykan::test::tempDir() / "pkn_ms_foreign_bound";
  std::filesystem::remove_all(tmp);
  const std::string t27 = (tmp / "t27").string();
  const std::string defer = (tmp / "defer").string();
  const std::string deep = (tmp / "deep").string();
  const std::string cap = (tmp / "cap").string();
  const std::string kTraitLib = R"(trait Named { view fn name() -> Str; }
class Dog { view fn name() -> Str { return "Rex"; } }
impl Named for Dog {}
fn greet<T: Named>(x: view T) -> Str { return "hello, " + x.name(); }
fn wrap<T>(x: T) -> Str { return greet(Dog()); }
fn wrapn() -> Str { return greet(Dog()); }
class Holder<T> { v: T; fn say() -> Str { return greet(Dog()); } }
fn wrap2<T>(x: T) -> Str { return greet(Dog()) + greet(Dog()); }
fn sh<T: Show>(x: view T) -> Str { return x.toString(); }
fn shw<T>(x: T) -> Str { return sh(x); }
)";
  writeFile(t27, "lib.pkn", kTraitLib);
  writeFile(defer, "lib.pkn", R"(trait Named { view fn name() -> Str; }
class Dog { view fn name() -> Str { return "Rex"; } }
impl Named for Dog {}
class Gen<T: Named> { v: int; }
impl Gen<T> { fn __init__() { self.v = 1; } }
class Holder<T> {
  g: Gen<Dog>;
}
impl Holder<T> { fn __init__() { self.g = Gen<Dog>(); } }
class Mid<T> { g: Gen<Dog>; }
class Outer<T> { m: Mid<T>; }
)");
  writeFile(deep, "lib.pkn", R"(trait Named { view fn name() -> Str; }
class Dog { view fn name() -> Str { return "Rex"; } }
impl Named for Dog {}
fn greet<T: Named>(x: view T) -> Str { return "hello, " + x.name(); }
fn w2<T>(x: T) -> Str { return greet(Dog()); }
fn w1<T>(x: T) -> Str { return w2(x); }
)");
  writeFile(t27, "lib2.pkn", R"(import lib;
fn go<T>(x: T) -> Str { return lib::wrap(x); }
)");
  writeFile(cap, "lib.pkn", R"(trait Named { view fn name() -> Str; }
trait Conv<U> { view fn conv() -> U; }
class Gen<T: Named> { v: T; }
struct SW<T: Named> { v: T; }
enum EW<T: Named> { A(v: T), B }
fn cv<T: Conv<int>>(x: view T) -> int { return x.conv(); }
fn two<A: Named, B: Named>(a: view A, b: view B) -> Str { return a.name() + b.name(); }
)");
  const std::string kImporterTrait = R"(import ::io;
import lib;
trait Named { view fn name() -> Str; }
class Cat { view fn name() -> Str { return "Tom"; } }
impl Named for Cat {}
fn main() -> int { io::println(lib::greet(Cat())); return 0; }
)";
  // An unrelated trait of the same name is not that one either.
  const std::string kUnrelatedTrait = R"(import ::io;
import lib;
trait Named { view fn other() -> int; }
class Cat { view fn name() -> Str { return "Tom"; } view fn other() -> int { return 1; } }
impl Named for Cat {}
fn main() -> int { io::println(lib::greet(Cat())); return 0; }
)";
  struct Case {
    std::string Dir, File, Source, Expected;
  };
  auto needs = [](const std::string &param, const std::string &trait) {
    return "requires '" + param + "' to implement trait '" + trait +
           "' of module 'lib', and another module's traits are not supported "
           "yet (#147)\n";
  };
  const std::string kRequires = needs("T", "Named");
  const std::string kOtherNamed =
      "c1.pkn:2:1: note: this module's trait 'Named' is a different trait\n";
  const Case cases[] = {
      {t27, "b.pkn",
       "import ::io;\nimport lib;\nfn main() -> int { "
       "io::println(lib::greet(lib::Dog())); return 0; }\n",
       "b.pkn:3:32: error: 'lib::greet' " + kRequires},
      {t27, "a.pkn",
       "import ::io;\nimport lib;\nfn main() -> int { "
       "io::println(lib::wrap(1)); return 0; }\n",
       "a.pkn:3:32: error: 'lib::wrap<int>' cannot be instantiated here: it "
       "uses 'lib::greet', which " +
           kRequires + "lib.pkn:5:34: note: 'lib::greet' is used here\n"},
      // Two uses in the body are one request.
      {t27, "twice.pkn",
       "import ::io;\nimport lib;\nfn main() -> int { "
       "io::println(lib::wrap2(1)); return 0; }\n",
       "twice.pkn:3:32: error: 'lib::wrap2<int>' cannot be instantiated here: "
       "it uses 'lib::greet', which " +
           kRequires + "lib.pkn:8:35: note: 'lib::greet' is used here\n"},
      {t27, "d.pkn",
       "import ::io;\nimport lib;\nfn main() -> int { h = lib::Holder<int>(1); "
       "io::println(lib::wrapn()); return 0; }\n",
       "d.pkn:3:24: error: 'Holder<int>' cannot be instantiated here: it uses "
       "'lib::greet', which " +
           kRequires + "lib.pkn:7:50: note: 'lib::greet' is used here\n"},
      // In this module's own template, once for its instantiations.
      {t27, "mine.pkn",
       "import ::io;\nimport lib;\nfn mine<T>(x: T) -> Str { return "
       "lib::greet(lib::Dog()); }\nfn main() -> int { io::println(mine(1) + "
       "mine(True)); return 0; }\n",
       "mine.pkn:3:34: error: 'lib::greet' " + kRequires +
           "mine.pkn:4:32: note: in instantiation of 'mine<int>' requested "
           "here\n"},
      {t27, "c2.pkn", kImporterTrait,
       "c2.pkn:6:32: error: 'lib::greet' " + kRequires +
           "c2.pkn:3:1: note: this module's trait 'Named' is a different "
           "trait\n"},
      {t27, "c3.pkn", kUnrelatedTrait,
       "c3.pkn:6:32: error: 'lib::greet' " + kRequires +
           "c3.pkn:3:1: note: this module's trait 'Named' is a different "
           "trait\n"},
      // A field and the `__init__` that assigns it are one request.
      {defer, "main.pkn",
       "import lib;\nclass M { h: lib::Holder<int>; }\nimpl M { fn __init__() "
       "{ self.h = lib::Holder<int>(); } }\nfn main() -> int { m = M(); "
       "return 0; }\n",
       "main.pkn:2:11: error: 'Holder<int>' cannot be instantiated here: it "
       "uses 'lib::Gen', which " +
           kRequires + "lib.pkn:7:3: note: 'lib::Gen' is used here\n"},
      {defer, "main2.pkn",
       "import lib;\nstruct S { k: lib::Gen<lib::Dog>; }\nfn main() -> int { "
       "return 0; }\n",
       "main2.pkn:2:12: error: 'lib::Gen' " + kRequires},
      // The instantiations between the request and the use are noted.
      {defer, "main3.pkn",
       "import lib;\nclass M { o: lib::Outer<int>; }\nfn main() -> int { "
       "return 0; }\n",
       "main3.pkn:2:11: error: 'Outer<int>' cannot be instantiated here: it "
       "uses 'lib::Gen', which " +
           kRequires +
           "lib.pkn:10:16: note: 'lib::Gen' is used here\n"
           "lib.pkn:11:18: note: in instantiation of 'Mid<int>' requested "
           "here\n"},
      // The request of w2 is in lib's file: the use is reported there.
      {deep, "a.pkn",
       "import ::io;\nimport lib;\nfn main() -> int { "
       "io::println(lib::w1(1)); return 0; }\n",
       "lib.pkn:5:32: error: 'lib::greet' " + kRequires +
           "lib.pkn:6:32: note: in instantiation of 'lib::w2<int>' requested "
           "here\n"},
      // So is a request of wrap by a third module's template.
      {t27, "chain.pkn",
       "import ::io;\nimport lib2;\nfn main() -> int { "
       "io::println(lib2::go(1)); return 0; }\n",
       "lib.pkn:5:34: error: 'lib::greet' " + kRequires +
           "lib2.pkn:2:32: note: in instantiation of 'lib::wrap<int>' "
           "requested here\n"},
      // This module's trait of that name satisfies no bound of a class, a
      // struct or an enum of lib either, and each parameter is reported.
      {cap, "c1.pkn",
       "import lib;\ntrait Named { view fn name() -> Str; }\n"
       "class Cat { view fn name() -> Str { return \"Tom\"; } }\n"
       "impl Named for Cat {}\nclass Hold { a: lib::Gen<Cat>; }\n"
       "fn main() -> int {\n  s = lib::SW<Cat>(Cat());\n"
       "  e = lib::EW<Cat>::B;\n  t = lib::two(Cat(), Cat());\n"
       "  return 0;\n}\n",
       "c1.pkn:5:14: error: 'lib::Gen' " + needs("T", "Named") + kOtherNamed +
           "c1.pkn:7:7: error: 'lib::SW' " + needs("T", "Named") + kOtherNamed +
           "c1.pkn:8:7: error: 'lib::EW' " + needs("T", "Named") + kOtherNamed +
           "c1.pkn:9:7: error: 'lib::two' " + needs("A", "Named") +
           kOtherNamed + "c1.pkn:9:7: error: 'lib::two' " +
           needs("B", "Named") + kOtherNamed},
      // Nor does a generic trait of that name.
      {cap, "c2.pkn",
       "import lib;\ntrait Conv<U> { view fn conv() -> U; }\n"
       "class Cat { view fn conv() -> int { return 3; } }\n"
       "impl Conv<int> for Cat {}\n"
       "fn main() -> int { return lib::cv(Cat()); }\n",
       "c2.pkn:5:27: error: 'lib::cv' " + needs("T", "Conv") +
           "c2.pkn:2:1: note: this module's trait 'Conv' is a different "
           "trait\n"},
  };
  for (const Case &c : cases) {
    auto r = semaCheckFile(writeFile(c.Dir, c.File, c.Source), c.Dir);
    EXPECT_FALSE(r.Ok) << c.File;
    EXPECT_EQ(diagSummary(r.Diagnostics), c.Expected) << c.File << "\n"
                                                      << r.Diagnostics;
  }

  // A built-in bound, a function that is not generic, and the module's own
  // instantiations check.
  auto ok = semaCheckFile(
      writeFile(t27, "ok.pkn",
                "import ::io;\nimport lib;\nfn main() -> int { "
                "io::println(lib::wrapn() + lib::sh(5) + lib::shw(6)); "
                "return 0; }\n"),
      t27);
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;
  auto own = semaCheckFile(
      writeFile(tmp.string(), "own.pkn",
                kTraitLib + "fn main() -> int { s = wrap(1) + wrap2(2) + "
                            "Holder<int>(3).say() + shw(4); return 0; }\n"),
      tmp.string());
  EXPECT_TRUE(own.Ok) << own.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// A bound check made before the `impl` blocks were known, of a template of
// another module's template, is reported in that template's file, with the
// instantiation notes, as one made after: never at that line of this file.
TEST(Module, DeferredBoundErrorNamesTheTemplatesFile) {
  auto tmp = paykan::test::tempDir() / "pkn_ms_deferred_bound";
  std::filesystem::remove_all(tmp);
  const std::string own = (tmp / "own").string();
  const std::string viaStdDir = (tmp / "std").string();
  writeFile(own, "lib.pkn", R"(class Set<K: Hashable> { k: K; }
struct _F { a: float; }
class Holder<T> {
  s: Set<_F>;
}
)");
  writeFile(viaStdDir, "lib.pkn", R"(import ::collections;
struct _F { a: float; }
class Holder<T> {
  s: collections::HashSet<_F>;
}
)");
  const std::string kMain =
      "import lib;\nclass M { h: lib::Holder<int>; }\nfn main() -> int { "
      "return 0; }\n";
  auto r = semaCheckFile(writeFile(own, "main.pkn", kMain), own);
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(diagSummary(r.Diagnostics),
            "lib.pkn:4:3: error: '_F' does not implement trait 'Hashable', "
            "which 'lib::Set' requires of 'K'\n"
            "main.pkn:2:11: note: in instantiation of 'Holder<int>' requested "
            "here\n")
      << r.Diagnostics;

  // Through a template of the standard library, which goes on to the
  // templates it uses.
  auto viaStd =
      semaCheckFile(writeFile(viaStdDir, "main.pkn", kMain), viaStdDir);
  EXPECT_FALSE(viaStd.Ok);
  const std::string summary = diagSummary(viaStd.Diagnostics);
  EXPECT_EQ(summary.rfind(
                "lib.pkn:4:3: error: '_F' does not implement trait "
                "'Hashable', which '::collections::HashSet' requires of 'K'\n"
                "main.pkn:2:11: note: in instantiation of 'Holder<int>' "
                "requested here\n",
                0),
            0u)
      << viaStd.Diagnostics;
  std::istringstream lines(summary);
  for (std::string line; std::getline(lines, line);) {
    if (line.rfind("main.pkn:", 0) == 0) {
      EXPECT_EQ(line, "main.pkn:2:11: note: in instantiation of 'Holder<int>' "
                      "requested here")
          << viaStd.Diagnostics;
    }
  }
  std::filesystem::remove_all(tmp);
}

// ─── ERR: an interface that contradicts itself ──────────────────────────────
//
// The .pkm reader rejects an IMPL record of a trait other than Hashable, or of
// a type the file does not describe; a seeded cache entry bypasses it, and
// whether the type has the trait's methods is Sema's to check.

TEST(Module, ExportedConformanceIsChecked) {
  auto tmp = paykan::test::tempDir() / "pkn_ms_forged_impl";
  std::filesystem::remove_all(tmp);
  auto libPath =
      writeFile(tmp.string(), "lib.pkn", "fn real() -> int { return 1; }\n");
  auto key = std::filesystem::canonical(libPath).string();

  using Info = paykan::sema::Sema::ModuleInfo;
  auto structOf = [](const char *name, const char *hashResult) {
    Info::StructInfo si;
    si.Name = name;
    si.OriginModule = "lib";
    si.Fields = {{"a", "int"}};
    if (hashResult) // `view fn hash() -> <hashResult>` of an `impl` block
      si.Methods = {{"hash", hashResult, {}, 6, {}}};
    return si;
  };
  Info info;
  info.ExportedStructs = {structOf("Bad", "Str"), structOf("Key", "int"),
                          structOf("Plain", nullptr)};
  info.ExportedImpls = {{"Bad", "Hashable"},
                        {"Ghost", "Hashable"},
                        {"Key", "Named"},
                        {"Key", "Show"},
                        {"Plain", "Hashable"}};
  paykan::sema::Sema::ModuleCache[key] = std::move(info);

  auto main = writeFile(tmp.string(), "main.pkn", R"(
import lib;
fn main() -> int { return 0; }
)");
  auto r = semaCheckFile(main, tmp.string());
  paykan::sema::Sema::ModuleCache.erase(key);

  EXPECT_FALSE(r.Ok);
  for (const char *expected :
       {"import of module 'lib': the interface says 'Bad' implements "
        "'Hashable', but its 'view fn hash() -> Str' does not match 'view fn "
        "hash() -> int'",
        "import of module 'lib': the interface says 'Ghost' implements "
        "'Hashable', but it describes no class or struct 'Ghost'",
        "import of module 'lib': the interface says 'Key' implements 'Named', "
        "which is not a built-in trait an 'impl' block implements",
        "import of module 'lib': the interface says 'Key' implements 'Show', "
        "which is not a built-in trait an 'impl' block implements",
        "import of module 'lib': the interface says 'Plain' implements "
        "'Hashable', but it has no method 'view fn hash() -> int'"})
    EXPECT_NE(r.Diagnostics.find(expected), std::string::npos)
        << "missing: " << expected << "\n"
        << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}
