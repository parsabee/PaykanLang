// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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
/// the pir globals, memory ops, optionals and ownership masks.
void expectOnlyStandardOps(mlir::ModuleOp mod) {
  namespace P = ::paykan::pir_dialect;
  mod.walk([&](mlir::Operation *op) {
    llvm::StringRef dialect = op->getName().getDialectNamespace();
    if (dialect == "builtin" || dialect == "func" || dialect == "scf" ||
        dialect == "arith")
      return;
    EXPECT_TRUE(
        (mlir::isa<P::CStrOp, P::DataOp, P::BytesOp, P::ExternObjectOp,
                   P::ExternVTableOp, P::VTableOp, P::AllocaOp, P::PtrLoadOp,
                   P::PtrStoreOp, P::PtrAddOp, P::MemCopyOp, P::MemZeroOp,
                   P::AddressOfOp, P::NullOp, P::FnAddrOp, P::ICallOp,
                   P::CastOp, P::SomeOp, P::NoneOp, P::IsSomeOp, P::UnwrapOp,
                   P::OwnedOp, P::OwnershipOp>(op)))
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
  // A lending function takes the ownership mask last, a ui64, and reads a
  // box parameter's bit in it; a call to a lending callee passes one, its
  // owned bits and the bits it passes on (docs/pir.md §3).
  for (const char *op :
       {"pir.func @lend(%arg0: !pir.box, %arg1: !pir.box, %arg2: !pir.obj, "
        "%arg3: !pir.ptr, %arg4: ui64)",
        "= owned %arg4[0]", "= owned %arg4[1]", "= ownership 0 [1](%1)",
        "= ownership 0 []()", "= ownership 2 [0](%0)", "= ownership 1 []()",
        "slot \"equals\" = @\"$rt.PaykanObject_equals\" : (!pir.obj, "
        "!pir.box, ui64) -> i64",
        "pir.func private @\"$rt.PaykanObject_equals\"(!pir.obj, !pir.box, "
        "i64) -> i64"})
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
        "int64_t rc = paykan_main();", "switch (", "case 7: {",
        "= v5 & UINT64_C(1);", "= v5 & UINT64_C(2);",
        "? UINT64_C(2) : UINT64_C(0);",
        "(PaykanObject *, PaykanShared *, uint64_t)"})
    EXPECT_NE(c.find(text), std::string::npos) << text << "\n" << c;
  // The ownership mask is a uint64_t, as Runtime.h's `owned` is.
  EXPECT_NE(c.find("void pk_main_Slend(PaykanShared* v1, PaykanShared* v2, "
                   "PaykanObject* v3, void* v4, uint64_t v5) {"),
            std::string::npos)
      << c;
  // The runtime's functions are Runtime.h's: never declared here.
  EXPECT_EQ(c.find("PaykanString_new("), c.find("PaykanString_new(v")) << c;
  // A retain and a release call Runtime.h's inline fast paths, which call
  // the functions only for the rest.
  EXPECT_NE(c.find("Paykan_retain_inline("), std::string::npos) << c;
  EXPECT_NE(c.find("Paykan_release_inline("), std::string::npos) << c;
  EXPECT_EQ(c.find("Paykan_retain("), std::string::npos) << c;
  EXPECT_EQ(c.find("Paykan_release("), std::string::npos) << c;
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

// Cases of INT64_MAX and INT64_MIN, the empty and tombstone keys of the
// DenseSet scf.index_switch keeps its cases in: one with another case and a
// default in tail position, and one of only the two as a statement.
const char *const kDenseSetKeySwitches = R"(module "main"
fn @pick(%x.1: i64) -> i64 {
  switch %x.1 {
    case 9223372036854775807 {
      ret 10
    }
    case 4 {
      ret 20
    }
    case -9223372036854775808 {
      ret 30
    }
    default {
      ret 40
    }
  }
}

fn @keys(%x.1: i64) -> i64 {
  local %r.0: i64
  store %r.0, 0
  switch %x.1 {
    case -9223372036854775808 {
      store %r.0, 1
    }
    case 9223372036854775807 {
      store %r.0, 2
    }
  }
  %v.2 = load %r.0
  ret %v.2
}

fn @main() -> i64 {
  %a.1 = call @pick(4)
  %b.2 = call @keys(9223372036854775807)
  %c.3 = add %a.1, %b.2
  ret %c.3
}
)";

