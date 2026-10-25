// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The C path of the mlir backend.  One conversion takes func, scf and arith
// to EmitC with upstream patterns and the pir ops the shared lowering keeps
// with the patterns below; the translation unit around them (includes, the
// optionals' structs, Runtime.h-declared symbols left undeclared, C's main)
// is added at the MLIR level too, and MLIR's C emitter prints the result.
// The patterns are the only part of the backend written for C alone.
//
// Types are C's as Runtime.h spells them: a box is `PaykanShared *`, an
// obj `PaykanObject *`, a weak `PaykanWeak *` and a ptr `void *` (what a
// native function's prototype in Runtime.h uses); an optional is the
// struct `pkrt_opt_<payload>`, `{ bool present; <payload> value; }`; the
// ownership mask (ui64) is a `uint64_t`, as Runtime.h's `owned`.  A
// runtime function is called through Runtime.h's prototype, whose pointer
// types may be more precise than PIR's: its pointer arguments are passed
// as `void *` and its pointer result is cast, so no table of the runtime's
// C types is needed; Paykan_retain and Paykan_release are called through
// Runtime.h's inline fast paths.  A function pointer is held as a `void *`
// through uintptr_t (ISO C converts neither to the other directly), and
// called through a helper per signature, `pkicall_<n>`.
//
// The output is strictly conforming C11 under Runtime.h's target
// assumptions: nothing a `-std=c11 -pedantic-errors -Wall -Wextra -Werror`
// build rejects (the CStrictC11 ctest).

#include "ToEmitC.h"

#include "Dialect/PIRDialect.h"
#include "Lowering/PIRToStandard.h"
#include "Names.h"
#include "ToolchainNames.h"

#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/ADT/Twine.h>
#include <mlir/Conversion/ArithToEmitC/ArithToEmitC.h>
#include <mlir/Conversion/FuncToEmitC/FuncToEmitC.h>
#include <mlir/Conversion/SCFToEmitC/SCFToEmitC.h>
#include <mlir/Dialect/Arith/IR/Arith.h>
#include <mlir/Dialect/EmitC/IR/EmitC.h>
#include <mlir/Dialect/EmitC/Transforms/TypeConversions.h>
#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/Dialect/SCF/IR/SCF.h>
#include <mlir/IR/SymbolTable.h>
#include <mlir/Pass/Pass.h>
#include <mlir/Target/Cpp/CppEmitter.h>
#include <mlir/Transforms/DialectConversion.h>
#include <mlir/Transforms/Passes.h>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>

namespace paykan::mlir_backend {

using namespace ::mlir;
namespace P = ::paykan::pir_dialect;

namespace {

/// The name the program's `main` takes, C's main() being the entry point
/// that calls it.
constexpr const char *kPaykanMain = "paykan_main";

Type opaque(MLIRContext *ctx, StringRef c) {
  return emitc::OpaqueType::get(ctx, c);
}
Type pointerTo(Type t) { return emitc::PointerType::get(t); }
Type voidPtr(MLIRContext *ctx) { return pointerTo(opaque(ctx, "void")); }

/// The struct of an optional of @p payload: pkrt_opt_i64, _f64, _bool, _char.
std::optional<std::string> optStruct(Type payload) {
  if (payload.isInteger(1))
    return std::string("pkrt_opt_bool");
  if (payload.isInteger(8))
    return std::string("pkrt_opt_char");
  if (payload.isInteger(64))
    return std::string("pkrt_opt_i64");
  if (payload.isF64())
    return std::string("pkrt_opt_f64");
  return std::nullopt;
}

/// The C spelling of an EmitC type, for the C written as text here.
std::optional<std::string> cSpelling(Type t) {
  if (auto i = dyn_cast<IntegerType>(t)) {
    if (i.getWidth() == 1)
      return std::string("bool");
    return std::string(i.isUnsigned() ? "uint" : "int") +
           std::to_string(i.getWidth()) + "_t";
  }
  if (t.isF64())
    return std::string("double");
  if (t.isF32())
    return std::string("float");
  if (auto o = dyn_cast<emitc::OpaqueType>(t))
    return o.getValue().str();
  if (auto p = dyn_cast<emitc::PointerType>(t)) {
    auto pointee = cSpelling(p.getPointee());
    if (!pointee)
      return std::nullopt;
    return *pointee + " *";
  }
  return std::nullopt;
}

/// `"<text>"`: a C string literal of @p data (trigraphs and all).
std::string cStringLiteral(StringRef data) {
  std::string out = "\"";
  for (unsigned char c : data) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '?': // never part of a trigraph
      out += "\\?";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\t':
      out += "\\t";
      break;
    case '\r':
      out += "\\r";
      break;
    default:
      if (c >= 0x20 && c < 0x7f) {
        out += static_cast<char>(c);
      } else {
        // Three octal digits, so a following digit is not taken in.
        char buf[8];
        std::snprintf(buf, sizeof buf, "\\%03o", c);
        out += buf;
      }
    }
  }
  return out + "\"";
}

/// An array initializer: `{a, b, ...}`, `{0}` when empty (C11 has no `{}`).
std::string braced(const std::vector<std::string> &items) {
  if (items.empty())
    return "{0}";
  std::string s = "{";
  for (size_t i = 0; i < items.size(); ++i)
    s += (i ? ", " : "") + items[i];
  return s + "}";
}

