// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The PIR -> LLVM translation (the mlir backend's LLVM path), tested on
// hand-written PIR text: parse, verify, translate, JIT, and check the exit
// code and output, or the LLVM IR.

#include "JIT.h"
#include "Pipeline.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Verifier.h"

#include "TestUtils.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace paykan;

namespace {

struct Run {
  int ExitCode = -1;
  std::string Out;
  std::string Err;
  bool Ok = false;
};

/// Parse, verify and translate a PIR program, then run it in the JIT, its
/// output going where the test's goes; its exit code, or -1 with the reason
/// in @p err.
int jitPIR(const std::string &text, const std::vector<std::string> &args,
           std::string &err) {
  pir::ParseError perr;
  auto program = pir::parseProgram(text, perr);
  if (!program) {
    err = "parse error: " + perr.str();
    return -1;
  }
  auto verrs = pir::verify(*program);
  if (!verrs.empty()) {
    err = "verifier: " + pir::formatErrors(verrs);
    return -1;
  }
  auto ctx = std::make_unique<llvm::LLVMContext>();
  auto module = mlir_backend::compileProgram(*program, *ctx, "t", "");
  if (!module) {
    err = module.status().message();
    return -1;
  }
  auto result = jit::runModule(std::move(*module), std::move(ctx), args);
  fflush(stdout);
  fflush(stderr);
  if (!result) {
    err = "JIT: " + llvm::toString(result.takeError());
    return -1;
  }
  return *result;
}

/// Parse, verify, translate and run a PIR program.
Run runPIR(const std::string &text, const std::vector<std::string> &args = {}) {
  Run r;
  std::string err;
  auto [savedOut, outPath] = test::redirectFdToTempFile(STDOUT_FILENO);
  auto [savedErr, errPath] = test::redirectFdToTempFile(STDERR_FILENO);
  r.ExitCode = jitPIR(text, args, err);
  test::restoreFd(STDOUT_FILENO, savedOut);
  test::restoreFd(STDERR_FILENO, savedErr);
  r.Out = test::drainAndRemoveTempFile(outPath);
  r.Err = test::drainAndRemoveTempFile(errPath) + err;
  r.Ok = err.empty();
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
  auto module = mlir_backend::compileProgram(*program, ctx, "t", "");
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
extern fn @$rt.PaykanString_new(ptr, i64) -> obj
extern fn @$rt.Paykan_print(obj) -> void
extern fn @$rt.PaykanString_destroy(obj) -> void
fn @main() -> i64 {
  %s = call @$rt.PaykanString_new(@.s, 6)
  call @$rt.Paykan_print(%s)
  call @$rt.PaykanString_destroy(%s)
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

// f64 `cmp ne` is IEEE's unordered not-equal (fcmp une); the other
// predicates are ordered (docs/pir.md).
TEST(PIRLLVM, FloatCmpPredicates) {
  std::string err;
  std::string ir = translatePIR(R"(module "t"
fn @f(%a: f64, %b: f64) -> bool {
  %ne = cmp ne %a, %b
  %eq = cmp eq %a, %b
  %lt = cmp lt %a, %b
  %le = cmp le %a, %b
  %gt = cmp gt %a, %b
  %ge = cmp ge %a, %b
  %x = select %ne, %eq, %lt
  %y = select %le, %gt, %ge
  %z = select %x, %y, %x
  ret %z
}
fn @main() -> i64 {
  %r = call @f(1.0, 2.0)
  ret 0
}
)",
                                err);
  ASSERT_FALSE(ir.empty()) << err;
  for (const char *expected :
       {"= fcmp une double ", "= fcmp oeq double ", "= fcmp olt double ",
        "= fcmp ole double ", "= fcmp ogt double ", "= fcmp oge double "})
    EXPECT_EQ(linesWith(ir, expected).size(), 1u) << expected << "\n" << ir;
  EXPECT_TRUE(linesWith(ir, "fcmp one").empty()) << ir;
}

TEST(PIRLLVM, NaNIsNotEqualToItself) {
  // inf - inf is a NaN: `ne` is true, `eq` and `lt` are false.
  auto r = runPIR(R"(module "t"
fn @main() -> i64 {
  %inf = mul 1.0e308, 10.0
  %nan = sub %inf, %inf
  %ne = cmp ne %nan, %nan
  %eq = cmp eq %nan, %nan
  %lt = cmp lt %nan, 1.0
  %a = select %ne, 1, 0
  %b = select %eq, 2, 0
  %c = select %lt, 4, 0
  %ab = add %a, %b
  %abc = add %ab, %c
  ret %abc
}
)");
  ASSERT_TRUE(r.Ok) << r.Err;
  EXPECT_EQ(r.ExitCode, 1);
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
extern fn @$rt.PaykanObject_toString(obj) -> box
extern fn @$rt.PaykanObject_equals(obj, box, i64) -> i64
extern fn @$rt.Paykan_release(box) -> void

class Counter {
  field n: i64
  vtable {
    destroy = @Counter_destroy : (obj) -> void
    toString = @$rt.PaykanObject_toString : (obj) -> box
    equals = @$rt.PaykanObject_equals : (obj, box) -> i64
    bump = @Counter_bump : (obj, i64) -> i64
  }
}

fn @Counter_destroy(%self: obj) -> void {
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

// The optimisation hints: string literals are mergeable (unnamed_addr,
// align 1).  A vtable-pointer load is not invariant: the pointer changes
// while the object is constructed (each `__init__` makes it one of its own
// class, `vtable.store`), so LLVM must not merge a load before such a store
// with one after it.
TEST(PIRLLVM, ConstantsAreMergeableAndVTableLoadsFollowStores) {
  std::string err;
  std::string ir = translatePIR(R"(module "t"
cstr @.s = "hi" len 2
cstr @.t = "hi" len 2
extern fn @$rt.PaykanObject_toString(obj) -> box
extern fn @$rt.PaykanObject_equals(obj, box, i64) -> i64

class C {
  vtable {
    destroy = @C_destroy : (obj) -> void
    toString = @$rt.PaykanObject_toString : (obj) -> box
    equals = @$rt.PaykanObject_equals : (obj, box) -> i64
    get = @C_get : (obj) -> i64
  }
}

fn @C_destroy(%self: obj) -> void {
  ret
}

fn @C_get(%self: obj) -> i64 {
  ret 7
}

fn @main() -> i64 {
  %s = ptr.load char, @.s
  %t = ptr.load char, @.t
  %o = new C
  %vt = vtable.load %o
  vtable.store %o, C
  %r = vcall %o : C [3] ()
  free %o
  ret %r
}
)",
                                err);
  ASSERT_FALSE(ir.empty()) << err;

  for (const char *name : {"@pk_t_S_2Es = ", "@pk_t_S_2Et = "}) {
    auto defs = linesWith(ir, name);
    ASSERT_EQ(defs.size(), 1u) << name << "\n" << ir;
    EXPECT_NE(
        defs[0].find("private unnamed_addr constant [3 x i8] c\"hi\\00\""),
        std::string::npos)
        << defs[0];
    EXPECT_NE(defs[0].find(", align 1"), std::string::npos) << defs[0];
  }

  // No load is invariant, and the vtable.store is a store of the class's
  // vtable into the object's header, between the two vtable-pointer loads
  // (vtable.load's and the one vcall does).
  EXPECT_TRUE(linesWith(ir, "!invariant.load").empty()) << ir;
  EXPECT_EQ(linesWith(ir, "store ptr @pkvt_").size(), 2u) << ir; // new's too
}

// An inline optional is an { i1, payload } value: passed and returned as
// one, `none` is its zero, `some` sets the presence flag.
TEST(PIRLLVM, OptionalsAreTwoFieldValues) {
  std::string err;
  std::string ir = translatePIR(R"(module "t"
fn @half(%n: i64) -> opt_f64 {
  %odd = rem %n, 2
  %bad = cmp ne %odd, 0
  %f = itof %n
  %h = div %f, 2.0
  %s = some %h
  %z = none opt_f64
  %r = select %bad, %z, %s
  ret %r
}

fn @main() -> i64 {
  %a = call @half(8)
  %has = is_some %a
  %v = unwrap %a
  %i = ftoi %v
  %r = select %has, %i, 99
  ret %r
}
)",
                                err);
  ASSERT_FALSE(ir.empty()) << err;
  EXPECT_FALSE(linesWith(ir, "define { i1, double } @pk_t_Shalf(i64").empty())
      << ir;
  EXPECT_FALSE(linesWith(ir, "insertvalue { i1, double }").empty()) << ir;
  EXPECT_FALSE(linesWith(ir, "zeroinitializer").empty()) << ir;
  EXPECT_FALSE(linesWith(ir, "extractvalue { i1, double }").empty()) << ir;

  // An object's size is its C layout's (the shared lowering's): the 16-byte
  // header, then the opt_bool field's 2 bytes, rounded up to the header's
  // alignment.
  ir = translatePIR(R"(module "t"
class C {
  field b: opt_bool
  vtable {
    destroy = @C.destroy : (obj) -> void
  }
}

fn @C.destroy(%self: obj) -> void {
  ret
}

fn @main() -> i64 {
  %o = new C
  free %o
  ret 0
}
)",
                    err);
  ASSERT_FALSE(ir.empty()) << err;
  auto mallocs = linesWith(ir, "call ptr @Paykan_malloc(");
  ASSERT_EQ(mallocs.size(), 1u) << ir;
  EXPECT_NE(mallocs[0].find("@Paykan_malloc(i64 24)"), std::string::npos)
      << mallocs[0];

  auto r = runPIR(R"(module "t"
fn @pick(%c: bool) -> opt_char {
  %s = some 'k'
  %n = none opt_char
  %r = select %c, %s, %n
  ret %r
}

fn @main() -> i64 {
  local %o: opt_char
  %a = call @pick(true)
  store %o, %a
  %b = call @pick(false)
  %l = load %o
  %v = unwrap %l
  %code = cast %v to i64
  %hb = is_some %b
  %nb = unwrap %b
  %zero = cast %nb to i64
  %t = select %hb, 1000, %code
  %r = add %t, %zero
  ret %r
}
)");
  ASSERT_TRUE(r.Ok) << r.Err;
  EXPECT_EQ(r.ExitCode, 'k');
}

TEST(PIRLLVM, CrossModuleCallsAndMainArgs) {
  auto r = runPIR(R"(module "main"
extern fn @helper(i64) -> i64 module "lib"
extern fn @$rt.PaykanString_length(obj) -> i64
fn @main(%args: ptr, %n: i64) -> i64 {
  %at = ptr.add %args, 16
  %arg = ptr.load box, %at
  %s = unbox %arg
  %len = call @$rt.PaykanString_length(%s)
  %h = call @helper(%n)
  %r = add %h, %len
  ret %r
}
module "lib"
fn @helper(%a: i64) -> i64 {
  %r = mul %a, 10
  ret %r
}
)",
                  {"script", "x", "yz"});
  ASSERT_TRUE(r.Ok) << r.Err;
  EXPECT_EQ(r.ExitCode, 32); // 3 arguments * 10 + the length of "yz"
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

// The runtime's panics are declared noreturn, as Runtime.h declares them, so
// a panic guard's branch ends at its call: the division after the guard is
// what follows the `if`, not the call (SW-02).
TEST(PIRLLVM, PanicsAreNoreturn) {
  const char *src = R"(module "t"
extern fn @$rt.Paykan_panic_div_by_zero() -> void
extern fn @$rt.Paykan_panic_index(i64, i64) -> void
fn @quot(%a: i64, %b: i64) -> i64 {
  %z = cmp eq %b, 0
  if %z {
    call @$rt.Paykan_panic_div_by_zero()
    unreachable
  }
  %q = div %a, %b
  %neg = cmp lt %q, 0
  if %neg {
    call @$rt.Paykan_panic_index(%q, 0)
    unreachable
  }
  ret %q
}
fn @main() -> i64 {
  %r = call @quot(84, 2)
  ret %r
}
)";
  std::string err;
  std::string ir = translatePIR(src, err);
  ASSERT_FALSE(ir.empty()) << err;
  for (const char *panic :
       {"@Paykan_panic_div_by_zero(", "@Paykan_panic_index("}) {
    auto decls = linesWith(ir, std::string("declare void ") + panic);
    ASSERT_EQ(decls.size(), 1u) << panic << "\n" << ir;
    const size_t at = ir.find(decls.front());
    const size_t attrs = ir.rfind("; Function Attrs:", at);
    ASSERT_NE(attrs, std::string::npos) << panic << "\n" << ir;
    EXPECT_EQ(ir.find('\n', attrs) + 1, at) << panic << "\n" << ir;
    EXPECT_NE(ir.substr(attrs, at - attrs).find("noreturn"), std::string::npos)
        << panic << "\n"
        << ir;
  }
  auto r = runPIR(src);
  ASSERT_TRUE(r.Ok) << r.Err;
  EXPECT_EQ(r.ExitCode, 42);
}

