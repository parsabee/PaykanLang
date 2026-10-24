// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// AST -> PIR lowering: structs (docs/language/11-structs.md).
//
// A struct is a tuple of its fields to the lowering (ast::StructType is a
// TupleType): a value held in memory (LoweringMem.cpp), copied, retained and
// released as a tuple is, printed and compared through a layout that also
// holds its name and its fields' names (Runtime.h, "Structs").  What is the
// struct's own:
//
//   * its fields are read and written in place, at their offsets, as a
//     tuple's elements are (a field of a reference type owns its box);
//   * its methods are the functions `S.m(self: ptr, ...)`, called directly:
//     `self` is the address of the receiver's storage (a variable's, a
//     field's, an element's, or a temporary's), so a method that is not a
//     `view fn` changes the receiver itself, as an `inout` parameter does;
//   * its constructor is the function `S(..., out: ptr)`: it zeroes the
//     value at `out`, where the caller's temporary is (a result held in
//     memory, as for any function), and runs `S.__init__` on it.

#include "LoweringInternal.h"
#include "Names.h"

#include <cassert>

namespace paykan::lowering {

using namespace names;
using pir::Type;

// -- Signatures

pir::Signature ModuleLowering::structMethodSignature(ast::MethodDecl *md) {
  // The receiver's address first; the parameters as a class method's (a
  // reference consumed as a box); a result held in memory written where a
  // last parameter points, an `inout` one its storage's address.
  pir::Signature sig;
  sig.Params.push_back(Type::Ptr);
  for (size_t i = 0; i < md->getNumParams(); ++i) {
    appendParamTypes(sig.Params, md->getParamTypes()[i],
                     md->getParamModes().mode(i));
    if (sig.Params.back() == Type::Void)
      sig.Params.back() = Type::Box;
  }
  sig.Ret = md->getReturnType()
                ? toPIRType(canonicalizeDeclType(md->getReturnType()))
                : Type::Void;
  if (md->getParamModes().Result == ast::ParamMode::Inout) {
    sig.Ret = Type::Ptr;
  } else if (ast::isMemType(md->getReturnType())) {
    sig.Params.push_back(Type::Ptr);
    sig.Ret = Type::Void;
  }
  return sig;
}

pir::Signature ModuleLowering::structConstructorSignature(ast::StructType *st) {
  pir::Signature sig;
  if (auto *initMd = st->findMethod(kMethodInit))
    for (size_t i = 0; i < initMd->getNumParams(); ++i) {
      appendParamTypes(sig.Params, initMd->getParamTypes()[i],
                       initMd->getParamModes().mode(i));
      if (sig.Params.back() == Type::Void)
        sig.Params.back() = Type::Box;
    }
  sig.Params.push_back(Type::Ptr); // the value to make
  sig.Ret = Type::Void;
  return sig;
}

// -- Declarations and bodies

void ModuleLowering::declareStruct(ast::StructDecl *sd) {
  auto *st = ASTCtx.lookupStructType(sd->getName());
  assert(st && "StructType must have been registered by Sema");
  for (auto *funcDecl : sd->getMethods()) {
    auto *md = st->findMethod(funcDecl->getName());
    if (!md)
      continue;
    pir::Function *fn =
        getOrCreateFunction(sd->getName() + kNameSep + funcDecl->getName(),
                            structMethodSignature(md));
    fn->Params[0].Name = kSelf;
    nameParams(*fn, 1, funcDecl->getParams());
  }
  pir::Function *ctor =
      getOrCreateFunction(sd->getName(), structConstructorSignature(st));
  for (auto *m : sd->getMethods())
    if (m->getName() == kMethodInit)
      nameParams(*ctor, 0, m->getParams());
  ctor->Params.back().Name = "out";
}

Val ModuleLowering::lowerStructDecl(ast::StructDecl *sd) {
  auto *st = ASTCtx.lookupStructType(sd->getName());
  assert(st && "StructType must have been registered by Sema");
  declareStruct(sd);

  // 1. Method bodies.
  for (auto *funcDecl : sd->getMethods()) {
    auto *md = st->findMethod(funcDecl->getName());
    if (!md)
      continue;
    pir::Function *fn =
        FuncByName.at(sd->getName() + kNameSep + funcDecl->getName());
    FunctionStateGuard fnState(*this, fn, md->getReturnType(),
                               /*methodClassTy=*/nullptr);
    {
      ScopeGuard guard(*this);
      // `self` names the receiver's storage, as an `inout` parameter of the
      // struct's type names its caller's.
      declareMemParam(kSelf, fn->Params[0], st, ast::ParamMode::Inout);
      size_t at = 1;
      for (size_t i = 0; i < funcDecl->getParams().size(); ++i)
        at +=
            declareParam(funcDecl->getParams()[i].getName(), fn->Params, at,
                         md->getParamTypes()[i], funcDecl->getParams()[i].Mode);
      emitBody(funcDecl->getBody());
    }
    emitImplicitReturn(fn->Sig);
  }

  // 2. The constructor: the value starts out all zero (so an optional field
  //    `__init__` leaves alone is None, and a reference field it assigns
  //    releases nothing), then `__init__` fills it in.
  pir::Function *ctorFn = FuncByName.at(sd->getName());
  FunctionStateGuard fnState(*this, ctorFn, nullptr, nullptr);
  Val out(pir::Operand::value(ctorFn->Params.back()), Type::Ptr);
  B.memZero(out, Val::i64(memLayout(st).Size));
  if (st->findMethod(kMethodInit)) {
    pir::Function *initFn = lookupTypeFunction(
        sd->getName(), sd->getName() + kNameSep + kMethodInit);
    assert(initFn && "__init__ must have been declared with the struct");
    std::vector<Val> initArgs = {out};
    for (size_t i = 0; i + 1 < ctorFn->Params.size(); ++i)
      initArgs.push_back(
          Val(pir::Operand::value(ctorFn->Params[i]), ctorFn->Params[i].Ty));
    B.call(initFn->Name, initFn->Sig, initArgs);
  }
  B.emitRetVoid();
  return Val();
}

// -- Fields

ast::StructType *ModuleLowering::getExprStructType(ast::Expr *e) {
  ast::Type *ty = e->getResolvedType();
  if (auto *id = ast::dyn_cast<ast::Identifier>(e); id && CurrentScope)
    if (ast::Type *t = CurrentScope->lookupASTType(id->getName()))
      ty = t;
  if (!ty)
    return nullptr;
  if (auto *st = ast::dyn_cast<ast::StructType>(canonicalizeDeclType(ty)))
    return st;
  return nullptr;
}

Val ModuleLowering::lowerStructFieldRead(ast::MemberAccessExpr *node,
                                         ast::StructType *st) {
  int idx = st->fieldIndex(node->getFieldName());
  assert(idx >= 0 && "Sema should have verified the field exists");
  // Read in place; a temporary's field, or a held one, is a +1 box
  // (exprProducesFreshBox).
  size_t mark = MemTemps.size();
  Val addr =
      emitTupleElementAddress(node->getReceiver(), static_cast<size_t>(idx));
  if (!addr)
    return Val();
  return ownElementRead(node, emitElementLoad(addr, node->getResolvedType()),
                        mark);
}

Val ModuleLowering::lowerStructFieldAssign(ast::MemberAssignStmt *node,
                                           ast::StructType *st) {
  int idx = st->fieldIndex(node->getFieldName());
  assert(idx >= 0 && "Sema should have verified the field exists");
  ast::Type *fieldTy = st->getElementType(static_cast<size_t>(idx));
  size_t mark = MemTemps.size();
  Val addr =
      emitTupleElementAddress(node->getReceiver(), static_cast<size_t>(idx));
  if (!addr)
    return Val();
  if (ast::isMemType(fieldTy)) {
    emitMemAssign(addr, node->getValue());
  } else if (ast::isRefType(fieldTy)) {
    // The slot owns a box: the new reference is taken before the old one
    // goes (`p.s = p.s;`); a zeroed slot's null box releases as nothing.
    if (Val box = emitAsShared(node->getValue())) {
      Val old = B.ptrLoad(addr, Type::Box, "old");
      B.ptrStore(addr, box);
      emitRelease(old);
    }
  } else if (Val v = emitExpr(node->getValue())) {
    B.ptrStore(addr, coerceTo(v, toPIRType(fieldTy)));
  }
  releaseMemTemps(mark);
  return Val();
}

// -- Method calls

Val ModuleLowering::ExprEmitter::emitStructMethodCall(ast::MethodCallExpr *node,
                                                      ast::StructType *st,
                                                      bool wantAddress) {
  const std::string &mname = node->getMethodName();
  ast::MethodDecl *method = st->findMethod(mname);
  pir::Function *fn =
      method ? L.lookupTypeFunction(st->getName(),
                                    st->getName() + kNameSep + mname)
             : nullptr;
  if (!fn) {
    L.reportInternalError("no function for method '" + mname + "' of struct '" +
                          st->getName() + "'");
    return Val();
  }
  // The receiver's storage, for the whole call: a variable's, a field's or
  // an element's own bytes, or a temporary's, released after the call.
  size_t mark = L.MemTemps.size();
  Val self = L.emitMemAddress(node->getReceiver());
  if (!self)
    return Val();
  std::vector<Val> args;
  std::vector<ExprValue> ownedArgs;
  std::vector<Val> keep;
  if (!emitMethodArgs(node, method, fn->Sig, /*userDefined=*/true, args,
                      ownedArgs, keep))
    return Val();
  args.insert(args.begin(), self);
  Val slot;
  if (ast::isMemType(node->getResolvedType()) && fn->Sig.Ret != Type::Ptr) {
    slot = L.emitMemResultSlot(node->getResolvedType());
    args.push_back(slot);
  }
  Val result = L.B.call(fn->Name, fn->Sig, args, "mcall");
  for (const auto &ev : ownedArgs)
    L.releaseIfOwned(ev);
  if (result && result.Ty == Type::Ptr) // an `inout` result
    return L.takeInoutResult(result, node, wantAddress, keep, mark);
  L.releaseAfterCall(keep);
  L.releaseMemTemps(mark);
  if (slot)
    return slot;
  return result;
}

} // namespace paykan::lowering
