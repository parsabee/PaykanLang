// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The PIR -> LLVM translation, tested on hand-written PIR text: parse,
// verify, translate, JIT, and check the exit code and output.

#include "JIT.h"
#include "PIRToLLVM.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Verifier.h"

#include "TestUtils.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <sstream>
#include <string>

using namespace paykan;

namespace {

struct Run {
  int ExitCode = -1;
  std::string Out;
  std::string Err;
  bool Ok = false;
};

/// Parse, verify, translate and run a PIR program.
Run runPIR(const std::string &text, const std::vector<std::string> &args = {}) {
  Run r;
  pir::ParseError perr;
  auto program = pir::parseProgram(text, perr);
  if (!program) {
    r.Err = "parse error: " + perr.str();
    return r;
  }
  auto verrs = pir::verify(*program);
  if (!verrs.empty()) {
    r.Err = "verifier: " + pir::formatErrors(verrs);
    return r;
  }
  auto ctx = std::make_unique<llvm::LLVMContext>();
  auto module = backend::llvm_backend::translateProgram(*program, *ctx, "t");
  if (!module) {
    r.Err = module.status().message();
    return r;
  }
  auto [savedOut, outPath] = test::redirectFdToTempFile(STDOUT_FILENO);
  auto [savedErr, errPath] = test::redirectFdToTempFile(STDERR_FILENO);
  auto result = jit::runModule(std::move(*module), std::move(ctx), args);
  fflush(stdout);
  fflush(stderr);
  test::restoreFd(STDOUT_FILENO, savedOut);
  test::restoreFd(STDERR_FILENO, savedErr);
  r.Out = test::drainAndRemoveTempFile(outPath);
  r.Err = test::drainAndRemoveTempFile(errPath);
  if (!result) {
    r.Err += "JIT: " + llvm::toString(result.takeError());
    return r;
  }
  r.ExitCode = *result;
  r.Ok = true;
  return r;
}

/// Parse and translate a PIR program; its LLVM IR as text ("" on failure,
/// with the reason in @p err).
std::string translatePIR(const std::string &text, std::string &err) {
  pir::ParseError perr;
  auto program = pir::parseProgram(text, perr);
  if (!program) {
    err = "parse error: " + perr.str();
    return "";
  }
  llvm::LLVMContext ctx;
  auto module = backend::llvm_backend::translateProgram(*program, ctx, "t");
  if (!module) {
    err = module.status().message();
    return "";
  }
  std::string ir;
  llvm::raw_string_ostream os(ir);
  (*module)->print(os, nullptr);
  return os.str();
}

/// FileCheck-style: every line of @p ir that contains @p needle.
std::vector<std::string> linesWith(const std::string &ir,
                                   const std::string &needle) {
  std::vector<std::string> out;
  std::istringstream in(ir);
  for (std::string line; std::getline(in, line);)
    if (line.find(needle) != std::string::npos)
      out.push_back(line);
  return out;
}

} // namespace

TEST(PIRLLVM, ArithmeticAndReturn) {
  auto r = runPIR(R"(module "t"
fn @main() -> i64 {
  %a = add 40, 2
  %b = mul %a, 2
  %c = sub %b, 4
  %d = div %c, 2
  %e = rem %d, 7
  ret %e
}
)");
  ASSERT_TRUE(r.Ok) << r.Err;
  EXPECT_EQ(r.ExitCode, 40 % 7);
}

TEST(PIRLLVM, PrintsThroughTheRuntime) {
  auto r = runPIR(R"(module "t"
cstr @.s = "hello\n" len 6
extern fn @PaykanString_new(ptr, i64) -> obj
extern fn @Paykan_print(obj) -> void
extern fn @PaykanString_destroy(obj) -> void
fn @main() -> i64 {
  %s = call @PaykanString_new(@.s, 6)
  call @Paykan_print(%s)
  call @PaykanString_destroy(%s)
  ret 0
}
)");
  ASSERT_TRUE(r.Ok) << r.Err;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.Out, "hello\n");
}

TEST(PIRLLVM, LoopsLocalsAndBranches) {
  // sum of 0..9 with break at 5 and continue on odd: 0+2+4 = 6
  auto r = runPIR(R"(module "t"
fn @main() -> i64 {
  local %i: i64
  local %sum: i64
  store %i, 0
  store %sum, 0
  while {
    %c = load %i
    %lt = cmp lt %c, 10
    cond %lt
  } {
    %cur = load %i
    %next = add %cur, 1
    store %i, %next
    %five = cmp eq %cur, 5
    if %five {
      break
    }
    %r = rem %cur, 2
    %odd = cmp ne %r, 0
    if %odd {
      continue
    }
    %s = load %sum
    %s2 = add %s, %cur
    store %sum, %s2
  }
  %out = load %sum
  ret %out
}
)");
  ASSERT_TRUE(r.Ok) << r.Err;
  EXPECT_EQ(r.ExitCode, 6);
}

TEST(PIRLLVM, FloatsSelectAndCasts) {
  auto r = runPIR(R"(module "t"
fn @main() -> i64 {
  %x = itof 3
  %y = mul %x, 2.5
  %gt = cmp gt %y, 7.0
  %i = select %gt, 1, 2
  %bits = cast %y to i64
  %back = cast %bits to f64
  %same = cmp eq %back, %y
  %j = cast %same to i64
  %c = cast 'A' to i64
  %k = add %i, %j
  %m = add %k, %c
  ret %m
}
)");
  ASSERT_TRUE(r.Ok) << r.Err;
  EXPECT_EQ(r.ExitCode, 1 + 1 + 65);
}

