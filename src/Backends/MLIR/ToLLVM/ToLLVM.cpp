// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The LLVM path of the mlir backend.  scf becomes cf (upstream), then one
// conversion takes func, cf and arith to the LLVM dialect with upstream
// patterns and the pir ops the shared lowering keeps with the patterns
// below: the only part of the backend written for LLVM alone.
//
// Types: every pir pointer (box, obj, ptr, weak) is !llvm.ptr; an optional
// is the literal struct { i1 present, payload }, passed by value as C passes
// `struct { bool present; <payload> value; }`.

#include "ToLLVM.h"

#include "Dialect/PIRDialect.h"

#include <llvm/Support/raw_ostream.h>
#include <mlir/Conversion/ArithToLLVM/ArithToLLVM.h>
#include <mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h>
#include <mlir/Conversion/FuncToLLVM/ConvertFuncToLLVM.h>
#include <mlir/Conversion/LLVMCommon/ConversionTarget.h>
#include <mlir/Conversion/LLVMCommon/TypeConverter.h>
#include <mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h>
#include <mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h>
#include <mlir/Dialect/Arith/IR/Arith.h>
#include <mlir/Dialect/ControlFlow/IR/ControlFlow.h>
#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/Dialect/LLVMIR/LLVMDialect.h>
#include <mlir/Dialect/SCF/IR/SCF.h>
#include <mlir/IR/BuiltinAttributes.h>
#include <mlir/Pass/Pass.h>
#include <mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h>
#include <mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h>
#include <mlir/Target/LLVMIR/Export.h>
#include <mlir/Transforms/DialectConversion.h>

