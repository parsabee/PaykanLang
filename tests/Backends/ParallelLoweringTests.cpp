// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The concurrency prototype through the mlir backend (docs/design/
// concurrency.md, samples/concurrency/README.md), on hand-written PIR as
// PIRMLIRTests.cpp and PIRLLVMTests.cpp test it.  A `parallel` while
// lowers to an scf.parallel over at most 256 chunks, each with partial
// slots of its own, folded in chunk order after the loop; a light body
// runs inline below 4096 iterations (PIRToStandard.cpp, lowerParallel);
// the LLVM path schedules the chunks dynamically, one at a time
// (ToLLVM.cpp, DynamicSchedulePass); an async fn is an async.func whose
// body is an async.execute region, and an await an async.await.  Each
// program is what `paykan --emit-pir` prints for a few lines of Paykan.

#include "Import/PIRImport.h"
#include "Pipeline.h"
#include "ToEmitC/ToEmitC.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Verifier.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <mlir/IR/BuiltinAttributes.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/MLIRContext.h>
#include <mlir/IR/Operation.h>
#include <mlir/IR/OwningOpRef.h>
#include <mlir/IR/Region.h>
#include <mlir/IR/Value.h>
#include <mlir/IR/Verifier.h>
#include <mlir/Parser/Parser.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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

/// Parse and verify the PIR @p text, import it into @p ctx and lower it to
/// @p stage: the module, or null with the reason in @p err.
mlir::OwningOpRef<mlir::ModuleOp> lowerText(const std::string &text,
                                            mlir::MLIRContext &ctx,
                                            mlir_backend::Stage stage,
                                            std::string &err) {
  pir::ParseError perr;
  auto program = pir::parseProgram(text, perr);
  if (!program) {
    err = "parse error: " + perr.str();
    return {};
  }
  if (auto verrs = pir::verify(*program); !verrs.empty()) {
    err = "verifier: " + pir::formatErrors(verrs);
    return {};
  }
  auto mod = mlir_backend::importProgram(*program, ctx);
  if (!mod) {
    err = mod.status().message();
    return {};
  }
  if (Status s = mlir_backend::lowerTo((*mod).get(), stage); !s) {
    err = s.message();
    return {};
  }
  return std::move(*mod);
}

/// The PIR @p text at @p stage, printed ("" on failure, with the reason in
/// @p err).
std::string stageText(const std::string &text, mlir_backend::Stage stage,
                      std::string &err) {
  auto ctx = mlir_backend::createContext();
  auto mod = lowerText(text, *ctx, stage, err);
  if (!mod)
    return "";
  return print(*mod);
}

/// Parse, verify and translate the PIR @p text; its LLVM IR as text ("" on
/// failure, with the reason in @p err).
std::string llvmIRText(const std::string &text, std::string &err) {
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
  llvm::LLVMContext ctx;
  auto module = mlir_backend::compileProgram(*program, ctx, "t", "");
  if (!module) {
    err = module.status().message();
    return "";
  }
  std::string ir;
  llvm::raw_string_ostream os(ir);
  (*module)->print(os, nullptr);
  os.flush();
  return ir;
}

/// The PIR @p text through the C path, printed as C ("" on failure, with
/// the reason in @p err).
std::string cText(const std::string &text, std::string &err) {
  auto ctx = mlir_backend::createContext();
  auto mod = lowerText(text, *ctx, mlir_backend::Stage::EmitC, err);
  if (!mod)
    return "";
  std::string c;
  llvm::raw_string_ostream os(c);
  if (Status s = mlir_backend::translateToC(*mod, os); !s) {
    err = s.message();
    return "";
  }
  os.flush();
  return c;
}

/// @p mlir parses back, verifies and prints as @p mlir.
void expectRoundTrip(const std::string &mlir, mlir::MLIRContext &ctx) {
  auto parsed = mlir::parseSourceString<mlir::ModuleOp>(mlir, &ctx);
  ASSERT_TRUE(parsed) << mlir;
  ASSERT_TRUE(mlir::succeeded(mlir::verify(*parsed))) << mlir;
  EXPECT_EQ(print(*parsed), mlir);
}

/// FileCheck-style: every line of @p text that contains @p needle.
std::vector<std::string> linesWith(const std::string &text,
                                   const std::string &needle) {
  std::vector<std::string> out;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);)
    if (line.find(needle) != std::string::npos)
      out.push_back(line);
  return out;
}

/// The ops are told apart by their names with the dialect ("scf.parallel"),
/// so the tests read as the printed MLIR does and need no dialect's
/// headers.
std::string nameOf(mlir::Operation *op) {
  return op->getName().getStringRef().str();
}

/// Every op under @p root, @p root itself left out, named @p name, in walk
/// order (an op before what it holds).
std::vector<mlir::Operation *> opsNamed(mlir::Operation *root,
                                        llvm::StringRef name) {
  std::vector<mlir::Operation *> ops;
  root->walk([&](mlir::Operation *op) {
    if (op != root && op->getName().getStringRef() == name)
      ops.push_back(op);
  });
  return ops;
}

/// Every op in @p region named @p name, in walk order.
std::vector<mlir::Operation *> opsIn(mlir::Region &region,
                                     llvm::StringRef name) {
  std::vector<mlir::Operation *> ops;
  region.walk([&](mlir::Operation *op) {
    if (op->getName().getStringRef() == name)
      ops.push_back(op);
  });
  return ops;
}

/// The function (pir.func, func.func or async.func) named @p symbol under
/// @p root, or null.
mlir::Operation *functionNamed(mlir::Operation *root, llvm::StringRef symbol) {
  mlir::Operation *found = nullptr;
  root->walk([&](mlir::Operation *op) {
    llvm::StringRef name = op->getName().getStringRef();
    if (name != "pir.func" && name != "func.func" && name != "async.func")
      return;
    auto sym = op->getAttrOfType<mlir::StringAttr>("sym_name");
    if (sym && sym.getValue() == symbol)
      found = op;
  });
  return found;
}

/// The op defining @p v when it is named @p name, else null.
mlir::Operation *definedBy(mlir::Value v, llvm::StringRef name) {
  mlir::Operation *op = v.getDefiningOp();
  return op && op->getName().getStringRef() == name ? op : nullptr;
}

/// The integer @p v is an arith.constant of, if it is one.
std::optional<int64_t> constantOf(mlir::Value v) {
  mlir::Operation *op = definedBy(v, "arith.constant");
  if (!op)
    return std::nullopt;
  auto value = op->getAttrOfType<mlir::IntegerAttr>("value");
  if (!value)
    return std::nullopt;
  return value.getInt();
}