/// What the conversion knows of the translation unit before it starts.
struct Unit {
  /// Functions Runtime.h declares (kRuntimeAttr): called through its
  /// prototypes, never declared here.
  llvm::StringSet<> RuntimeFunctions;
  /// The C symbols of the native functions the unit declares (kNativeAttr):
  /// hidden from the headers' declarations (addPrelude).
  std::set<std::string> NativeFunctions;
  /// A global's address in C: its name (an array), or `&name`.
  llvm::StringMap<std::string> Addresses;
  /// The pkicall_<n> helper of each indirect call's signature (PIR types).
  llvm::MapVector<Type, std::string> Trampolines;
  /// The optionals' structs: name -> payload type (EmitC).
  std::map<std::string, Type> Optionals;
  /// The module holding the program's main, and main's shape.
  ModuleOp MainModule;
  bool MainTakesArgs = false;
  bool MainReturns = false;
  std::string Error;

  void fail(const std::string &message) {
    if (Error.empty())
      Error = message;
  }
};

/// PIR's and the standard dialects' types in EmitC.
class EmitCTypeConverter : public TypeConverter {
public:
  EmitCTypeConverter(MLIRContext *ctx, Unit &unit) {
    addConversion([](Type t) -> std::optional<Type> {
      if (emitc::isSupportedEmitCType(t))
        return t;
      return std::nullopt;
    });
    addConversion(
        [ctx](P::BoxType) { return pointerTo(opaque(ctx, "PaykanShared")); });
    addConversion(
        [ctx](P::ObjType) { return pointerTo(opaque(ctx, "PaykanObject")); });
    addConversion(
        [ctx](P::WeakType) { return pointerTo(opaque(ctx, "PaykanWeak")); });
    addConversion([ctx](P::PtrType) { return voidPtr(ctx); });
    addConversion([this, ctx, &unit](P::OptType t) -> std::optional<Type> {
      auto name = optStruct(t.getPayload());
      Type payload = convertType(t.getPayload());
      if (!name || !payload)
        return std::nullopt;
      unit.Optionals.emplace(*name, payload);
      return opaque(ctx, *name);
    });
    populateEmitCSizeTTypeConversions(*this);
  }
};

template <typename Op> struct CPattern : public OpConversionPattern<Op> {
  CPattern(const TypeConverter &types, MLIRContext *ctx, const Unit &unit,
           PatternBenefit benefit = 1)
      : OpConversionPattern<Op>(types, ctx, benefit), U(unit) {}
  const Unit &U;

  Type convert(Type t) const {
    return this->getTypeConverter()->convertType(t);
  }
  Type ptr() const { return voidPtr(this->getContext()); }
  /// @p v as @p t: itself, or a C cast.
  static Value castTo(ConversionPatternRewriter &r, Location loc, Type t,
                      Value v) {
    if (v.getType() == t)
      return v;
    return emitc::CastOp::create(r, loc, t, v);
  }
  /// A variable holding @p v, for C's `.member` and `&`.
  static Value variable(ConversionPatternRewriter &r, Location loc, Value v) {
    Value var =
        emitc::VariableOp::create(r, loc, emitc::LValueType::get(v.getType()),
                                  emitc::OpaqueAttr::get(r.getContext(), ""));
    emitc::AssignOp::create(r, loc, var, v);
    return var;
  }
  /// `var.<member>`, read.
  static Value member(ConversionPatternRewriter &r, Location loc, Value var,
                      StringRef name, Type t) {
    Value m =
        emitc::MemberOp::create(r, loc, emitc::LValueType::get(t), name, var);
    return emitc::LoadOp::create(r, loc, t, m);
  }
  /// `*(T *)p`, an lvalue.
  Value deref(ConversionPatternRewriter &r, Location loc, Value p,
              Type t) const {
    Value typed = castTo(r, loc, pointerTo(t), p);
    return emitc::DereferenceOp::create(r, loc, emitc::LValueType::get(t),
                                        typed);
  }
};

// -- Functions and calls

/// A runtime function's declaration: Runtime.h's.
struct RuntimeDeclLowering : CPattern<func::FuncOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(func::FuncOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    if (!op.isDeclaration() || !op->hasAttr(kRuntimeAttr))
      return failure();
    r.eraseOp(op);
    return success();
  }
};

/// The function a call of runtime function @p callee calls: Runtime.h's
/// inline fast path of Paykan_retain and Paykan_release, the function
/// itself for any other.
StringRef calledRuntime(StringRef callee) {
  if (callee == names::kPaykanRetain)
    return names::kPaykanRetainInline;
  if (callee == names::kPaykanRelease)
    return names::kPaykanReleaseInline;
  return callee;
}

/// A call, by name.  A runtime function's pointer arguments go as `void *`
/// and its pointer result is cast; an unread result is not kept (C would
/// warn about the variable).
struct CallLowering : CPattern<func::CallOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(func::CallOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    if (op->getNumResults() > 1)
      return failure();
    Location loc = op.getLoc();
    bool runtime = U.RuntimeFunctions.contains(op.getCallee());
    SmallVector<Value> args;
    for (Value v : a.getOperands())
      args.push_back(runtime && isa<emitc::PointerType>(v.getType())
                         ? castTo(r, loc, ptr(), v)
                         : v);
    bool used = op->getNumResults() && !op->getResult(0).use_empty();
    Type result = used ? convert(op->getResult(0).getType()) : Type();
    if (used && !result)
      return failure();
    Type callResult =
        runtime && result && isa<emitc::PointerType>(result) ? ptr() : result;
    auto call = emitc::CallOpaqueOp::create(
        r, loc, callResult ? TypeRange{callResult} : TypeRange{},
        runtime ? calledRuntime(op.getCallee()) : op.getCallee(), args);
    if (!used) {
      r.eraseOp(op);
      return success();
    }
    r.replaceOp(op, castTo(r, loc, result, call.getResult(0)));
    return success();
  }
};

