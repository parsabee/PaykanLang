// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// PIR -> LLVM IR.  One pass over a verified program (docs/pir.md); every PIR
// instruction maps to one or a few LLVM instructions, structured control
// flow becomes basic blocks, and `local` slots become entry-block allocas.

#include "PIRToLLVM.h"

#include "Names.h"
#include "Version.h"
#include "paykan/pir/Printer.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/raw_ostream.h>

#include <unordered_map>
#include <vector>

namespace paykan::backend::llvm_backend {

namespace {

using namespace pir;

/// The translation of one PIR module of a program.
class Translator {
public:
  Translator(const Program &program, size_t index, llvm::LLVMContext &ctx)
      : P(program), Ctx(ctx), B(ctx),
        M(std::make_unique<llvm::Module>(program.Modules[index].Name, ctx)),
        Index(index) {}

  StatusOr<std::unique_ptr<llvm::Module>> run() {
    const Module &module = P.Modules[Index];
    const bool isMain = Index == 0;
    // Declarations first, so bodies can reference anything in any order.
    declareModule(module, isMain);
    if (Status s = defineModule(module, isMain); !s)
      return s;
    std::string err;
    llvm::raw_string_ostream os(err);
    if (llvm::verifyModule(*M, &os))
      return Status::error("PIR -> LLVM translation produced invalid IR:\n" +
                           err);
    return std::move(M);
  }

private:
  const Program &P;
  llvm::LLVMContext &Ctx;
  llvm::IRBuilder<> B;
  std::unique_ptr<llvm::Module> M;
  size_t Index;

  // -- Names -----------------------------------------------------------------

  /// The LLVM name of a symbol defined by @p module.
  static std::string mangle(const Module &module, bool isMain,
                            const std::string &name) {
    if (isMain)
      return name;
    return module.Name + "::" + name;
  }

  static std::string vtableName(const std::string &className) {
    return className + names::kVTableSuffix;
  }

  const Module *findModule(const std::string &name, bool &isMain) const {
    for (size_t i = 0; i < P.Modules.size(); ++i)
      if (P.Modules[i].Name == name) {
        isMain = i == 0;
        return &P.Modules[i];
      }
    return nullptr;
  }

  /// LLVM name of a function @p name as seen from @p module: the module's own
  /// definition, a runtime symbol, or another module's definition.
  std::string functionName(const Module &module, bool isMain,
                           const std::string &name) const {
    const Function *f = module.findFunction(name);
    if (!f)
      return name;
    if (!f->IsExtern)
      return mangle(module, isMain, name);
    if (f->Module.empty())
      return name; // runtime
    bool otherIsMain = false;
    if (const Module *other = findModule(f->Module, otherIsMain))
      return mangle(*other, otherIsMain, name);
    return name;
  }

  /// LLVM name of class @p name's vtable / struct as seen from @p module.
  std::string classSymbol(const Module &module, bool isMain,
                          const std::string &name) const {
    const Class *c = module.findClass(name);
    if (c && c->IsExtern) {
      bool otherIsMain = false;
      if (const Module *other = findModule(c->Module, otherIsMain))
        return mangle(*other, otherIsMain, name);
    }
    return mangle(module, isMain, name);
  }

  // -- Types -----------------------------------------------------------------

  llvm::Type *llvmType(Type t) {
    switch (t) {
    case Type::Void:
      return llvm::Type::getVoidTy(Ctx);
    case Type::I64:
      return llvm::Type::getInt64Ty(Ctx);
    case Type::F64:
      return llvm::Type::getDoubleTy(Ctx);
    case Type::Bool:
      return llvm::Type::getInt1Ty(Ctx);
    case Type::Char:
      return llvm::Type::getInt8Ty(Ctx);
    case Type::Box:
    case Type::Obj:
    case Type::Ptr:
      return llvm::PointerType::getUnqual(Ctx);
    }
    return llvm::Type::getVoidTy(Ctx);
  }

  llvm::FunctionType *functionType(const Signature &sig) {
    std::vector<llvm::Type *> params;
    for (Type t : sig.Params)
      params.push_back(llvmType(t));
    return llvm::FunctionType::get(llvmType(sig.Ret), params, false);
  }