/// The float @p v is an arith.constant of, if it is one.
std::optional<double> floatConstantOf(mlir::Value v) {
  mlir::Operation *op = definedBy(v, "arith.constant");
  if (!op)
    return std::nullopt;
  auto value = op->getAttrOfType<mlir::FloatAttr>("value");
  if (!value)
    return std::nullopt;
  return value.getValueAsDouble();
}

/// The symbol a call-like @p op calls ("" for none).
std::string calleeOf(mlir::Operation *op) {
  auto callee = op->getAttrOfType<mlir::FlatSymbolRefAttr>("callee");
  return callee ? callee.getValue().str() : "";
}

/// The name of the slot a pir.alloca @p op makes ("" for another op).
std::string allocaName(mlir::Operation *op) {
  if (!op || op->getName().getStringRef() != "pir.alloca")
    return "";
  auto name = op->getAttrOfType<mlir::StringAttr>("name");
  return name ? name.getValue().str() : "";
}

/// The pir.alloca ops under @p root whose slot is named @p name.
std::vector<mlir::Operation *> allocasNamed(mlir::Operation *root,
                                            llvm::StringRef name) {
  std::vector<mlir::Operation *> out;
  for (mlir::Operation *op : opsNamed(root, "pir.alloca"))
    if (allocaName(op) == name)
      out.push_back(op);
  return out;
}

/// The slot (a pir.alloca) the address @p ptr is, or is an offset into
/// (pir.ptr_add); null for another address.
mlir::Operation *slotOf(mlir::Value ptr) {
  if (mlir::Operation *add = definedBy(ptr, "pir.ptr_add"))
    ptr = add->getOperand(0);
  return definedBy(ptr, "pir.alloca");
}

/// The pir.ptr_store ops under @p root that write the slot @p slot itself
/// (its address is the store's first operand, the value its second).
std::vector<mlir::Operation *> storesTo(mlir::Operation *root,
                                        mlir::Operation *slot) {
  std::vector<mlir::Operation *> out;
  for (mlir::Operation *op : opsNamed(root, "pir.ptr_store"))
    if (op->getOperand(0) == slot->getResult(0))
      out.push_back(op);
  return out;
}

/// The op after @p op in its block named @p name, or null.
mlir::Operation *nextNamed(mlir::Operation *op, llvm::StringRef name) {
  for (mlir::Operation *n = op->getNextNode(); n; n = n->getNextNode())
    if (n->getName().getStringRef() == name)
      return n;
  return nullptr;
}

/// The `k = min(count, 256)` (an arith.minsi) a loop over the chunks runs
/// to: @p loop's upper bound, its second operand, is an index cast of it.
/// Null for a loop over anything else.
mlir::Operation *chunkCountOf(mlir::Operation *loop) {
  if (loop->getNumOperands() < 2)
    return nullptr;
  mlir::Operation *cast = definedBy(loop->getOperand(1), "arith.index_cast");
  if (!cast)
    return nullptr;
  mlir::Operation *min = definedBy(cast->getOperand(0), "arith.minsi");
  if (!min)
    return nullptr;
  for (mlir::Value v : min->getOperands())
    if (constantOf(v) == 256)
      return min;
  return nullptr;
}

/// The scf.if a light body's loop runs under in @p fn, on the trip count
/// against the inline threshold (`count < 4096`), or null.
mlir::Operation *inlineTest(mlir::Operation *fn) {
  for (mlir::Operation *op : opsNamed(fn, "scf.if")) {
    mlir::Operation *cmp = definedBy(op->getOperand(0), "arith.cmpi");
    if (cmp && cmp->getNumOperands() == 2 &&
        constantOf(cmp->getOperand(1)) == 4096)
      return op;
  }
  return nullptr;
}

// `s += i` over 0..n, a light body: no call, no loop of its own.
//
//   fn sum(n: int) -> int {
//     s = 0;
//     parallel for i in 0..n { s += i; }
//     return s;
//   }
const char *const kLightSum = R"(module "main"

fn @sum(%n.1: i64) -> i64 {
  local %n.0: i64
  local %s.1: i64
  local %s.part.2: i64
  local %par.k.3: i64
  local %i.4: i64
  store %n.0, %n.1
  store %s.1, 0
  %n.2 = load %n.0
  %for.has.3 = cmp lt 0, %n.2
  %for.dist.4 = sub %n.2, 0
  %5 = sub %for.dist.4, 1
  %6 = add %5, 1
  %par.count.7 = select %for.has.3, %6, 0
  store %s.part.2, 0
  store %par.k.3, 0
  while {
    %par.kv.8 = load %par.k.3
    %par.go.9 = ucmp lt %par.kv.8, %par.count.7
    cond %par.go.9
  } {
    %par.idx.10 = load %par.k.3
    %par.next.11 = add %par.idx.10, 1
    store %par.k.3, %par.next.11
    %par.value.12 = add 0, %par.idx.10
    store %i.4, %par.value.12
    %s.13 = load %s.part.2
    %i.14 = load %i.4
    %add.15 = add %s.13, %i.14
    store %s.part.2, %add.15
  } parallel %par.k.3, %par.count.7 privates(%i.4) reduce(%s.part.2 add)
  %s.16 = load %s.1
  %s.part.17 = load %s.part.2
  %s.fold.18 = add %s.16, %s.part.17
  store %s.1, %s.fold.18
  %s.19 = load %s.1
  ret %s.19
}

fn @main() -> i64 {
  %call.1 = call @sum(10)
  %sub.2 = sub %call.1, 45
  ret %sub.2
}
)";

// Two reductions in one loop, an `add` of an i64 and a `mul` of an f64.
//
//   fn both(n: int) -> int {
//     s = 0;
//     p = 1.0;
//     parallel for i in 0..n {
//       s += i;
//       p *= 2.0;
//     }
//     return s + int(p);
//   }
const char *const kTwoReductions = R"(module "main"

