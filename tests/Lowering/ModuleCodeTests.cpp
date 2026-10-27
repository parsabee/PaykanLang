// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// What a module's `.pkm` keeps of its code (paykan/modules/ModuleCode.h,
// docs/design/pkm.md §5.13): the instantiations its interface exposes, the
// trim, the split into CODE and the code a backend compiles, and the check
// that a file's CODE defines what its interface says (§8.4); and that the
// lowering stops at an import it cannot lower.

#include "TestUtils.h"
#include "paykan/lowering/Lowering.h"
#include "paykan/modules/ModuleCode.h"
#include "paykan/modules/ModuleResolver.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Verifier.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <set>
#include <sstream>
#include <string>

using namespace paykan;

namespace {

// A module as the lowering makes it: its own code, the copies of the
// instantiations it uses, and the ones it only has (an unused method, a
// generic function's copy, a closure).
const char *const kModule = R"(module "m"
extern fn @"lib::f"() -> i64 module "lib" symbol @f
extern fn @"lib::g"() -> i64 module "lib" symbol @g
cstr @.str0 = "api" len 3
cstr @.str1 = "copy" len 4

shared class Box<int> {
  field v: i64
  vtable {
    destroy = @Box<int>.destroy : (obj) -> void
  }
}

shared class Box<Str> {
  field v: box
  vtable {
    destroy = @Box<Str>.destroy : (obj) -> void
  }
}

shared fn @Box<int>.destroy(%self.1: obj) -> void {
  ret
}

shared fn @Box<int>.get(%self.1: obj) -> i64 {
  %v.2 = field.load %self.1, Box<int>.v
  %s.3 = ptr.load char, @.str1
  ret %v.2
}

shared fn @Box<int>.unused(%self.1: obj) -> i64 {
  %v.2 = call @"lib::g"()
  ret %v.2
}

shared fn @Box<Str>.destroy(%self.1: obj) -> void {
  ret
}

shared fn @Box<Str>.peek(%self.1: obj) -> i64 {
  ret 0
}

shared fn @"$lib::first<int>"() -> i64 {
  ret 1
}

fn @pick<int>() -> i64 {
  ret 2
}

fn @fn.m.0.call(%closure.1: ptr) -> i64 {
  ret 3
}

fn @api(%o.1: obj) -> i64 {
  %v.2 = call @Box<int>.get(%o.1)
  %w.3 = call @"lib::f"()
  %s.4 = ptr.load char, @.str0
  %r.5 = add %v.2, %w.3
  ret %r.5
}

fn @__helper() -> i64 {
  ret 4
}
)";

pir::Module parsed() {
  pir::ParseError err;
  auto p = pir::parseProgram(kModule, err);
  EXPECT_TRUE(p) << err.str();
  return p ? std::move(p->Modules.front()) : pir::Module();
}

/// Its interface: `api` hands out a Box<Str> (its Box<int> it only uses).
pkm::Interface interface() {
  pkm::Interface iface;
  iface.Module = "m";
  pkm::FuncRec api;
  api.Name = "api";
  api.ReturnTypeName = "Box<Str>";
  api.ParamTypeNames = {"int"};
  iface.Functions.push_back(api);
  return iface;
}

bool has(const pir::Module &m, const std::string &name) {
  return m.findFunction(name) || m.findClass(name);
}

} // namespace