// -- Reference counts
//
// `retain` and `release` have their fast paths inline (ToLLVM.cpp,
// InlineReferenceCountsPass): a null box is skipped, a retain of a count
// below the limit increments the header's 32-bit count at offset 8 and a
// release of one above one decrements it; the runtime's Paykan_retain and
// Paykan_release are called only for the rest, on the unlikely branch.

/// A class C whose destroy prints "destroyed", and @strong(%b): the strong
/// count of %b's object (its header's 64 bits at 8: the weak count, 0
/// here, above the strong one).
const char *const kCounted = R"(module "t"
cstr @.d = "destroyed\n" len 10
cstr @.k = "kept\n" len 5
extern fn @$rt.PaykanObject_toString(obj) -> box
extern fn @$rt.PaykanObject_equals(obj, box, i64) -> i64
extern fn @$rt.PaykanString_new(ptr, i64) -> obj
extern fn @$rt.Paykan_print(obj) -> void
extern fn @$rt.Paykan_dispose(obj) -> void

class C {
  vtable {
    destroy = @C_destroy : (obj) -> void
    toString = @$rt.PaykanObject_toString : (obj) -> box
    equals = @$rt.PaykanObject_equals : (obj, box) -> i64
  }
}

fn @say(%s: ptr, %n: i64) -> void {
  %o = call @$rt.PaykanString_new(%s, %n)
  call @$rt.Paykan_print(%o)
  call @$rt.Paykan_dispose(%o)
  ret
}