  /// The struct type of a class: {vtable ptr, backpointer, fields...}.
  /// Struct types are per translation, never looked up by name in the
  /// context: a cached module loaded earlier may carry an older layout of a
  /// class under the same name.
  llvm::StructType *classType(const std::string &symbol, const Class &c) {
    auto it = StructTypes.find(symbol);
    if (it != StructTypes.end())
      return it->second;
    std::vector<llvm::Type *> elems{llvm::PointerType::getUnqual(Ctx),
                                    llvm::PointerType::getUnqual(Ctx)};
    for (const Field &f : c.Fields)
      elems.push_back(llvmType(f.Ty));
    auto *ty =
        llvm::StructType::create(Ctx, elems, symbol + names::kStructSuffix);
    StructTypes[symbol] = ty;
    return ty;
  }
  std::unordered_map<std::string, llvm::StructType *> StructTypes;

  // -- Runtime helpers the ops need even when the module does not declare
  // -- them (first-class ops: new/free/retain/release/box/unbox).
  llvm::FunctionCallee runtimeFn(const char *name, llvm::Type *ret,
                                 std::vector<llvm::Type *> params) {
    return M->getOrInsertFunction(name,
                                  llvm::FunctionType::get(ret, params, false));
  }
  llvm::FunctionCallee fnMalloc() {
    return runtimeFn(names::kPaykanMalloc, llvm::PointerType::getUnqual(Ctx),
                     {llvm::Type::getInt64Ty(Ctx)});
  }
  llvm::FunctionCallee fnFree() {
    return runtimeFn(names::kPaykanFree, llvm::Type::getVoidTy(Ctx),
                     {llvm::PointerType::getUnqual(Ctx)});
  }
  llvm::FunctionCallee fnRetain() {
    return runtimeFn(names::kPaykanRetain, llvm::Type::getVoidTy(Ctx),
                     {llvm::PointerType::getUnqual(Ctx)});
  }
  llvm::FunctionCallee fnRelease() {
    return runtimeFn(names::kPaykanRelease, llvm::Type::getVoidTy(Ctx),
                     {llvm::PointerType::getUnqual(Ctx)});
  }
  llvm::FunctionCallee fnSharedNew() {
    return runtimeFn(names::kPaykanSharedNew, llvm::PointerType::getUnqual(Ctx),
                     {llvm::PointerType::getUnqual(Ctx)});
  }
  llvm::FunctionCallee fnSharedGet() {
    return runtimeFn(names::kPaykanSharedGet, llvm::PointerType::getUnqual(Ctx),
                     {llvm::PointerType::getUnqual(Ctx)});
  }

  // -- Module-level declarations ---------------------------------------------

  void declareModule(const Module &module, bool isMain) {
    for (const CStrGlobal &g : module.CStrs) {
      auto *init = llvm::ConstantDataArray::getString(Ctx, g.Data, true);
      new llvm::GlobalVariable(*M, init->getType(), true,
                               llvm::GlobalValue::PrivateLinkage, init,
                               mangle(module, isMain, g.Name));
    }
    for (const DataGlobal &g : module.Datas) {
      auto *init = llvm::ConstantDataArray::get(Ctx, g.Words);
      new llvm::GlobalVariable(*M, init->getType(), true,
                               llvm::GlobalValue::PrivateLinkage, init,
                               mangle(module, isMain, g.Name));
    }
    for (const BytesGlobal &g : module.Bytes) {
      auto *init = llvm::ConstantDataArray::get(Ctx, g.Bytes);
      new llvm::GlobalVariable(*M, init->getType(), true,
                               llvm::GlobalValue::PrivateLinkage, init,
                               mangle(module, isMain, g.Name));
    }
    for (const ExternGlobal &g : module.Externs) {
      if (!M->getNamedGlobal(g.Name))
        new llvm::GlobalVariable(*M, llvm::Type::getInt8Ty(Ctx), false,
                                 llvm::GlobalValue::ExternalLinkage, nullptr,
                                 g.Name);
    }
    for (const Class &c : module.Classes) {
      std::string sym = classSymbol(module, isMain, c.Name);
      classType(sym, c);
      // The vtable: one function pointer per slot, in slot order.  An extern
      // class's vtable is defined by its own module; only its address is
      // taken here.
      auto *vtTy = llvm::ArrayType::get(llvm::PointerType::getUnqual(Ctx),
                                        c.IsExtern ? 0 : c.VTable.size());
      if (!M->getNamedGlobal(vtableName(sym)))
        new llvm::GlobalVariable(*M, vtTy, true,
                                 llvm::GlobalValue::ExternalLinkage, nullptr,
                                 vtableName(sym));
    }
    for (const Function &f : module.Functions) {
      std::string name = functionName(module, isMain, f.Name);
      if (M->getFunction(name))
        continue;
      llvm::Function::Create(functionType(f.Sig),
                             llvm::GlobalValue::ExternalLinkage, name, *M);
    }
  }

