// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The PIR pass manager and the dead-code pass (docs/pir.md §12), and the
// shared and compiled items they work with (§4).

#include "paykan/pir/Binary.h"
#include "paykan/pir/DeadCode.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Pass.h"
#include "paykan/pir/Printer.h"
#include "paykan/pir/Verifier.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

using namespace paykan;
using namespace paykan::pir;

namespace {

Program parse(const char *text) {
  ParseError err;
  auto p = parseProgram(text, err);
  EXPECT_TRUE(p) << err.str();
  return p ? std::move(*p) : Program();
}

std::string printed(const Program &p) {
  std::ostringstream os;
  print(p, os);
  return os.str();
}

const Module &module(const Program &p, const std::string &name) {
  for (const Module &m : p.Modules)
    if (m.Name == name)
      return m;
  static const Module none;
  ADD_FAILURE() << "no module " << name;
  return none;
}

// What `main` cannot reach: a class no instruction names (and the functions
// only its vtable reaches), a function nobody calls, and in the other
// modules what only a dropped declaration named, a module further each
// time.
const char *const kUnreachable = R"(module "main"
extern fn @"lib::used"() -> i64 module "lib" symbol @used
extern fn @"lib::other"() -> i64 module "lib" symbol @other

cstr @.str0 = "kept" len 4
cstr @.str1 = "dropped" len 7

class Kept {
  field n: i64
  vtable {
    destroy = @Kept.destroy : (obj) -> void
    get = @Kept.get : (obj) -> i64
  }
}

class Dropped {
  field n: i64
  vtable {
    destroy = @Dropped.destroy : (obj) -> void
    get = @Dropped.get : (obj) -> i64
  }
}

fn @Kept.destroy(%self.1: obj) -> void {
  free %self.1
  ret
}

fn @Kept.get(%self.1: obj) -> i64 {
  %n.2 = field.load %self.1, Kept.n
  ret %n.2
}

fn @Dropped.destroy(%self.1: obj) -> void {
  free %self.1
  ret
}

fn @Dropped.get(%self.1: obj) -> i64 {
  %n.2 = call @onlyDropped()
  ret %n.2
}

fn @onlyDropped() -> i64 {
  %s.1 = ptr.load char, @.str1
  ret 2
}

fn @unused() -> i64 {
  %n.1 = call @"lib::other"()
  ret %n.1
}

fn @main() -> i64 {
  %o.1 = new Kept
  field.store %o.1, Kept.n, 1
  %v.2 = vcall %o.1 : Kept [1] ()
  %u.3 = call @"lib::used"()
  %s.4 = add %v.2, %u.3
  %c.5 = ptr.load char, @.str0
  ret %s.4
}

module "lib"
extern fn @"deep::f"() -> i64 module "deep" symbol @f

fn @used() -> i64 {
  %n.1 = call @helper()
  ret %n.1
}

fn @helper() -> i64 {
  ret 3
}

fn @other() -> i64 {
  %n.1 = call @"deep::f"()
  ret %n.1
}

fn @alsoUnused() -> i64 {
  ret 4
}

module "deep"

fn @f() -> i64 {
  ret 5
}
)";

// A module whose code is compiled (docs/pir.md §4): its PIR declares its
// functions, each with the names its compiled body uses, and holds its
// copies of instantiations, which a program may share.
const char *const kCompiled = R"(module "main"

extern fn @"lib::count"() -> i64 module "lib" symbol @count

fn @main() -> i64 {
  %n.1 = call @"lib::count"()
  ret %n.1
}

module "lib"

shared class Box<int> {
  field v: i64
  vtable {
    destroy = @Box<int>.destroy : (obj) -> void
  }
}

extern fn @count() -> i64 module "lib" uses @Box<int>, @Box<int>.get
extern fn @spare() -> i64 module "lib" uses @Box<int>.set
extern fn @__helper() -> i64 module "lib"

shared fn @Box<int>.destroy(%self.1: obj) -> void {
  ret
}

shared fn @Box<int>.get(%self.1: obj) -> i64 {
  %v.2 = field.load %self.1, Box<int>.v
  %h.3 = call @__helper()
  %s.4 = add %v.2, %h.3
  ret %s.4
}