fn @C_destroy(%self: obj) -> void {
  call @say(@.d, 10)
  ret
}

fn @strong(%b: box) -> i64 {
  %o = unbox %b
  %p = cast %o to ptr
  %at = ptr.add %p, 8
  %n = ptr.load i64, %at
  ret %n
}
)";

// A lending function takes the ownership mask after its parameters, and
// `owned %p` reads its box parameter's bit (docs/pir.md §3): a call's mask
// has the bit of each argument it hands over, owned or passed with a true
// bit, and not of one it lends.
const char *const kMaskBits = R"(module "t"
fn @bits(%a: box, %n: i64, %b: box, %c: box) -> i64 {
  %oa = owned %a
  %ob = owned %b
  %oc = owned %c
  %x = select %oa, 1, 0
  %y = select %ob, 2, 0
  %z = select %oc, 4, 0
  %xy = add %x, %y
  %r = add %xy, %z
  ret %r
}
fn @fwd(%x: box, %y: box) -> i64 {
  %ox = owned %x
  %r = call @bits(owned(%ox) %x, 0, lent %y, %y)
  ret %r
}
fn @main() -> i64 {
  %t = call @bits(null box, 0, null box, null box)
  %u = call @bits(lent null box, 0, null box, lent null box)
  %v = call @fwd(lent null box, null box)
  %w = call @fwd(null box, null box)
  %t1 = mul %t, 1000
  %u1 = mul %u, 100
  %v1 = mul %v, 10
  %tu = add %t1, %u1
  %tuv = add %tu, %v1
  %r = add %tuv, %w
  ret %r
}
)";