  /// Address of a module-level symbol referenced as an operand (`@sym`).
  llvm::Value *symbolAddress(const Module &module, bool isMain,
                             const std::string &name) {
    if (auto *g = M->getNamedGlobal(mangle(module, isMain, name)))
      return g;
    if (auto *g = M->getNamedGlobal(name))
      return g; // extern obj / vtable
    if (auto *f = M->getFunction(functionName(module, isMain, name)))
      return f;
    return nullptr;
  }

  // -- Definitions -----------------------------------------------------------

  Status defineModule(const Module &module, bool isMain) {
    for (const Class &c : module.Classes) {
      if (c.IsExtern)
        continue;
      std::string sym = classSymbol(module, isMain, c.Name);
      auto *vt = M->getNamedGlobal(vtableName(sym));
      std::vector<llvm::Constant *> slots;
      for (const VTableEntry &e : c.VTable) {
        if (e.Target.empty()) {
          slots.push_back(llvm::ConstantPointerNull::get(
              llvm::PointerType::getUnqual(Ctx)));
          continue;
        }
        llvm::Function *target =
            M->getFunction(functionName(module, isMain, e.Target));
        if (!target)
          return Status::error("vtable of '" + c.Name +
                               "' names unknown function '" + e.Target + "'");
        slots.push_back(target);
      }
      vt->setInitializer(llvm::ConstantArray::get(
          llvm::cast<llvm::ArrayType>(vt->getValueType()), slots));
    }
    for (const Function &f : module.Functions) {
      if (f.IsExtern)
        continue;
      if (Status s = defineFunction(module, isMain, f); !s)
        return s;
    }
    return Status::ok();
  }

  // -- Function bodies -------------------------------------------------------

  struct FunctionState {
    const Module *Mod = nullptr;
    bool IsMain = false;
    const Function *Fn = nullptr;
    llvm::Function *LLFn = nullptr;
    std::unordered_map<ValueId, llvm::Value *> Values;
    std::vector<llvm::AllocaInst *> Locals;
    /// Innermost-first loop targets for break / continue.
    std::vector<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>> Loops;
    std::string Error;
  };
  FunctionState *S = nullptr;

  Status defineFunction(const Module &module, bool isMain, const Function &f) {
    FunctionState st;
    st.Mod = &module;
    st.IsMain = isMain;
    st.Fn = &f;
    st.LLFn = M->getFunction(functionName(module, isMain, f.Name));
    S = &st;

    auto *entry = llvm::BasicBlock::Create(Ctx, "entry", st.LLFn);
    B.SetInsertPoint(entry);
    size_t i = 0;
    for (llvm::Argument &arg : st.LLFn->args()) {
      const Value &p = f.Params[i++];
      arg.setName(p.Name);
      st.Values[p.Id] = &arg;
    }
    for (const Local &l : f.Locals)
      st.Locals.push_back(B.CreateAlloca(llvmType(l.Ty), nullptr, l.Name));

    bool terminated = emitBlock(f.Body);
    if (!st.Error.empty()) {
      S = nullptr;
      return Status::error("in function '@" + f.Name + "': " + st.Error);
    }
    if (!terminated) {
      // Only a void function may fall off its end (the verifier checks).
      if (f.Sig.Ret == Type::Void)
        B.CreateRetVoid();
      else
        B.CreateUnreachable();
    }
    S = nullptr;
    return Status::ok();
  }

  llvm::Value *operand(const Operand &op) {
    struct V {
      Translator &T;
      llvm::Value *operator()(ValueId id) const {
        auto it = T.S->Values.find(id);
        if (it == T.S->Values.end()) {
          T.S->Error = "use of undefined value %" + std::to_string(id);
          return llvm::UndefValue::get(llvm::Type::getInt64Ty(T.Ctx));
        }
        return it->second;
      }
      llvm::Value *operator()(int64_t v) const { return T.B.getInt64(v); }
      llvm::Value *operator()(double v) const {
        return llvm::ConstantFP::get(T.Ctx, llvm::APFloat(v));
      }
      llvm::Value *operator()(bool v) const { return T.B.getInt1(v); }
      llvm::Value *operator()(char v) const {
        return T.B.getInt8(static_cast<uint8_t>(v));
      }
      llvm::Value *operator()(const Operand::Null &) const {
        return llvm::ConstantPointerNull::get(
            llvm::PointerType::getUnqual(T.Ctx));
      }
      llvm::Value *operator()(const SymbolRef &s) const {
        llvm::Value *v = T.symbolAddress(*T.S->Mod, T.S->IsMain, s.Name);
        if (!v) {
          T.S->Error = "reference to unknown symbol @" + s.Name;
          return llvm::ConstantPointerNull::get(
              llvm::PointerType::getUnqual(T.Ctx));
        }
        return v;
      }
    };
    return std::visit(V{*this}, op.V);
  }

