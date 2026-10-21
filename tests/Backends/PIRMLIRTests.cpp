// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// PIR -> the `pir` MLIR dialect: every item and instruction of docs/pir.md,
// and every sample program, imports into MLIR that verifies, and the printed
// MLIR parses back to the same text.  The shared lowering
// (docs/design/mlir-backend.md) leaves only func, scf, arith and the pir ops
// both targets lower, and its output round-trips too.

#include "Dialect/PIRDialect.h"
#include "Import/PIRImport.h"
#include "Lowering/PIRToStandard.h"
#include "PIR/Programs.h"
#include "Pipeline.h"
#include "ToEmitC/ToEmitC.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Verifier.h"

#include <llvm/Support/raw_ostream.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/MLIRContext.h>
#include <mlir/IR/Verifier.h>
#include <mlir/Parser/Parser.h>
#include <mlir/Pass/PassManager.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using namespace paykan;

namespace {

std::string print(mlir::ModuleOp mod) {
  std::string text;
  llvm::raw_string_ostream os(text);
  mod.print(os);
  return text;
}

/// Import the PIR @p text; its MLIR, printed ("" on failure, with the reason
/// in @p err).
std::string importText(const std::string &text, mlir::MLIRContext &ctx,
                       std::string &err) {
  pir::ParseError perr;
  auto program = pir::parseProgram(text, perr);
  if (!program) {
    err = "parse error: " + perr.str();
    return "";
  }
  if (auto verrs = pir::verify(*program); !verrs.empty()) {
    err = "verifier: " + pir::formatErrors(verrs);
    return "";
  }
  auto mod = mlir_backend::importProgram(*program, ctx);
  if (!mod) {
    err = mod.status().message();
    return "";
  }
  return print((*mod).get());
}

/// Import the PIR @p text and run the shared lowering on it: the module, or
/// null with the reason in @p err.
mlir::OwningOpRef<mlir::ModuleOp>
lowerText(const std::string &text, mlir::MLIRContext &ctx, std::string &err) {
  pir::ParseError perr;
  auto program = pir::parseProgram(text, perr);
  if (!program) {
    err = "parse error: " + perr.str();
    return {};
  }
  auto mod = mlir_backend::importProgram(*program, ctx);
  if (!mod) {
    err = mod.status().message();
    return {};
  }
  mlir::PassManager pm(&ctx);
  pm.addPass(mlir_backend::createPIRToStandardPass());
  if (mlir::failed(pm.run((*mod).get()))) {
    err = "the shared lowering failed";
    return {};
  }
  return std::move(*mod);
}

/// The PIR @p text through the shared lowering (Stage::SCF), printed (""
/// on failure, with the reason in @p err).
std::string scfText(const std::string &text, mlir::MLIRContext &ctx,
                    std::string &err) {
  pir::ParseError perr;
  auto program = pir::parseProgram(text, perr);
  if (!program) {
    err = "parse error: " + perr.str();
    return "";
  }
  auto mod = mlir_backend::importProgram(*program, ctx);
  if (!mod) {
    err = mod.status().message();
    return "";
  }
  if (Status s = mlir_backend::lowerTo((*mod).get(), mlir_backend::Stage::SCF);
      !s) {
    err = s.message();
    return "";
  }
  return print((*mod).get());
}

/// Every op of @p mod is one both targets lower: func, scf and arith, and
/// the pir globals, memory ops and optionals.
void expectOnlyStandardOps(mlir::ModuleOp mod) {
  namespace P = ::paykan::pir_dialect;
  mod.walk([&](mlir::Operation *op) {
    llvm::StringRef dialect = op->getName().getDialectNamespace();
    if (dialect == "builtin" || dialect == "func" || dialect == "scf" ||
        dialect == "arith")
      return;
    EXPECT_TRUE((
        mlir::isa<P::CStrOp, P::DataOp, P::BytesOp, P::ExternObjectOp,
                  P::ExternVTableOp, P::VTableOp, P::AllocaOp, P::PtrLoadOp,
                  P::PtrStoreOp, P::PtrAddOp, P::MemCopyOp, P::MemZeroOp,
                  P::AddressOfOp, P::NullOp, P::FnAddrOp, P::ICallOp, P::CastOp,
                  P::SomeOp, P::NoneOp, P::IsSomeOp, P::UnwrapOp>(op)))
        << op->getName().getStringRef().str();
  });
}

/// The PIR @p text through the C path: lowered to EmitC (left in @p emitc,
/// with its own context in @p ctx) and printed as C ("" on failure, with
/// the reason in @p err).
std::string emitCText(const std::string &text,
                      std::unique_ptr<mlir::MLIRContext> &ctx,
                      mlir::OwningOpRef<mlir::ModuleOp> &emitc,
                      std::string &err) {
  pir::ParseError perr;
  auto program = pir::parseProgram(text, perr);
  if (!program) {
    err = "parse error: " + perr.str();
    return "";
  }
  ctx = mlir_backend::createContext();
  auto mod = mlir_backend::importProgram(*program, *ctx);
  if (!mod) {
    err = mod.status().message();
    return "";
  }
  emitc = std::move(*mod);
  if (Status s = mlir_backend::lowerTo(*emitc, mlir_backend::Stage::EmitC);
      !s) {
    err = s.message();
    return "";
  }
  std::string c;
  llvm::raw_string_ostream os(c);
  if (Status s = mlir_backend::translateToC(*emitc, os); !s) {
    err = s.message();
    return "";
  }
  return c;
}

/// Every op of @p mod is EmitC's: the C path left nothing else.
void expectOnlyEmitCOps(mlir::ModuleOp mod) {
  mod.walk([&](mlir::Operation *op) {
    llvm::StringRef dialect = op->getName().getDialectNamespace();
    EXPECT_TRUE(dialect == "emitc" || mlir::isa<mlir::ModuleOp>(op))
        << op->getName().getStringRef().str();
  });
}

/// @p mlir parses back, verifies and prints as @p mlir.
void expectRoundTrip(const std::string &mlir, mlir::MLIRContext &ctx) {
  auto parsed = mlir::parseSourceString<mlir::ModuleOp>(mlir, &ctx);
  ASSERT_TRUE(parsed) << mlir;
  ASSERT_TRUE(mlir::succeeded(mlir::verify(*parsed))) << mlir;
  EXPECT_EQ(print(*parsed), mlir);
}

TEST(PIRMLIR, EveryItemAndInstruction) {
  mlir::MLIRContext ctx;
  std::string err;
  std::string mlir = importText(pir::test::kProgram, ctx, err);
  ASSERT_FALSE(mlir.empty()) << err;
  for (const char *op :
       {"pir.cstr @\".str0\" = \"Hello\\0A\"", "pir.data", "pir.bytes",
        "pir.extern_object", "pir.extern_vtable", "pir.class \"Point\"",
        "pir.class \"Point3\" : \"Point\"", "extern module \"lib\"", "field",
        "slot \"area\" :", "pir.func private @\"$rt.PaykanString_new\"",
        "module = \"lib\"", "ptr_load", "ptr_store", "field_load",
        "switch %9 : i64 [1, 7] {"})
    EXPECT_NE(mlir.find(op), std::string::npos) << op << "\n" << mlir;
  expectRoundTrip(mlir, ctx);
}

TEST(PIRMLIR, SharedLoweringOfEveryItemAndInstruction) {
  mlir::MLIRContext ctx;
  std::string err;
  auto mod = lowerText(pir::test::kProgram, ctx, err);
  ASSERT_TRUE(mod) << err;
  expectOnlyStandardOps(*mod);
  std::string mlir = print(*mod);
  // Link names; the layout of Point3 (the header, x, name, z at 32); its
  // vtable; the class Adder of module "lib"; the switch.
  for (const char *text :
       {"func.func private @PaykanString_new", "pir.vtable @pkvt_main_SPoint",
        "pir.extern_vtable @pkvt_lib_SAdder", "pk_main_SPoint__destroy",
        "func.func private @pk_lib_Shelper", "scf.index_switch", "case 7 {"})
    EXPECT_NE(mlir.find(text), std::string::npos) << text << "\n" << mlir;
  expectRoundTrip(mlir, ctx);
}

// The C path: every pir op the shared lowering keeps has its EmitC, and the
// unit around it is C's (Runtime.h, the optionals' structs, the vtables as
// PaykanMethod arrays, C's main() calling the program's).
TEST(PIRMLIR, EmitCOfEveryItemAndInstruction) {
  std::unique_ptr<mlir::MLIRContext> ctx;
  mlir::OwningOpRef<mlir::ModuleOp> mod;
  std::string err;
  std::string c = emitCText(pir::test::kProgram, ctx, mod, err);
  ASSERT_FALSE(c.empty()) << err;
  expectOnlyEmitCOps(*mod);
  for (const char *text :
       {"#include \"Runtime.h\"",
        "typedef struct { bool present; int64_t value; } pkrt_opt_i64;",
        "PaykanMethod pkvt_main_SPoint[", "(PaykanMethod)pk_main_SPoint__",
        "extern PaykanMethod pkvt_lib_SAdder[];",
        "static const char pk_main_S_2Estr0[7] = \"Hello\\n\";",
        "static int64_t paykan_main(void);", "int main(int argc, char **argv)",
        "int64_t rc = paykan_main();", "switch (", "case 7: {"})
    EXPECT_NE(c.find(text), std::string::npos) << text << "\n" << c;
  // The runtime's functions are Runtime.h's: never declared here.
  EXPECT_EQ(c.find("PaykanString_new("), c.find("PaykanString_new(v")) << c;
  // C11 spells an empty parameter list (void): no declaration or
  // definition (a top-level line) has `()`.
  std::istringstream lines(c);
  for (std::string line; std::getline(lines, line);) {
    if (line.empty() || line[0] == ' ')
      continue;
    EXPECT_EQ(line.find("()"), std::string::npos) << line;
  }
}

// Switches on an i64, a bool and a char: one in tail position whose every
// case returns, and one in a loop whose cases break.
const char *const kSwitches = R"(module "main"
fn @pick(%x.1: i64) -> i64 {
  switch %x.1 {
    case -1 {
      ret 10
    }
    case 4294967296 {
      ret 20
    }
    default {
      ret 30
    }
  }
}

fn @flag(%b.1: bool) -> i64 {
  local %r.0: i64
  store %r.0, 0
  switch %b.1 {
    case true {
      store %r.0, 1
    }
    case false {
      store %r.0, 2
    }
  }
  %v.2 = load %r.0
  ret %v.2
}

fn @letter(%c.1: char) -> i64 {
  local %r.0: i64
  store %r.0, 0
  while {
    cond true
  } {
    switch %c.1 {
      case 'a' {
        break
      }
      default {
        store %r.0, 1
        break
      }
    }
  }
  %v.2 = load %r.0
  ret %v.2
}

fn @main() -> i64 {
  %a.1 = call @pick(4294967296)
  ret %a.1
}
)";

