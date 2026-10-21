// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// PIR -> the `pir` MLIR dialect.  One pass over a verified module: every
// item, statement and instruction becomes the op of the same name, every
// value the op's result, and every constant operand an op of its own
// (arith.constant, pir.null, pir.addressof) next to its use.

#include "PIRImport.h"

#include "Dialect/PIRDialect.h"
#include "Lowering/PIRToStandard.h"

#include <llvm/Support/raw_ostream.h>
#include <mlir/Dialect/Arith/IR/Arith.h>
#include <mlir/IR/Builders.h>
#include <mlir/IR/Diagnostics.h>
#include <mlir/IR/Verifier.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace paykan::mlir_backend {

namespace {

namespace P = paykan::pir;
using namespace ::mlir;
using namespace ::paykan::pir_dialect;

class Importer {
public:
  Importer(const P::Module &module, bool isMain, MLIRContext &ctx)
      : M(module), IsMain(isMain), Ctx(ctx), B(&ctx),
        Loc(UnknownLoc::get(&ctx)) {}

  StatusOr<OwningOpRef<ModuleOp>> run() {
    OwningOpRef<ModuleOp> mod = ModuleOp::create(Loc, M.Name);
    if (IsMain)
      (*mod)->setAttr(kMainModuleAttr, UnitAttr::get(&Ctx));
    B.setInsertionPointToEnd(mod->getBody());
    importGlobals();
    for (const P::Class &c : M.Classes)
      importClass(c);
    for (const P::Function &f : M.Functions)
      importFunction(f);
    if (!Error.empty())
      return Status::error("PIR -> MLIR import of module '" + M.Name +
                           "': " + Error);
    return mod;
  }

private:
  const P::Module &M;
  bool IsMain;
  MLIRContext &Ctx;
  OpBuilder B;
  Location Loc;
  std::string Error;

  // -- Types

  Type type(P::Type t) {
    switch (t) {
    case P::Type::Void:
      return {};
    case P::Type::I64:
      return B.getI64Type();
    case P::Type::F64:
      return B.getF64Type();
    case P::Type::Bool:
      return B.getI1Type();
    case P::Type::Char:
      return B.getI8Type();
    case P::Type::Box:
      return BoxType::get(&Ctx);
    case P::Type::Obj:
      return ObjType::get(&Ctx);
    case P::Type::Ptr:
      return PtrType::get(&Ctx);
    case P::Type::Weak:
      return WeakType::get(&Ctx);
    case P::Type::OptI64:
    case P::Type::OptF64:
    case P::Type::OptBool:
    case P::Type::OptChar:
      return OptType::get(&Ctx, type(P::optionalPayload(t)));
    case P::Type::Mem:
      break; // a slot's type: slotType()
    }
    return {};
  }

  /// The type of a local or field: a `mem` one's size and alignment are the
  /// slot's.
  Type slotType(P::Type t, int64_t size, int64_t align) {
    if (t == P::Type::Mem)
      return MemType::get(&Ctx, size, align);
    return type(t);
  }

  FunctionType functionType(const P::Signature &sig) {
    std::vector<Type> params;
    params.reserve(sig.Params.size());
    for (P::Type t : sig.Params)
      params.push_back(type(t));
    std::vector<Type> results;
    if (sig.Ret != P::Type::Void)
      results.push_back(type(sig.Ret));
    return B.getFunctionType(params, results);
  }

  // -- Module-level items

  void importGlobals() {
    for (const P::CStrGlobal &g : M.CStrs)
      CStrOp::create(B, Loc, g.Name, g.Data);
    for (const P::DataGlobal &g : M.Datas)
      DataOp::create(B, Loc, g.Name, g.Words);
    for (const P::BytesGlobal &g : M.Bytes) {
      std::vector<int8_t> bytes(g.Bytes.begin(), g.Bytes.end());
      BytesOp::create(B, Loc, g.Name, bytes);
    }
    for (const P::ExternGlobal &g : M.Externs) {
      if (g.K == P::ExternGlobal::Object)
        ExternObjectOp::create(B, Loc, g.Name);
      else
        ExternVTableOp::create(B, Loc, g.Name);
    }
  }

