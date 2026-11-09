// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The check of a value moved out of borrowed storage (#368;
// docs/design/concurrency.md, "Owned paths and borrowed roots").  A function
// that may return a value it moved out of storage its caller named is
// MovesOut, and leaves the check to its caller.  A call of one whose slot is
// on an owned path needs no check: nothing else can reach what that path
// names (Sema::onOwnedPath).  Otherwise the call checks the value: every
// object it uniquely holds has a count of 1, or something still holds it.

#include "LoweringInternal.h"
#include "Names.h"
#include "Sema.h"

#include <functional>

namespace paykan::lowering {

using pir::Type;

namespace {

/// Calls @p f on each `return` of @p s, not those of a lambda in it.
void eachReturn(const ast::Stmt *s,
                const std::function<void(const ast::ReturnStmt *)> &f) {
  if (!s)
    return;
  if (const auto *r = ast::dyn_cast<ast::ReturnStmt>(s)) {
    f(r);
  } else if (const auto *c = ast::dyn_cast<ast::CompoundStmt>(s)) {
    for (const ast::Stmt *st : c->getStatements())
      eachReturn(st, f);
  } else if (const auto *i = ast::dyn_cast<ast::IfStmt>(s)) {
    eachReturn(i->getThenBranch(), f);
    eachReturn(i->getElseBranch(), f);
  } else if (const auto *w = ast::dyn_cast<ast::WhileStmt>(s)) {
    eachReturn(w->getBody(), f);
  } else if (const auto *fs = ast::dyn_cast<ast::ForStmt>(s)) {
    eachReturn(fs->getBody(), f);
  } else if (const auto *m = ast::dyn_cast<ast::MatchStmt>(s)) {
    for (const ast::MatchArm *arm : m->getArms())
      eachReturn(arm->getBody(), f);
  } else if (const auto *l = ast::dyn_cast<ast::LockStmt>(s)) {
    eachReturn(l->getBody(), f);
  }
}

/// `mem::replace`, `mem::take`, and the take under a `Buffer`'s `pop`.
bool isKnownMoveOut(const std::string &name) {
  return name.starts_with("$::memory::replace<") ||
         name.starts_with("$::memory::take<") ||
         (name.starts_with("Buffer<") && name.ends_with(">.pop"));
}

} // namespace

void ModuleLowering::computeMovesOut() {
  if (MovesOutKnown)
    return;
  MovesOutKnown = true;
  for (const auto &[name, fn] : Bodies) {
    bool inoutParam = false;
    for (const ast::Param &p : fn->getParams())
      inoutParam = inoutParam || p.Mode == ast::ParamMode::Inout;
    if (isKnownMoveOut(name) || (fn->isNative() && inoutParam))
      MovesOut.insert(fn);
  }
  // A function returning, as it is, a value moved out of what its caller
  // named; to a fixed point, through the calls among them.
  for (bool changed = true; changed;) {
    changed = false;
    for (const auto &[name, fn] : Bodies) {
      if (MovesOut.count(fn) || !fn->getBody())
        continue;
      bool moves = false;
      eachReturn(fn->getBody(), [&](const ast::ReturnStmt *r) {
        const ast::Expr *e = r->getReturnValue();
        moves = moves || (e && e->getMovedFrom() && e->isMovedFromParam() &&
                          valueMovesOut(e));
      });
      if (moves) {
        MovesOut.insert(fn);
        changed = true;
      }
    }
  }
}

bool ModuleLowering::methodMovesOut(ast::ClassType *ct,
                                    const std::string &name) {
  const ast::MethodDecl *md = ct->findMethod(name);
  if (md && md->isImpl()) {
    ast::ClassType *owner = ct->implOwner(name);
    const ast::FuncDecl *fn =
        owner ? bodyOf(owner->getName() + names::kNameSep + name) : nullptr;
    return !fn || MovesOut.count(fn);
  }
  // An unresolved virtual call (the design: counts as moving out).
  if (!isExactClass(ct) && !isClosedClass(ct))
    return true;
  bool moves = false;
  for (ast::ClassType *c = ct; c; c = c->getSuperClass()) {
    if (c->isBuiltin())
      break; // the runtime's, which keeps none of it
    if (const ast::FuncDecl *fn =
            bodyOf(c->getName() + names::kNameSep + name)) {
      moves = MovesOut.count(fn) != 0;
      break;
    }
    if (!OwnTypes.count(c->getName()) && !ImportedCopies.count(c->getName()))
      return true; // another module's class, compiled there
  }
  if (auto sub = Subclasses.find(ct); sub != Subclasses.end())
    for (ast::ClassType *c : sub->second)
      if (const ast::FuncDecl *fn =
              bodyOf(c->getName() + names::kNameSep + name))
        moves = moves || MovesOut.count(fn);
  return moves;
}

bool ModuleLowering::valueMovesOut(const ast::Expr *e) {
  if (ast::isa<ast::CallValueExpr>(e))
    return true; // a function value: not known
  if (const auto *call = ast::dyn_cast<ast::CallExpr>(e)) {
    if (call->calleeIsValue())
      return true; // a function value: not known
    const std::string &name = call->getCalleeName();
    if (const ast::FuncDecl *fn = bodyOf(name))
      return MovesOut.count(fn) != 0;
    // A constructor makes a new value; another module's function is not
    // known here.
    return !ASTCtx.lookupClassType(name) && !ASTCtx.lookupStructType(name);
  }
  const auto *mc = ast::dyn_cast<ast::MethodCallExpr>(e);
  if (!mc)
    return false;
  if (mc->isFieldCall())
    return true; // a function value held in a field
  ast::Type *recv = ast::stripOptional(
      const_cast<ast::Expr *>(mc->getReceiver())->getResolvedType());
  if (auto *ct = recv ? ast::dyn_cast<ast::ClassType>(recv) : nullptr)
    return !ct->isBuiltin() && methodMovesOut(ct, mc->getMethodName());
  if (auto *st = recv ? ast::dyn_cast<ast::StructType>(recv) : nullptr) {
    const ast::FuncDecl *fn =
        bodyOf(st->getName() + names::kNameSep + mc->getMethodName());
    return !fn || MovesOut.count(fn);
  }
  return false;
}

void ModuleLowering::emitMovedValueCheck(const std::string &place,
                                         ast::Type *ty, const Val &v) {
  Val unique;
  if (ast::isMemType(ty) && v.Ty == Type::Ptr)
    unique = callRuntime(names::kPaykanMovedUniqueMem,
                         {emitTypeDescriptor(ty), v}, "moved.unique");
  else if (v.Ty == Type::Box)
    unique = callRuntime(names::kPaykanMovedUnique, {v}, "moved.unique");
  else
    return;
  pir::If *s =
      B.openIf(B.cmp(pir::CmpPred::Eq, unique, Val::i64(0), "moved.held"),
               /*withElse=*/false);
  B.enter(*s->Then);
  callRuntime(names::kPaykanPanicMoved, {internString(place)});
  B.leave();
}

Val ModuleLowering::emitMoveOutCheck(ast::Expr *e, Val v) {
  const std::string *place = e->getMovedFrom();
  if (!place || !v)
    return v;
  computeMovesOut();
  // Returned as it is from what the function's caller named: the caller's
  // to check.
  if ((e == TailReturn && e->isMovedFromParam()) || !valueMovesOut(e))
    return v;
  emitMovedValueCheck(*place, e->getResolvedType(), v);
  return v;
}

void ModuleLowering::emitSwapCheck(ast::Expr *e) {
  const auto *into = const_cast<ast::Expr *>(e->getMovedInto());
  if (!into)
    return;
  auto *place = const_cast<ast::Expr *>(into);
  ast::Type *ty = place->getResolvedType();
  Val at = emitPlaceAddress(place);
  if (!at)
    return;
  emitMovedValueCheck(*e->getMovedFrom(), ty,
                      ast::isMemType(ty) ? at
                                         : B.ptrLoad(at, Type::Box, "moved"));
}

bool ModuleLowering::holdsUnique(const ast::Type *ty) {
  if (!ty || ast::isFunctionValue(ty) || ast::isa<ast::AtomicType>(ty))
    return false; // a function value's closure and a cell's handle copy
  if (const auto *qt = ast::dyn_cast<ast::QualifiedType>(ty))
    return qt->getQualifier().Form == ast::QualForm::Unique &&
           holdsUnique(qt->getInnerType());
  if (const auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    return holdsUnique(ot->getInnerType());
  if (ast::isCountedRef(ty))
    return true;
  if (const auto *et = ast::dyn_cast<ast::EnumType>(ty)) {
    for (size_t i = 0; i < et->getNumVariants(); ++i)
      if (const ast::StructType *p = et->getPayload(i); p && holdsUnique(p))
        return true;
    return false;
  }
  if (const auto *st = ast::dyn_cast<ast::StructType>(ty)) {
    for (size_t i = 0; i < st->getArity(); ++i)
      if (st->findFieldQualifier(st->getFieldName(i)).Form ==
              ast::QualForm::Unique &&
          holdsUnique(st->getElementType(i)))
        return true;
    return false;
  }
  if (const auto *tt = ast::dyn_cast<ast::TupleType>(ty)) {
    for (size_t i = 0; i < tt->getArity(); ++i)
      if (tt->getElemQual(i).Form == ast::QualForm::Unique &&
          holdsUnique(tt->getElementType(i)))
        return true;
    return false;
  }
  if (const auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty))
    return ft->getElemQual().Form == ast::QualForm::Unique &&
           holdsUnique(ft->getElementType());
  return false;
}

bool ModuleLowering::movesBytes(const ast::UnaryExpr *u) {
  const ast::Expr *operand = u->getOperand();
  return !u->moveIsCopy() &&
         holdsUnique(const_cast<ast::Expr *>(operand)->getResolvedType()) &&
         (ast::isa<ast::Identifier>(operand) ||
          ast::isa<ast::MemberAccessExpr>(operand) ||
          ast::isa<ast::TupleIndexExpr>(operand) ||
          ast::isa<ast::SubscriptExpr>(operand));
}

ModuleLowering::MemValue ModuleLowering::emitMemMoved(ast::UnaryExpr *u) {
  // The bytes leave their place with what they own: a teardown of the
  // place finds zeros.
  ast::Expr *operand = u->getOperand();
  MemValue from = emitMemRead(operand);
  if (!from.Addr || from.Temp)
    return from;
  MemLayout l = memLayout(memTypeOf(operand));
  pir::LocalId tmp = B.addMemLocal("moved", l.Size, l.Align);
  Val addr = B.localAddr(tmp, "moved.addr");
  B.memCopy(addr, from.Addr, Val::i64(l.Size));
  B.memZero(from.Addr, Val::i64(l.Size));
  return {addr, /*Temp=*/true};
}

void ModuleLowering::takeFromMatchSlot(const ast::MatchArm *arm, const Val &own,
                                       const Val &from, ast::Type *ty) {
  // The binding has the bytes.  A move-only value keeps one owner: the
  // match's slot gives up its references to the binding.  An `inout` arm
  // writes back, so it keeps the retained copy.
  if (holdsUnique(ty) && arm->getMode() != ast::ParamMode::Inout)
    B.memZero(from, Val::i64(memLayout(ty).Size));
  else
    emitMemRetain(own, ty);
}

void ModuleLowering::dropMatchSlot(const Val &addr, ast::Type *ty) {
  if (!holdsUnique(ty))
    return;
  emitMemRelease(addr, ty);
  B.memZero(addr, Val::i64(memLayout(ty).Size));
}

void ModuleLowering::endViewHold(const ast::VarDecl *vd) {
  const std::string &name = vd->getName();
  if (vd->getMode() == ast::ParamMode::Inout) {
    auto holds = CurrentScope->ObjectHolds.find(name);
    if (holds == CurrentScope->ObjectHolds.end())
      return;
    for (const Scope::VarMeta &h : holds->second) {
      if (h.ASTType && ast::isMemType(h.ASTType)) {
        Val addr = B.localAddr(h.Local, name + ".obj.addr");
        emitMemRelease(addr, h.ASTType);
        B.memZero(addr, Val::i64(memLayout(h.ASTType).Size));
      } else {
        emitRelease(B.load(h.Local, name + ".obj"));
        B.store(h.Local, Val::null(Type::Box));
      }
    }
    return;
  }
  if (holdsBox(name)) {
    emitRelease(loadVarBox(name));
    B.store(CurrentScope->lookup(name), Val::null(Type::Box));
    return;
  }
  ast::Type *ty = CurrentScope->lookupASTType(name);
  if (!ast::isMemType(ty) || !memHoldsRefs(ty))
    return;
  Val addr = B.localAddr(CurrentScope->lookup(name), name + ".addr");
  emitMemRelease(addr, ty);
  B.memZero(addr, Val::i64(memLayout(ty).Size));
}

void ModuleLowering::emitStatements(const std::vector<ast::Stmt *> &stmts) {
  const auto ends = sema::viewHoldEnds(stmts);
  for (size_t i = 0; i < stmts.size(); ++i) {
    visit(stmts[i]);
    if (!B.isTerminated())
      for (const ast::VarDecl *vd : ends[i])
        endViewHold(vd);
  }
}

bool ModuleLowering::isPartOfVariable(const ast::Expr *e) {
  for (;;) {
    if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e))
      e = ma->getReceiver();
    else if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(e))
      e = ti->getTuple();
    else if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(e))
      e = se->getArray();
    else
      break;
  }
  return ast::isa<ast::Identifier>(e);
}

bool ModuleLowering::matchHoldsNothing(const ast::MatchStmt *node) {
  for (const ast::MatchArm *arm : node->getArms())
    if (arm->hasBinding())
      return false;
  return isPartOfVariable(node->getSubject());
}

} // namespace paykan::lowering