namespace paykan::mlir_backend {

using namespace ::mlir;
namespace P = ::paykan::pir_dialect;

namespace {

Type llvmPtr(MLIRContext *ctx) { return LLVM::LLVMPointerType::get(ctx); }

/// The type converter: LLVM's, plus the pir types.
class PIRToLLVMTypeConverter : public LLVMTypeConverter {
public:
  explicit PIRToLLVMTypeConverter(MLIRContext *ctx) : LLVMTypeConverter(ctx) {
    addConversion([ctx](P::BoxType) { return llvmPtr(ctx); });
    addConversion([ctx](P::ObjType) { return llvmPtr(ctx); });
    addConversion([ctx](P::PtrType) { return llvmPtr(ctx); });
    addConversion([ctx](P::WeakType) { return llvmPtr(ctx); });
    addConversion([this, ctx](P::OptType t) -> Type {
      return LLVM::LLVMStructType::getLiteral(
          ctx, {IntegerType::get(ctx, 1), convertType(t.getPayload())});
    });
  }
};

template <typename Op> struct PIRPattern : public OpConversionPattern<Op> {
  using OpConversionPattern<Op>::OpConversionPattern;
  Type ptr() const { return llvmPtr(this->getContext()); }
  Value i64(ConversionPatternRewriter &r, Location loc, int64_t v) const {
    return LLVM::ConstantOp::create(r, loc, r.getI64Type(),
                                    r.getI64IntegerAttr(v));
  }
};

// -- Memory

struct AllocaLowering : PIRPattern<P::AllocaOp> {
  using PIRPattern::PIRPattern;
  LogicalResult matchAndRewrite(P::AllocaOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    Type bytes = LLVM::LLVMArrayType::get(r.getI8Type(), op.getSize());
    r.replaceOpWithNewOp<LLVM::AllocaOp>(op, ptr(), bytes,
                                         i64(r, op.getLoc(), 1),
                                         static_cast<unsigned>(op.getAlign()));
    return success();
  }
};

struct PtrLoadLowering : PIRPattern<P::PtrLoadOp> {
  using PIRPattern::PIRPattern;
  LogicalResult matchAndRewrite(P::PtrLoadOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    Type t = getTypeConverter()->convertType(op.getType());
    auto load = r.replaceOpWithNewOp<LLVM::LoadOp>(op, t, a.getPtr());
    if (op.getVtable())
      load.setInvariant(true); // !invariant.load
    return success();
  }
};

struct PtrStoreLowering : PIRPattern<P::PtrStoreOp> {
  using PIRPattern::PIRPattern;
  LogicalResult matchAndRewrite(P::PtrStoreOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    r.replaceOpWithNewOp<LLVM::StoreOp>(op, a.getValue(), a.getPtr());
    return success();
  }
};

struct PtrAddLowering : PIRPattern<P::PtrAddOp> {
  using PIRPattern::PIRPattern;
  LogicalResult matchAndRewrite(P::PtrAddOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    r.replaceOpWithNewOp<LLVM::GEPOp>(op, ptr(), r.getI8Type(), a.getPtr(),
                                      ValueRange(a.getOffset()));
    return success();
  }
};

struct MemCopyLowering : PIRPattern<P::MemCopyOp> {
  using PIRPattern::PIRPattern;
  LogicalResult matchAndRewrite(P::MemCopyOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    // The bytes' alignment is not known: copied as unaligned.
    r.replaceOpWithNewOp<LLVM::MemmoveOp>(op, a.getDst(), a.getSrc(),
                                          a.getCount(), false);
    return success();
  }
};

struct MemZeroLowering : PIRPattern<P::MemZeroOp> {
  using PIRPattern::PIRPattern;
  LogicalResult matchAndRewrite(P::MemZeroOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    Value zero = LLVM::ConstantOp::create(r, op.getLoc(), r.getI8Type(),
                                          r.getI8IntegerAttr(0));
    r.replaceOpWithNewOp<LLVM::MemsetOp>(op, a.getDst(), zero, a.getCount(),
                                         false);
    return success();
  }
};

struct AddressOfLowering : PIRPattern<P::AddressOfOp> {
  using PIRPattern::PIRPattern;
  LogicalResult matchAndRewrite(P::AddressOfOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    r.replaceOpWithNewOp<LLVM::AddressOfOp>(op, ptr(), op.getSymbol());
    return success();
  }
};

struct FnAddrLowering : PIRPattern<P::FnAddrOp> {
  using PIRPattern::PIRPattern;
  LogicalResult matchAndRewrite(P::FnAddrOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    r.replaceOpWithNewOp<LLVM::AddressOfOp>(op, ptr(), op.getCallee());
    return success();
  }
};

struct NullLowering : PIRPattern<P::NullOp> {
  using PIRPattern::PIRPattern;
  LogicalResult matchAndRewrite(P::NullOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    r.replaceOpWithNewOp<LLVM::ZeroOp>(op, ptr());
    return success();
  }
};

/// A pointer cast: free between pointers, ptrtoint / inttoptr otherwise.
struct CastLowering : PIRPattern<P::CastOp> {
  using PIRPattern::PIRPattern;
  LogicalResult matchAndRewrite(P::CastOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    Type to = getTypeConverter()->convertType(op.getType());
    Value v = a.getValue();
    bool fromPtr = isa<LLVM::LLVMPointerType>(v.getType());
    bool toPtr = isa<LLVM::LLVMPointerType>(to);
    if (fromPtr && toPtr)
      r.replaceOp(op, v);
    else if (fromPtr)
      r.replaceOpWithNewOp<LLVM::PtrToIntOp>(op, to, v);
    else if (toPtr)
      r.replaceOpWithNewOp<LLVM::IntToPtrOp>(op, to, v);
    else
      return r.notifyMatchFailure(op, "not a pointer cast");
    return success();
  }
};

struct ICallLowering : PIRPattern<P::ICallOp> {
  using PIRPattern::PIRPattern;
  LogicalResult matchAndRewrite(P::ICallOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    SmallVector<Type> params;
    for (Value v : a.getArgs())
      params.push_back(v.getType());
    Type result = op->getNumResults()
                      ? getTypeConverter()->convertType(op->getResultTypes()[0])
                      : LLVM::LLVMVoidType::get(getContext());
    auto fnType = LLVM::LLVMFunctionType::get(result, params);
    SmallVector<Value> operands{a.getCallee()};
    operands.append(a.getArgs().begin(), a.getArgs().end());
    auto call = LLVM::CallOp::create(r, op.getLoc(), fnType, operands);
    r.replaceOp(op, call->getResults());
    return success();
  }
};

// -- Optionals

struct SomeLowering : PIRPattern<P::SomeOp> {
  using PIRPattern::PIRPattern;
  LogicalResult matchAndRewrite(P::SomeOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    Location loc = op.getLoc();
    Type t = getTypeConverter()->convertType(op.getType());
    Value s = LLVM::PoisonOp::create(r, loc, t);
    Value present =
        LLVM::ConstantOp::create(r, loc, r.getI1Type(), r.getBoolAttr(true));
    s = LLVM::InsertValueOp::create(r, loc, s, present, ArrayRef<int64_t>{0});
    r.replaceOpWithNewOp<LLVM::InsertValueOp>(op, s, a.getValue(),
                                              ArrayRef<int64_t>{1});
    return success();
  }
};

struct NoneLowering : PIRPattern<P::NoneOp> {
  using PIRPattern::PIRPattern;
  LogicalResult matchAndRewrite(P::NoneOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    r.replaceOpWithNewOp<LLVM::ZeroOp>(
        op, getTypeConverter()->convertType(op.getType()));
    return success();
  }
};

struct IsSomeLowering : PIRPattern<P::IsSomeOp> {
  using PIRPattern::PIRPattern;
  LogicalResult matchAndRewrite(P::IsSomeOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    r.replaceOpWithNewOp<LLVM::ExtractValueOp>(op, a.getValue(),
                                               ArrayRef<int64_t>{0});
    return success();
  }
};

struct UnwrapLowering : PIRPattern<P::UnwrapOp> {
  using PIRPattern::PIRPattern;
  LogicalResult matchAndRewrite(P::UnwrapOp op, OpAdaptor a,
                                ConversionPatternRewriter &r) const override {
    r.replaceOpWithNewOp<LLVM::ExtractValueOp>(op, a.getValue(),
                                               ArrayRef<int64_t>{1});
    return success();
  }
};

// -- Globals

/// A private constant whose address is not significant, so identical ones
/// may be merged: string literals and byte data (aligned 1), words (8).
void constantGlobal(ConversionPatternRewriter &r, Operation *op, Type type,
                    StringRef name, Attribute value, uint64_t align) {
  auto g = LLVM::GlobalOp::create(r, op->getLoc(), type, /*isConstant=*/true,
                                  LLVM::Linkage::Private, name, value, align);
  g.setUnnamedAddr(LLVM::UnnamedAddr::Global);
  r.eraseOp(op);
}

struct CStrLowering : PIRPattern<P::CStrOp> {
  using PIRPattern::PIRPattern;
  LogicalResult matchAndRewrite(P::CStrOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    std::string data = op.getValue().str();
    data.push_back('\0');
    Type t = LLVM::LLVMArrayType::get(r.getI8Type(), data.size());
    constantGlobal(r, op, t, op.getSymName(), r.getStringAttr(data), 1);
    return success();
  }
};

struct DataLowering : PIRPattern<P::DataOp> {
  using PIRPattern::PIRPattern;
  LogicalResult matchAndRewrite(P::DataOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    ArrayRef<int64_t> words = op.getWords();
    auto shape = RankedTensorType::get({static_cast<int64_t>(words.size())},
                                       r.getI64Type());
    constantGlobal(r, op,
                   LLVM::LLVMArrayType::get(r.getI64Type(), words.size()),
                   op.getSymName(), DenseIntElementsAttr::get(shape, words), 8);
    return success();
  }
};

struct BytesLowering : PIRPattern<P::BytesOp> {
  using PIRPattern::PIRPattern;
  LogicalResult matchAndRewrite(P::BytesOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    ArrayRef<int8_t> bytes = op.getBytes();
    auto shape = RankedTensorType::get({static_cast<int64_t>(bytes.size())},
                                       r.getI8Type());
    constantGlobal(r, op, LLVM::LLVMArrayType::get(r.getI8Type(), bytes.size()),
                   op.getSymName(), DenseIntElementsAttr::get(shape, bytes), 1);
    return success();
  }
};

/// A global defined elsewhere (the runtime, another module): only its
/// address is taken.
template <typename Op> struct ExternGlobalLowering : PIRPattern<Op> {
  using PIRPattern<Op>::PIRPattern;
  LogicalResult matchAndRewrite(Op op, typename Op::Adaptor,
                                ConversionPatternRewriter &r) const override {
    LLVM::GlobalOp::create(r, op.getLoc(), r.getI8Type(), /*isConstant=*/false,
                           LLVM::Linkage::External, op.getSymName(),
                           Attribute());
    r.eraseOp(op);
    return success();
  }
};

/// A vtable: an array of function pointers, a null one for an abstract slot.
struct VTableLowering : PIRPattern<P::VTableOp> {
  using PIRPattern::PIRPattern;
  LogicalResult matchAndRewrite(P::VTableOp op, OpAdaptor,
                                ConversionPatternRewriter &r) const override {
    Location loc = op.getLoc();
    ArrayAttr entries = op.getEntries();
    Type t = LLVM::LLVMArrayType::get(ptr(), entries.size());
    auto g = LLVM::GlobalOp::create(r, loc, t, /*isConstant=*/true,
                                    LLVM::Linkage::External, op.getSymName(),
                                    Attribute(), 8);
    Block *init = r.createBlock(&g.getInitializerRegion());
    r.setInsertionPointToStart(init);
    Value array = LLVM::UndefOp::create(r, loc, t);
    for (auto [i, e] : llvm::enumerate(entries)) {
      Value fn;
      if (auto sym = dyn_cast<FlatSymbolRefAttr>(e))
        fn = LLVM::AddressOfOp::create(r, loc, ptr(), sym.getValue());
      else
        fn = LLVM::ZeroOp::create(r, loc, ptr());
      array = LLVM::InsertValueOp::create(
          r, loc, array, fn, ArrayRef<int64_t>{static_cast<int64_t>(i)});
    }
    LLVM::ReturnOp::create(r, loc, array);
    r.eraseOp(op);
    return success();
  }
};

struct ConvertToLLVMPass
    : public PassWrapper<ConvertToLLVMPass, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ConvertToLLVMPass)