  void setResult(const Instr &in, llvm::Value *v) {
    if (in.Result.Id == kNoValue)
      return;
    // Only instructions take the PIR name: naming a global or an argument
    // would rename that global.
    if (v && llvm::isa<llvm::Instruction>(v) && !v->getType()->isVoidTy())
      v->setName(in.Result.Name);
    S->Values[in.Result.Id] = v;
  }

  /// Emits the statements of a block into the current insertion point.
  /// Returns true when the block ended with a terminator.
  bool emitBlock(const Block &b) {
    for (const Stmt &s : b.Stmts) {
      if (!S->Error.empty())
        return true;
      if (const auto *in = std::get_if<Instr>(&s)) {
        emitInstr(*in);
      } else if (const auto *i = std::get_if<If>(&s)) {
        if (emitIf(*i))
          return true;
      } else if (const auto *w = std::get_if<While>(&s)) {
        emitWhile(*w);
      } else if (std::holds_alternative<Break>(s)) {
        B.CreateBr(S->Loops.back().second);
        return true;
      } else if (std::holds_alternative<Continue>(s)) {
        B.CreateBr(S->Loops.back().first);
        return true;
      } else if (const auto *r = std::get_if<Return>(&s)) {
        if (r->Value)
          B.CreateRet(operand(*r->Value));
        else
          B.CreateRetVoid();
        return true;
      } else {
        B.CreateUnreachable();
        return true;
      }
    }
    return false;
  }

  /// Returns true when both branches terminate (no merge block is reached).
  bool emitIf(const If &i) {
    llvm::Value *cond = operand(i.Cond);
    auto *thenBB = llvm::BasicBlock::Create(Ctx, "then", S->LLFn);
    auto *elseBB =
        i.Else ? llvm::BasicBlock::Create(Ctx, "else", S->LLFn) : nullptr;
    auto *mergeBB = llvm::BasicBlock::Create(Ctx, "endif", S->LLFn);
    B.CreateCondBr(cond, thenBB, elseBB ? elseBB : mergeBB);

    B.SetInsertPoint(thenBB);
    bool thenTerm = i.Then ? emitBlock(*i.Then) : false;
    if (!thenTerm)
      B.CreateBr(mergeBB);

    bool elseTerm = false;
    if (elseBB) {
      B.SetInsertPoint(elseBB);
      elseTerm = emitBlock(*i.Else);
      if (!elseTerm)
        B.CreateBr(mergeBB);
    }

    if (elseBB && thenTerm && elseTerm) {
      // Nothing reaches the merge block; keep the function well formed.
      B.SetInsertPoint(mergeBB);
      B.CreateUnreachable();
      return true;
    }
    B.SetInsertPoint(mergeBB);
    return false;
  }

  void emitWhile(const While &w) {
    auto *condBB = llvm::BasicBlock::Create(Ctx, "loop.cond", S->LLFn);
    auto *bodyBB = llvm::BasicBlock::Create(Ctx, "loop.body", S->LLFn);
    auto *exitBB = llvm::BasicBlock::Create(Ctx, "loop.exit", S->LLFn);
    B.CreateBr(condBB);

    B.SetInsertPoint(condBB);
    if (w.CondBlock)
      emitBlock(*w.CondBlock); // never terminates (verifier)
    B.CreateCondBr(operand(w.Cond), bodyBB, exitBB);

    B.SetInsertPoint(bodyBB);
    S->Loops.emplace_back(condBB, exitBB);
    bool term = w.Body ? emitBlock(*w.Body) : false;
    S->Loops.pop_back();
    if (!term)
      B.CreateBr(condBB);

    B.SetInsertPoint(exitBB);
  }

  // -- Instructions ----------------------------------------------------------

  const Class *lookupClass(const std::string &name, std::string &symbol) {
    const Class *c = S->Mod->findClass(name);
    if (!c) {
      S->Error = "unknown class '" + name + "'";
      return nullptr;
    }
    symbol = classSymbol(*S->Mod, S->IsMain, name);
    return c;
  }