  void importClass(const P::Class &c) {
    auto cls = ClassOp::create(
        B, Loc, c.Name,
        c.Super.empty() ? StringAttr() : B.getStringAttr(c.Super), c.IsExtern,
        c.Module.empty() ? StringAttr() : B.getStringAttr(c.Module));
    if (c.Shared)
      cls->setAttr(kSharedAttr, B.getUnitAttr());
    OpBuilder::InsertionGuard guard(B);
    B.setInsertionPointToEnd(&cls.getBody().emplaceBlock());
    for (const P::Field &f : c.Fields)
      FieldOp::create(B, Loc, f.Name, slotType(f.Ty, f.Size, f.Align));
    for (const P::VTableEntry &e : c.VTable)
      SlotOp::create(B, Loc, e.Slot,
                     e.Target.empty() ? FlatSymbolRefAttr()
                                      : FlatSymbolRefAttr::get(&Ctx, e.Target),
                     functionType(e.Sig));
  }

  // -- Functions

  /// Per-function state.
  std::unordered_map<P::ValueId, Value> Values;
  std::vector<Value> Locals;
  const P::Function *Fn = nullptr;

  void importFunction(const P::Function &f) {
    auto fn = FuncOp::create(
        B, Loc, f.Name, functionType(f.Sig),
        /*sym_visibility=*/StringAttr(),
        /*arg_attrs=*/ArrayAttr(),
        /*res_attrs=*/ArrayAttr(),
        f.Module.empty() ? StringAttr() : B.getStringAttr(f.Module),
        f.Symbol.empty() ? StringAttr() : B.getStringAttr(f.Symbol));
    if (f.Shared)
      fn->setAttr(kSharedAttr, B.getUnitAttr());
    if (f.IsExtern) {
      // A declaration: private, as a symbol without a definition must be.
      fn.setPrivate();
      return;
    }
    Fn = &f;
    Values.clear();
    Locals.clear();
    OpBuilder::InsertionGuard guard(B);
    Block *entry = fn.addEntryBlock();
    B.setInsertionPointToEnd(entry);
    for (size_t i = 0; i < f.Params.size(); ++i)
      Values[f.Params[i].Id] = entry->getArgument(static_cast<unsigned>(i));
    for (const P::Local &l : f.Locals)
      Locals.push_back(LocalOp::create(
          B, Loc, LocalType::get(&Ctx, slotType(l.Ty, l.Size, l.Align)),
          l.Name));
    importBlock(f.Body);
    Fn = nullptr;
  }

  void fail(const std::string &message) {
    if (Error.empty())
      Error =
          (Fn ? "in function '@" + Fn->Name + "': " : std::string()) + message;
  }

  /// Import @p b's statements into a new block of @p region.
  void importRegion(Region &region, const P::Block *b) {
    OpBuilder::InsertionGuard guard(B);
    B.setInsertionPointToEnd(&region.emplaceBlock());
    if (b)
      importBlock(*b);
  }

  void importBlock(const P::Block &b) {
    for (const P::Stmt &s : b.Stmts) {
      if (const auto *in = std::get_if<P::Instr>(&s)) {
        importInstr(*in);
      } else if (const auto *i = std::get_if<P::If>(&s)) {
        auto op = IfOp::create(B, Loc, operand(i->Cond));
        importRegion(op.getThenRegion(), i->Then.get());
        // An empty else is no else (and `else {}` would parse back as none).
        if (i->Else && !i->Else->Stmts.empty())
          importRegion(op.getElseRegion(), i->Else.get());
      } else if (const auto *sw = std::get_if<P::Switch>(&s)) {
        Value v = operand(sw->Value);
        SmallVector<Attribute> cases;
        for (const P::Switch::Case &c : sw->Cases)
          cases.push_back(constant(c.Value));
        auto op =
            SwitchOp::create(B, Loc, v, B.getArrayAttr(cases), cases.size());
        for (auto [region, c] : llvm::zip(op.getCaseRegions(), sw->Cases))
          importRegion(region, c.Body.get());
        // An empty default is no default, as an empty else is no else.
        if (sw->Default && !sw->Default->Stmts.empty())
          importRegion(op.getDefaultRegion(), sw->Default.get());
      } else if (const auto *w = std::get_if<P::While>(&s)) {
        auto op = WhileOp::create(B, Loc);
        {
          OpBuilder::InsertionGuard guard(B);
          B.setInsertionPointToEnd(&op.getCondRegion().emplaceBlock());
          if (w->CondBlock)
            importBlock(*w->CondBlock);
          CondOp::create(B, Loc, operand(w->Cond));
        }
        importRegion(op.getBodyRegion(), w->Body.get());
      } else if (std::holds_alternative<P::Break>(s)) {
        BreakOp::create(B, Loc);
      } else if (std::holds_alternative<P::Continue>(s)) {
        ContinueOp::create(B, Loc);
      } else if (const auto *r = std::get_if<P::Return>(&s)) {
        ReturnOp::create(B, Loc, r->Value ? operand(*r->Value) : Value());
      } else {
        UnreachableOp::create(B, Loc);
      }
    }
  }