/// A call through a function pointer: pkicall_<n>(fn, args...).
struct ICallLowering : CPattern<P::ICallOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::ICallOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    SmallVector<Type> params(op.getArgs().getTypes());
    auto sig = FunctionType::get(getContext(), params, op->getResultTypes());
    auto helper = U.Trampolines.find(sig);
    if (helper == U.Trampolines.end())
      return failure();
    SmallVector<Value> args{a.getCallee()};
    args.append(a.getArgs().begin(), a.getArgs().end());
    bool used = op->getNumResults() && !op->getResult(0).use_empty();
    Type result = used ? convert(op->getResult(0).getType()) : Type();
    auto call = emitc::CallOpaqueOp::create(
        r, op.getLoc(), result ? TypeRange{result} : TypeRange{},
        helper->second, args);
    if (used)
      r.replaceOp(op, call.getResults());
    else
      r.eraseOp(op);
    return success();
  }
};

/// A function's address: `(void *)(uintptr_t)f`.
struct FnAddrLowering : CPattern<P::FnAddrOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::FnAddrOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    Location loc = op.getLoc();
    Value fn = emitc::LiteralOp::create(
        r, loc, opaque(getContext(), "PaykanMethod"), op.getCallee());
    Value bits =
        emitc::CastOp::create(r, loc, opaque(getContext(), "uintptr_t"), fn);
    r.replaceOp(op, castTo(r, loc, ptr(), bits));
    return success();
  }
};

// -- Memory

/// A local slot: a zeroed array of the alignment's unsigned integers, which
/// gives C the slot's alignment.
struct AllocaLowering : CPattern<P::AllocaOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::AllocaOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    int64_t align = op.getAlign(), size = op.getSize();
    if (align != 1 && align != 2 && align != 4 && align != 8)
      return r.notifyMatchFailure(op, "an alignment C has no integer for");
    Location loc = op.getLoc();
    Type word = IntegerType::get(getContext(), static_cast<unsigned>(8 * align),
                                 IntegerType::Unsigned);
    int64_t words = std::max<int64_t>((size + align - 1) / align, 1);
    Value slot =
        emitc::VariableOp::create(r, loc, emitc::ArrayType::get({words}, word),
                                  emitc::OpaqueAttr::get(getContext(), "{0}"));
    Value first = emitc::CastOp::create(r, loc, pointerTo(word), slot);
    r.replaceOp(op, castTo(r, loc, ptr(), first));
    return success();
  }
};

struct PtrLoadLowering : CPattern<P::PtrLoadOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::PtrLoadOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    Type t = convert(op.getType());
    if (!t)
      return failure();
    Location loc = op.getLoc();
    // A vtable's memory is read as the PaykanMethods it holds (#62): an
    // object's vtable pointer as a `PaykanMethod *`, a slot as a
    // `PaykanMethod`, which becomes a `void *` through uintptr_t.
    Type method = opaque(getContext(), "PaykanMethod");
    if (op.getVtable() || op.getMethod()) {
      Type held = op.getVtable() ? pointerTo(method) : method;
      Value v =
          emitc::LoadOp::create(r, loc, held, deref(r, loc, a.getPtr(), held));
      if (op.getMethod())
        v = emitc::CastOp::create(r, loc, opaque(getContext(), "uintptr_t"), v);
      r.replaceOp(op, castTo(r, loc, t, v));
      return success();
    }
    Value lv = deref(r, loc, a.getPtr(), t);
    r.replaceOpWithNewOp<emitc::LoadOp>(op, t, lv);
    return success();
  }
};

struct PtrStoreLowering : CPattern<P::PtrStoreOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::PtrStoreOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    Value v = a.getValue();
    // A new object's vtable pointer is written as the `PaykanMethod *` it
    // is read as (PtrLoadLowering).
    if (op.getVtable())
      v = castTo(r, op.getLoc(),
                 pointerTo(opaque(getContext(), "PaykanMethod")), v);
    Value lv = deref(r, op.getLoc(), a.getPtr(), v.getType());
    emitc::AssignOp::create(r, op.getLoc(), lv, v);
    r.eraseOp(op);
    return success();
  }
};

/// `(void *)((uint8_t *)p + offset)`
struct PtrAddLowering : CPattern<P::PtrAddOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::PtrAddOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    Location loc = op.getLoc();
    Type bytes =
        pointerTo(IntegerType::get(getContext(), 8, IntegerType::Unsigned));
    Value p = castTo(r, loc, bytes, a.getPtr());
    Value sum = emitc::AddOp::create(r, loc, bytes, p, a.getOffset());
    r.replaceOp(op, castTo(r, loc, ptr(), sum));
    return success();
  }
};