  llvm::Value *fieldPtr(const Instr &in, const Class *&c, const Field *&fld) {
    std::string sym;
    c = lookupClass(in.ClassName, sym);
    if (!c)
      return nullptr;
    size_t idx = 0;
    fld = nullptr;
    for (size_t i = 0; i < c->Fields.size(); ++i)
      if (c->Fields[i].Name == in.Field) {
        idx = i;
        fld = &c->Fields[i];
      }
    if (!fld) {
      S->Error = "class '" + in.ClassName + "' has no field '" + in.Field + "'";
      return nullptr;
    }
    return B.CreateStructGEP(classType(sym, *c), operand(in.Args[0]),
                             static_cast<unsigned>(2 + idx), in.Field);
  }

  void emitInstr(const Instr &in) {
    switch (in.Op) {
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Rem: {
      llvm::Value *a = operand(in.Args[0]), *b = operand(in.Args[1]);
      bool fp = a->getType()->isDoubleTy();
      llvm::Value *r = nullptr;
      switch (in.Op) {
      case Opcode::Add:
        r = fp ? B.CreateFAdd(a, b) : B.CreateAdd(a, b);
        break;
      case Opcode::Sub:
        r = fp ? B.CreateFSub(a, b) : B.CreateSub(a, b);
        break;
      case Opcode::Mul:
        r = fp ? B.CreateFMul(a, b) : B.CreateMul(a, b);
        break;
      case Opcode::Div:
        r = fp ? B.CreateFDiv(a, b) : B.CreateSDiv(a, b);
        break;
      default:
        r = fp ? B.CreateFRem(a, b) : B.CreateSRem(a, b);
        break;
      }
      setResult(in, r);
      break;
    }
    case Opcode::Neg: {
      llvm::Value *a = operand(in.Args[0]);
      setResult(in,
                a->getType()->isDoubleTy() ? B.CreateFNeg(a) : B.CreateNeg(a));
      break;
    }
    case Opcode::Not:
      setResult(in, B.CreateNot(operand(in.Args[0])));
      break;
    case Opcode::Cmp: {
      llvm::Value *a = operand(in.Args[0]), *b = operand(in.Args[1]);
      llvm::Value *r = nullptr;
      if (a->getType()->isDoubleTy()) {
        switch (in.Pred) {
        case CmpPred::Eq:
          r = B.CreateFCmpOEQ(a, b);
          break;
        case CmpPred::Ne:
          r = B.CreateFCmpONE(a, b);
          break;
        case CmpPred::Lt:
          r = B.CreateFCmpOLT(a, b);
          break;
        case CmpPred::Le:
          r = B.CreateFCmpOLE(a, b);
          break;
        case CmpPred::Gt:
          r = B.CreateFCmpOGT(a, b);
          break;
        case CmpPred::Ge:
          r = B.CreateFCmpOGE(a, b);
          break;
        }
      } else {
        switch (in.Pred) {
        case CmpPred::Eq:
          r = B.CreateICmpEQ(a, b);
          break;
        case CmpPred::Ne:
          r = B.CreateICmpNE(a, b);
          break;
        case CmpPred::Lt:
          r = B.CreateICmpSLT(a, b);
          break;
        case CmpPred::Le:
          r = B.CreateICmpSLE(a, b);
          break;
        case CmpPred::Gt:
          r = B.CreateICmpSGT(a, b);
          break;
        case CmpPred::Ge:
          r = B.CreateICmpSGE(a, b);
          break;
        }
      }
      setResult(in, r);
      break;
    }
    case Opcode::Select:
      setResult(in, B.CreateSelect(operand(in.Args[0]), operand(in.Args[1]),
                                   operand(in.Args[2])));
      break;
    case Opcode::IToF:
      setResult(in, B.CreateSIToFP(operand(in.Args[0]),
                                   llvm::Type::getDoubleTy(Ctx)));
      break;
    case Opcode::Cast:
      setResult(in, emitCast(operand(in.Args[0]), in.CastTo));
      break;
    case Opcode::Call: {
      llvm::Function *callee =
          M->getFunction(functionName(*S->Mod, S->IsMain, in.Callee));
      if (!callee) {
        S->Error = "call to unknown function @" + in.Callee;
        return;
      }
      std::vector<llvm::Value *> args;
      for (const Operand &a : in.Args)
        args.push_back(operand(a));
      setResult(in, B.CreateCall(callee, args));
      break;
    }
    case Opcode::VCall: {
      llvm::Value *recv = operand(in.Args[0]);
      // The vtable pointer is the object's first word; the slot holds the
      // function pointer.
      auto *ptrTy = llvm::PointerType::getUnqual(Ctx);
      llvm::Value *vt = B.CreateLoad(ptrTy, recv, "vtable");
      llvm::Value *slotPtr = B.CreateConstGEP1_64(ptrTy, vt, in.Slot, "slot");
      llvm::Value *fn = B.CreateLoad(ptrTy, slotPtr, "method");
      std::vector<llvm::Value *> args;
      for (const Operand &a : in.Args)
        args.push_back(operand(a));
      setResult(in, B.CreateCall(functionType(in.Sig), fn, args));
      break;
    }
    case Opcode::Retain:
      B.CreateCall(fnRetain(), {operand(in.Args[0])});
      break;
    case Opcode::Release:
      B.CreateCall(fnRelease(), {operand(in.Args[0])});
      break;
    case Opcode::Box:
      setResult(in, B.CreateCall(fnSharedNew(), {operand(in.Args[0])}));
      break;
    case Opcode::Unbox:
      setResult(in, B.CreateCall(fnSharedGet(), {operand(in.Args[0])}));
      break;
    case Opcode::New: {
      std::string sym;
      const Class *c = lookupClass(in.ClassName, sym);
      if (!c)
        return;
      llvm::StructType *ty = classType(sym, *c);
      const llvm::DataLayout &dl = M->getDataLayout();
      llvm::Value *size = B.getInt64(dl.getTypeAllocSize(ty));
      llvm::Value *obj = B.CreateCall(fnMalloc(), {size});
      auto *ptrTy = llvm::PointerType::getUnqual(Ctx);
      // Header: vtable, null backpointer; then every field zeroed.
      B.CreateStore(M->getNamedGlobal(vtableName(sym)),
                    B.CreateStructGEP(ty, obj, 0));
      B.CreateStore(llvm::ConstantPointerNull::get(ptrTy),
                    B.CreateStructGEP(ty, obj, 1));
      for (size_t i = 0; i < c->Fields.size(); ++i) {
        llvm::Type *ft = llvmType(c->Fields[i].Ty);
        B.CreateStore(llvm::Constant::getNullValue(ft),
                      B.CreateStructGEP(ty, obj, static_cast<unsigned>(2 + i)));
      }
      setResult(in, obj);
      break;
    }
    case Opcode::Free:
      B.CreateCall(fnFree(), {operand(in.Args[0])});
      break;
    case Opcode::FieldLoad: {
      const Class *c;
      const Field *fld;
      llvm::Value *p = fieldPtr(in, c, fld);
      if (!p)
        return;
      setResult(in, B.CreateLoad(llvmType(fld->Ty), p));
      break;
    }
    case Opcode::FieldStore: {
      const Class *c;
      const Field *fld;
      llvm::Value *p = fieldPtr(in, c, fld);
      if (!p)
        return;
      B.CreateStore(operand(in.Args[1]), p);
      break;
    }
    case Opcode::VTableLoad:
      setResult(in, B.CreateLoad(llvm::PointerType::getUnqual(Ctx),
                                 operand(in.Args[0])));
      break;
    case Opcode::VTableAddr: {
      if (!in.ClassName.empty()) {
        std::string sym;
        if (!lookupClass(in.ClassName, sym))
          return;
        setResult(in, M->getNamedGlobal(vtableName(sym)));
      } else {
        setResult(in, operand(in.Args[0]));
      }
      break;
    }
    case Opcode::Load: {
      llvm::AllocaInst *slot = S->Locals[in.Local];
      setResult(in, B.CreateLoad(slot->getAllocatedType(), slot));
      break;
    }
    case Opcode::Store:
      B.CreateStore(operand(in.Args[0]), S->Locals[in.Local]);
      break;
    }
  }

