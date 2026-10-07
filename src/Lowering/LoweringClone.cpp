// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// AST -> PIR lowering: `cp` (deep clone) and `mv` (hand-over) of the
// ownership prototype (docs/design/ownership-proto.md).
//
// `cp e` of a reference calls the module's dispatcher `@.clone(box) -> box`
// (a fresh +1 box, null for null).  A user class object goes to the
// `C.$clone` of its dynamic class (found by vtable address), which allocates
// a C without `__init__` and fills it with `C.$copy`: the superclass's
// `$copy`, then C's fields (values copied, `own` references cloned, view and
// `mut` references retained).  Any other object is copied shallowly by the
// runtime (Paykan_clone_shallow) and each reference slot of the copy is then
// replaced by its clone.  The class functions exist only with --ownership.

#include "LoweringInternal.h"
#include "Names.h"

#include <algorithm>

namespace paykan::lowering {

using namespace names;
using pir::CmpPred;
using pir::Type;

namespace {
constexpr const char *kCloneDispatcher = ".clone";
constexpr const char *kCloneFn = "$clone";
constexpr const char *kCopyFn = "$copy";
} // namespace

pir::Function *ModuleLowering::cloneDispatcher() {
  if (!CloneDispatcher) {
    CloneDispatcher =
        getOrCreateFunction(kCloneDispatcher, {{Type::Box}, Type::Box});
    CloneDispatcher->Params[0].Name = "src";
  }
  return CloneDispatcher;
}

Val ModuleLowering::emitCloneCall(const Val &box) {
  pir::Function *fn = cloneDispatcher();
  return B.call(fn->Name, fn->Sig, {box}, "clone");
}

void ModuleLowering::declareCloneFunctions(ast::ClassDecl *node) {
  if (!SemaCtx.Ownership)
    return;
  getOrCreateFunction(node->getName() + kNameSep + kCloneFn,
                      {{Type::Obj}, Type::Box});
  getOrCreateFunction(node->getName() + kNameSep + kCopyFn,
                      {{Type::Obj, Type::Obj}, Type::Void});
}

void ModuleLowering::emitCloneFunctions(ast::ClassDecl *node,
                                        ast::ClassType *ct) {
  if (!SemaCtx.Ownership)
    return;
  const std::string &name = node->getName();
  pir::Function *copyFn = FuncByName.at(name + kNameSep + kCopyFn);
  {
    FunctionStateGuard fnState(*this, copyFn, nullptr, nullptr);
    Val src(pir::Operand::value(copyFn->Params[0]), Type::Obj);
    Val dst(pir::Operand::value(copyFn->Params[1]), Type::Obj);
    if (ast::ClassType *super = ct->getSuperClass();
        super && !super->isBuiltin())
      if (pir::Function *superCopy =
              lookupClassFunction(super, super->getName() + kNameSep + kCopyFn))
        B.call(superCopy->Name, superCopy->Sig, {src, dst});
    for (auto &[fname, fty] : ct->getFields()) {
      ast::Qualifier qual = ast::Qualifier::View;
      for (ast::VarDecl *field : node->getFields())
        if (field->getName() == fname)
          qual = field->getQualifier();
      Type t = toPIRType(fty);
      Val v =
          B.fieldLoad(src, name, fname, t == Type::Void ? Type::Box : t, fname);
      if (ast::isRefType(fty)) {
        if (qual == ast::Qualifier::Own)
          v = emitCloneCall(v);
        else
          emitRetain(v);
      }
      B.fieldStore(dst, name, fname, v);
    }
    B.emitRetVoid();
  }
  pir::Function *cloneFn = FuncByName.at(name + kNameSep + kCloneFn);
  FunctionStateGuard fnState(*this, cloneFn, nullptr, nullptr);
  Val src(pir::Operand::value(cloneFn->Params[0]), Type::Obj);
  Val dst = B.newObject(name, "obj");
  B.call(copyFn->Name, copyFn->Sig, {src, dst});
  B.emitRet(emitSharedNew(dst, name + ".shared"));
}

void ModuleLowering::emitCloneDispatcher() {
  if (!CloneDispatcher)
    return;
  FunctionStateGuard fnState(*this, CloneDispatcher, nullptr, nullptr);
  Val src(pir::Operand::value(CloneDispatcher->Params[0]), Type::Box);
  // None stays None.
  pir::If *isNull =
      B.openIf(B.cmp(CmpPred::Eq, src, Val::null(Type::Box), "is.null"), false);
  B.enter(*isNull->Then);
  B.emitRet(Val::null(Type::Box));
  B.leave();
  Val raw = emitSharedGet(src, "src.obj");

  // A user class: its own clone function, by the dynamic class.
  std::vector<ast::ClassType *> classes;
  for (auto &[cname, ct] : ASTCtx.getClassTypes())
    if (ct && !ct->isBuiltin() && cname == ct->getName())
      classes.push_back(ct);
  std::sort(classes.begin(), classes.end(),
            [](ast::ClassType *a, ast::ClassType *b) {
              return a->getName() < b->getName();
            });
  for (ast::ClassType *ct : classes) {
    pir::Function *fn =
        lookupClassFunction(ct, ct->getName() + kNameSep + kCloneFn);
    if (!fn)
      continue;
    pir::If *is = B.openIf(emitIsExactType(raw, ct), false);
    B.enter(*is->Then);
    B.emitRet(B.call(fn->Name, fn->Sig, {raw}, "clone"));
    B.leave();
  }

  // Anything else: a shallow copy whose reference slots are then cloned.
  Val dst = callRuntime(kPaykanCloneShallow, {raw}, "copy");
  Val dstRaw = emitSharedGet(dst, "copy.obj");
  Val n = callRuntime(kPaykanCloneSlots, {dstRaw}, "slots");
  pir::LocalId idx = B.addLocal("i", Type::I64);
  B.store(idx, Val::i64(0));
  pir::While *w = B.openWhile();
  B.enter(*w->CondBlock);
  B.setWhileCond(w, B.cmp(CmpPred::Lt, B.load(idx, "i"), n, "more"));
  B.leave();
  B.enterLoopBody(*w->Body);
  Val i = B.load(idx, "i");
  Val elem = callRuntime(kPaykanCloneSlotGet, {dstRaw, i}, "elem");
  pir::If *isRef =
      B.openIf(B.cmp(CmpPred::Ne, elem, Val::null(Type::Box), "is.ref"), false);
  B.enter(*isRef->Then);
  Val c = emitCloneCall(elem);
  callRuntime(kPaykanCloneSlotSet, {dstRaw, i, c});
  emitRelease(c);
  B.leave();
  B.store(idx, B.binary(pir::Opcode::Add, i, Val::i64(1), "i.next"));
  B.leaveLoopBody();
  B.emitRet(dst);
}

// `cp e`: a reference is cloned (a fresh +1 box, as exprAlreadyShared and
// exprProducesFreshBox say); a value is just read.
Val ModuleLowering::ExprEmitter::visitCopyExpr(ast::CopyExpr *node) {
  ast::Type *ty = node->getResolvedType();
  if (!ty || !ast::isRefType(ty))
    return L.emitExpr(node->getOperand());
  Val src = L.emitAsSharedRaw(node->getOperand());
  if (!src)
    return src;
  Val copy = L.emitCloneCall(src);
  L.emitRelease(src);
  return copy;
}

// `mv x`: the box in x's slot is handed over (+1, no retain) and the slot
// cleared, so x's scope cleanup releases null.  A value is just read.
Val ModuleLowering::ExprEmitter::visitMoveExpr(ast::MoveExpr *node) {
  ast::Type *ty = node->getResolvedType();
  ast::Expr *op = node->getOperand();
  if (!ty || !ast::isRefType(ty))
    return L.emitExpr(op);
  auto *id = ast::dyn_cast<ast::Identifier>(op);
  if (!id || !L.CurrentScope->isOwned(id->getName()))
    return L.emitAsSharedRaw(op); // Sema allows only owned locals
  pir::LocalId local = L.CurrentScope->lookup(id->getName());
  Val box = L.B.load(local, id->getName());
  L.B.store(local, Val::null(Type::Box));
  return box;
}

} // namespace paykan::lowering