// A switch is an scf.index_switch on its value as an index, a bool or a
// char zero-extended (as its cases are).  In tail position its cases yield
// the returned value, with no exit flag; one in a loop sets the loop's.
// Both targets switch.
TEST(PIRMLIR, SwitchesAreIndexSwitches) {
  auto ctx = mlir_backend::createContext();
  std::string err;
  std::string mlir = importText(kSwitches, *ctx, err);
  ASSERT_FALSE(mlir.empty()) << err;
  expectRoundTrip(mlir, *ctx);

  std::string scf = scfText(kSwitches, *ctx, err);
  ASSERT_FALSE(scf.empty()) << err;
  for (const char *text :
       {"scf.index_switch %", "-> i64", "case -1 {", "case 4294967296 {",
        "arith.index_castui", "case 0 {", "case 1 {", "case 97 {"})
    EXPECT_NE(scf.find(text), std::string::npos) << text << "\n" << scf;
  EXPECT_EQ(scf.find("pir.switch"), std::string::npos) << scf;
  // Only @letter, whose cases break, has an exit flag.
  size_t flag = scf.find("pir.alloca \"exit\"");
  EXPECT_NE(flag, std::string::npos) << scf;
  EXPECT_EQ(scf.find("pir.alloca \"exit\"", flag + 1), std::string::npos)
      << scf;

  mlir::OwningOpRef<mlir::ModuleOp> mod;
  std::string c = emitCText(kSwitches, ctx, mod, err);
  ASSERT_FALSE(c.empty()) << err;
  EXPECT_NE(c.find("switch ("), std::string::npos) << c;
}