  /// The `cast` table of docs/pir.md §6.
  llvm::Value *emitCast(llvm::Value *v, Type to) {
    llvm::Type *from = v->getType();
    llvm::Type *dst = llvmType(to);
    if (from == dst)
      return v;
    if (from->isDoubleTy() && to == Type::I64)
      return B.CreateBitCast(v, dst);
    if (from->isIntegerTy(64) && to == Type::F64)
      return B.CreateBitCast(v, dst);
    if (from->isIntegerTy() && dst->isIntegerTy()) {
      if (from->getIntegerBitWidth() < dst->getIntegerBitWidth())
        return B.CreateZExt(v, dst);
      return B.CreateTrunc(v, dst);
    }
    if (from->isPointerTy() && dst->isIntegerTy())
      return B.CreatePtrToInt(v, dst);
    if (from->isIntegerTy() && dst->isPointerTy())
      return B.CreateIntToPtr(v, dst);
    return v; // ptr <-> ptr
  }
};

} // namespace

StatusOr<std::unique_ptr<llvm::Module>>
translateModule(const Program &program, size_t index, llvm::LLVMContext &ctx) {
  if (index >= program.Modules.size())
    return Status::error("PIR module index out of range");
  return Translator(program, index, ctx).run();
}

namespace {

/// Link @p parts (main module first) into one module named @p name.
StatusOr<std::unique_ptr<llvm::Module>>
linkParts(const Program &program,
          std::vector<std::unique_ptr<llvm::Module>> parts,
          const std::string &name) {
  std::unique_ptr<llvm::Module> main = std::move(parts.front());
  main->setModuleIdentifier(name);
  for (size_t i = 1; i < parts.size(); ++i)
    if (llvm::Linker::linkModules(*main, std::move(parts[i])))
      return Status::error("failed to link module '" + program.Modules[i].Name +
                           "'");
  return std::move(main);
}

} // namespace

StatusOr<std::unique_ptr<llvm::Module>>
translateProgram(const Program &program, llvm::LLVMContext &ctx,
                 const std::string &moduleName) {
  if (program.Modules.empty())
    return Status::error("empty PIR program");
  std::vector<std::unique_ptr<llvm::Module>> parts;
  for (size_t i = 0; i < program.Modules.size(); ++i) {
    auto part = translateModule(program, i, ctx);
    if (!part)
      return part.status();
    parts.push_back(std::move(*part));
  }
  return linkParts(program, std::move(parts), moduleName);
}

// -- Bitcode cache -----------------------------------------------------------
//
// An imported module's LLVM IR depends only on its PIR text (which spells out
// every layout and signature it uses from other modules) and on the compiler
// that translates it, so that text is the cache key.  Entries live under
// <projectRoot>/.paykan_cache/<module path relative to the root>.bc, stamped
// with the key; anything that does not parse or carries another key is
// recompiled and rewritten.

namespace {

/// Bumped whenever the generated code's ABI (object layout, calling
/// conventions) changes, so entries from an older compiler are never linked.
///   v3: PIR-based translation.
constexpr uint64_t kPaykanABIVersion = 3;
constexpr const char *kABIVersionFlag = "paykan.abi.version";
constexpr const char *kCacheKeyMD = "paykan.cache.key";

/// Identity of the running compiler binary (size and modification time of
/// the executable), so a development build with different codegen but the
/// same version string never serves entries written by the previous build.
llvm::StringRef compilerBuildId() {
  static const std::string id = [] {
    std::string exe = llvm::sys::fs::getMainExecutable(
        nullptr, reinterpret_cast<void *>(&compilerBuildId));
    llvm::sys::fs::file_status st;
    if (exe.empty() || llvm::sys::fs::status(exe, st))
      return std::string();
    return std::to_string(st.getSize()) + ":" +
           std::to_string(
               st.getLastModificationTime().time_since_epoch().count());
  }();
  return id;
}

std::string cacheKey(const Module &module) {
  llvm::SHA256 hasher;
  hasher.update(toString(module));
  hasher.update("|paykan ");
  hasher.update(kVersion);
  hasher.update("|llvm ");
  hasher.update(LLVM_VERSION_STRING);
  hasher.update("|build ");
  hasher.update(compilerBuildId());
  hasher.update("|abi ");
  hasher.update(std::to_string(kPaykanABIVersion));
  return llvm::toHex(hasher.final());
}

/// <projectRoot>/.paykan_cache/<path relative to the root>.bc; a module
/// outside the project mirrors its full path (minus the root directory).
/// An empty project root means the current directory.
std::string cachePath(const std::string &resolvedPath,
                      const std::string &projectRoot) {
  llvm::SmallString<256> path(projectRoot);
  llvm::sys::path::append(path, names::kCacheDir);

  llvm::SmallString<256> canonRoot;
  if (llvm::sys::fs::real_path(projectRoot.empty() ? "." : projectRoot,
                               canonRoot))
    canonRoot = projectRoot;

  llvm::StringRef rel = resolvedPath;
  if (!canonRoot.empty() && rel.starts_with(canonRoot) &&
      (rel.size() == canonRoot.size() ||
       llvm::sys::path::is_separator(rel[canonRoot.size()]) ||
       llvm::sys::path::is_separator(canonRoot.back()))) {
    rel = rel.drop_front(canonRoot.size());
    while (!rel.empty() && llvm::sys::path::is_separator(rel.front()))
      rel = rel.drop_front(1);
  } else {
    rel = llvm::sys::path::relative_path(rel);
  }
  for (auto comp = llvm::sys::path::begin(rel), end = llvm::sys::path::end(rel);
       comp != end; ++comp)
    llvm::sys::path::append(path, *comp);
  llvm::sys::path::replace_extension(path, ".bc");
  return std::string(path);
}

bool cachedModuleIsValid(const llvm::Module &mod, llvm::StringRef key) {
  auto *flag = llvm::mdconst::extract_or_null<llvm::ConstantInt>(
      mod.getModuleFlag(kABIVersionFlag));
  if (!flag || flag->getZExtValue() != kPaykanABIVersion)
    return false;
  auto *nmd = mod.getNamedMetadata(kCacheKeyMD);
  if (!nmd || nmd->getNumOperands() != 1 ||
      nmd->getOperand(0)->getNumOperands() != 1)
    return false;
  auto *md = llvm::dyn_cast<llvm::MDString>(nmd->getOperand(0)->getOperand(0));
  return md && md->getString() == key;
}

void stampCachedModule(llvm::Module &mod, llvm::StringRef key) {
  if (!mod.getModuleFlag(kABIVersionFlag))
    mod.addModuleFlag(llvm::Module::Error, kABIVersionFlag, kPaykanABIVersion);
  auto *nmd = mod.getOrInsertNamedMetadata(kCacheKeyMD);
  nmd->clearOperands();
  auto &ctx = mod.getContext();
  nmd->addOperand(llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, key)}));
}

