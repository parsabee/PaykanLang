// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Tests of the compile-and-run harness itself (CodeGenTestUtils.h).

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

#if PAYKAN_TEST_HAVE_LLVM
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#endif

#include <memory>
#include <string>

using namespace paykan::test;

#if PAYKAN_TEST_HAVE_LLVM
// A JIT failure must reach the test as a message, not an anonymous -1: here
// main calls a function no module or runtime table defines, so the JIT cannot
// resolve it.  The returned error ("JIT: Failed to materialize symbols: ...
// main") lands in StdErr next to ORC's session report, which names the
// missing symbol.
TEST(Harness, JITErrorMessageReachesStdErr) {
  const std::string missing = "paykan_harness_test_no_such_symbol";
  auto ctx = std::make_unique<llvm::LLVMContext>();
  auto module = std::make_unique<llvm::Module>("harness", *ctx);
  auto *i32 = llvm::Type::getInt32Ty(*ctx);
  auto *fnTy = llvm::FunctionType::get(i32, /*isVarArg=*/false);
  llvm::FunctionCallee callee = module->getOrInsertFunction(missing, fnTy);
  auto *mainFn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                        "main", *module);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(*ctx, "entry", mainFn));
  b.CreateRet(b.CreateCall(callee));

  auto r = detail::runJITModule(std::move(module), std::move(ctx), nullptr);
  EXPECT_FALSE(r.CompileOk);
  EXPECT_EQ(r.ExitCode, -1);
  EXPECT_NE(r.StdErr.find("JIT: "), std::string::npos) << r.StdErr;
  EXPECT_NE(r.StdErr.find(missing), std::string::npos) << r.StdErr;
}
#else
TEST(Harness, JITErrorMessageReachesStdErr) {
  GTEST_SKIP() << "the JIT belongs to the LLVM backend";
}
#endif