shared fn @Box<int>.set(%self.1: obj, %v.2: i64) -> void {
  field.store %self.1, Box<int>.v, %v.2
  ret
}
)";

} // namespace

TEST(PIRShared, SharedAndCompiledItemsRoundTrip) {
  Program p = parse(kCompiled);
  ASSERT_EQ(p.Modules.size(), 2u);
  EXPECT_TRUE(verify(p).empty()) << formatErrors(verify(p));
  const Module &lib = module(p, "lib");
  const Function *count = lib.findFunction("count");
  ASSERT_TRUE(count);
  EXPECT_TRUE(lib.isCompiled(*count));
  EXPECT_TRUE(lib.defines(*count));
  EXPECT_EQ(count->Uses,
            (std::vector<std::string>{"Box<int>", "Box<int>.get"}));
  ASSERT_TRUE(lib.findClass("Box<int>"));
  EXPECT_TRUE(lib.findClass("Box<int>")->Shared);
  EXPECT_TRUE(lib.findFunction("Box<int>.get")->Shared);
  EXPECT_FALSE(count->Shared);
  // The text is the printer's, and the binary form keeps it all.
  EXPECT_EQ(printed(p), kCompiled);
  auto decoded = binary::decode(binary::encode(lib));
  ASSERT_TRUE(decoded) << decoded.status().message();
  std::ostringstream a, b;
  print(lib, a);
  print(*decoded, b);
  EXPECT_EQ(a.str(), b.str());
}

TEST(PIRShared, VerifierChecksSharedAndCompiledItems) {
  auto errors = [](const char *text) {
    ParseError err;
    auto p = parseProgram(text, err);
    EXPECT_TRUE(p) << err.str();
    return p ? formatErrors(verify(*p)) : std::string();
  };
  // Uses belong to a compiled declaration, and name what the module
  // declares.
  EXPECT_NE(errors("module \"m\"\nextern fn @f() -> i64 module \"x\" uses @g\n"
                   "fn @main() -> i64 {\n  ret 0\n}\n")
                .find("has uses but is not a compiled declaration"),
            std::string::npos);
  EXPECT_NE(errors("module \"m\"\nextern fn @f() -> i64 module \"m\" uses @g\n"
                   "fn @main() -> i64 {\n  ret 0\n}\n")
                .find("uses '@g', which the module does not declare"),
            std::string::npos);
  // An extern of a compiled function links; one of a shared function is
  // shared.
  EXPECT_EQ(errors("module \"main\"\nextern fn @f() -> i64 module \"m\"\n"
                   "fn @main() -> i64 {\n  %r.1 = call @f()\n  ret %r.1\n}\n"
                   "module \"m\"\nextern fn @f() -> i64 module \"m\"\n"),
            "");
  EXPECT_NE(errors("module \"main\"\nextern fn @f() -> i64 module \"m\"\n"
                   "fn @main() -> i64 {\n  %r.1 = call @f()\n  ret %r.1\n}\n"
                   "module \"m\"\nshared fn @f() -> i64 {\n  ret 1\n}\n")
                .find("is not shared but its definition in module 'm' is"),
            std::string::npos);
  EXPECT_NE(errors("module \"m\"\nextern shared fn @$rt.Paykan_f() -> i64\n"
                   "fn @main() -> i64 {\n  ret 0\n}\n")
                .find("is shared"),
            std::string::npos);
}