struct MemCopyLowering : CPattern<P::MemCopyOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::MemCopyOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    emitc::CallOpaqueOp::create(
        r, op.getLoc(), TypeRange{}, "memmove",
        ValueRange{a.getDst(), a.getSrc(), a.getCount()});
    r.eraseOp(op);
    return success();
  }
};

struct MemZeroLowering : CPattern<P::MemZeroOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::MemZeroOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    // memset(dst, 0, count)
    ArrayAttr args = r.getArrayAttr(
        {r.getIndexAttr(0), r.getI32IntegerAttr(0), r.getIndexAttr(1)});
    emitc::CallOpaqueOp::create(r, op.getLoc(), TypeRange{}, "memset",
                                ValueRange{a.getDst(), a.getCount()}, args);
    r.eraseOp(op);
    return success();
  }
};

/// A global's address (a `void *`, or the obj of a runtime object).
struct AddressOfLowering : CPattern<P::AddressOfOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::AddressOfOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    auto it = U.Addresses.find(op.getSymbol());
    if (it == U.Addresses.end())
      return r.notifyMatchFailure(op, "an unknown global");
    Type t = convert(op.getType());
    if (!t)
      return failure();
    Location loc = op.getLoc();
    Value addr = emitc::LiteralOp::create(r, loc, ptr(), it->second);
    // The cast drops a constant global's const, which C would not do itself.
    r.replaceOp(op, emitc::CastOp::create(r, loc, t, addr));
    return success();
  }
};

struct NullLowering : CPattern<P::NullOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::NullOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    Type t = convert(op.getType());
    if (!t)
      return failure();
    r.replaceOpWithNewOp<emitc::LiteralOp>(op, t, "NULL");
    return success();
  }
};

/// A pointer cast: between pointers, or to and from an integer.
struct CastLowering : CPattern<P::CastOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::CastOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    Type t = convert(op.getType());
    if (!t)
      return failure();
    r.replaceOp(op, castTo(r, op.getLoc(), t, a.getValue()));
    return success();
  }
};

/// arith.bitcast (between f64 and i64), which C makes with memcpy.
struct BitcastLowering : CPattern<arith::BitcastOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(arith::BitcastOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    Location loc = op.getLoc();
    Type to = convert(op.getType());
    Value in = a.getIn();
    if (!to || !to.isIntOrFloat() || !in.getType().isIntOrFloat())
      return failure();
    Value src = variable(r, loc, in);
    Value dst =
        emitc::VariableOp::create(r, loc, emitc::LValueType::get(to),
                                  emitc::OpaqueAttr::get(getContext(), ""));
    Value dstAddr = emitc::AddressOfOp::create(r, loc, pointerTo(to), dst);
    Value srcAddr =
        emitc::AddressOfOp::create(r, loc, pointerTo(in.getType()), src);
    ArrayAttr args = r.getArrayAttr({r.getIndexAttr(0), r.getIndexAttr(1),
                                     r.getI32IntegerAttr(static_cast<int32_t>(
                                         to.getIntOrFloatBitWidth() / 8))});
    emitc::CallOpaqueOp::create(r, loc, TypeRange{}, "memcpy",
                                ValueRange{dstAddr, srcAddr}, args);
    r.replaceOpWithNewOp<emitc::LoadOp>(op, to, dst);
    return success();
  }
};

/// A float comparison C's operator makes as IEEE 754 does: `==`, `<`, `<=`,
/// `>`, `>=` ordered and `!=` unordered, which is what the shared lowering
/// asks for (upstream's pattern adds NaN tests of its own around them).
struct FloatCmpLowering : CPattern<arith::CmpFOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(arith::CmpFOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    std::optional<emitc::CmpPredicate> p;
    switch (op.getPredicate()) {
    case arith::CmpFPredicate::OEQ:
      p = emitc::CmpPredicate::eq;
      break;
    case arith::CmpFPredicate::UNE:
      p = emitc::CmpPredicate::ne;
      break;
    case arith::CmpFPredicate::OLT:
      p = emitc::CmpPredicate::lt;
      break;
    case arith::CmpFPredicate::OLE:
      p = emitc::CmpPredicate::le;
      break;
    case arith::CmpFPredicate::OGT:
      p = emitc::CmpPredicate::gt;
      break;
    case arith::CmpFPredicate::OGE:
      p = emitc::CmpPredicate::ge;
      break;
    default:
      return failure(); // upstream's
    }
    r.replaceOpWithNewOp<emitc::CmpOp>(op, op.getType(), *p, a.getLhs(),
                                       a.getRhs());
    return success();
  }
};

/// INT64_MIN, which C cannot spell as a literal (its negation of
/// 9223372036854775808 is out of range).
struct Int64MinLowering : CPattern<arith::ConstantOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(arith::ConstantOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    auto v = dyn_cast<IntegerAttr>(op.getValue());
    if (!v || !v.getType().isInteger(64) ||
        v.getValue() != llvm::APInt::getSignedMinValue(64))
      return failure();
    r.replaceOpWithNewOp<emitc::LiteralOp>(op, op.getType(), "INT64_MIN");
    return success();
  }
};

// -- The ownership mask: a uint64_t

/// `UINT64_C(<v>)`: a mask constant, which C spells with its type.
Value maskConstant(ConversionPatternRewriter &r, Location loc, Type t,
                   uint64_t v) {
  return emitc::LiteralOp::create(r, loc, t,
                                  "UINT64_C(" + std::to_string(v) + ")");
}