TEST(PIRLLVM, TheOwnershipMaskCarriesEachArgumentsBit) {
  auto r = runPIR(kMaskBits);
  ASSERT_TRUE(r.Ok) << r.Err;
  EXPECT_EQ(r.ExitCode, 7000 + 200 + 40 + 5);
  std::string err;
  std::string ir = translatePIR(kMaskBits, err);
  ASSERT_FALSE(ir.empty()) << err;
  // The mask is the last parameter, an i64 tested bit by bit ...
  EXPECT_FALSE(
      linesWith(ir, "(ptr %0, i64 %1, ptr %2, ptr %3, i64 %4)").empty())
      << ir;
  EXPECT_FALSE(linesWith(ir, "and i64 %4, 4").empty()) << ir;
  // ... a constant where every bit is known, and a passed bit selected.
  EXPECT_FALSE(linesWith(ir, "ptr null, ptr null, i64 7)").empty()) << ir;
  EXPECT_FALSE(linesWith(ir, "ptr null, ptr null, i64 2)").empty()) << ir;
  EXPECT_FALSE(linesWith(ir, "select i1 ").empty()) << ir;
  EXPECT_FALSE(linesWith(ir, "or i64 ").empty()) << ir;
}

// Only the first 64 box parameters have a bit: the 65th is always handed
// over, whatever the mask says, and a call hands it over.
TEST(PIRLLVM, TheSixtyFifthBoxParameterHasNoBit) {
  std::string params, owned, lent63, lent0;
  for (int i = 0; i < 65; ++i) {
    const std::string sep = i ? ", " : "";
    params += sep + "%b" + std::to_string(i) + ": box";
    owned += sep + "null box";
    lent63 += sep + (i == 63 ? "lent null box" : "null box");
    lent0 += sep + (i == 0 ? "lent null box" : "null box");
  }
  const std::string text = "module \"t\"\nfn @wide(" + params +
                           ") -> i64 {\n"
                           "  %o0 = owned %b0\n"
                           "  %o63 = owned %b63\n"
                           "  %x = select %o0, 1, 0\n"
                           "  %y = select %o63, 10, 0\n"
                           "  %r = add %x, %y\n"
                           "  ret %r\n}\n"
                           "fn @main() -> i64 {\n"
                           "  %a = call @wide(" +
                           owned +
                           ")\n"
                           "  %b = call @wide(" +
                           lent63 +
                           ")\n"
                           "  %c = call @wide(" +
                           lent0 +
                           ")\n"
                           "  %a1 = mul %a, 10000\n"
                           "  %b1 = mul %b, 100\n"
                           "  %ab = add %a1, %b1\n"
                           "  %r = add %ab, %c\n"
                           "  ret %r\n}\n";
  auto r = runPIR(text);
  ASSERT_TRUE(r.Ok) << r.Err;
  EXPECT_EQ(r.ExitCode, 11 * 10000 + 1 * 100 + 10);
  std::string err;
  std::string ir = translatePIR(text, err);
  ASSERT_FALSE(ir.empty()) << err;
  // Every bit set (the 65th has none), and every bit but 63's.
  EXPECT_FALSE(linesWith(ir, "ptr null, i64 -1)").empty()) << ir;
  EXPECT_FALSE(linesWith(ir, "ptr null, i64 9223372036854775807)").empty())
      << ir;
}