// What `main` cannot reach: a class no op names (and the functions only its
// vtable reaches), a function nobody calls, and in the other modules what
// only a dropped declaration named, a module further at each round.
const char *const kUnreachable = R"(module "main"
extern fn @"lib::used"() -> i64 module "lib" symbol @used
extern fn @"lib::other"() -> i64 module "lib" symbol @other

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

TEST(PIRMLIR, UnreachableCodeIsDropped) {
  auto ctx = mlir_backend::createContext();
  pir::ParseError perr;
  auto program = pir::parseProgram(kUnreachable, perr);
  ASSERT_TRUE(program) << perr.str();
  ASSERT_TRUE(pir::verify(*program).empty())
      << pir::formatErrors(pir::verify(*program));
  auto mod = mlir_backend::importReachable(*program, *ctx);
  ASSERT_TRUE(mod) << mod.status().message();
  ASSERT_TRUE(mlir::succeeded(mlir::verify((*mod).get())));
  std::string text = print((*mod).get());
  for (const char *kept :
       {"pir.func @main()", "pir.class \"Kept\"", "pir.func @Kept.get(",
        "pir.func @Kept.destroy(", "@\"lib::used\"", "pir.func @used()",
        "pir.func @helper()", "module @deep"})
    EXPECT_NE(text.find(kept), std::string::npos) << kept << "\n" << text;
  for (const char *dropped :
       {"Dropped", "onlyDropped", "@unused", "@other", "lib::other",
        "alsoUnused", "deep::f", "pir.func @f("})
    EXPECT_EQ(text.find(dropped), std::string::npos) << dropped << "\n" << text;
  // What stays keeps the import's visibility: definitions public.
  EXPECT_EQ(text.find("pir.func private @used"), std::string::npos) << text;

  // And it still lowers.
  Status lowered =
      mlir_backend::lowerTo((*mod).get(), mlir_backend::Stage::LLVM);
  EXPECT_TRUE(lowered) << lowered.message();
}