  /// A constant operand (a switch case) as an attribute of the type the
  /// operand has as a value.
  Attribute constant(const P::Operand &op) {
    if (const auto *v = std::get_if<bool>(&op.V))
      return B.getIntegerAttr(B.getI1Type(), *v ? 1 : 0);
    if (const auto *v = std::get_if<char>(&op.V))
      return B.getIntegerAttr(B.getI8Type(), static_cast<int8_t>(*v));
    if (const auto *v = std::get_if<int64_t>(&op.V))
      return B.getI64IntegerAttr(*v);
    fail("a switch case that is not an integer, char or bool constant");
    return B.getI64IntegerAttr(0);
  }

  /// An operand as a value: a constant is materialised where it is used.
  Value operand(const P::Operand &op) {
    if (const auto *id = std::get_if<P::ValueId>(&op.V)) {
      auto it = Values.find(*id);
      if (it == Values.end()) {
        fail("use of undefined value %" + std::to_string(*id));
        return arith::ConstantIntOp::create(B, Loc, B.getI64Type(), 0);
      }
      return it->second;
    }
    if (const auto *v = std::get_if<int64_t>(&op.V))
      return arith::ConstantIntOp::create(B, Loc, B.getI64Type(), *v);
    if (const auto *v = std::get_if<double>(&op.V))
      return arith::ConstantFloatOp::create(B, Loc, B.getF64Type(),
                                            llvm::APFloat(*v));
    if (const auto *v = std::get_if<bool>(&op.V))
      return arith::ConstantIntOp::create(B, Loc, B.getI1Type(), *v ? 1 : 0);
    if (const auto *v = std::get_if<char>(&op.V))
      return arith::ConstantIntOp::create(B, Loc, B.getI8Type(),
                                          static_cast<int8_t>(*v));
    if (const auto *n = std::get_if<P::Operand::Null>(&op.V))
      return NullOp::create(B, Loc, type(n->Ty));
    const auto &sym = std::get<P::SymbolRef>(op.V);
    Type t = PtrType::get(&Ctx);
    for (const P::ExternGlobal &g : M.Externs)
      if (g.Name == sym.Name && g.K == P::ExternGlobal::Object)
        t = ObjType::get(&Ctx);
    return AddressOfOp::create(B, Loc, t, sym.Name);
  }

  std::vector<Value> operands(const P::Instr &in, size_t from = 0) {
    std::vector<Value> vs;
    for (size_t i = from; i < in.Args.size(); ++i)
      vs.push_back(operand(in.Args[i]));
    return vs;
  }

  void define(const P::Instr &in, Value v) {
    if (in.Result.Id != P::kNoValue)
      Values[in.Result.Id] = v;
  }

  /// The single result of @p op, or null for a void call.
  static Value resultOf(Operation *op) {
    return op->getNumResults() ? op->getResult(0) : Value();
  }

  Value local(const P::Instr &in) {
    if (in.Local >= Locals.size()) {
      fail("reference to unknown local " + std::to_string(in.Local));
      return {};
    }
    return Locals[in.Local];
  }

  static CmpPredicate predicate(P::CmpPred p) {
    switch (p) {
    case P::CmpPred::Eq:
      return CmpPredicate::eq;
    case P::CmpPred::Ne:
      return CmpPredicate::ne;
    case P::CmpPred::Lt:
      return CmpPredicate::lt;
    case P::CmpPred::Le:
      return CmpPredicate::le;
    case P::CmpPred::Gt:
      return CmpPredicate::gt;
    case P::CmpPred::Ge:
      return CmpPredicate::ge;
    }
    return CmpPredicate::eq;
  }