/// `pir.owned %m[k]`: (m & 2^k) != 0.
struct OwnedLowering : CPattern<P::OwnedOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::OwnedOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    Location loc = op.getLoc();
    Type t = a.getMask().getType();
    Value set = emitc::BitwiseAndOp::create(
        r, loc, t, a.getMask(),
        maskConstant(r, loc, t, uint64_t{1} << op.getBit()));
    r.replaceOpWithNewOp<emitc::CmpOp>(op, r.getI1Type(),
                                       emitc::CmpPredicate::ne, set,
                                       maskConstant(r, loc, t, 0));
    return success();
  }
};

/// `pir.ownership`: the owned bits, or each passed bit while its bool is
/// true.
struct OwnershipLowering : CPattern<P::OwnershipOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::OwnershipOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    Location loc = op.getLoc();
    Type t = convert(op.getType());
    if (!t)
      return failure();
    Value mask = maskConstant(r, loc, t, op.getOwned());
    for (auto [k, passed] : llvm::zip(op.getPassedBits(), a.getPassed())) {
      Value set = emitc::ConditionalOp::create(
          r, loc, t, passed, maskConstant(r, loc, t, uint64_t{1} << k),
          maskConstant(r, loc, t, 0));
      mask = emitc::BitwiseOrOp::create(r, loc, t, mask, set);
    }
    r.replaceOp(op, mask);
    return success();
  }
};

// -- Optionals: the struct { bool present; <payload> value; }

struct SomeLowering : CPattern<P::SomeOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::SomeOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    Type t = convert(op.getType());
    if (!t)
      return failure();
    Location loc = op.getLoc();
    Type i1 = r.getI1Type();
    Value var =
        emitc::VariableOp::create(r, loc, emitc::LValueType::get(t),
                                  emitc::OpaqueAttr::get(getContext(), ""));
    Value present = emitc::MemberOp::create(r, loc, emitc::LValueType::get(i1),
                                            "present", var);
    emitc::AssignOp::create(
        r, loc, present,
        emitc::ConstantOp::create(r, loc, i1, r.getBoolAttr(true)));
    Value value = a.getValue();
    Value slot = emitc::MemberOp::create(
        r, loc, emitc::LValueType::get(value.getType()), "value", var);
    emitc::AssignOp::create(r, loc, slot, value);
    r.replaceOpWithNewOp<emitc::LoadOp>(op, t, var);
    return success();
  }
};

struct NoneLowering : CPattern<P::NoneOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::NoneOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    Type t = convert(op.getType());
    if (!t)
      return failure();
    Value var = emitc::VariableOp::create(
        r, op.getLoc(), emitc::LValueType::get(t),
        emitc::OpaqueAttr::get(getContext(), "{false, 0}"));
    r.replaceOpWithNewOp<emitc::LoadOp>(op, t, var);
    return success();
  }
};

struct IsSomeLowering : CPattern<P::IsSomeOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::IsSomeOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    Location loc = op.getLoc();
    Value var = variable(r, loc, a.getValue());
    r.replaceOp(op, member(r, loc, var, "present", r.getI1Type()));
    return success();
  }
};

struct UnwrapLowering : CPattern<P::UnwrapOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::UnwrapOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    Type t = convert(op.getType());
    if (!t)
      return failure();
    Location loc = op.getLoc();
    Value var = variable(r, loc, a.getValue());
    r.replaceOp(op, member(r, loc, var, "value", t));
    return success();
  }
};

// -- Globals

/// `static const <element> <name>[<n>] = <init>;`
void constantArray(ConversionPatternRewriter &r, Operation *op, StringRef name,
                   Type element, size_t n, const std::string &init) {
  auto type = emitc::ArrayType::get(
      {static_cast<int64_t>(std::max<size_t>(n, 1))}, element);
  emitc::GlobalOp::create(r, op->getLoc(), name, type,
                          emitc::OpaqueAttr::get(r.getContext(), init),
                          /*extern_specifier=*/false,
                          /*static_specifier=*/true,
                          /*const_specifier=*/true);
  r.eraseOp(op);
}

struct CStrLowering : CPattern<P::CStrOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::CStrOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    constantArray(r, op, op.getSymName(), opaque(getContext(), "char"),
                  op.getValue().size() + 1, cStringLiteral(op.getValue()));
    return success();
  }
};

struct DataLowering : CPattern<P::DataOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::DataOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    std::vector<std::string> words;
    for (int64_t w : op.getWords())
      words.push_back(w == INT64_MIN ? std::string("INT64_MIN")
                                     : std::to_string(w));
    constantArray(r, op, op.getSymName(), r.getI64Type(), words.size(),
                  braced(words));
    return success();
  }
};

struct BytesLowering : CPattern<P::BytesOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::BytesOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    std::vector<std::string> bytes;
    for (int8_t b : op.getBytes())
      bytes.push_back(
          std::to_string(static_cast<unsigned>(static_cast<uint8_t>(b))));
    constantArray(r, op, op.getSymName(),
                  IntegerType::get(getContext(), 8, IntegerType::Unsigned),
                  bytes.size(), braced(bytes));
    return success();
  }
};

