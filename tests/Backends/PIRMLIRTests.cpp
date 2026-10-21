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

/// The PIR @p text through the shared lowering, switch formation included
/// (Stage::SCF), printed ("" on failure, with the reason in @p err).
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
        "module = \"lib\"", "ptr_load", "ptr_store", "field_load"})
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
  // vtable; and the class Adder of module "lib".
  for (const char *text :
       {"func.func private @PaykanString_new", "pir.vtable @pkvt_main_SPoint",
        "pir.extern_vtable @pkvt_lib_SAdder", "pk_main_SPoint__destroy",
        "func.func private @pk_lib_Shelper"})
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
        "int64_t rc = paykan_main();"})
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

// A chain of ifs testing one integer against constants, `match` on an
// enum or on integer literals, as PIR (src/Lowering/LoweringMatch.cpp).
// The second test of 2 is never taken.
const char *const kEqualityChain = R"(module "main"
fn @pick(%x.1: i64) -> i64 {
  local %r.0: i64
  store %r.0, 0
  %t.2 = cmp eq %x.1, 0
  if %t.2 {
    store %r.0, 10
  } else {
    %t.3 = cmp eq %x.1, 2
    if %t.3 {
      store %r.0, 20
    } else {
      %t.4 = cmp eq %x.1, 2
      if %t.4 {
        store %r.0, 99
      } else {
        %t.5 = cmp eq %x.1, 5
        if %t.5 {
          store %r.0, 30
        } else {
          store %r.0, 40
        }
      }
    }
  }
  %v.6 = load %r.0
  ret %v.6
}

fn @main() -> i64 {
  %a.1 = call @pick(5)
  ret %a.1
}
)";

// The chain is one scf.index_switch, a case for each value tested first,
// and both targets switch: C on the index's size_t, LLVM on its i64.
TEST(PIRMLIR, AnEqualityChainIsASwitch) {
  auto ctx = mlir_backend::createContext();
  std::string err;
  std::string mlir = scfText(kEqualityChain, *ctx, err);
  ASSERT_FALSE(mlir.empty()) << err;
  for (const char *text :
       {"scf.index_switch", "case 0 {", "case 2 {", "case 5 {", "default {"})
    EXPECT_NE(mlir.find(text), std::string::npos) << text << "\n" << mlir;
  EXPECT_EQ(mlir.find("scf.if"), std::string::npos) << mlir;
  EXPECT_EQ(mlir.find("arith.cmpi"), std::string::npos) << mlir;
  EXPECT_EQ(mlir.find("99"), std::string::npos) << mlir;
  expectRoundTrip(mlir, *ctx);

  mlir::OwningOpRef<mlir::ModuleOp> mod;
  std::string c = emitCText(kEqualityChain, ctx, mod, err);
  ASSERT_FALSE(c.empty()) << err;
  EXPECT_NE(c.find("switch ("), std::string::npos) << c;
  EXPECT_NE(c.find("case 5: {"), std::string::npos) << c;
}

// What is not a chain of equality tests on one integer stays scf.if: a
// single test, tests of two values, an ordering test, a negative constant.
TEST(PIRMLIR, OtherTestsStayIfs) {
  const char *programs[] = {
      R"(module "main"
fn @f(%x.1: i64) -> i64 {
  local %r.0: i64
  store %r.0, 0
  %t.2 = cmp eq %x.1, 1
  if %t.2 {
    store %r.0, 1
  } else {
    store %r.0, 2
  }
  %v.3 = load %r.0
  ret %v.3
}
)",
      R"(module "main"
fn @f(%x.1: i64, %y.2: i64) -> i64 {
  local %r.0: i64
  store %r.0, 0
  %t.3 = cmp eq %x.1, 1
  if %t.3 {
    store %r.0, 1
  } else {
    %t.4 = cmp eq %y.2, 2
    if %t.4 {
      store %r.0, 2
    }
  }
  %v.5 = load %r.0
  ret %v.5
}
)",
      R"(module "main"
fn @f(%x.1: i64) -> i64 {
  local %r.0: i64
  store %r.0, 0
  %t.2 = cmp lt %x.1, 1
  if %t.2 {
    store %r.0, 1
  } else {
    %t.3 = cmp lt %x.1, 2
    if %t.3 {
      store %r.0, 2
    }
  }
  %v.4 = load %r.0
  ret %v.4
}
)",
      R"(module "main"
fn @f(%x.1: i64) -> i64 {
  local %r.0: i64
  store %r.0, 0
  %t.2 = cmp eq %x.1, -1
  if %t.2 {
    store %r.0, 1
  } else {
    %t.3 = cmp eq %x.1, -2
    if %t.3 {
      store %r.0, 2
    }
  }
  %v.4 = load %r.0
  ret %v.4
}
)"};
  for (const char *text : programs) {
    auto ctx = mlir_backend::createContext();
    std::string err;
    std::string mlir = scfText(text, *ctx, err);
    ASSERT_FALSE(mlir.empty()) << err << "\n" << text;
    EXPECT_EQ(mlir.find("scf.index_switch"), std::string::npos) << mlir;
    EXPECT_NE(mlir.find("scf.if"), std::string::npos) << mlir;
  }
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