  /// The result types of an instruction that may produce no value.
  std::vector<Type> results(const P::Instr &in) {
    if (in.Result.Ty == P::Type::Void)
      return {};
    return {type(in.Result.Ty)};
  }

  void importInstr(const P::Instr &in) {
    Type rt = type(in.Result.Ty);
    auto arg = [&](size_t i) { return operand(in.Args[i]); };
    switch (in.Op) {
    case P::Opcode::Add:
      return define(in, AddOp::create(B, Loc, arg(0), arg(1)));
    case P::Opcode::Sub:
      return define(in, SubOp::create(B, Loc, arg(0), arg(1)));
    case P::Opcode::Mul:
      return define(in, MulOp::create(B, Loc, arg(0), arg(1)));
    case P::Opcode::Div:
      return define(in, DivOp::create(B, Loc, arg(0), arg(1)));
    case P::Opcode::Rem:
      return define(in, RemOp::create(B, Loc, arg(0), arg(1)));
    case P::Opcode::UDiv:
      return define(in, UDivOp::create(B, Loc, arg(0), arg(1)));
    case P::Opcode::URem:
      return define(in, URemOp::create(B, Loc, arg(0), arg(1)));
    case P::Opcode::Neg:
      return define(in, NegOp::create(B, Loc, arg(0)));
    case P::Opcode::Not:
      return define(in, NotOp::create(B, Loc, arg(0)));
    case P::Opcode::Cmp:
      return define(in,
                    CmpOp::create(B, Loc, predicate(in.Pred), arg(0), arg(1)));
    case P::Opcode::UCmp:
      return define(in,
                    UCmpOp::create(B, Loc, predicate(in.Pred), arg(0), arg(1)));
    case P::Opcode::Select:
      return define(in, SelectOp::create(B, Loc, arg(0), arg(1), arg(2)));
    case P::Opcode::IToF:
      return define(in, IToFOp::create(B, Loc, arg(0)));
    case P::Opcode::FToI:
      return define(in, FToIOp::create(B, Loc, arg(0)));
    case P::Opcode::UToF:
      return define(in, UToFOp::create(B, Loc, arg(0)));
    case P::Opcode::FToU:
      return define(in, FToUOp::create(B, Loc, arg(0)));
    case P::Opcode::FRound32:
      return define(in, FRound32Op::create(B, Loc, arg(0)));
    case P::Opcode::Cast:
      return define(in, CastOp::create(B, Loc, type(in.CastTo), arg(0)));
    case P::Opcode::Wrap:
      return define(in, WrapOp::create(B, Loc, arg(0), in.Slot));
    case P::Opcode::Mask:
      return define(in, MaskOp::create(B, Loc, arg(0), in.Slot));
    case P::Opcode::Call:
      return define(in, resultOf(CallOp::create(B, Loc, results(in), in.Callee,
                                                operands(in))));
    case P::Opcode::VCall:
      return define(in,
                    resultOf(VCallOp::create(B, Loc, results(in), in.ClassName,
                                             in.Slot, operands(in))));
    case P::Opcode::ICall:
      return define(in, resultOf(ICallOp::create(B, Loc, results(in), arg(0),
                                                 operands(in, 1))));
    case P::Opcode::FnAddr:
      return define(in, FnAddrOp::create(B, Loc, in.Callee));
    case P::Opcode::Retain:
      RetainOp::create(B, Loc, arg(0));
      return;
    case P::Opcode::Release:
      ReleaseOp::create(B, Loc, arg(0));
      return;
    case P::Opcode::Box:
      return define(in, BoxOp::create(B, Loc, arg(0)));
    case P::Opcode::Unbox:
      return define(in, UnboxOp::create(B, Loc, arg(0)));
    case P::Opcode::New:
      return define(in, NewOp::create(B, Loc, in.ClassName));
    case P::Opcode::Free:
      FreeOp::create(B, Loc, arg(0));
      return;
    case P::Opcode::FieldLoad:
      return define(
          in, FieldLoadOp::create(B, Loc, rt, arg(0), in.ClassName, in.Field));
    case P::Opcode::FieldStore:
      FieldStoreOp::create(B, Loc, arg(0), in.ClassName, in.Field, arg(1));
      return;
    case P::Opcode::FieldAddr:
      return define(
          in, FieldAddrOp::create(B, Loc, arg(0), in.ClassName, in.Field));
    case P::Opcode::VTableLoad:
      return define(in, VTableLoadOp::create(B, Loc, arg(0)));
    case P::Opcode::VTableAddr:
      // `vtable.addr Class`, or of an extern vtable (`vtable.addr @$rt.X`).
      if (in.ClassName.empty())
        return define(in, arg(0));
      return define(in, VTableAddrOp::create(B, Loc, in.ClassName));
    case P::Opcode::Load:
      if (Value l = local(in))
        define(in, LoadOp::create(B, Loc, l));
      return;
    case P::Opcode::Store:
      if (Value l = local(in))
        StoreOp::create(B, Loc, arg(0), l);
      return;
    case P::Opcode::LocalAddr:
      if (Value l = local(in))
        define(in, LocalAddrOp::create(B, Loc, l));
      return;
    case P::Opcode::PtrLoad:
      return define(in, PtrLoadOp::create(B, Loc, rt, arg(0)));
    case P::Opcode::PtrStore:
      PtrStoreOp::create(B, Loc, arg(0), arg(1));
      return;
    case P::Opcode::PtrAdd:
      return define(in, PtrAddOp::create(B, Loc, arg(0), arg(1)));
    case P::Opcode::MemCopy:
      MemCopyOp::create(B, Loc, arg(0), arg(1), arg(2));
      return;
    case P::Opcode::MemZero:
      MemZeroOp::create(B, Loc, arg(0), arg(1));
      return;
    case P::Opcode::Some:
      return define(in, SomeOp::create(B, Loc, rt, arg(0)));
    case P::Opcode::None:
      return define(in, NoneOp::create(B, Loc, rt));
    case P::Opcode::IsSome:
      return define(in, IsSomeOp::create(B, Loc, arg(0)));
    case P::Opcode::Unwrap:
      return define(in, UnwrapOp::create(B, Loc, arg(0)));
    case P::Opcode::WeakNew:
      return define(in, WeakNewOp::create(B, Loc, arg(0)));
    case P::Opcode::WeakLoad:
      return define(in, WeakLoadOp::create(B, Loc, arg(0)));
    case P::Opcode::WeakRelease:
      WeakReleaseOp::create(B, Loc, arg(0));
      return;
    }
    fail("unknown opcode");
  }
};

/// Verify @p op, returning the diagnostics as an error.
Status verifyImport(Operation *op) {
  std::string diagnostics;
  ScopedDiagnosticHandler handler(op->getContext(), [&](Diagnostic &d) {
    llvm::raw_string_ostream os(diagnostics);
    os << d << "\n";
    return success();
  });
  if (failed(verify(op)))
    return Status::error("the imported MLIR does not verify:\n" + diagnostics);
  return Status::ok();
}

} // namespace