TEST(ModuleCode, ExposedTypesAreThoseTheInterfaceSpells) {
  pkm::Interface iface;
  pkm::FuncRec f;
  f.Name = "make";
  f.ReturnTypeName = "Box<Pair<int, Str>>";
  f.ParamTypeNames = {"view List<int>", "fn(Opt<int>) -> Res<Str>"};
  iface.Functions.push_back(f);
  pkm::ClassRec own;
  own.Name = "Own";
  own.IsLocal = true;
  own.SuperClassName = "Base<int>";
  own.Fields = {{"items", "Set<int>", false}, {"__hidden", "Map<int>", false}};
  own.Methods = {{"get", "Tree<int>", {}, 0, {}}};
  iface.Classes.push_back(own);
  // An instantiation the module only uses: its members count only once it
  // is exposed.
  pkm::ClassRec internal;
  internal.Name = "Lone<int>";
  internal.IsLocal = false;
  internal.Fields = {{"x", "Deep<int>", false}};
  iface.Classes.push_back(internal);
  // An exposed one's members are exposed in turn.
  pkm::StructRec pair;
  pair.Name = "Pair<int, Str>";
  pair.IsLocal = false;
  pair.Fields = {{"a", "Leaf<int>", false}};
  iface.Structs.push_back(pair);
  EXPECT_EQ(
      modules::exposedTypes(iface),
      (std::set<std::string>{"Base<int>", "Box<Pair<int, Str>>", "Leaf<int>",
                             "List<int>", "Opt<int>", "Pair<int, Str>",
                             "Res<Str>", "Set<int>", "Tree<int>"}));
}

TEST(ModuleCode, TrimKeepsOwnCodeAndExposedCopies) {
  pir::Module m = parsed();
  modules::trimModuleCode(m, interface());
  EXPECT_TRUE(pir::verify(m).empty()) << pir::formatErrors(pir::verify(m));
  // Its own code, private or not; the copies it uses; the exposed ones
  // whole, used or not.
  for (const char *kept :
       {"api", "__helper", "Box<int>", "Box<int>.get", "Box<int>.destroy",
        "Box<Str>", "Box<Str>.destroy", "Box<Str>.peek", "lib::f"})
    EXPECT_TRUE(has(m, kept)) << kept;
  // What nothing of that reaches.
  for (const char *dropped : {"Box<int>.unused", "$lib::first<int>",
                              "pick<int>", "fn.m.0.call", "lib::g"})
    EXPECT_FALSE(has(m, dropped)) << dropped;
  ASSERT_EQ(m.CStrs.size(), 2u);
}

TEST(ModuleCode, SplitLeavesCopiesInCodeAndCompilesTheRest) {
  pir::Module m = parsed();
  modules::trimModuleCode(m, interface());
  modules::ModuleCode parts = modules::splitModuleCode(std::move(m));
  const pir::Module &code = parts.Code, &compiled = parts.Compiled;
  EXPECT_TRUE(pir::verify(code).empty())
      << pir::formatErrors(pir::verify(code));
  EXPECT_TRUE(pir::verify(compiled).empty())
      << pir::formatErrors(pir::verify(compiled));

  // CODE: the copies with their bodies, and the module's functions
  // declared, each with what its compiled body uses.
  const pir::Function *get = code.findFunction("Box<int>.get");
  ASSERT_TRUE(get);
  EXPECT_FALSE(get->IsExtern);
  const pir::Function *api = code.findFunction("api");
  ASSERT_TRUE(api);
  EXPECT_TRUE(code.isCompiled(*api));
  EXPECT_EQ(api->Uses, (std::vector<std::string>{"Box<int>.get", "lib::f"}));
  ASSERT_TRUE(code.findFunction("__helper"));
  EXPECT_TRUE(code.isCompiled(*code.findFunction("__helper")));
  // What only compiled code uses of the constants is compiled with it;
  // a copy's stays.
  ASSERT_EQ(code.CStrs.size(), 1u);
  EXPECT_EQ(code.CStrs[0].Name, ".str1");
  EXPECT_TRUE(code.findFunction("lib::f")); // the compiled `api` uses it

  // The compiled part: the definitions, a copy it calls declared (shared,
  // linked by its own name), nothing it does not use.
  const pir::Function *capi = compiled.findFunction("api");
  ASSERT_TRUE(capi);
  EXPECT_FALSE(capi->IsExtern);
  const pir::Function *cget = compiled.findFunction("Box<int>.get");
  ASSERT_TRUE(cget);
  EXPECT_TRUE(cget->IsExtern && cget->Shared);
  EXPECT_FALSE(compiled.findFunction("Box<Str>.peek"));
  ASSERT_EQ(compiled.CStrs.size(), 1u);
  EXPECT_EQ(compiled.CStrs[0].Name, ".str0");
}

