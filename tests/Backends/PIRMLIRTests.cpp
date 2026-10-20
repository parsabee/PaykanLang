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
  }
}

} // namespace