/// A vtable: `PaykanMethod <name>[n] = {(PaykanMethod)f, NULL, ...};`, as
/// Runtime.h declares the runtime's own.
struct VTableLowering : CPattern<P::VTableOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::VTableOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    std::vector<std::string> entries;
    for (Attribute e : op.getEntries()) {
      if (auto sym = dyn_cast<FlatSymbolRefAttr>(e))
        entries.push_back("(PaykanMethod)" + sym.getValue().str());
      else
        entries.emplace_back("NULL"); // an empty slot
    }
    auto type = emitc::ArrayType::get(
        {static_cast<int64_t>(std::max<size_t>(entries.size(), 1))},
        opaque(getContext(), "PaykanMethod"));
    emitc::GlobalOp::create(
        r, op.getLoc(), op.getSymName(), type,
        emitc::OpaqueAttr::get(getContext(), braced(entries)));
    r.eraseOp(op);
    return success();
  }
};

/// Another module's vtable: `extern PaykanMethod <name>[];`.  The runtime's
/// is Runtime.h's.
struct ExternVTableLowering : CPattern<P::ExternVTableOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::ExternVTableOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    if (!op->hasAttr(kRuntimeAttr))
      emitc::VerbatimOp::create(
          r, op.getLoc(),
          ("extern PaykanMethod " + op.getSymName() + "[];").str());
    r.eraseOp(op);
    return success();
  }
};

/// A runtime object: Runtime.h's.
struct ExternObjectLowering : CPattern<P::ExternObjectOp> {
  using CPattern::CPattern;
  LogicalResult matchAndRewrite(P::ExternObjectOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    if (!op->hasAttr(kRuntimeAttr))
      return r.notifyMatchFailure(op, "an extern object not the runtime's");
    r.eraseOp(op);
    return success();
  }
};

// -- The translation unit

/// The modules of the operated-on module: it, or each one nested in it.
std::vector<ModuleOp> modulesOf(ModuleOp top) {
  std::vector<ModuleOp> ms;
  for (auto nested : top.getOps<ModuleOp>())
    ms.push_back(nested);
  if (ms.empty())
    ms.push_back(top);
  return ms;
}

/// What the conversion needs to know of @p m before it starts, and the
/// changes that make it C: unread constants dropped (C warns about an
/// unused static const), and the program's main renamed for C's main().
void prepare(ModuleOp m, Unit &u) {
  std::vector<Operation *> unused;
  for (Operation &op : *m.getBody()) {
    if (auto f = dyn_cast<func::FuncOp>(op)) {
      if (f->hasAttr(kRuntimeAttr))
        u.RuntimeFunctions.insert(f.getSymName());
      if (f->hasAttr(kNativeAttr))
        u.NativeFunctions.insert(f.getSymName().str());
      if (f.getSymName() == "main" && !f.isDeclaration()) {
        u.MainModule = m;
        u.MainTakesArgs = f.getNumArguments() == 2;
        u.MainReturns = f.getNumResults() == 1;
      }
      continue;
    }
    if (isa<P::CStrOp, P::DataOp, P::BytesOp>(op)) {
      if (SymbolTable::symbolKnownUseEmpty(&op, m)) {
        unused.push_back(&op);
        continue;
      }
    } else if (!isa<P::VTableOp, P::ExternVTableOp, P::ExternObjectOp>(op)) {
      continue;
    }
    StringRef name = SymbolTable::getSymbolName(&op);
    bool isObject = isa<P::ExternObjectOp>(op);
    u.Addresses[name] = (isObject ? "&" : "") + name.str();
  }
  for (Operation *op : unused)
    op->erase();
  if (u.MainModule == m) {
    auto main = m.lookupSymbol<func::FuncOp>("main");
    if (failed(SymbolTable::replaceAllSymbolUses(
            main, StringAttr::get(m.getContext(), kPaykanMain), m)))
      u.fail("cannot rename main");
    SymbolTable::setSymbolName(main, kPaykanMain);
    main.setPrivate(); // static: C's main() is the unit's entry point
  }
  // Each indirect call's signature gets its helper.
  m.walk([&](P::ICallOp call) {
    SmallVector<Type> params(call.getArgs().getTypes());
    auto sig =
        FunctionType::get(m.getContext(), params, call->getResultTypes());
    if (!u.Trampolines.count(sig))
      u.Trampolines.insert(
          {sig, "pkicall_" + std::to_string(u.Trampolines.size())});
  });
}