// Such a case is an scf.if on the value's being it, around the dispatch among
// the other cases: the scf.index_switch of @pick's case 4, and nothing but
// @keys's default (none).  Both targets take it: the C spells INT64_MIN.
TEST(PIRMLIR, DenseSetKeyCasesAreIfs) {
  auto ctx = mlir_backend::createContext();
  std::string err;
  std::string scf = scfText(kDenseSetKeySwitches, *ctx, err);
  ASSERT_FALSE(scf.empty()) << err;
  expectRoundTrip(scf, *ctx);
  for (const char *text : {"arith.constant 9223372036854775807 : i64",
                           "arith.constant -9223372036854775808 : i64",
                           "arith.cmpi eq", "scf.if", "case 4 {"})
    EXPECT_NE(scf.find(text), std::string::npos) << text << "\n" << scf;
  size_t sw = scf.find("scf.index_switch");
  ASSERT_NE(sw, std::string::npos) << scf;
  EXPECT_EQ(scf.find("scf.index_switch", sw + 1), std::string::npos) << scf;
  EXPECT_EQ(scf.find("case 9223372036854775807"), std::string::npos) << scf;
  EXPECT_EQ(scf.find("case -9223372036854775808"), std::string::npos) << scf;

  mlir::OwningOpRef<mlir::ModuleOp> mod;
  std::string c = emitCText(kDenseSetKeySwitches, ctx, mod, err);
  ASSERT_FALSE(c.empty()) << err;
  expectOnlyEmitCOps(*mod);
  for (const char *text : {"INT64_MIN", "9223372036854775807", "switch ("})
    EXPECT_NE(c.find(text), std::string::npos) << text << "\n" << c;
}

// Native functions bound to C library functions a header declares with
// other C types (finding 80): the C renames each one's symbol while it
// includes the headers, and declares it from its PIR signature after them,
// with the compilers' warning of a library function's other declaration off.
// A native extern that shares a runtime extern's declaration is the
// runtime's: Runtime.h declares it.
const char *kLibcNatives = R"(module "main"
extern fn @$rt.PaykanString_length(obj) -> i64
extern fn @$c.PaykanString_length(obj) -> i64
extern fn @$c.llabs(i64) -> i64
extern fn @$c.exit(i64) -> void
fn @main() -> i64 {
  %a.1 = call @$c.llabs(-3)
  call @$c.exit(%a.1)
  ret 0
}
)";

TEST(PIRMLIR, NativeSymbolsAreHiddenFromTheHeaders) {
  std::unique_ptr<mlir::MLIRContext> ctx;
  mlir::OwningOpRef<mlir::ModuleOp> mod;
  std::string err;
  std::string c = emitCText(kLibcNatives, ctx, mod, err);
  ASSERT_FALSE(c.empty()) << err;
  expectOnlyEmitCOps(*mod);
  const size_t math = c.find("#include <math.h>");
  const size_t runtime = c.find("#include \"Runtime.h\"");
  ASSERT_NE(math, std::string::npos) << c;
  ASSERT_NE(runtime, std::string::npos) << c;
  for (const char *sym : {"exit", "llabs"}) {
    const std::string define =
        std::string("#define ") + sym + " pk__hidden_" + sym + "\n";
    const std::string undef = std::string("#undef ") + sym + "\n";
    EXPECT_LT(c.find(define), math) << sym << "\n" << c;
    EXPECT_GT(c.find(undef), runtime) << sym << "\n" << c;
    EXPECT_NE(c.find(undef), std::string::npos) << sym << "\n" << c;
  }
  for (const char *text :
       {"extern int64_t llabs(int64_t);", "extern void exit(int64_t);",
        "#pragma clang diagnostic ignored "
        "\"-Wincompatible-library-redeclaration\"",
        "#pragma GCC diagnostic ignored \"-Wbuiltin-declaration-mismatch\""})
    EXPECT_NE(c.find(text), std::string::npos) << text << "\n" << c;
  EXPECT_EQ(c.find("PaykanString_length pk__hidden"), std::string::npos) << c;
  EXPECT_EQ(c.find("PaykanString_length(PaykanObject"), std::string::npos) << c;

  // A unit without native functions has neither.
  std::unique_ptr<mlir::MLIRContext> plainCtx;
  mlir::OwningOpRef<mlir::ModuleOp> plainMod;
  std::string plain = emitCText(pir::test::kProgram, plainCtx, plainMod, err);
  ASSERT_FALSE(plain.empty()) << err;
  EXPECT_EQ(plain.find("pk__hidden_"), std::string::npos) << plain;
  EXPECT_EQ(plain.find("#pragma"), std::string::npos) << plain;
}