fn @both(%n.1: i64) -> i64 {
  local %n.0: i64
  local %s.1: i64
  local %p.2: f64
  local %s.part.3: i64
  local %p.part.4: f64
  local %par.k.5: i64
  local %i.6: i64
  store %n.0, %n.1
  store %s.1, 0
  store %p.2, 1.0
  %n.2 = load %n.0
  %for.has.3 = cmp lt 0, %n.2
  %for.dist.4 = sub %n.2, 0
  %5 = sub %for.dist.4, 1
  %6 = add %5, 1
  %par.count.7 = select %for.has.3, %6, 0
  store %s.part.3, 0
  store %p.part.4, 1.0
  store %par.k.5, 0
  while {
    %par.kv.8 = load %par.k.5
    %par.go.9 = ucmp lt %par.kv.8, %par.count.7
    cond %par.go.9
  } {
    %par.idx.10 = load %par.k.5
    %par.next.11 = add %par.idx.10, 1
    store %par.k.5, %par.next.11
    %par.value.12 = add 0, %par.idx.10
    store %i.6, %par.value.12
    %s.13 = load %s.part.3
    %i.14 = load %i.6
    %add.15 = add %s.13, %i.14
    store %s.part.3, %add.15
    %p.16 = load %p.part.4
    %fmul.17 = mul %p.16, 2.0
    store %p.part.4, %fmul.17
  } parallel %par.k.5, %par.count.7 privates(%i.6) reduce(%s.part.3 add, %p.part.4 mul)
  %s.18 = load %s.1
  %s.part.19 = load %s.part.3
  %s.fold.20 = add %s.18, %s.part.19
  store %s.1, %s.fold.20
  %p.21 = load %p.2
  %p.part.22 = load %p.part.4
  %p.fold.23 = mul %p.21, %p.part.22
  store %p.2, %p.fold.23
  %s.24 = load %s.1
  %p.25 = load %p.2
  %conv.lo.26 = cmp ge %p.25, -9.2233720368547758e+18
  %conv.hi.27 = cmp lt %p.25, 9.2233720368547758e+18
  %conv.ok.28 = select %conv.lo.26, %conv.hi.27, false
  %conv.bad.29 = not %conv.ok.28
  if %conv.bad.29 {
    call @$rt.Paykan_panic_float_to_int(%p.25)
    unreachable
  }
  %conv.int.30 = ftoi %p.25
  %add.31 = add %s.24, %conv.int.30
  ret %add.31
}

fn @main() -> i64 {
  %call.1 = call @both(10)
  %sub.2 = sub %call.1, 1069
  ret %sub.2
}
extern fn @$rt.Paykan_panic_float_to_int(f64) -> void
)";

// A body with a loop of its own: heavy however short the parallel loop.
//
//   fn work(n: int) -> int {
//     s = 0;
//     parallel for i in 0..n {
//       x = i;
//       for k in 0..3 { x = x + 1; }
//       s += x;
//     }
//     return s;
//   }
const char *const kHeavyLoop = R"(module "main"

fn @work(%n.1: i64) -> i64 {
  local %n.0: i64
  local %s.1: i64
  local %s.part.2: i64
  local %par.k.3: i64
  local %i.4: i64
  local %x.5: i64
  local %for.more.6: bool
  local %for.cur.7: i64
  local %k.8: i64
  store %n.0, %n.1
  store %s.1, 0
  %n.2 = load %n.0
  %for.has.3 = cmp lt 0, %n.2
  %for.dist.4 = sub %n.2, 0
  %5 = sub %for.dist.4, 1
  %6 = add %5, 1
  %par.count.7 = select %for.has.3, %6, 0
  store %s.part.2, 0
  store %par.k.3, 0
  while {
    %par.kv.8 = load %par.k.3
    %par.go.9 = ucmp lt %par.kv.8, %par.count.7
    cond %par.go.9
  } {
    %par.idx.10 = load %par.k.3
    %par.next.11 = add %par.idx.10, 1
    store %par.k.3, %par.next.11
    %par.value.12 = add 0, %par.idx.10
    store %i.4, %par.value.12
    %i.13 = load %i.4
    store %x.5, %i.13
    %for.has.14 = cmp lt 0, 3
    %for.dist.15 = sub 3, 0
    %16 = sub %for.dist.15, 1
    %for.last.17 = add 0, %16
    store %for.more.6, %for.has.14
    store %for.cur.7, 0
    store %k.8, 0
    while {
      %for.go.18 = load %for.more.6
      cond %for.go.18
    } {
      %k.19 = load %for.cur.7
      store %k.8, %k.19
      %for.again.20 = cmp ne %k.19, %for.last.17
      store %for.more.6, %for.again.20
      %for.next.21 = add %k.19, 1
      store %for.cur.7, %for.next.21
      %x.22 = load %x.5
      %add.23 = add %x.22, 1
      store %x.5, %add.23
    }
    %s.24 = load %s.part.2
    %x.25 = load %x.5
    %add.26 = add %s.24, %x.25
    store %s.part.2, %add.26
  } parallel %par.k.3, %par.count.7 privates(%i.4, %x.5, %for.more.6, %for.cur.7, %k.8) reduce(%s.part.2 add)
  %s.27 = load %s.1
  %s.part.28 = load %s.part.2
  %s.fold.29 = add %s.27, %s.part.28
  store %s.1, %s.fold.29
  %s.30 = load %s.1
  ret %s.30
}

fn @main() -> i64 {
  %call.1 = call @work(10)
  %sub.2 = sub %call.1, 75
  ret %sub.2
}
)";

// A body that calls a function of the program: heavy too.
//
//   fn step(x: int) -> int { return x + 1; }
//   fn work(n: int) -> int {
//     s = 0;
//     parallel for i in 0..n { s += step(i); }
//     return s;
//   }
const char *const kHeavyCall = R"(module "main"

fn @step(%x.1: i64) -> i64 {
  local %x.0: i64
  store %x.0, %x.1
  %x.2 = load %x.0
  %add.3 = add %x.2, 1
  ret %add.3
}

fn @work(%n.1: i64) -> i64 {
  local %n.0: i64
  local %s.1: i64
  local %s.part.2: i64
  local %par.k.3: i64
  local %i.4: i64
  store %n.0, %n.1
  store %s.1, 0
  %n.2 = load %n.0
  %for.has.3 = cmp lt 0, %n.2
  %for.dist.4 = sub %n.2, 0
  %5 = sub %for.dist.4, 1
  %6 = add %5, 1
  %par.count.7 = select %for.has.3, %6, 0
  store %s.part.2, 0
  store %par.k.3, 0
  while {
    %par.kv.8 = load %par.k.3
    %par.go.9 = ucmp lt %par.kv.8, %par.count.7
    cond %par.go.9
  } {
    %par.idx.10 = load %par.k.3
    %par.next.11 = add %par.idx.10, 1
    store %par.k.3, %par.next.11
    %par.value.12 = add 0, %par.idx.10
    store %i.4, %par.value.12
    %s.13 = load %s.part.2
    %i.14 = load %i.4
    %call.15 = call @step(%i.14)
    %add.16 = add %s.13, %call.15
    store %s.part.2, %add.16
  } parallel %par.k.3, %par.count.7 privates(%i.4) reduce(%s.part.2 add)
  %s.17 = load %s.1
  %s.part.18 = load %s.part.2
  %s.fold.19 = add %s.17, %s.part.18
  store %s.1, %s.fold.19
  %s.20 = load %s.1
  ret %s.20
}

fn @main() -> i64 {
  %call.1 = call @work(10)
  %sub.2 = sub %call.1, 55
  ret %sub.2
}
)";

// A dot product: the bounds checks call the runtime's panic, which does not
// make a body heavy.
//
//   fn dot(xs: view float[], ys: view float[]) -> float {
//     s = 0.0;
//     parallel for i in 0..xs.len() { s += xs[i] * ys[i]; }
//     return s;
//   }
const char *const kDot = R"(module "main"