  StringRef getArgument() const override { return "pir-to-llvm"; }
  StringRef getDescription() const override {
    return "Lower func, cf, arith and the pir ops to the LLVM dialect";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<LLVM::LLVMDialect>();
  }

  void runOnOperation() override {
    MLIRContext *ctx = &getContext();
    PIRToLLVMTypeConverter types(ctx);
    LLVMConversionTarget target(*ctx);
    target.addLegalOp<ModuleOp>();
    RewritePatternSet patterns(ctx);
    arith::populateArithToLLVMConversionPatterns(types, patterns);
    cf::populateControlFlowToLLVMConversionPatterns(types, patterns);
    populateFuncToLLVMConversionPatterns(types, patterns);
    patterns.add<AllocaLowering, PtrLoadLowering, PtrStoreLowering,
                 PtrAddLowering, MemCopyLowering, MemZeroLowering,
                 AddressOfLowering, FnAddrLowering, NullLowering, CastLowering,
                 ICallLowering, SomeLowering, NoneLowering, IsSomeLowering,
                 UnwrapLowering, CStrLowering, DataLowering, BytesLowering,
                 ExternGlobalLowering<P::ExternObjectOp>,
                 ExternGlobalLowering<P::ExternVTableOp>, VTableLowering>(types,
                                                                          ctx);
    // Each nested module (one per PIR module) on its own.
    FrozenRewritePatternSet frozen(std::move(patterns));
    for (ModuleOp m : modules())
      if (failed(applyFullConversion(m, target, frozen)))
        return signalPassFailure();
  }