TEST(ModuleCode, ACodeProvidesWhatTheProgramDeclaresFromIt) {
  pir::Module m = parsed();
  modules::trimModuleCode(m, interface());
  modules::ModuleCode parts = modules::splitModuleCode(std::move(m));
  pir::ParseError err;
  auto program = pir::parseProgram(
      "module \"main\"\nextern fn @\"m::api\"(obj) -> i64 module \"m\" symbol "
      "@api\n",
      err);
  ASSERT_TRUE(program) << err.str();
  EXPECT_TRUE(modules::providesFor(parts.Code, *program));
  // A copy the module dropped, which another module of the program used.
  auto dropped = pir::parseProgram(
      "module \"main\"\nextern shared fn @\"$lib::first<int>\"() -> i64 module "
      "\"m\"\n",
      err);
  ASSERT_TRUE(dropped) << err.str();
  EXPECT_FALSE(modules::providesFor(parts.Code, *dropped));
}

// A module file is usable only when its CODE defines what its interface
// says the module defines (docs/design/pkm.md §8.4 step 7): a file whose
// IFACE names a class, a slot, a field, a method or a function its CODE
// does not have is rejected when it is loaded, not lowered against.
TEST(ModuleCode, CodeMatchesTheInterfaceItsFileHolds) {
  const pir::Module code = parsed();
  auto check = [&](const std::function<void(pkm::Interface &)> &change) {
    pkm::Interface iface = interface();
    pkm::ClassRec box;
    box.Name = "Box<int>";
    box.Fields = {{"v", "int", false}};
    // The vtable's slots, then the initializer and an `impl` method, which
    // have none.
    box.Methods = {{"destroy", "void", {}, 0, {}},
                   {"__init__", "void", {"int"}, 0, {}},
                   {"get", "int", {}, pkm::kMethodImpl, {}}};
    iface.Classes.push_back(box);
    pkm::StructRec other;
    other.Name = "Box<Str>";
    other.IsLocal = false; // another module's: its own CODE defines it
    other.Methods = {{"missing", "int", {}, 0, {}}};
    iface.Structs.push_back(other);
    change(iface);
    Status s = modules::checkCodeMatchesInterface(code, iface);
    return s ? std::string() : s.message();
  };
  EXPECT_EQ(check([](pkm::Interface &) {}), "");
  EXPECT_EQ(check([](pkm::Interface &i) { i.Classes[0].Name = "Box<long>"; }),
            "class 'Box<long>' is not in CODE");
  EXPECT_EQ(
      check([](pkm::Interface &i) { i.Classes[0].SuperClassName = "Base"; }),
      "class 'Box<int>': its superclass is '' in CODE, 'Base' in IFACE");
  EXPECT_EQ(
      check([](pkm::Interface &i) { i.Classes[0].Fields[0].FieldName = "w"; }),
      "class 'Box<int>': its fields are not the ones in CODE");
  EXPECT_EQ(
      check([](pkm::Interface &i) { i.Classes[0].Methods[0].Name = "drop"; }),
      "class 'Box<int>': slot 0 of its vtable is 'destroy' in CODE, 'drop' in "
      "IFACE");
  EXPECT_EQ(check([](pkm::Interface &i) {
              i.Classes[0].Methods.push_back({"peek", "int", {}, 0, {}});
            }),
            "class 'Box<int>': its vtable has 1 slots in CODE, 2 in IFACE");
  // A struct's methods take its address first.
  EXPECT_EQ(check([](pkm::Interface &i) {
              i.Structs[0].IsLocal = true;
              i.Structs[0].Methods = {{"peek", "int", {}, 0, {}}};
            }),
            "method 'Box<Str>.peek' is (obj) -> i64 in CODE, (ptr) -> i64 by "
            "its types in IFACE");
  EXPECT_EQ(check([](pkm::Interface &i) { i.Structs[0].IsLocal = true; }),
            "method 'Box<Str>.missing' is not in CODE");
  EXPECT_EQ(check([](pkm::Interface &i) { i.Functions[0].Name = "apu"; }),
            "function 'apu' is not in CODE");
  // An abstract class (a pure virtual method) has no constructor function.
  EXPECT_EQ(check([](pkm::Interface &i) {
              i.Classes[0].Methods[0].Flags = pkm::kMethodPure;
              i.Functions[0].Name = "Box<int>";
            }),
            "");
  EXPECT_EQ(check([](pkm::Interface &i) { i.Functions[0].Name = "Box<int>"; }),
            "function 'Box<int>' is not in CODE");
  // An extern of another module is not the module's own.
  EXPECT_EQ(check([](pkm::Interface &i) { i.Functions[0].Name = "lib::f"; }),
            "function 'lib::f' is not in CODE");
}