fn @dot(%xs.1: ptr, %xs.len.2: i64, %ys.3: ptr, %ys.len.4: i64) -> f64 {
  local %xs.0: ptr
  local %xs.len.1: i64
  local %ys.2: ptr
  local %ys.len.3: i64
  local %s.4: f64
  local %s.part.5: f64
  local %par.k.6: i64
  local %i.7: i64
  store %xs.0, %xs.1
  store %xs.len.1, %xs.len.2
  store %ys.2, %ys.3
  store %ys.len.3, %ys.len.4
  store %s.4, 0.0
  %xs.len.5 = load %xs.len.1
  %for.has.6 = cmp lt 0, %xs.len.5
  %for.dist.7 = sub %xs.len.5, 0
  %8 = sub %for.dist.7, 1
  %9 = add %8, 1
  %par.count.10 = select %for.has.6, %9, 0
  store %s.part.5, 0.0
  store %par.k.6, 0
  while {
    %par.kv.11 = load %par.k.6
    %par.go.12 = ucmp lt %par.kv.11, %par.count.10
    cond %par.go.12
  } {
    %par.idx.13 = load %par.k.6
    %par.next.14 = add %par.idx.13, 1
    store %par.k.6, %par.next.14
    %par.value.15 = add 0, %par.idx.13
    store %i.7, %par.value.15
    %s.16 = load %s.part.5
    %xs.addr.17 = load %xs.0
    %i.18 = load %i.7
    %xs.len.19 = load %xs.len.1
    %idx.neg.20 = cmp lt %i.18, 0
    %idx.over.21 = cmp ge %i.18, %xs.len.19
    %idx.bad.22 = select %idx.neg.20, true, %idx.over.21
    if %idx.bad.22 {
      call @$rt.Paykan_panic_index(%i.18, %xs.len.19)
      unreachable
    }
    %elem.off.23 = mul %i.18, 8
    %elem.addr.24 = ptr.add %xs.addr.17, %elem.off.23
    %elem.25 = ptr.load f64, %elem.addr.24
    %ys.addr.26 = load %ys.2
    %i.27 = load %i.7
    %ys.len.28 = load %ys.len.3
    %idx.neg.29 = cmp lt %i.27, 0
    %idx.over.30 = cmp ge %i.27, %ys.len.28
    %idx.bad.31 = select %idx.neg.29, true, %idx.over.30
    if %idx.bad.31 {
      call @$rt.Paykan_panic_index(%i.27, %ys.len.28)
      unreachable
    }
    %elem.off.32 = mul %i.27, 8
    %elem.addr.33 = ptr.add %ys.addr.26, %elem.off.32
    %elem.34 = ptr.load f64, %elem.addr.33
    %fmul.35 = mul %elem.25, %elem.34
    %fadd.36 = add %s.16, %fmul.35
    store %s.part.5, %fadd.36
  } parallel %par.k.6, %par.count.10 privates(%i.7) reduce(%s.part.5 add)
  %s.37 = load %s.4
  %s.part.38 = load %s.part.5
  %s.fold.39 = add %s.37, %s.part.38
  store %s.4, %s.fold.39
  %s.40 = load %s.4
  ret %s.40
}

fn @main() -> i64 {
  local %xs.0: mem(64, 8)
  local %i.1: i64
  local %ys.2: mem(64, 8)
  local %i.3: i64
  %xs.addr.1 = local.addr %xs.0
  ptr.store %xs.addr.1, 2.0
  store %i.1, 1
  while {
    %i.2 = load %i.1
    %more.3 = cmp lt %i.2, 8
    cond %more.3
  } {
    %i.4 = load %i.1
    %elem.off.5 = mul %i.4, 8
    %elem.addr.6 = ptr.add %xs.addr.1, %elem.off.5
    ptr.store %elem.addr.6, 2.0
    %i.next.7 = add %i.4, 1
    store %i.1, %i.next.7
  }
  %ys.addr.8 = local.addr %ys.2
  ptr.store %ys.addr.8, 3.0
  store %i.3, 1
  while {
    %i.9 = load %i.3
    %more.10 = cmp lt %i.9, 8
    cond %more.10
  } {
    %i.11 = load %i.3
    %elem.off.12 = mul %i.11, 8
    %elem.addr.13 = ptr.add %ys.addr.8, %elem.off.12
    ptr.store %elem.addr.13, 3.0
    %i.next.14 = add %i.11, 1
    store %i.3, %i.next.14
  }
  %xs.addr.15 = local.addr %xs.0
  %ys.addr.16 = local.addr %ys.2
  %call.17 = call @dot(%xs.addr.15, 8, %ys.addr.16, 8)
  %conv.lo.18 = cmp ge %call.17, -9.2233720368547758e+18
  %conv.hi.19 = cmp lt %call.17, 9.2233720368547758e+18
  %conv.ok.20 = select %conv.lo.18, %conv.hi.19, false
  %conv.bad.21 = not %conv.ok.20
  if %conv.bad.21 {
    call @$rt.Paykan_panic_float_to_int(%call.17)
    unreachable
  }
  %conv.int.22 = ftoi %call.17
  %sub.23 = sub %conv.int.22, 48
  ret %sub.23
}
extern fn @$rt.Paykan_panic_index(i64, i64) -> void
extern fn @$rt.Paykan_panic_float_to_int(f64) -> void
)";

// An async function, a task started by calling it and awaited; `async fn
// main` runs as a task of its own that the plain @main awaits.
//
//   async fn twice(n: int) -> int { return n * 2; }
//   async fn main() -> int {
//     let t = twice(21);
//     return (await t) - 42;
//   }
const char *const kTwice = R"(module "main"

async fn @twice(%n.1: i64) -> i64 {
  local %n.0: i64
  store %n.0, %n.1
  %n.2 = load %n.0
  %mul.3 = mul %n.2, 2
  ret %mul.3
}

async fn @main.async() -> i64 {
  local %t.0: ptr
  %task.1 = async_call @twice(21)
  store %t.0, %task.1
  %t.2 = load %t.0
  %awaited.3 = await i64, %t.2
  %sub.4 = sub %awaited.3, 42
  ret %sub.4
}

fn @main() -> i64 {
  %task.1 = async_call @main.async()
  %result.2 = await i64, %task.1
  ret %result.2
}
)";

} // namespace