TEST(PIRLLVM, FToITruncatesTowardZero) {
  auto r = runPIR(R"(module "t"
fn @main() -> i64 {
  %a = ftoi 2.9
  %b = ftoi -2.9
  %c = mul %a, 10
  %d = add %c, %b
  ret %d
}
)");
  ASSERT_TRUE(r.Ok) << r.Err;
  EXPECT_EQ(r.ExitCode, 18); // 2 * 10 + -2
}

TEST(PIRLLVM, ClassesVTablesAndFields) {
  auto r = runPIR(R"(module "t"
extern fn @PaykanObject_toString(obj) -> box
extern fn @PaykanObject_equals(obj, box) -> i64
extern fn @Paykan_release(box) -> void

class Counter {
  field n: i64
  vtable {
    destroy = @Counter_destroy : (obj) -> void
    toString = @PaykanObject_toString : (obj) -> box
    equals = @PaykanObject_equals : (obj, box) -> i64
    bump = @Counter_bump : (obj, i64) -> i64
  }
}

fn @Counter_destroy(%self: obj) -> void {
  free %self
  ret
}

fn @Counter_bump(%self: obj, %by: i64) -> i64 {
  %n = field.load %self, Counter.n
  %n2 = add %n, %by
  field.store %self, Counter.n, %n2
  ret %n2
}

fn @main() -> i64 {
  %o = new Counter
  %vt = vtable.load %o
  %want = vtable.addr Counter
  %same = cmp eq %vt, %want
  %r1 = vcall %o : Counter [3] (5)
  %r2 = vcall %o : (obj, i64) -> i64 [3] (6)
  %b = box %o
  %o2 = unbox %b
  %n = field.load %o2, Counter.n
  release %b
  %x = select %same, %n, 0
  ret %x
}
)");
  ASSERT_TRUE(r.Ok) << r.Err;
  EXPECT_EQ(r.ExitCode, 11);
}

// The optimisation hints the legacy AST code generator emitted: string
// literals are mergeable (unnamed_addr, align 1) and vtable-pointer loads are
// invariant, so LLVM can hoist and merge them.
TEST(PIRLLVM, ConstantsAreMergeableAndVTableLoadsInvariant) {
  std::string err;
  std::string ir = translatePIR(R"(module "t"
cstr @.s = "hi" len 2
cstr @.t = "hi" len 2
extern fn @PaykanObject_toString(obj) -> box
extern fn @PaykanObject_equals(obj, box) -> i64

class C {
  vtable {
    destroy = @C_destroy : (obj) -> void
    toString = @PaykanObject_toString : (obj) -> box
    equals = @PaykanObject_equals : (obj, box) -> i64
    get = @C_get : (obj) -> i64
  }
}

fn @C_destroy(%self: obj) -> void {
  free %self
  ret
}

fn @C_get(%self: obj) -> i64 {
  ret 7
}

fn @main() -> i64 {
  %o = new C
  %vt = vtable.load %o
  %r = vcall %o : C [3] ()
  free %o
  ret %r
}
)",
                                err);
  ASSERT_FALSE(ir.empty()) << err;

  for (const char *name : {"@.s = ", "@.t = "}) {
    auto defs = linesWith(ir, name);
    ASSERT_EQ(defs.size(), 1u) << name << "\n" << ir;
    EXPECT_NE(
        defs[0].find("private unnamed_addr constant [3 x i8] c\"hi\\00\""),
        std::string::npos)
        << defs[0];
    EXPECT_NE(defs[0].find(", align 1"), std::string::npos) << defs[0];
  }

  // Both vtable-pointer loads (vtable.load and the one vcall does) carry
  // !invariant.load; the slot load does not (it is a load from the vtable,
  // not of the object's header).
  auto vtLoads = linesWith(ir, "= load ptr, ptr %o");
  ASSERT_EQ(vtLoads.size(), 2u) << ir;
  for (const auto &l : vtLoads)
    EXPECT_NE(l.find("!invariant.load !"), std::string::npos) << l;
  EXPECT_FALSE(linesWith(ir, "!{}").empty()) << ir;
}

TEST(PIRLLVM, CrossModuleCallsAndMainArgs) {
  auto r = runPIR(R"(module "main"
extern fn @helper(i64) -> i64 module "lib"
extern fn @Paykan_release(box) -> void
extern fn @PaykanShared_get(box) -> obj
extern fn @PaykanArray_length(obj) -> i64
fn @main(%args: box) -> i64 {
  %arr = unbox %args
  %n = call @PaykanArray_length(%arr)
  %h = call @helper(%n)
  release %args
  ret %h
}
module "lib"
fn @helper(%a: i64) -> i64 {
  %r = mul %a, 10
  ret %r
}
)",
                  {"script", "x", "y"});
  ASSERT_TRUE(r.Ok) << r.Err;
  EXPECT_EQ(r.ExitCode, 30);
}

TEST(PIRLLVM, UnreachableAfterNoreturnCall) {
  auto r = runPIR(R"(module "t"
fn @main() -> i64 {
  %z = cmp eq 1, 1
  if %z {
    ret 3
  } else {
    ret 4
  }
}
)");
  ASSERT_TRUE(r.Ok) << r.Err;
  EXPECT_EQ(r.ExitCode, 3);
}