// An interface describes its types consistently (docs/design/pkm.md §8.4
// step 4): an importer reconstructs a class from its superclass, so the
// superclass is one the interface describes, ancestors end at the root and
// the superclass's vtable begins the class's; and every class and struct
// but an enum's variant has an initializer.
TEST(ModuleCode, AnInterfaceDescribesItsTypesConsistently) {
  auto check = [](const std::function<void(pkm::Interface &)> &change) {
    pkm::Interface iface;
    iface.Module = "m";
    pkm::ClassRec shape;
    shape.Name = "Shape";
    shape.IsLocal = false;
    shape.OriginModule = "lib";
    shape.Methods = {{"destroy", "void", {}, 0, {}},
                     {"area", "int", {}, pkm::kMethodPure, {}},
                     {"__init__", "void", {}, 0, {}}};
    pkm::ClassRec sq = shape;
    sq.Name = "Sq";
    sq.SuperClassName = "Shape";
    sq.IsLocal = true;
    sq.OriginModule = "m";
    sq.Methods = {{"destroy", "void", {}, 0, {}},
                  {"area", "int", {}, 0, {}},
                  {"side", "int", {}, 0, {}},
                  {"___init__", "void", {"int"}, 0, {}}};
    iface.Classes = {shape, sq};
    pkm::EnumRec opt;
    opt.Name = "Opt";
    opt.Variants = {"Some", "Other"};
    opt.PayloadVariants = {"Some"};
    iface.Enums = {opt};
    pkm::StructRec some;
    some.Name = "Opt::Some";
    some.Fields = {{"value", "int", false}};
    pkm::StructRec pair;
    pair.Name = "Pair";
    pair.Methods = {{"__init__", "void", {}, 0, {}}};
    iface.Structs = {some, pair};
    change(iface);
    Status s = modules::checkInterface(iface);
    return s ? std::string() : s.message();
  };
  EXPECT_EQ(check([](pkm::Interface &) {}), "");
  EXPECT_EQ(
      check([](pkm::Interface &i) { i.Classes[1].SuperClassName = "Shapes"; }),
      "class 'Sq' extends 'Shapes', which it does not describe");
  EXPECT_EQ(check([](pkm::Interface &i) { i.Classes[0].Methods[0].Name = ""; }),
            "the vtable of class 'Sq' does not start with the slots of its "
            "superclass 'Shape'");
  EXPECT_EQ(check([](pkm::Interface &i) {
              std::swap(i.Classes[1].Methods[0], i.Classes[1].Methods[1]);
            }),
            "the vtable of class 'Sq' does not start with the slots of its "
            "superclass 'Shape'");
  EXPECT_EQ(
      check([](pkm::Interface &i) { i.Classes[0].SuperClassName = "Sq"; }),
      "class 'Shape' is its own ancestor");
  EXPECT_EQ(check([](pkm::Interface &i) { i.Classes[1].Methods.pop_back(); }),
            "class 'Sq' has no initializer");
  EXPECT_EQ(check([](pkm::Interface &i) { i.Structs[1].Methods.clear(); }),
            "struct 'Pair' has no initializer");
  EXPECT_EQ(check([](pkm::Interface &i) { i.Enums[0].PayloadVariants = {}; }),
            "struct 'Opt::Some' has no initializer");
  EXPECT_EQ(check([](pkm::Interface &i) { i.Structs[1].Name = "Sq"; }),
            "'Sq' is a class and a struct or enum");
  EXPECT_EQ(check([](pkm::Interface &i) { i.Structs[1].Name = "Opt"; }),
            "'Opt' is a struct and an enum");
}