// The annotated while imports as one pir.parallel (no pir.while) that
// names the counter, the privates, the partial and its operation; its body
// is the loop's with the counter's first load, the index, as the block
// argument and its first store, the increment, left out.
TEST(ParallelLowering, AParallelWhileImportsAsAPIRParallel) {
  auto ctx = mlir_backend::createContext();
  std::string err;
  auto mod = lowerText(kLightSum, *ctx, mlir_backend::Stage::PIR, err);
  ASSERT_TRUE(mod) << err;
  mlir::Operation *top = mod->getOperation();
  const std::string text = print(*mod);
  mlir::Operation *fn = functionNamed(top, "sum");
  ASSERT_TRUE(fn) << text;
  auto parallels = opsNamed(fn, "pir.parallel");
  ASSERT_EQ(parallels.size(), 1u) << text;
  EXPECT_TRUE(opsNamed(fn, "pir.while").empty()) << text;
  EXPECT_NE(text.find("counter %par.k : !pir.local<i64> privates(%i : "
                      "!pir.local<i64>) partials(%s.part : !pir.local<i64>) "
                      "reductions [0] {"),
            std::string::npos)
      << text;
  // The counter is the op's second operand (after the count): the body
  // neither loads nor stores it.
  mlir::Operation *par = parallels[0];
  mlir::Value counter = par->getOperand(1);
  for (mlir::Operation *load : opsNamed(par, "pir.load"))
    EXPECT_NE(load->getOperand(0), counter) << text;
  for (mlir::Operation *store : opsNamed(par, "pir.store"))
    EXPECT_NE(store->getOperand(1), counter) << text;
  EXPECT_EQ(par->getRegion(0).front().getNumArguments(), 1u) << text;
  expectRoundTrip(text, *ctx);
}

// The shared lowering: an scf.parallel over k = min(count, 256) chunks,
// each with a slot of its own for the iteration's local and for the
// partial, which starts at the operation's identity, accumulates over the
// chunk's range in an inner loop and is written once into the chunk's slot
// of the function's 256 partials; then a loop over the same k chunks, in
// order, folding each slot into the function's partial (the design's
// reproducible reduction); and the counter left at the count, as the loop
// run in order leaves it.
TEST(ParallelLowering, TheChunksArePartialsFoldedInOrder) {
  auto ctx = mlir_backend::createContext();
  std::string err;
  auto mod = lowerText(kLightSum, *ctx, mlir_backend::Stage::SCF, err);
  ASSERT_TRUE(mod) << err;
  mlir::Operation *top = mod->getOperation();
  const std::string scf = print(*mod);
  for (const char *left : {"pir.parallel", "pir.while", "pir.local"})
    EXPECT_TRUE(opsNamed(top, left).empty()) << left << "\n" << scf;
  mlir::Operation *fn = functionNamed(top, "pk_main_Ssum");
  ASSERT_TRUE(fn) << scf;
  EXPECT_EQ(nameOf(fn), "func.func") << scf;

  auto parallels = opsNamed(fn, "scf.parallel");
  ASSERT_EQ(parallels.size(), 1u) << scf;
  mlir::Operation *par = parallels[0];
  // Over the chunks, 0 to k by 1.
  mlir::Operation *k = chunkCountOf(par);
  ASSERT_TRUE(k) << scf;
  EXPECT_EQ(constantOf(par->getOperand(0)).value_or(-1), 0) << scf;
  EXPECT_EQ(constantOf(par->getOperand(2)).value_or(-1), 1) << scf;
  mlir::Value count = k->getOperand(0);

  // The partials of every chunk, 256 slots of the reduction's i64, are the
  // function's, allocated before the choice of path with its other slots.
  auto arrays = allocasNamed(fn, "partials");
  ASSERT_EQ(arrays.size(), 1u) << scf;
  EXPECT_EQ(arrays[0]->getParentOp(), fn) << scf;
  EXPECT_EQ(arrays[0]->getAttrOfType<mlir::IntegerAttr>("size").getInt(),
            256 * 8)
      << scf;

  // The chunk's own slots: the loop variable, and the partial, which
  // starts at add's identity before the chunk's range runs.
  EXPECT_EQ(allocasNamed(par, "private").size(), 1u) << scf;
  auto partial = allocasNamed(par, "partial");
  ASSERT_EQ(partial.size(), 1u) << scf;
  EXPECT_EQ(partial[0]->getParentOp(), par) << scf;
  auto partialStores = storesTo(par, partial[0]);
  ASSERT_EQ(partialStores.size(), 2u) << scf; // the identity, the body's
  EXPECT_EQ(constantOf(partialStores[0]->getOperand(1)).value_or(-1), 0) << scf;
  // The chunk's range, an inner loop: the body accumulates into the
  // chunk's partial, not the function's.
  auto inner = opsNamed(par, "scf.for");
  ASSERT_EQ(inner.size(), 1u) << scf;
  EXPECT_EQ(inner[0]->getParentOp(), par) << scf;
  EXPECT_EQ(partialStores[1]->getParentOp(), inner[0]) << scf;
  EXPECT_FALSE(opsNamed(inner[0], "arith.addi").empty()) << scf;
  EXPECT_TRUE(opsNamed(inner[0], "scf.parallel").empty()) << scf;
  // Written once, after the range, into the chunk's slot of the partials.
  size_t copies = 0;
  for (mlir::Operation *store : opsNamed(par, "pir.ptr_store"))
    if (slotOf(store->getOperand(0)) == arrays[0]) {
      ++copies;
      EXPECT_EQ(store->getParentOp(), par) << scf;
      EXPECT_TRUE(definedBy(store->getOperand(1), "pir.ptr_load")) << scf;
    }
  EXPECT_EQ(copies, 1u) << scf;

  // The fold, after the chunks: the same k, in order, each chunk's slot
  // added into the function's partial.
  mlir::Operation *fold = nextNamed(par, "scf.for");
  ASSERT_TRUE(fold) << scf;
  EXPECT_EQ(chunkCountOf(fold), k) << scf;
  EXPECT_EQ(constantOf(fold->getOperand(0)).value_or(-1), 0) << scf;
  EXPECT_EQ(constantOf(fold->getOperand(2)).value_or(-1), 1) << scf;
  EXPECT_EQ(opsNamed(fold, "arith.addi").size(), 1u) << scf;
  size_t reads = 0;
  for (mlir::Operation *load : opsNamed(fold, "pir.ptr_load"))
    reads += slotOf(load->getOperand(0)) == arrays[0];
  EXPECT_EQ(reads, 1u) << scf;
  auto foldStores = opsNamed(fold, "pir.ptr_store");
  ASSERT_EQ(foldStores.size(), 1u) << scf;
  EXPECT_EQ(allocaName(slotOf(foldStores[0]->getOperand(0))), "s.part") << scf;

  // The counter.
  auto counters = allocasNamed(fn, "par.k");
  ASSERT_EQ(counters.size(), 1u) << scf;
  bool leftAtCount = false;
  for (mlir::Operation *store : storesTo(fn, counters[0]))
    leftAtCount |= store->getOperand(1) == count;
  EXPECT_TRUE(leftAtCount) << scf;

  // Printed, it reads as the design describes it, and parses back.
  for (const char *text :
       {"arith.constant 256 : i64", "arith.minsi %",
        "pir.alloca \"partials\", 2048, 8",
        "scf.parallel (%arg1) = (%c0) to (%", "pir.alloca \"private\", 8, 8",
        "pir.alloca \"partial\", 8, 8", "scf.reduce"})
    EXPECT_NE(scf.find(text), std::string::npos) << text << "\n" << scf;
  expectRoundTrip(scf, *ctx);
}