TEST(PIRLLVM, RetainAndReleaseHaveTheirFastPathsInline) {
  std::string err;
  std::string ir = translatePIR(std::string(kCounted) + R"(
fn @main() -> i64 {
  %o = new C
  %b = box %o
  retain %b
  release %b
  release %b
  ret 0
}
)",
                                err);
  ASSERT_FALSE(ir.empty()) << err;
  // Each a test of the count, the common case weighted 2000 to 1 ...
  EXPECT_EQ(linesWith(ir, "icmp ne i32 ").size(), 1u) << ir;
  EXPECT_EQ(linesWith(ir, ", 2147483647").size(), 1u) << ir;
  EXPECT_EQ(linesWith(ir, "icmp sgt i32 ").size(), 2u) << ir;
  EXPECT_EQ(linesWith(ir, "!prof").size(), 3u) << ir;
  EXPECT_FALSE(linesWith(ir, "!{!\"branch_weights\", i32 2000, i32 1}").empty())
      << ir;
  // ... an increment or a decrement of the count in place ...
  EXPECT_EQ(linesWith(ir, "load i32, ptr").size(), 3u) << ir;
  EXPECT_EQ(linesWith(ir, "add i32 ").size(), 1u) << ir;
  EXPECT_EQ(linesWith(ir, "sub i32 ").size(), 2u) << ir;
  EXPECT_EQ(linesWith(ir, "store i32 ").size(), 3u) << ir;
  // ... and the runtime for the rest.
  EXPECT_EQ(linesWith(ir, "call void @Paykan_retain(").size(), 1u) << ir;
  EXPECT_EQ(linesWith(ir, "call void @Paykan_release(").size(), 2u) << ir;
}