TEST(PIRMLIR, LinkNamesAreCIdentifiers) {
  EXPECT_EQ(mlir_backend::linkName("main", "f"), "pk_main_Sf");
  EXPECT_EQ(mlir_backend::linkName("geometry::shapes", "Point.move"),
            "pk_geometry_3A_3Ashapes_SPoint_2Emove");
  // `_` is doubled, so module "a_S" name "b" and module "a" name "S_b" differ.
  EXPECT_NE(mlir_backend::linkName("a_S", "b"),
            mlir_backend::linkName("a", "S_b"));
  EXPECT_EQ(mlir_backend::vtableName("m", "Box<int>"), "pkvt_m_SBox_3Cint_3E");
}

/// The PIR of a source file, as `paykan --emit-pir` prints it.
std::string emitPIR(const std::filesystem::path &src, std::string &err) {
  std::string cmd =
      std::string(PAYKAN_BIN) + " --emit-pir \"" + src.string() + "\" 2>&1";
  FILE *fp = popen(cmd.c_str(), "r");
  if (!fp) {
    err = "cannot run " + cmd;
    return "";
  }
  std::string out;
  std::array<char, 4096> buf{};
  while (fgets(buf.data(), buf.size(), fp))
    out += buf.data();
  if (pclose(fp) != 0) {
    err = out;
    return "";
  }
  return out;
}

TEST(PIRMLIR, EverySample) {
  std::vector<std::filesystem::path> sources;
  for (const char *dir : {"codegen", "leak-check"})
    for (const auto &e : std::filesystem::directory_iterator(
             std::filesystem::path(PAYKAN_SAMPLES_DIR) / dir))
      if (e.path().extension() == ".pkn")
        sources.push_back(e.path());
  ASSERT_FALSE(sources.empty());
  mlir::MLIRContext ctx;
  for (const auto &src : sources) {
    SCOPED_TRACE(src.string());
    std::string err;
    std::string pirText = emitPIR(src, err);
    ASSERT_FALSE(pirText.empty()) << err;
    std::string mlir = importText(pirText, ctx, err);
    ASSERT_FALSE(mlir.empty()) << err;
    expectRoundTrip(mlir, ctx);
    auto lowered = lowerText(pirText, ctx, err);
    ASSERT_TRUE(lowered) << err;
    expectOnlyStandardOps(*lowered);
    expectRoundTrip(print(*lowered), ctx);
    // And through the C path (CStrictC11 compiles and runs the C).
    std::unique_ptr<mlir::MLIRContext> cctx;
    mlir::OwningOpRef<mlir::ModuleOp> emitc;
    std::string c = emitCText(pirText, cctx, emitc, err);
    ASSERT_FALSE(c.empty()) << err;
    expectOnlyEmitCOps(*emitc);
  }
}

} // namespace