void loadImportDialects(MLIRContext &ctx) {
  ctx.getOrLoadDialect<PIRDialect>();
  ctx.getOrLoadDialect<arith::ArithDialect>();
}

StatusOr<OwningOpRef<ModuleOp>> importModule(const P::Program &program,
                                             size_t index, MLIRContext &ctx) {
  if (index >= program.Modules.size())
    return Status::error("PIR module index out of range");
  loadImportDialects(ctx);
  auto mod = Importer(program.Modules[index], index == 0, ctx).run();
  if (!mod)
    return mod.status();
  if (Status s = verifyImport((*mod)->getOperation()); !s)
    return s;
  return mod;
}

StatusOr<OwningOpRef<ModuleOp>> importProgram(const P::Program &program,
                                              MLIRContext &ctx) {
  loadImportDialects(ctx);
  OwningOpRef<ModuleOp> top = ModuleOp::create(UnknownLoc::get(&ctx));
  for (size_t i = 0; i < program.Modules.size(); ++i) {
    auto mod = Importer(program.Modules[i], i == 0, ctx).run();
    if (!mod)
      return mod.status();
    top->getBody()->push_back((*mod).release());
  }
  if (Status s = verifyImport(top->getOperation()); !s)
    return s;
  return top;
}

} // namespace paykan::mlir_backend