// A `retain nonnull`, of a box that is never null (docs/pir.md §6), and a
// release of the same value test no box for null; a release of another
// value, a load of the slot the box was stored in, still does (SW-14).
TEST(PIRLLVM, ANonNullRetainTestsNoBoxForNull) {
  auto program = [](const char *retain) {
    return std::string(kCounted) + R"(
fn @main() -> i64 {
  local %l: box
  %o = new C
  %b = box %o
  )" + retain +
           R"( %b
  %two = call @strong(%b)
  release %b
  store %l, %b
  %c = load %l
  release %c
  %r = mul %two, 10
  ret %r
}
)";
  };
  std::string err;
  std::string ir = translatePIR(program("retain nonnull"), err);
  ASSERT_FALSE(ir.empty()) << err;
  EXPECT_EQ(linesWith(ir, "icmp eq ptr").size(), 1u) << ir;
  // The counts as before: a test of each, the call for the rest.
  EXPECT_EQ(linesWith(ir, "icmp ne i32 ").size(), 1u) << ir;
  EXPECT_EQ(linesWith(ir, "icmp sgt i32 ").size(), 2u) << ir;
  EXPECT_EQ(linesWith(ir, "call void @Paykan_retain(").size(), 1u) << ir;
  EXPECT_EQ(linesWith(ir, "call void @Paykan_release(").size(), 2u) << ir;
  EXPECT_EQ(ir.find("nonnull"), std::string::npos) << ir;
  ir = translatePIR(program("retain"), err);
  ASSERT_FALSE(ir.empty()) << err;
  EXPECT_EQ(linesWith(ir, "icmp eq ptr").size(), 3u) << ir;

  auto r = runPIR(program("retain nonnull"));
  ASSERT_TRUE(r.Ok) << r.Err;
  EXPECT_EQ(r.ExitCode, 20);
  EXPECT_EQ(r.Out, "destroyed\n");
}

// A drop function is called from its own module only (docs/pir.md §7): it
// links internally, so that LLVM may inline it where it is called.
TEST(PIRLLVM, ADropFunctionLinksInsideItsModule) {
  const std::string program = std::string(kCounted) + R"(
fn @.drop.Pair(%at: ptr) -> void {
  %a = ptr.load box, %at
  release %a
  %second = ptr.add %at, 8
  %b = ptr.load box, %second
  release %b
  ret
}

fn @main() -> i64 {
  local %p: mem(16, 8)
  %o1 = new C
  %b1 = box %o1
  %o2 = new C
  %b2 = box %o2
  %first = local.addr %p
  ptr.store %first, %b1
  %second = ptr.add %first, 8
  ptr.store %second, %b2
  %at = local.addr %p
  call @.drop.Pair(%at)
  ret 0
}
)";
  std::string err;
  std::string ir = translatePIR(program, err);
  ASSERT_FALSE(ir.empty()) << err;
  const std::vector<std::string> internal = linesWith(ir, "define internal");
  ASSERT_EQ(internal.size(), 1u) << ir;
  EXPECT_NE(internal.front().find("drop"), std::string::npos) << ir;

  auto r = runPIR(program);
  ASSERT_TRUE(r.Ok) << r.Err;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.Out, "destroyed\ndestroyed\n");
}

TEST(PIRLLVM, InlineRetainsAndReleasesCountAndTheLastOneDestroys) {
  auto r = runPIR(std::string(kCounted) + R"(
fn @main() -> i64 {
  local %l: box
  %o = new C
  %b = box %o
  retain %b
  retain %b
  %three = call @strong(%b)
  release %b
  release %b
  %one = call @strong(%b)
  call @say(@.k, 5)
  release %b
  store %l, null box
  %z = load %l
  retain %z
  release %z
  %t = mul %three, 10
  %r = add %t, %one
  ret %r
}
)");
  ASSERT_TRUE(r.Ok) << r.Err;
  EXPECT_EQ(r.ExitCode, 31);
  EXPECT_EQ(r.Out, "kept\ndestroyed\n");
}