TEST(PIRDeadCode, AProgramKeepsWhatItsEntryPointReaches) {
  Program p = parse(kUnreachable);
  ASSERT_TRUE(verify(p).empty()) << formatErrors(verify(p));
  dropDeadCode(p, Roots::entryPoint(p));
  EXPECT_TRUE(verify(p).empty()) << formatErrors(verify(p));
  const Module &main = module(p, "main");
  for (const char *kept : {"main", "Kept.get", "Kept.destroy", "lib::used"})
    EXPECT_TRUE(main.findFunction(kept)) << kept;
  for (const char *dropped :
       {"Dropped.get", "onlyDropped", "unused", "lib::other"})
    EXPECT_FALSE(main.findFunction(dropped)) << dropped;
  EXPECT_TRUE(main.findClass("Kept"));
  EXPECT_FALSE(main.findClass("Dropped"));
  ASSERT_EQ(main.CStrs.size(), 1u);
  EXPECT_EQ(main.CStrs[0].Name, ".str0");
  const Module &lib = module(p, "lib");
  EXPECT_TRUE(lib.findFunction("used"));
  EXPECT_TRUE(lib.findFunction("helper"));
  EXPECT_FALSE(lib.findFunction("other"));
  EXPECT_FALSE(lib.findFunction("alsoUnused"));
  EXPECT_FALSE(lib.findFunction("deep::f"));
  // A module everything of which is dropped stays, empty.
  const Module &deep = module(p, "deep");
  EXPECT_TRUE(deep.Functions.empty());
  EXPECT_EQ(p.Modules.size(), 3u);
}

TEST(PIRDeadCode, ACompiledDeclarationKeepsWhatItsCodeUses) {
  Program p = parse(kCompiled);
  dropDeadCode(p, Roots::entryPoint(p));
  EXPECT_TRUE(verify(p).empty()) << formatErrors(verify(p));
  const Module &lib = module(p, "lib");
  // `count` is called: what its compiled body uses stays, and what that
  // reaches (the class's vtable, the helper the copy calls).
  for (const char *kept :
       {"count", "Box<int>.get", "Box<int>.destroy", "__helper"})
    EXPECT_TRUE(lib.findFunction(kept)) << kept;
  EXPECT_TRUE(lib.findClass("Box<int>"));
  // `spare` is not, nor what only it uses.
  EXPECT_FALSE(lib.findFunction("spare"));
  EXPECT_FALSE(lib.findFunction("Box<int>.set"));
}

TEST(PIRDeadCode, RootsChooseWhatAModuleKeeps) {
  Program p = parse(kUnreachable);
  Roots roots;
  roots.Functions = [](const Module &m, const Function &f) {
    return m.Name == "lib" && f.Name == "other";
  };
  roots.Classes = [](const Module &m, const Class &c) {
    return m.Name == "main" && c.Name == "Dropped";
  };
  dropDeadCode(p, roots);
  const Module &main = module(p, "main");
  EXPECT_FALSE(main.findFunction("main"));
  EXPECT_TRUE(main.findClass("Dropped"));
  EXPECT_TRUE(main.findFunction("Dropped.get"));
  EXPECT_TRUE(main.findFunction("onlyDropped"));
  EXPECT_FALSE(main.findClass("Kept"));
  EXPECT_TRUE(module(p, "lib").findFunction("other"));
  EXPECT_FALSE(module(p, "lib").findFunction("used"));
  EXPECT_TRUE(module(p, "deep").findFunction("f"));
}

TEST(PIRDeadCode, ReferencesAreEachNameOnceInOrder) {
  Program p = parse(kUnreachable);
  const Function *main = module(p, "main").findFunction("main");
  ASSERT_TRUE(main);
  EXPECT_EQ(references(*main),
            (std::vector<std::string>{"Kept", "lib::used", ".str0"}));
}

namespace {

/// A pass that erases a function another module declares.
class BreakingPass : public Pass {
public:
  std::string_view name() const override { return "breaking"; }
  Status run(Program &program, Statistics &) override {
    std::erase_if(program.Modules.back().Functions,
                  [](const Function &f) { return f.Name == "f"; });
    return Status::ok();
  }
};

} // namespace

TEST(PIRPassManager, RunsPassesInOrderAndVerifiesWhenAsked) {
  Program p = parse(kUnreachable);
  PassManager pm;
  pm.add(createDeadCodePass(Roots::entryPoint(p)));
  ASSERT_TRUE(pm.run(p));
  EXPECT_FALSE(module(p, "main").findClass("Dropped"));

  Program q = parse(kUnreachable);
  PassManager checked(/*verifyEach=*/true);
  checked.add(std::make_unique<BreakingPass>());
  Status s = checked.run(q);
  ASSERT_FALSE(s);
  EXPECT_NE(s.message().find("PIR pass 'breaking' left a program that does "
                             "not verify"),
            std::string::npos)
      << s.message();
}
