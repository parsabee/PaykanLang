// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// PIR -> the `pir` MLIR dialect: every item and instruction of docs/pir.md,
// and every sample program, imports into MLIR that verifies, and the printed
// MLIR parses back to the same text.

#include "Import/PIRImport.h"
#include "PIR/Programs.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Verifier.h"

#include <llvm/Support/raw_ostream.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/MLIRContext.h>
#include <mlir/IR/Verifier.h>
#include <mlir/Parser/Parser.h>

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
  }
}

} // namespace