/// The C written as text around the converted module, at the top of the
/// unit: includes, the optionals' structs and the indirect-call helpers.
void addPrelude(ModuleOp top, Unit &u, const TypeConverter &types) {
  OpBuilder b(top.getContext());
  b.setInsertionPointToStart(top.getBody());
  Location loc = top.getLoc();
  emitc::VerbatimOp::create(
      b, loc, "/* Generated by paykan: the mlir backend's C (EmitC). */");
  // A native function's C symbol may name a function a header declares
  // with other C types (`native fn llabs(x: int) -> int = "llabs"` against
  // stdlib.h's `long long llabs(long long)`, an `exit` taking an int64_t):
  // the headers are included with the symbol renamed, so their declaration
  // is of another name, and the native function is declared from its own
  // signature after them.  The #undef also drops a macro a header defines
  // for the function (macOS's string.h defines memcpy as one when it
  // fortifies, and math.h isnan), so the C calls the native function.
  for (const std::string &sym : u.NativeFunctions)
    emitc::VerbatimOp::create(
        b, loc, (llvm::Twine("#define ") + sym + " pk__hidden_" + sym).str());
  for (const char *h :
       {"math.h", "stdbool.h", "stdint.h", "stdlib.h", "string.h"})
    emitc::IncludeOp::create(b, loc, h, /*is_standard_include=*/true);
  emitc::IncludeOp::create(b, loc, toolchain::tcnames::kRuntimeHeader);
  for (const std::string &sym : u.NativeFunctions)
    emitc::VerbatimOp::create(b, loc, "#undef " + sym);
  // The compilers know the C library's functions and warn of another
  // declaration of one: the native function's is meant.
  if (!u.NativeFunctions.empty())
    emitc::VerbatimOp::create(
        b, loc,
        "#if defined(__clang__)\n"
        "#pragma clang diagnostic ignored "
        "\"-Wincompatible-library-redeclaration\"\n"
        "#elif defined(__GNUC__)\n"
        "#pragma GCC diagnostic ignored \"-Wbuiltin-declaration-mismatch\"\n"
        "#endif");
  for (const auto &[name, payload] : u.Optionals)
    emitc::VerbatimOp::create(b, loc,
                              "typedef struct { bool present; " +
                                  cSpelling(payload).value_or("?") +
                                  " value; } " + name + ";");
  // Each native function is declared once, here, ahead of every module's
  // part: another module may call its symbol as a function of the runtime
  // (a float's `%` calls fmod, an index check Paykan_panic_index, C's main()
  // Paykan_heap_dump) without a declaration of its own, and the header's
  // is of another name.  Every module's declaration of a symbol has one
  // signature (lowering::checkNativeSymbols), the runtime's when it is one.
  std::vector<emitc::FuncOp> natives;
  for (ModuleOp m : modulesOf(top))
    for (auto f : m.getOps<emitc::FuncOp>())
      if (f.isExternal() && u.NativeFunctions.count(f.getSymName().str()))
        natives.push_back(f);
  std::set<std::string> declared;
  for (emitc::FuncOp f : natives) {
    if (declared.insert(f.getSymName().str()).second)
      f->moveBefore(b.getInsertionBlock(), b.getInsertionPoint());
    else
      f.erase();
  }
  for (const auto &[type, name] : u.Trampolines) {
    auto sig = cast<FunctionType>(type);
    std::string ret = "void";
    if (sig.getNumResults()) {
      auto r = cSpelling(types.convertType(sig.getResult(0)));
      if (!r)
        return u.fail("an indirect call's result has no C type");
      ret = *r;
    }
    std::string params = "void *fn", fnParams, args;
    for (auto [i, t] : llvm::enumerate(sig.getInputs())) {
      auto c = cSpelling(types.convertType(t));
      if (!c)
        return u.fail("an indirect call's argument has no C type");
      std::string a = "a" + std::to_string(i);
      params += ", " + *c + " " + a;
      fnParams += (i ? ", " : "") + *c;
      args += (i ? ", " : "") + a;
    }
    if (fnParams.empty())
      fnParams = "void";
    // static R pkicall_<n>(void *fn, P0 a0, ...) {
    //   return ((R (*)(P0, ...))(uintptr_t)fn)(a0, ...); }
    emitc::VerbatimOp::create(
        b, loc,
        (llvm::Twine("static ") + ret + " " + name + "(" + params + ") { " +
         (ret == "void" ? "" : "return ") + "((" + ret + " (*)(" + fnParams +
         "))(uintptr_t)fn)(" + args + "); }")
            .str());
  }
}

/// C's main(): the runtime set up as `run` does it, then the program's
/// main, whose result is the exit status.
void addEntryPoint(Unit &u) {
  if (!u.MainModule)
    return;
  ModuleOp m = u.MainModule;
  OpBuilder b(m.getContext());
  b.setInsertionPointToEnd(m.getBody());
  std::ostringstream c;
  c << "int main(int argc, char **argv) {\n"
    << "  int track = getenv(\"PAYKAN_TRACK_HEAP\") != NULL;\n"
    << "  if (track) {\n"
    << "    " << names::kPaykanHeapSetTracking << "(1);\n"
    << "    " << names::kPaykanHeapReset << "();\n"
    << "  }\n";
  std::string call = std::string(kPaykanMain) + "()";
  if (u.MainTakesArgs) {
    // args: view Str[], a slice of the arguments' strings (Paykan_args_new,
    // which also honours PAYKAN_NO_ARGS).
    c << "  int64_t args_len;\n"
      << "  PaykanShared **args = " << names::kPaykanArgsNew
      << "(argc, argv, &args_len);\n";
    call = std::string(kPaykanMain) + "(args, args_len)";
  } else {
    c << "  (void)argc;\n  (void)argv;\n";
  }
  if (u.MainReturns)
    c << "  int64_t rc = " << call << ";\n";
  else
    c << "  " << call << ";\n";
  if (u.MainTakesArgs)
    c << "  " << names::kPaykanArgsFree << "(args, args_len);\n";
  c << "  if (track)\n    " << names::kPaykanHeapDump << "();\n"
    << "  return " << (u.MainReturns ? "(int)rc" : "0") << ";\n}";
  emitc::VerbatimOp::create(b, m.getLoc(), c.str());
}

