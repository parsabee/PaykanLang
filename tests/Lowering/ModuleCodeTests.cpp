// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// What a module's `.pkm` keeps of its code (paykan/modules/ModuleCode.h,
// docs/design/pkm.md §5.13): the instantiations its interface exposes, the
// trim, and the split into CODE and the code a backend compiles.

#include "paykan/modules/ModuleCode.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Verifier.h"

#include <gtest/gtest.h>

#include <set>
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