/// Write @p mod atomically (temporary file + rename) to @p path; best effort,
/// a failure just leaves the entry absent.
void writeCacheFile(const llvm::Module &mod, llvm::StringRef path) {
  auto parentDir = llvm::sys::path::parent_path(path);
  if (!parentDir.empty() && llvm::sys::fs::create_directories(parentDir))
    return;
  int fd = -1;
  llvm::SmallString<256> tmpPath;
  if (llvm::sys::fs::createUniqueFile(path + ".%%%%%%%%.tmp", fd, tmpPath))
    return;
  bool written;
  {
    llvm::raw_fd_ostream out(fd, /*shouldClose=*/true);
    llvm::WriteBitcodeToFile(mod, out);
    out.close();
    written = !out.has_error();
    out.clear_error();
  }
  if (!written || llvm::sys::fs::rename(tmpPath, path))
    (void)llvm::sys::fs::remove(tmpPath);
}

/// A valid cached translation under @p key, or null.
std::unique_ptr<llvm::Module> loadCached(const std::string &path,
                                         llvm::StringRef key,
                                         llvm::LLVMContext &ctx) {
  auto buf = llvm::MemoryBuffer::getFile(path);
  if (!buf)
    return nullptr;
  auto mod = llvm::parseBitcodeFile((*buf)->getMemBufferRef(), ctx);
  if (!mod) {
    llvm::consumeError(mod.takeError());
    return nullptr;
  }
  if (!cachedModuleIsValid(**mod, key))
    return nullptr;
  return std::move(*mod);
}

} // namespace