/// Declarations of every function the module defines, at its top: a body
/// or a vtable may use one defined below it.  And `(void)p;` for each
/// parameter a function does not read (C warns about an unused one).
void declareFunctions(ModuleOp m) {
  OpBuilder b(m.getContext());
  b.setInsertionPointToStart(m.getBody());
  for (auto f : m.getOps<emitc::FuncOp>())
    if (!f.isExternal())
      emitc::DeclareFuncOp::create(b, f.getLoc(), f.getSymName());
  for (auto f : m.getOps<emitc::FuncOp>()) {
    if (f.isExternal())
      continue;
    OpBuilder body(m.getContext());
    body.setInsertionPointToStart(&f.getBody().front());
    for (BlockArgument p : f.getArguments())
      if (p.use_empty())
        emitc::VerbatimOp::create(
            body, f.getLoc(), body.getStringAttr("(void){};"), ValueRange{p});
  }
}

struct ConvertToEmitCPass
    : public PassWrapper<ConvertToEmitCPass, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ConvertToEmitCPass)

  StringRef getArgument() const override { return "pir-to-emitc"; }
  StringRef getDescription() const override {
    return "Lower func, scf, arith and the pir ops to EmitC, as one C unit";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<emitc::EmitCDialect>();
  }

  void runOnOperation() override {
    MLIRContext *ctx = &getContext();
    ModuleOp top = getOperation();
    Unit u;
    std::vector<ModuleOp> modules = modulesOf(top);
    for (ModuleOp m : modules)
      prepare(m, u);

    EmitCTypeConverter types(ctx, u);
    ConversionTarget target(*ctx);
    target.addLegalDialect<emitc::EmitCDialect>();
    target.addLegalOp<ModuleOp>();
    target.addIllegalDialect<func::FuncDialect, scf::SCFDialect,
                             arith::ArithDialect, P::PIRDialect>();
    RewritePatternSet patterns(ctx);
    populateFuncToEmitCPatterns(types, patterns);
    populateSCFToEmitCConversionPatterns(patterns, types);
    populateArithToEmitCPatterns(types, patterns);
    // Ahead of the upstream patterns for the same ops.
    patterns.add<RuntimeDeclLowering, CallLowering, FloatCmpLowering,
                 Int64MinLowering>(types, ctx, u, /*benefit=*/2);
    patterns.add<ICallLowering, FnAddrLowering, AllocaLowering, PtrLoadLowering,
                 PtrStoreLowering, PtrAddLowering, MemCopyLowering,
                 MemZeroLowering, AddressOfLowering, NullLowering, CastLowering,
                 BitcastLowering, OwnedLowering, OwnershipLowering,
                 SomeLowering, NoneLowering, IsSomeLowering, UnwrapLowering,
                 CStrLowering, DataLowering, BytesLowering, VTableLowering,
                 ExternVTableLowering, ExternObjectLowering>(types, ctx, u);
    if (failed(applyFullConversion(top, target, std::move(patterns))))
      return signalPassFailure();

    for (ModuleOp m : modules)
      declareFunctions(m);
    addPrelude(top, u, types);
    addEntryPoint(u);
    if (!u.Error.empty()) {
      top.emitError("the C path failed: ") << u.Error;
      return signalPassFailure();
    }
  }
};

/// C11's spelling of an empty parameter list, `(void)`, in place of the
/// `()` MLIR's C emitter prints (an old-style declaration in C, which
/// Clang's -Wstrict-prototypes rejects): on a function's declaration or
/// definition, the only top-level lines that end in `();` or `() {`.
std::string withPrototypes(const std::string &c) {
  std::string out;
  out.reserve(c.size());
  size_t at = 0;
  while (at < c.size()) {
    size_t end = c.find('\n', at);
    std::string line =
        c.substr(at, end == std::string::npos ? std::string::npos : end - at);
    bool topLevel = !line.empty() && line[0] != ' ' && line[0] != '#' &&
                    line[0] != '/' && line[0] != '}';
    for (const char *tail : {"();", "() {"}) {
      std::string_view t(tail);
      if (topLevel && line.size() > t.size() &&
          line.compare(line.size() - t.size(), t.size(), t) == 0) {
        line.insert(line.size() - t.size() + 1, "void");
        break;
      }
    }
    out += line;
    if (end == std::string::npos)
      break;
    out += '\n';
    at = end + 1;
  }
  return out;
}

} // namespace

void loadEmitCDialects(MLIRContext &ctx) {
  ctx.getOrLoadDialect<emitc::EmitCDialect>();
}

void addEmitCPasses(PassManager &pm) {
  // Unread values and results dropped first: C warns about a variable it
  // never reads.
  pm.addPass(createCanonicalizerPass());
  pm.addPass(std::make_unique<ConvertToEmitCPass>());
}

Status translateToC(ModuleOp module, llvm::raw_ostream &os) {
  std::string diagnostics;
  ScopedDiagnosticHandler handler(module.getContext(), [&](Diagnostic &d) {
    llvm::raw_string_ostream ds(diagnostics);
    ds << d << "\n";
    return success();
  });
  std::string c;
  llvm::raw_string_ostream cs(c);
  if (failed(emitc::translateToCpp(module, cs)))
    return Status::error("EmitC -> C translation failed:\n" + diagnostics);
  os << withPrototypes(c);
  return Status::ok();
}

} // namespace paykan::mlir_backend