  /// The modules to convert: the operated-on one, or each one nested in it.
  std::vector<ModuleOp> modules() {
    ModuleOp top = getOperation();
    std::vector<ModuleOp> ms;
    for (auto nested : top.getOps<ModuleOp>())
      ms.push_back(nested);
    if (ms.empty())
      ms.push_back(top);
    return ms;
  }
};

} // namespace

void loadLLVMDialects(MLIRContext &ctx) {
  DialectRegistry registry;
  registerBuiltinDialectTranslation(registry);
  registerLLVMDialectTranslation(registry);
  ctx.appendDialectRegistry(registry);
  ctx.getOrLoadDialect<LLVM::LLVMDialect>();
  ctx.getOrLoadDialect<cf::ControlFlowDialect>();
}

void addLLVMDialectPasses(PassManager &pm) {
  pm.addPass(createSCFToControlFlowPass());
  pm.addPass(std::make_unique<ConvertToLLVMPass>());
  pm.addPass(createReconcileUnrealizedCastsPass());
}

StatusOr<std::unique_ptr<llvm::Module>>
translateToLLVMIR(ModuleOp module, llvm::LLVMContext &ctx,
                  const std::string &name) {
  std::string diagnostics;
  ScopedDiagnosticHandler handler(module.getContext(), [&](Diagnostic &d) {
    llvm::raw_string_ostream os(diagnostics);
    os << d << "\n";
    return success();
  });
  auto llvmModule = translateModuleToLLVMIR(module, ctx, name);
  if (!llvmModule)
    return Status::error("MLIR -> LLVM IR translation failed:\n" + diagnostics);
  return llvmModule;
}

} // namespace paykan::mlir_backend