TEST(PIRMLIR, LinkNamesAreCIdentifiers) {
  EXPECT_EQ(mlir_backend::linkName("main", "f"), "pk_main_Sf");
  EXPECT_EQ(mlir_backend::linkName("geometry::shapes", "Point.move"),
            "pk_geometry_3A_3Ashapes_SPoint_2Emove");
  // `_` is doubled, so module "a_S" name "b" and module "a" name "S_b" differ.
  EXPECT_NE(mlir_backend::linkName("a_S", "b"),
            mlir_backend::linkName("a", "S_b"));
  EXPECT_EQ(mlir_backend::vtableName("m", "Box<int>"), "pkvt_m_SBox_3Cint_3E");
  // A shared symbol's has no module, whichever module holds its copy.
  EXPECT_EQ(mlir_backend::linkName("", "Box<int>.get"),
            "pk__SBox_3Cint_3E_2Eget");
  EXPECT_EQ(mlir_backend::vtableName("", "Box<int>"), "pkvt__SBox_3Cint_3E");
}

// A shared function or class links under its own name, whichever module
// holds the copy and whichever declares it.
TEST(PIRMLIR, SharedSymbolsLinkWithoutTheirModule) {
  const char *src = R"(module "main"
extern shared fn @"Box<int>.get"(obj) -> i64 module "lib"
extern shared class "Box<int>" module "lib" {
  field v: i64
}

fn @main() -> i64 {
  %o.1 = new "Box<int>"
  %v.2 = call @"Box<int>.get"(%o.1)
  ret %v.2
}

module "lib"

shared class "Box<int>" {
  field v: i64
  vtable {
    destroy = @"Box<int>.destroy" : (obj) -> void
  }
}

shared fn @"Box<int>.destroy"(%self.1: obj) -> void {
  ret
}

shared fn @"Box<int>.get"(%self.1: obj) -> i64 {
  %v.2 = field.load %self.1, "Box<int>".v
  ret %v.2
}
)";
  auto ctx = mlir_backend::createContext();
  pir::ParseError perr;
  auto program = pir::parseProgram(src, perr);
  ASSERT_TRUE(program) << perr.str();
  ASSERT_TRUE(pir::verify(*program).empty())
      << pir::formatErrors(pir::verify(*program));
  auto mod = mlir_backend::importProgram(*program, *ctx);
  ASSERT_TRUE(mod) << mod.status().message();
  ASSERT_TRUE(mlir::succeeded(mlir::verify((*mod).get())));
  ASSERT_TRUE(mlir_backend::lowerTo((*mod).get(), mlir_backend::Stage::LLVM));
  std::string text = print((*mod).get());
  EXPECT_NE(text.find("@pk__SBox_3Cint_3E_2Eget"), std::string::npos) << text;
  EXPECT_NE(text.find("@pkvt__SBox_3Cint_3E"), std::string::npos) << text;
  EXPECT_EQ(text.find("pk_lib_SBox"), std::string::npos) << text;
  EXPECT_EQ(text.find("pkvt_lib_SBox"), std::string::npos) << text;
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