// Where the types an interface gives decide PIR types by their names alone,
// a file's CODE has those types (docs/design/pkm.md §8.4 step 7): its
// fields, its functions, its methods and its vtable slots, and a class's
// constructor takes what its initializer takes and returns its box.  The
// vtable of a class the module defines has no empty slot (the lowering
// fills a pure virtual method's with its trap).  A file that disagreed was
// lowered against, and the program crashed or the lowering stopped.
TEST(ModuleCode, CodeHasTheTypesTheInterfaceGives) {
  const char *const text = R"(module "m"
class Point {
  field x: i64
  field name: box
  vtable {
    destroy = @Point.destroy : (obj) -> void
    equals = @Point.equals : (obj, box) -> i64
    norm = @Point.norm : (obj, f64) -> f64
  }
}
fn @Point.destroy(%self.1: obj) -> void {
  ret
}
fn @Point.equals(%self.1: obj, %o.2: box) -> i64 {
  ret 0
}
fn @Point.norm(%self.1: obj, %p.2: f64) -> f64 {
  ret %p.2
}
fn @Point.__init__(%self.1: obj, %x.2: i64, %name.3: box) -> void {
  ret
}
fn @Point(%x.1: i64, %name.2: box) -> box {
  %o.3 = new Point
  %b.4 = box %o.3
  ret %b.4
}
fn @scale(%p.1: box, %by.2: i64, %xs.3: ptr, %n.4: i64, %acc.5: ptr) -> box {
  ret %p.1
}
)";
  auto parse = [&] {
    pir::ParseError err;
    auto program = pir::parseProgram(text, err);
    EXPECT_TRUE(program) << err.str();
    return program ? std::move(program->Modules.front()) : pir::Module();
  };
  const pir::Module code = parse();
  ASSERT_TRUE(pir::verify(code).empty())
      << pir::formatErrors(pir::verify(code));
  auto check =
      [&](const std::function<void(pkm::Interface &, pir::Module &)> &change) {
        pkm::Interface iface;
        iface.Module = "m";
        pkm::ClassRec point;
        point.Name = "Point";
        point.Fields = {{"x", "int", false}, {"name", "Str", false}};
        point.Methods = {{"destroy", "void", {}, 0, {}},
                         {"equals", "bool", {"Obj"}, pkm::kMethodView, {}},
                         {"norm", "float", {"f32"}, 0, {}},
                         {"__init__", "void", {"int", "Str"}, 0, {}}};
        iface.Classes = {point};
        pkm::FuncRec ctor;
        ctor.Name = "Point";
        ctor.ReturnTypeName = "Point";
        ctor.ParamTypeNames = {"int", "Str"};
        // A slice parameter is its address and its length, an `inout` one its
        // address.
        pkm::FuncRec scale;
        scale.Name = "scale";
        scale.ReturnTypeName = "Point";
        scale.ParamTypeNames = {"Point", "u8", "int[]", "int"};
        scale.Modes.Modes = {pkm::kModeValue, pkm::kModeValue, pkm::kModeView,
                             pkm::kModeInout};
        scale.Modes.Names = {"p", "by", "xs", "acc"};
        iface.Functions = {ctor, scale};
        pir::Module m = parse();
        change(iface, m);
        Status s = modules::checkCodeMatchesInterface(m, iface);
        return s ? std::string() : s.message();
      };
  auto fn = [](pir::Module &m, const std::string &name) -> pir::Function & {
    return *std::find_if(
        m.Functions.begin(), m.Functions.end(),
        [&](const pir::Function &f) { return f.Name == name; });
  };
  EXPECT_EQ(check([](pkm::Interface &, pir::Module &) {}), "");
  EXPECT_EQ(check([](pkm::Interface &i, pir::Module &) {
              i.Classes[0].Fields[0].TypeName = "Str";
            }),
            "class 'Point': field 'x' is i64 in CODE, box by its type in "
            "IFACE");
  EXPECT_EQ(check([](pkm::Interface &i, pir::Module &) {
              i.Classes[0].Fields[0].Weak = true;
            }),
            "class 'Point': field 'x' is i64 in CODE, weak by its type in "
            "IFACE");
  // A type whose PIR type its name does not decide is not compared.
  EXPECT_EQ(check([](pkm::Interface &i, pir::Module &) {
              i.Classes[0].Fields[0].TypeName = "int?";
            }),
            "");
  EXPECT_EQ(check([](pkm::Interface &i, pir::Module &) {
              i.Classes[0].Methods[2].ReturnTypeName = "int";
            }),
            "method 'Point.norm' is (obj, f64) -> f64 in CODE, (obj, f64) -> "
            "i64 by its types in IFACE");
  EXPECT_EQ(check([](pkm::Interface &, pir::Module &m) {
              m.Classes[0].VTable[2].Sig.Params[1] = pir::Type::I64;
            }),
            "class 'Point': slot 2 ('norm') of its vtable is (obj, i64) -> f64 "
            "in CODE, (obj, f64) -> f64 by its types in IFACE");
  EXPECT_EQ(check([](pkm::Interface &, pir::Module &m) {
              m.Classes[0].VTable[1].Target.clear();
            }),
            "class 'Point': slot 1 ('equals') of its vtable is empty in CODE");
  EXPECT_EQ(check([&](pkm::Interface &, pir::Module &m) {
              fn(m, "Point").Sig.Ret = pir::Type::Void;
            }),
            "class 'Point': its constructor is (i64, box) -> void in CODE, its "
            "initializer (obj, i64, box) -> void");
  EXPECT_EQ(check([&](pkm::Interface &i, pir::Module &) {
              i.Functions[1].ReturnTypeName = "int";
            }),
            "function 'scale' is (box, i64, ptr, i64, ptr) -> box in CODE, "
            "(box, i64, ptr, i64, ptr) -> i64 by its types in IFACE");
  EXPECT_EQ(check([&](pkm::Interface &i, pir::Module &) {
              i.Functions[1].Modes.Modes[3] = pkm::kModeValue;
            }),
            "function 'scale' is (box, i64, ptr, i64, ptr) -> box in CODE, "
            "(box, i64, ptr, i64, i64) -> box by its types in IFACE");
  // A native function's signature is C's.
  EXPECT_EQ(check([&](pkm::Interface &i, pir::Module &) {
              i.Functions[1].ReturnTypeName = "int";
              i.Functions[1].Native = true;
            }),
            "");
}