// Two reductions: each has its own 256 partials and its own slot in every
// chunk, starting at its operation's identity (0 for add, 1.0 for mul),
// and the fold combines each with its operation.
TEST(ParallelLowering, EachReductionHasItsOwnPartials) {
  auto ctx = mlir_backend::createContext();
  std::string err;
  auto mod = lowerText(kTwoReductions, *ctx, mlir_backend::Stage::SCF, err);
  ASSERT_TRUE(mod) << err;
  const std::string scf = print(*mod);
  mlir::Operation *fn = functionNamed(mod->getOperation(), "pk_main_Sboth");
  ASSERT_TRUE(fn) << scf;
  auto parallels = opsNamed(fn, "scf.parallel");
  ASSERT_EQ(parallels.size(), 1u) << scf;
  mlir::Operation *par = parallels[0];
  mlir::Operation *k = chunkCountOf(par);
  ASSERT_TRUE(k) << scf;

  auto arrays = allocasNamed(fn, "partials");
  ASSERT_EQ(arrays.size(), 2u) << scf;
  for (mlir::Operation *array : arrays) {
    EXPECT_EQ(array->getParentOp(), fn) << scf;
    EXPECT_EQ(array->getAttrOfType<mlir::IntegerAttr>("size").getInt(), 256 * 8)
        << scf;
  }
  // In the order of the annotation: the i64 add, then the f64 mul.
  auto partials = allocasNamed(par, "partial");
  ASSERT_EQ(partials.size(), 2u) << scf;
  auto sumStores = storesTo(par, partials[0]);
  auto productStores = storesTo(par, partials[1]);
  ASSERT_EQ(sumStores.size(), 2u) << scf;
  ASSERT_EQ(productStores.size(), 2u) << scf;
  EXPECT_EQ(constantOf(sumStores[0]->getOperand(1)).value_or(-1), 0) << scf;
  EXPECT_EQ(floatConstantOf(productStores[0]->getOperand(1)).value_or(-1.0),
            1.0)
      << scf;
  // Each chunk writes each partial into its own array, once.
  for (mlir::Operation *array : arrays) {
    size_t copies = 0;
    for (mlir::Operation *store : opsNamed(par, "pir.ptr_store"))
      if (slotOf(store->getOperand(0)) == array) {
        ++copies;
        EXPECT_EQ(store->getParentOp(), par) << scf;
      }
    EXPECT_EQ(copies, 1u) << scf;
  }
  // The fold adds the one and multiplies the other (its other
  // multiplications are the slots' byte offsets, i64).
  mlir::Operation *fold = nextNamed(par, "scf.for");
  ASSERT_TRUE(fold) << scf;
  EXPECT_EQ(chunkCountOf(fold), k) << scf;
  EXPECT_EQ(opsNamed(fold, "arith.addi").size(), 1u) << scf;
  EXPECT_EQ(opsNamed(fold, "arith.mulf").size(), 1u) << scf;
  EXPECT_TRUE(opsNamed(fold, "arith.addf").empty()) << scf;
  std::vector<std::string> folded;
  for (mlir::Operation *store : opsNamed(fold, "pir.ptr_store"))
    folded.push_back(allocaName(slotOf(store->getOperand(0))));
  EXPECT_EQ(folded, (std::vector<std::string>{"s.part", "p.part"})) << scf;
  expectRoundTrip(scf, *ctx);
}

// A light body, one with no call and no loop of its own, runs in order on
// the calling thread below 4096 iterations, where a fork and a join cost
// more than the loop (benchmarks/README.md): an scf.if on the trip count,
// its then-region the loop over 0..count on the function's own slots, its
// else-region the chunks and the fold.  The runtime's own calls, a bounds
// check's panic, leave a body light.
TEST(ParallelLowering, ALightBodyRunsInlineBelowTheThreshold) {
  struct Case {
    const char *program;
    const char *function;
  };
  for (const Case &c :
       {Case{kLightSum, "pk_main_Ssum"}, Case{kDot, "pk_main_Sdot"}}) {
    SCOPED_TRACE(c.function);
    auto ctx = mlir_backend::createContext();
    std::string err;
    auto mod = lowerText(c.program, *ctx, mlir_backend::Stage::SCF, err);
    ASSERT_TRUE(mod) << err;
    const std::string scf = print(*mod);
    mlir::Operation *fn = functionNamed(mod->getOperation(), c.function);
    ASSERT_TRUE(fn) << scf;
    mlir::Operation *test = inlineTest(fn);
    ASSERT_TRUE(test) << scf;
    EXPECT_EQ(test->getParentOp(), fn) << scf;
    mlir::Operation *cmp = definedBy(test->getOperand(0), "arith.cmpi");
    ASSERT_TRUE(cmp) << scf;
    mlir::Value count = cmp->getOperand(0);
    // Strictly below the threshold: `count < 4096`.
    size_t tests = 0;
    for (const std::string &line : linesWith(scf, "arith.cmpi slt, "))
      tests += line.find(", %c4096_i64") != std::string::npos;
    EXPECT_EQ(tests, 1u) << scf;

    // Then: one loop, 0..count, on the function's slots.
    mlir::Region &then = test->getRegion(0);
    EXPECT_TRUE(opsIn(then, "scf.parallel").empty()) << scf;
    auto loops = opsIn(then, "scf.for");
    ASSERT_EQ(loops.size(), 1u) << scf;
    EXPECT_EQ(constantOf(loops[0]->getOperand(0)).value_or(-1), 0) << scf;
    mlir::Operation *cast =
        definedBy(loops[0]->getOperand(1), "arith.index_cast");
    ASSERT_TRUE(cast) << scf;
    EXPECT_EQ(cast->getOperand(0), count) << scf;
    for (mlir::Operation *alloca : opsIn(then, "pir.alloca"))
      EXPECT_TRUE(allocaName(alloca) != "partial" &&
                  allocaName(alloca) != "private")
          << scf;
    // Else: the chunks, then the fold.
    mlir::Region &other = test->getRegion(1);
    auto parallels = opsIn(other, "scf.parallel");
    ASSERT_EQ(parallels.size(), 1u) << scf;
    ASSERT_TRUE(chunkCountOf(parallels[0])) << scf;
    EXPECT_EQ(chunkCountOf(parallels[0])->getOperand(0), count) << scf;
    EXPECT_TRUE(nextNamed(parallels[0], "scf.for")) << scf;
    // The partials are the function's whichever path runs.
    auto arrays = allocasNamed(fn, "partials");
    ASSERT_EQ(arrays.size(), 1u) << scf;
    EXPECT_EQ(arrays[0]->getParentOp(), fn) << scf;
  }

  // The dot product's body calls the runtime's panic under its bounds
  // checks, in the chunks as in the loop in order.
  auto ctx = mlir_backend::createContext();
  std::string err;
  auto mod = lowerText(kDot, *ctx, mlir_backend::Stage::SCF, err);
  ASSERT_TRUE(mod) << err;
  const std::string scf = print(*mod);
  mlir::Operation *fn = functionNamed(mod->getOperation(), "pk_main_Sdot");
  ASSERT_TRUE(fn) << scf;
  auto parallels = opsNamed(fn, "scf.parallel");
  ASSERT_EQ(parallels.size(), 1u) << scf;
  size_t panics = 0;
  for (mlir::Operation *call : opsNamed(parallels[0], "func.call"))
    panics += calleeOf(call) == "Paykan_panic_index";
  EXPECT_EQ(panics, 2u) << scf;
  EXPECT_EQ(opsNamed(parallels[0], "arith.addf").size(), 1u) << scf;
}