// A switch on an i64 with a case of INT64_MAX or INT64_MIN, the empty and
// tombstone keys of the DenseSet that scf.index_switch keeps its cases in,
// failed the pipeline ("'scf.index_switch' op has duplicate case value").
// Each such case is now tested on its own, around the dispatch among the
// others.  Every order of the two among the cases 0 and 7, and of fewer
// cases, each in tail position with a default, returning from every case
// without one, and storing in every case with a default and without one:
// each function, given each case value and three of no case, gives what is
// worked out here.  main returns the number of the first check that fails.
TEST(PIRLLVM, SwitchCasesOfTheDenseSetKeys) {
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  std::vector<std::vector<int64_t>> orders;
  std::vector<int64_t> four = {kMin, 0, 7, kMax};
  do
    orders.push_back(four);
  while (std::next_permutation(four.begin(), four.end()));
  for (const auto &few : std::vector<std::vector<int64_t>>{
           {kMax, kMin}, {kMin, kMax}, {kMax}, {kMin}, {kMax, 7}, {7, kMin}})
    orders.push_back(few);
  const std::vector<int64_t> inputs = {kMax, kMin, 0, 7, 1, kMax - 1, kMin + 1};
  enum class Form { Tail, Returns, Stores, StoresWithoutDefault };
  struct Check {
    std::string Fn;
    int64_t In;
    int64_t Want;
  };
  std::vector<Check> checks;
  std::ostringstream text;
  text << "module \"t\"\n";
  for (const auto &order : orders)
    for (Form form : {Form::Tail, Form::Returns, Form::Stores,
                      Form::StoresWithoutDefault}) {
      const std::string fn = "@f" + std::to_string(checks.size());
      const bool stores =
          form == Form::Stores || form == Form::StoresWithoutDefault;
      const bool withDefault = form == Form::Tail || form == Form::Stores;
      const char *set = stores ? "store %r.0, " : "ret ";
      text << "fn " << fn << "(%x.1: i64) -> i64 {\n";
      if (stores)
        text << "  local %r.0: i64\n  store %r.0, 5\n";
      text << "  switch %x.1 {\n";
      for (size_t i = 0; i < order.size(); ++i)
        text << "    case " << order[i] << " {\n      " << set << 10 * (i + 1)
             << "\n    }\n";
      if (withDefault)
        text << "    default {\n      " << set << "99\n    }\n";
      text << "  }\n";
      if (stores)
        text << "  %v.2 = load %r.0\n  ret %v.2\n";
      if (form == Form::Returns)
        text << "  ret 99\n";
      text << "}\n";
      for (int64_t in : inputs) {
        int64_t want = form == Form::StoresWithoutDefault ? 5 : 99;
        for (size_t i = 0; i < order.size(); ++i)
          if (order[i] == in)
            want = 10 * (static_cast<int64_t>(i) + 1);
        checks.push_back({fn, in, want});
      }
    }
  text << "fn @main() -> i64 {\n";
  for (size_t i = 0; i < checks.size(); ++i)
    text << "  %r." << 2 * i + 1 << " = call " << checks[i].Fn << "("
         << checks[i].In << ")\n  %bad." << 2 * i + 2 << " = cmp ne %r."
         << 2 * i + 1 << ", " << checks[i].Want << "\n  if %bad." << 2 * i + 2
         << " {\n    ret " << i + 1 << "\n  }\n";
  text << "  ret 0\n}\n";
  auto r = runPIR(text.str());
  ASSERT_TRUE(r.Ok) << r.Err;
  ASSERT_GE(r.ExitCode, 0);
  const auto failed = static_cast<size_t>(r.ExitCode);
  ASSERT_LE(failed, checks.size());
  if (failed > 0) {
    const Check &c = checks[failed - 1];
    ADD_FAILURE() << c.Fn << "(" << c.In << ") is not " << c.Want;
  }
}

TEST(PIRLLVMDeathTest, AnInlineRetainAtTheLimitCallsTheRuntime) {
  // A count at the limit takes the call, which panics rather than wrap.
  const std::string text = std::string(kCounted) + R"(
fn @main() -> i64 {
  %o = new C
  %b = box %o
  %p = cast %o to ptr
  %at = ptr.add %p, 8
  ptr.store %at, 2147483647
  retain %b
  ret 0
}
)";
  std::string err;
  EXPECT_DEATH(jitPIR(text, {}, err),
               "too many strong references to one object");
}