StatusOr<std::unique_ptr<llvm::Module>>
compileProgram(const Program &program, llvm::LLVMContext &ctx,
               const std::string &moduleName, const std::string &projectRoot) {
  if (program.Modules.empty())
    return Status::error("empty PIR program");
  std::vector<std::unique_ptr<llvm::Module>> parts;
  for (size_t i = 0; i < program.Modules.size(); ++i) {
    if (i > 0) {
      const Module &module = program.Modules[i];
      std::string key = cacheKey(module);
      std::string path = cachePath(module.Name, projectRoot);
      if (auto cached = loadCached(path, key, ctx)) {
        parts.push_back(std::move(cached));
        continue;
      }
      auto part = translateModule(program, i, ctx);
      if (!part)
        return part.status();
      stampCachedModule(**part, key);
      writeCacheFile(**part, path);
      parts.push_back(std::move(*part));
      continue;
    }
    auto part = translateModule(program, i, ctx);
    if (!part)
      return part.status();
    parts.push_back(std::move(*part));
  }
  return linkParts(program, std::move(parts), moduleName);
}

void optimizeModule(llvm::Module &module, unsigned level) {
  static const llvm::OptimizationLevel levels[] = {
      llvm::OptimizationLevel::O0,
      llvm::OptimizationLevel::O1,
      llvm::OptimizationLevel::O2,
      llvm::OptimizationLevel::O3,
  };
  llvm::OptimizationLevel lvl = levels[level < 4 ? level : 3];
  if (lvl == llvm::OptimizationLevel::O0)
    return;

  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;

  llvm::PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(lvl);
  MPM.run(module, MAM);
}

} // namespace paykan::backend::llvm_backend