// A body with a loop of its own, or a call of the program's function, may
// be heavy however short the loop: no test on the trip count, the chunks
// directly in the function, the loop or the call in the chunk's range.
TEST(ParallelLowering, ABodyWithALoopOrACallAlwaysForks) {
  struct Case {
    const char *program;
    const char *inside;
    const char *callee;
    size_t privates;
  };
  for (const Case &c : {Case{kHeavyLoop, "scf.while", "", 5},
                        Case{kHeavyCall, "func.call", "pk_main_Sstep", 1}}) {
    SCOPED_TRACE(c.inside);
    auto ctx = mlir_backend::createContext();
    std::string err;
    auto mod = lowerText(c.program, *ctx, mlir_backend::Stage::SCF, err);
    ASSERT_TRUE(mod) << err;
    const std::string scf = print(*mod);
    mlir::Operation *fn = functionNamed(mod->getOperation(), "pk_main_Swork");
    ASSERT_TRUE(fn) << scf;
    EXPECT_FALSE(inlineTest(fn)) << scf;
    for (mlir::Operation *constant : opsNamed(fn, "arith.constant"))
      EXPECT_NE(constantOf(constant->getResult(0)).value_or(0), 4096) << scf;
    EXPECT_EQ(scf.find("4096"), std::string::npos) << scf;

    auto parallels = opsNamed(fn, "scf.parallel");
    ASSERT_EQ(parallels.size(), 1u) << scf;
    mlir::Operation *par = parallels[0];
    EXPECT_EQ(par->getParentOp(), fn) << scf;
    mlir::Operation *k = chunkCountOf(par);
    ASSERT_TRUE(k) << scf;
    // The chunk's range holds the body's loop or call; every local of an
    // iteration (the body's own loop's too) has a slot in the chunk.
    auto range = opsNamed(par, "scf.for");
    ASSERT_FALSE(range.empty()) << scf;
    EXPECT_EQ(range[0]->getParentOp(), par) << scf;
    auto inside = opsNamed(range[0], c.inside);
    ASSERT_EQ(inside.size(), 1u) << scf;
    if (*c.callee)
      EXPECT_EQ(calleeOf(inside[0]), c.callee) << scf;
    EXPECT_EQ(allocasNamed(par, "private").size(), c.privates) << scf;
    EXPECT_EQ(allocasNamed(par, "partial").size(), 1u) << scf;
    // The fold follows all the same.
    mlir::Operation *fold = nextNamed(par, "scf.for");
    ASSERT_TRUE(fold) << scf;
    EXPECT_EQ(chunkCountOf(fold), k) << scf;
    EXPECT_EQ(fold->getParentOp(), fn) << scf;
    expectRoundTrip(scf, *ctx);
  }
}

// The LLVM path: the chunks become an omp.parallel / omp.wsloop, and the
// loop carries `schedule(dynamic = 1)`, which convert-scf-to-openmp leaves
// off (the runtime's default, static, lets a thread on a slow core set the
// loop's time, benchmarks/README.md).  In the LLVM IR that is a dispatched
// loop, __kmpc_dispatch_init / __kmpc_dispatch_next, not the static
// __kmpc_for_static_init.
TEST(ParallelLowering, TheChunksAreScheduledDynamicallyOneAtATime) {
  std::string err;
  const std::string llvm = stageText(kLightSum, mlir_backend::Stage::LLVM, err);
  ASSERT_FALSE(llvm.empty()) << err;
  EXPECT_EQ(llvm.find("scf."), std::string::npos) << llvm;
  for (const char *text :
       {"llvm.func @pk_main_Ssum(%arg0: i64) -> i64", "omp.parallel {",
        "omp.loop_nest (%arg1) : i64 = (", "omp.yield", "omp.terminator"})
    EXPECT_NE(llvm.find(text), std::string::npos) << text << "\n" << llvm;
  auto loops = linesWith(llvm, "omp.wsloop");
  ASSERT_EQ(loops.size(), 1u) << llvm;
  const std::string &loop = loops[0];
  const std::string schedule = "omp.wsloop schedule(dynamic = ";
  const size_t at = loop.find(schedule);
  ASSERT_NE(at, std::string::npos) << loop;
  // The chunk is the constant 1, defined just before the loop.
  const size_t from = at + schedule.size();
  const std::string chunk = loop.substr(from, loop.find(' ', from) - from);
  EXPECT_EQ(loop.substr(from), chunk + " : i64) {") << loop;
  const size_t def = llvm.rfind(chunk + " = ", llvm.find(loop));
  ASSERT_NE(def, std::string::npos) << llvm;
  const std::string line = llvm.substr(def, llvm.find('\n', def) - def);
  EXPECT_EQ(line, chunk + " = llvm.mlir.constant(1 : i64) : i64") << llvm;

  const std::string ir = llvmIRText(kLightSum, err);
  ASSERT_FALSE(ir.empty()) << err;
  EXPECT_FALSE(linesWith(ir, "define i64 @pk_main_Ssum(i64").empty()) << ir;
  EXPECT_FALSE(linesWith(ir, "@__kmpc_fork_call(").empty()) << ir;
  EXPECT_FALSE(linesWith(ir, "call void @__kmpc_dispatch_init_").empty()) << ir;
  EXPECT_FALSE(linesWith(ir, "@__kmpc_dispatch_next_").empty()) << ir;
  EXPECT_TRUE(linesWith(ir, "__kmpc_for_static_init").empty()) << ir;
  EXPECT_TRUE(linesWith(ir, "__kmpc_for_static_fini").empty()) << ir;
}