// The lowering stops at an import it cannot lower: the importer is not
// lowered without it (it went on, and stopped at an assertion on the first
// name the import declares).  Lowered with a resolver that did not load
// `::error`, the program has neither its code nor its analysis.
TEST(ModuleCode, LoweringStopsAtAnImportItCannotLower) {
  auto [parseOk, driver] = test::parse(R"(import ::error;
fn main() -> int {
  e = error::none();
  if e.isOk() {
    return 0;
  }
  return 1;
}
)");
  ASSERT_TRUE(parseOk);
  std::ostringstream diagOS;
  sema::DiagEngine diag(diagOS);
  sema::Sema sema(driver->getASTContext(), diag, "");
  auto ctx = sema.run(driver->getRoot());
  ASSERT_TRUE(ctx) << diagOS.str();
  modules::ModuleResolver other({}, modules::ModuleResolver::hostIdentity());
  pir::Program program;
  std::ostringstream errs;
  EXPECT_FALSE(lowering::lowerProgram(ctx, driver->getRoot(), "main", "",
                                      program, errs, &other));
  EXPECT_EQ(errs.str(), "internal compiler error: lowering failed for "
                        "imported module '::error'\n");
  // With the resolver that loaded it, it lowers.
  pir::Program lowered;
  std::ostringstream none;
  EXPECT_TRUE(
      lowering::lowerProgram(ctx, driver->getRoot(), "main", "", lowered, none))
      << none.str();
}