// An async fn is an async.func whose body is an async.execute region (the
// runtime hands it to its worker pool, so a call returns at once with the
// task), the function awaiting the region's value for its result; a call
// of an async fn is an async.call, an await an async.await.  The LLVM
// path makes each a coroutine on MLIR's async runtime; the plain @main,
// no coroutine, blocks on its task.
TEST(ParallelLowering, AnAsyncFunctionIsAnAsyncFuncWithAnExecuteBody) {
  auto ctx = mlir_backend::createContext();
  std::string err;
  auto mod = lowerText(kTwice, *ctx, mlir_backend::Stage::SCF, err);
  ASSERT_TRUE(mod) << err;
  mlir::Operation *top = mod->getOperation();
  const std::string scf = print(*mod);
  for (const char *left : {"pir.async_call", "pir.await"})
    EXPECT_TRUE(opsNamed(top, left).empty()) << left << "\n" << scf;

  mlir::Operation *twice = functionNamed(top, "pk_main_Stwice");
  ASSERT_TRUE(twice) << scf;
  EXPECT_EQ(nameOf(twice), "async.func") << scf;
  auto executes = opsNamed(twice, "async.execute");
  ASSERT_EQ(executes.size(), 1u) << scf;
  EXPECT_EQ(executes[0]->getParentOp(), twice) << scf;
  EXPECT_EQ(opsNamed(executes[0], "async.yield").size(), 1u) << scf;
  EXPECT_FALSE(opsNamed(executes[0], "arith.muli").empty()) << scf;
  // The region's results: the token, then the value the function awaits.
  auto awaits = opsNamed(twice, "async.await");
  ASSERT_EQ(awaits.size(), 1u) << scf;
  EXPECT_EQ(awaits[0]->getParentOp(), twice) << scf;
  ASSERT_EQ(executes[0]->getNumResults(), 2u) << scf;
  EXPECT_EQ(awaits[0]->getOperand(0), executes[0]->getResult(1)) << scf;

  // main.async starts a task by an async.call inside its body and awaits
  // it there, a suspension point of the task.
  mlir::Operation *mainAsync = functionNamed(top, "pk_main_Smain_2Easync");
  ASSERT_TRUE(mainAsync) << scf;
  EXPECT_EQ(nameOf(mainAsync), "async.func") << scf;
  auto calls = opsNamed(mainAsync, "async.call");
  ASSERT_EQ(calls.size(), 1u) << scf;
  EXPECT_EQ(calleeOf(calls[0]), "pk_main_Stwice") << scf;
  EXPECT_EQ(nameOf(calls[0]->getParentOp()), "async.execute") << scf;
  size_t inBody = 0;
  for (mlir::Operation *await : opsNamed(mainAsync, "async.await"))
    inBody += nameOf(await->getParentOp()) == "async.execute";
  EXPECT_EQ(inBody, 1u) << scf;
  EXPECT_EQ(opsNamed(mainAsync, "async.await").size(), 2u) << scf;

  // The plain @main: a task of main.async, awaited; no region of its own.
  mlir::Operation *mainFn = functionNamed(top, "main");
  ASSERT_TRUE(mainFn) << scf;
  EXPECT_EQ(nameOf(mainFn), "func.func") << scf;
  auto mainCalls = opsNamed(mainFn, "async.call");
  ASSERT_EQ(mainCalls.size(), 1u) << scf;
  EXPECT_EQ(calleeOf(mainCalls[0]), "pk_main_Smain_2Easync") << scf;
  EXPECT_EQ(opsNamed(mainFn, "async.await").size(), 1u) << scf;
  EXPECT_TRUE(opsNamed(mainFn, "async.execute").empty()) << scf;

  for (const char *text :
       {"async.func @pk_main_Stwice(%arg0: i64) -> !async.value<i64> {",
        "%token, %bodyResults = execute -> !async.value<i64> {",
        "async.yield %", "await %bodyResults : !async.value<i64>",
        "async.call @pk_main_Stwice(%c21_i64) : (i64) -> !async.value<i64>",
        "async.await %"})
    EXPECT_NE(scf.find(text), std::string::npos) << text << "\n" << scf;

  // The LLVM dialect: coroutines on the async runtime, nothing of the
  // async dialect left.
  const std::string llvm = stageText(kTwice, mlir_backend::Stage::LLVM, err);
  ASSERT_FALSE(llvm.empty()) << err;
  EXPECT_EQ(llvm.find("async."), std::string::npos) << llvm;
  // NOLINTBEGIN(bugprone-suspicious-missing-comma)
  for (const char *text :
       {"llvm.func @pk_main_Stwice(%arg0: i64) -> !llvm.ptr attributes "
        "{passthrough = [\"presplitcoroutine\"]}",
        "llvm.intr.coro.begin", "llvm.intr.coro.suspend",
        "llvm.call @mlirAsyncRuntimeExecute(",
        "llvm.call @mlirAsyncRuntimeAwaitValueAndExecute(",
        "llvm.func @main() -> i64", "llvm.call @mlirAsyncRuntimeAwaitValue("})
    // NOLINTEND(bugprone-suspicious-missing-comma)
    EXPECT_NE(llvm.find(text), std::string::npos) << text << "\n" << llvm;

  const std::string ir = llvmIRText(kTwice, err);
  ASSERT_FALSE(ir.empty()) << err;
  EXPECT_FALSE(linesWith(ir, "define ptr @pk_main_Stwice(i64").empty()) << ir;
  EXPECT_FALSE(linesWith(ir, "define i64 @main()").empty()) << ir;
  for (const char *call :
       {"@mlirAsyncRuntimeExecute(", "@mlirAsyncRuntimeAwaitValueAndExecute(",
        "@mlirAsyncRuntimeAwaitValue("})
    EXPECT_FALSE(linesWith(ir, call).empty()) << call << "\n" << ir;
}

// The C path runs a parallel loop in order: one loop in the function, no
// chunks, no partials, no OpenMP.
TEST(ParallelLowering, TheCPathRunsAParallelLoopInOrder) {
  std::string err;
  const std::string c = cText(kLightSum, err);
  ASSERT_FALSE(c.empty()) << err;
  const std::string signature = "int64_t pk_main_Ssum(int64_t v1) {\n";
  const size_t at = c.find(signature);
  ASSERT_NE(at, std::string::npos) << c;
  const std::string body = c.substr(at, c.find("\n}\n", at) - at);
  EXPECT_EQ(linesWith(body, "for (").size(), 1u) << body;
  for (const char *text : {"omp_", "kmpc", "#pragma"})
    EXPECT_EQ(c.find(text), std::string::npos) << text << "\n" << c;
}
