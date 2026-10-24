// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// AST -> PIR lowering: `inout` parameters and locals
// (docs/language/03-functions.md, "Parameter modes";
// docs/language/02-basics.md, "Local borrows").
//
// An `inout` parameter is a PIR `ptr`: the address of its caller's storage.
// The callee keeps the address in the parameter's local and reads and writes
// through it (ptr.load / ptr.store), so every write reaches the caller at
// once.  A caller passes a variable's `local.addr`, for one of its own
// `inout` parameters or locals the address it holds, and for a field of an
// object the field's real address (`field.addr`, never a copy).  Objects do
// not move, so a field's address is valid as long as its object lives, and
// the caller keeps the object alive for the call.  Sema has checked that
// each argument is such a place, of exactly the parameter's type, and that
// no place is passed twice.
//
// An `inout` local (`x: inout = k;`) is a `ptr` local holding the address of
// what it names, the same way; a hidden owned local keeps a field's object
// alive for as long as the local can be used.
//
// A `match` arm's `n: inout T` (docs/language/08-match.md, "Borrowing the
// subject") narrows the subject to T: a variant of a tagged union, the
// value of an optional, a subclass.  Another reference to the subject's
// object (`g = h`, a parameter that is `self`) can change what it holds
// while the arm runs, which no check of names sees, so n is never the
// subject's storage narrowed: it is a variable of its own, a copy of the
// matched value as a plain binding is, assigned back to the subject's
// storage, as `subject = n` would, after n's last use in the arm, or on a
// way out of the arm before it (bindInoutArm, emitArmBody).  Until then the
// subject cannot be used but through n (Sema), so a program sees the
// subject change as n does.  The storage is found once, when the match
// starts, and the matched value is read there (emitMatchPlace): a call on
// the way to it (`b.slot().s`, with an `inout` result) runs once, and n
// goes back where its value came from.
//
// For a reference type the storage holds a box, and the `inout` is a mutable
// reference to it, not a share of it: it retains nothing and releases
// nothing at its end.  Reading it borrows the box, as reading a variable
// does; assigning it replaces the box like an assignment to the variable
// (release the old, store the new).  A `view` parameter is passed by value
// like any other.
//
// So a call can replace what a variable of its caller holds while the
// caller still uses a value it read from the variable earlier in the same
// statement: `s + repl(s)`, `l.greet(repl(l))`, `s.n.greet(s.reset())` (a
// struct's method that may change `self`).  Such a read takes a reference
// of its own (noteHeldReads), as a call's result has one, so that what it
// read lives until it is used, whatever the call does to the variable.

#include "LoweringInternal.h"

#include <algorithm>
#include <cassert>
#include <functional>

namespace paykan::lowering {

using pir::Type;

namespace {

/// Calls @p fn with each operand @p e evaluates; a lambda's body runs when
/// the lambda is called, and what it captures it copies.
void forEachOperand(ast::Expr *e, const std::function<void(ast::Expr *)> &fn) {
  if (auto *x = ast::dyn_cast<ast::BinaryExpr>(e)) {
    fn(x->getLHS());
    fn(x->getRHS());
  } else if (auto *x = ast::dyn_cast<ast::UnaryExpr>(e)) {
    fn(x->getOperand());
  } else if (auto *x = ast::dyn_cast<ast::CallExpr>(e)) {
    for (ast::Expr *a : x->getArguments())
      fn(a);
  } else if (auto *x = ast::dyn_cast<ast::MethodCallExpr>(e)) {
    fn(x->getReceiver());
    for (ast::Expr *a : x->getArguments())
      fn(a);
  } else if (auto *x = ast::dyn_cast<ast::CallValueExpr>(e)) {
    fn(x->getCallee());
    for (ast::Expr *a : x->getArguments())
      fn(a);
  } else if (auto *x = ast::dyn_cast<ast::TernaryExpr>(e)) {
    fn(x->getCondition());
    fn(x->getTrueExpr());
    fn(x->getFalseExpr());
  } else if (auto *x = ast::dyn_cast<ast::MemberAccessExpr>(e)) {
    fn(x->getReceiver());
  } else if (auto *x = ast::dyn_cast<ast::SubscriptExpr>(e)) {
    fn(x->getArray());
    fn(x->getIndex());
  } else if (auto *x = ast::dyn_cast<ast::TupleIndexExpr>(e)) {
    fn(x->getTuple());
  } else if (auto *x = ast::dyn_cast<ast::ArrayLiteralExpr>(e)) {
    for (ast::Expr *a : x->getElements())
      fn(a);
  } else if (auto *x = ast::dyn_cast<ast::TupleLiteralExpr>(e)) {
    for (ast::Expr *a : x->getElements())
      fn(a);
  }
}

} // namespace

ast::Identifier *ModuleLowering::storageRoot(ast::Expr *place) {
  for (ast::Expr *e = place;;) {
    if (auto *id = ast::dyn_cast<ast::Identifier>(e))
      return id;
    auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e);
    auto *se = ast::dyn_cast<ast::SubscriptExpr>(e);
    if (ma && getExprStructType(ma->getReceiver()))
      e = ma->getReceiver();
    else if (auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(e))
      e = ti->getTuple();
    else if (se && ast::dyn_cast<ast::FixedArrayType>(
                       se->getArray()->getResolvedType()))
      e = se->getArray();
    else
      return nullptr;
  }
}

void ModuleLowering::noteHeldReads(ast::Stmt *s) {
  // The statement's own expressions, and the places it assigns to.
  std::vector<ast::Expr *> exprs;
  std::vector<ast::Expr *> targets;
  if (auto *x = ast::dyn_cast<ast::ReturnStmt>(s)) {
    exprs = {x->getReturnValue()};
  } else if (auto *x = ast::dyn_cast<ast::AssignStmt>(s)) {
    exprs = {x->getValue()};
  } else if (auto *x = ast::dyn_cast<ast::DeclStmt>(s)) {
    if (auto *vd = ast::dyn_cast<ast::VarDecl>(x->getDecl()))
      exprs = {vd->getInitExpr()};
  } else if (auto *x = ast::dyn_cast<ast::ExprStmt>(s)) {
    exprs = {x->getExpr()};
  } else if (auto *x = ast::dyn_cast<ast::IfStmt>(s)) {
    exprs = {x->getCondition()};
  } else if (auto *x = ast::dyn_cast<ast::WhileStmt>(s)) {
    exprs = {x->getCondition()};
  } else if (auto *x = ast::dyn_cast<ast::ForStmt>(s)) {
    exprs = {x->getStart(), x->getEnd(), x->getStep()};
  } else if (auto *x = ast::dyn_cast<ast::MatchStmt>(s)) {
    exprs = {x->getSubject()};
  } else if (auto *x = ast::dyn_cast<ast::DestructureStmt>(s)) {
    exprs = {x->getValue()};
  } else if (auto *x = ast::dyn_cast<ast::MemberAssignStmt>(s)) {
    exprs = {x->getReceiver(), x->getValue()};
    if (getExprStructType(x->getReceiver()))
      targets = {x->getReceiver()};
  } else if (auto *x = ast::dyn_cast<ast::SubscriptAssignStmt>(s)) {
    exprs = {x->getArray(), x->getIndex(), x->getValue()};
    targets = {x->getArray()};
  }
  std::erase(exprs, nullptr);
  // The variables a call of the statement may change, and the places
  // through which it does, which are no reads; nor is the place the
  // statement assigns to in place (a struct's field, an element).
  std::unordered_set<std::string> changed;
  std::unordered_set<const ast::Expr *> places;
  std::function<void(ast::Expr *)> findChanges = [&](ast::Expr *e) {
    if (e->isChangedByCall())
      if (ast::Identifier *root = storageRoot(e)) {
        changed.insert(root->getName());
        places.insert(root);
      }
    forEachOperand(e, findChanges);
  };
  for (ast::Expr *e : exprs)
    findChanges(e);
  if (changed.empty())
    return;
  for (ast::Expr *t : targets)
    if (ast::Identifier *root = storageRoot(t))
      places.insert(root);
  std::function<void(ast::Expr *)> findReads = [&](ast::Expr *e) {
    if (auto *id = ast::dyn_cast<ast::Identifier>(e);
        id && !id->isFunctionRef() && changed.count(id->getName()) &&
        !places.count(id)) {
      ast::Type *ty = CurrentScope->lookupASTType(id->getName());
      if (ast::isRefType(ty) || (ast::isMemType(ty) && memHoldsRefs(ty)))
        HeldReads.insert(id);
    }
    forEachOperand(e, findReads);
  };
  for (ast::Expr *e : exprs)
    findReads(e);
}

void ModuleLowering::Scope::declareInout(const std::string &name,
                                         pir::LocalId local, pir::Type pointee,
                                         ast::Type *astTy) {
  Locals[name] = local;
  InoutVars[name] = pointee;
  if (astTy)
    ASTTypeMap[name] = astTy;
}

pir::Type ModuleLowering::Scope::inoutType(const std::string &name) const {
  if (Locals.count(name)) {
    auto it = InoutVars.find(name);
    return it == InoutVars.end() ? Type::Void : it->second;
  }
  return Parent ? Parent->inoutType(name) : Type::Void;
}

Type ModuleLowering::paramPIRType(ast::Type *ty, ast::ParamMode mode) {
  // A value held in memory is passed by address in every mode
  // (LoweringMem.cpp).
  return mode == ast::ParamMode::Inout || ast::isMemType(ty)
             ? Type::Ptr
             : toPIRType(canonicalizeDeclType(ty));
}

void ModuleLowering::appendParamTypes(std::vector<Type> &out, ast::Type *ty,
                                      ast::ParamMode mode) {
  if (ast::dyn_cast<ast::SliceType>(ty)) {
    out.push_back(Type::Ptr);
    out.push_back(Type::I64);
    return;
  }
  out.push_back(paramPIRType(ty, mode));
}

void ModuleLowering::nameParams(pir::Function &fn, size_t first,
                                const std::vector<ast::Param> &params) {
  size_t at = first;
  for (const ast::Param &p : params) {
    if (at < fn.Params.size())
      fn.Params[at++].Name = p.getName();
    if (ast::dyn_cast<ast::SliceType>(p.ParamType) && at < fn.Params.size())
      fn.Params[at++].Name = p.getName() + ".len";
  }
}

size_t ModuleLowering::declareParam(const std::string &name,
                                    const std::vector<pir::Value> &params,
                                    size_t at, ast::Type *astTy,
                                    ast::ParamMode mode) {
  const pir::Value &arg = params[at];
  if (auto *st = ast::dyn_cast<ast::SliceType>(astTy)) {
    // The address of the elements, reached as an `inout` array's, and their
    // number in `<name>.len`.
    pir::LocalId addr = B.addLocal(name, Type::Ptr);
    B.store(addr, Val(pir::Operand::value(arg), Type::Ptr));
    CurrentScope->declareInout(name, addr, Type::Mem, st);
    pir::LocalId len = B.addLocal(name + ".len", Type::I64);
    B.store(len, Val(pir::Operand::value(params[at + 1]), Type::I64));
    CurrentScope->declare(name + ".len", len, ASTCtx.getIntTy());
    return 2;
  }
  if (ast::isMemType(astTy)) {
    declareMemParam(name, arg, canonicalizeDeclType(astTy), mode);
    return 1;
  }
  pir::LocalId local = B.addLocal(name, arg.Ty);
  if (mode == ast::ParamMode::View)
    B.markView(local);
  B.store(local, Val(pir::Operand::value(arg), arg.Ty));
  // Ref-typed parameters arrive as owned +1 boxes: declared as owned, the
  // scope cleanup releases them.
  if (arg.Ty == Type::Ptr)
    CurrentScope->declareInout(name, local,
                               toPIRType(canonicalizeDeclType(astTy)),
                               canonicalizeDeclType(astTy));
  else if (astTy && ast::isRefType(astTy))
    CurrentScope->declare(name, local, astTy);
  else
    CurrentScope->declare(name, local, nullptr);
  return 1;
}

bool ModuleLowering::storeInout(const std::string &name, pir::LocalId local,
                                const Val &v) {
  Type pointee = CurrentScope->inoutType(name);
  if (pointee == Type::Void)
    return false;
  B.ptrStore(B.load(local, name + ".addr"), coerceTo(v, pointee));
  return true;
}

bool ModuleLowering::holdsBox(const std::string &name) const {
  return CurrentScope->isOwned(name) ||
         CurrentScope->inoutType(name) == Type::Box;
}

Val ModuleLowering::loadVarBox(const std::string &name) {
  Val slot = B.load(CurrentScope->lookup(name), name);
  if (CurrentScope->inoutType(name) == Type::Box)
    return B.ptrLoad(slot, Type::Box, name);
  return slot;
}

Val ModuleLowering::loadWeakVar(const std::string &name) {
  return B.weakLoad(B.load(CurrentScope->lookup(name), name + ".weak"), name);
}

void ModuleLowering::storeWeakVar(const std::string &name, const Val &box) {
  pir::LocalId local = CurrentScope->lookup(name);
  Val weak = B.weakNew(box, name + ".weak");
  if (!box.isNull())
    emitRelease(box); // the +1 handed over; the weak reference remains
  B.weakRelease(B.load(local, "old.weak"));
  B.store(local, weak);
}

bool ModuleLowering::isWeakFieldAccess(const ast::Expr *e) const {
  const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e);
  if (!ma)
    return false;
  const auto *ct = ast::dyn_cast<ast::ClassType>(
      ma->getReceiver()->getResolvedType()); // null-safe
  if (!ct) // a variable's narrowed type, or `self` (both read-only here)
    ct = getExprClassType(const_cast<ast::Expr *>(ma->getReceiver()));
  return ct && ct->isWeakField(ma->getFieldName());
}

void ModuleLowering::storeVarBox(const std::string &name, const Val &newBox) {
  pir::LocalId local = CurrentScope->lookup(name);
  if (CurrentScope->inoutType(name) == Type::Box) {
    Val addr = B.load(local, name + ".addr");
    emitRelease(B.ptrLoad(addr, Type::Box, "old.box"));
    B.ptrStore(addr, newBox);
    return;
  }
  emitRelease(B.load(local, "old.box"));
  B.store(local, newBox);
}

Val ModuleLowering::emitInoutArg(ast::Expr *arg, std::vector<Val> &keep,
                                 bool keepObject) {
  // A call's `inout` result: the address it returns (Sema allows no other
  // call here).
  if (ast::isa<ast::CallExpr>(arg) || ast::isa<ast::MethodCallExpr>(arg))
    return emitCallAddress(arg);
  if (auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(arg)) {
    ast::Expr *recvExpr = ma->getReceiver();
    // A struct's field: its address in the struct's bytes, which are a
    // place of their own, a variable's or a field's (Sema takes no
    // temporary's and no element's).  An object holding them is kept as for
    // a field of its own: an alias that replaces it (`g.h = ...` while
    // `o.h.p.s` is bound) cannot free them.
    if (auto *st = getExprStructType(recvExpr)) {
      int idx = st->fieldIndex(ma->getFieldName());
      assert(idx >= 0 && "Sema should have verified the field exists");
      Val base = emitInoutArg(recvExpr, keep, keepObject);
      if (!base)
        return Val();
      int64_t off = tupleElementOffset(st, static_cast<size_t>(idx));
      return off == 0 ? base : B.ptrAdd(base, Val::i64(off), "elem.addr");
    }
    ast::ClassType *ct = getExprClassType(recvExpr);
    if (!ct) {
      reportInternalError("an 'inout' field of a non-class receiver");
      return Val();
    }
    Val obj;
    if (ast::isa<ast::Identifier>(recvExpr) && !keepObject &&
        !isHeldRead(recvExpr)) {
      // A variable, or `self`: it holds the object for the whole call (the
      // callee cannot reassign the caller's variables), unless another part
      // of the statement may (a held read, noteHeldReads).
      obj = emitExpr(recvExpr);
    } else {
      // Anything else (a chain `a.b.f`, a call, an element) may lose its
      // object during the call: the caller keeps a reference of its own.
      Val box = emitAsShared(recvExpr);
      if (!box)
        return Val();
      keep.push_back(box);
      obj = emitSharedGet(box, "inout.obj");
    }
    if (!obj)
      return Val();
    getOrCreateClass(ct);
    return B.fieldAddr(obj, ct->getName(), ma->getFieldName(),
                       ma->getFieldName() + ".addr");
  }
  auto *id = ast::dyn_cast<ast::Identifier>(arg);
  if (!id || !CurrentScope->hasLocal(id->getName())) {
    reportInternalError("an 'inout' argument is not a variable or a field");
    return Val();
  }
  pir::LocalId local = CurrentScope->lookup(id->getName());
  if (CurrentScope->inoutType(id->getName()) != Type::Void)
    return B.load(local, id->getName() + ".addr"); // passed on
  return B.localAddr(local, id->getName() + ".addr");
}

Val ModuleLowering::emitInoutLocal(ast::VarDecl *node) {
  emitInoutBinding(node->getName(), node->getInitExpr(),
                   canonicalizeDeclType(node->getType()));
  return Val();
}

void ModuleLowering::holdObjects(const std::string &name, std::vector<Val> keep,
                                 size_t mark) {
  // The objects of a struct's field and of an `inout` result's storage
  // (noteBorrowedReads, takeInoutResult) come in MemTemps past @p mark and
  // are held the same way; a copy of a value held in memory there
  // (holdObjectsIn) moves into a hidden local of its own.
  for (size_t i = mark; i < MemTemps.size(); ++i) {
    const auto &[v, ty] = MemTemps[i];
    if (!ty) {
      keep.push_back(v);
      continue;
    }
    MemLayout l = memLayout(ty);
    pir::LocalId hold = B.addMemLocal(name + ".obj", l.Size, l.Align);
    B.memCopy(B.localAddr(hold, name + ".obj.addr"), v, Val::i64(l.Size));
    CurrentScope->declare(name + ".obj", hold, ty);
  }
  MemTemps.resize(mark);
  for (const Val &box : keep) {
    pir::LocalId hold = B.addLocal(name + ".obj", Type::Box);
    B.store(hold, box);
    CurrentScope->declare(name + ".obj", hold, ASTCtx.getObjTy());
  }
}

void ModuleLowering::emitInoutBinding(const std::string &name, ast::Expr *place,
                                      ast::Type *astTy) {
  std::vector<Val> keep;
  const size_t mark = MemTemps.size();
  Val addr = emitInoutArg(place, keep, /*keepObject=*/true);
  if (!addr)
    return;
  holdObjects(name, std::move(keep), mark);
  pir::LocalId local = B.addLocal(name, Type::Ptr);
  B.store(local, addr);
  // A value held in memory is reached through the address
  // (LoweringMem.cpp).
  ast::Type *placeTy = astTy ? astTy : place->getResolvedType();
  if (ast::isMemType(placeTy)) {
    CurrentScope->declareInout(name, local, Type::Mem, placeTy);
    return;
  }
  CurrentScope->declareInout(name, local, toPIRType(astTy), astTy);
}

Val ModuleLowering::emitPlaceAddress(ast::Expr *place) {
  if (auto *te = ast::dyn_cast<ast::TernaryExpr>(place)) {
    Val cond = emitExpr(te->getCondition());
    if (!cond)
      return Val();
    pir::LocalId addr = B.addLocal("res.addr", Type::Ptr);
    pir::If *s = B.openIf(cond, true);
    B.enter(*s->Then);
    if (Val a = emitPlaceAddress(te->getTrueExpr()))
      B.store(addr, a);
    B.leave();
    B.enter(*s->Else);
    if (Val a = emitPlaceAddress(te->getFalseExpr()))
      B.store(addr, a);
    B.leave();
    return B.load(addr, "res.addr");
  }
  // The objects on the way belong to `self` or to an `inout` parameter,
  // which the caller holds: nothing needs keeping past the address.
  std::vector<Val> keep;
  const size_t mark = MemTemps.size();
  Val addr = emitInoutArg(place, keep);
  releaseAfterCall(keep);
  releaseMemTemps(mark);
  return addr;
}

Val ModuleLowering::emitCallAddress(ast::Expr *call) {
  Emitter.WantAddress = true;
  Val addr = Emitter.visit(call);
  Emitter.WantAddress = false;
  if (addr && addr.Ty != Type::Ptr) {
    reportInternalError("an 'inout' place is a call without an 'inout' result");
    return Val();
  }
  return addr;
}

Val ModuleLowering::loadInoutResult(const Val &addr, ast::Type *astTy) {
  // A value held in memory is used by its address, as a variable's is.
  if (ast::isMemType(astTy))
    return addr;
  Type t = toPIRType(canonicalizeDeclType(astTy));
  Val v = B.ptrLoad(addr, t, "res");
  if (t == Type::Box)
    emitRetain(v);
  return v;
}

Val ModuleLowering::takeInoutResult(const Val &addr, ast::Expr *call,
                                    bool wantAddress,
                                    const std::vector<Val> &keep, size_t mark,
                                    const InoutPlaces &places,
                                    const ExprValue &recv) {
  // The storage is the receiver's, or an `inout` argument's, or in an
  // object such storage holds (`b.k` of `b: inout B`): what holds the object
  // it is in (a field reached through objects, a held receiver) and what
  // those places hold now stay held while the address is used.
  if (wantAddress || ast::isMemType(call->getResolvedType())) {
    for (const Val &box : keep)
      MemTemps.emplace_back(box, nullptr);
    if (recv.isOwned())
      MemTemps.emplace_back(recv.V, nullptr);
    for (const auto &[at, ty] : places)
      holdObjectsIn(at, ty);
    return addr;
  }
  Val v = loadInoutResult(addr, call->getResolvedType());
  releaseAfterCall(keep);
  releaseMemTemps(mark);
  releaseIfOwned(recv);
  return v;
}

void ModuleLowering::holdObjectsIn(const Val &addr, ast::Type *ty) {
  if (!reachesObject(ty))
    return;
  if (!ast::isMemType(ty)) {
    Val box = B.ptrLoad(addr, Type::Box, "held.obj");
    emitRetain(box);
    MemTemps.emplace_back(box, nullptr);
    return;
  }
  MemLayout l = memLayout(ty);
  pir::LocalId tmp = B.addMemLocal("held", l.Size, l.Align);
  Val copy = B.localAddr(tmp, "held.addr");
  B.memCopy(copy, addr, Val::i64(l.Size));
  emitMemRetain(copy, ty);
  MemTemps.emplace_back(copy, ty);
}

void ModuleLowering::releaseAfterCall(const std::vector<Val> &keep) {
  for (const Val &box : keep)
    emitRelease(box);
}

bool ModuleLowering::checkImportedSlot(ast::ClassType *ct, uint32_t slot,
                                       const pir::Signature &sig) {
  const std::string *origin = typeOrigin(ct->getName());
  auto mod = origin ? PL.ByName.find(*origin) : PL.ByName.end();
  const pir::Class *def =
      mod == PL.ByName.end() ? nullptr : mod->second->findClass(ct->getName());
  if (def && mod->second->defines(*def) && slot < def->VTable.size() &&
      def->VTable[slot].Sig == sig)
    return true;
  reportInternalError("a call of slot " + std::to_string(slot) + " of '" +
                      ct->getName() +
                      "' does not match the class's definition in its module");
  return false;
}

// static
bool ModuleLowering::hasInoutArm(const ast::MatchStmt *node) {
  return std::any_of(node->getArms().begin(), node->getArms().end(),
                     [](const ast::MatchArm *arm) {
                       return arm->hasBinding() &&
                              arm->getMode() == ast::ParamMode::Inout;
                     });
}

std::optional<pir::LocalId> ModuleLowering::emitMatchPlace(ast::Expr *subject) {
  std::vector<Val> keep;
  const size_t mark = MemTemps.size();
  Val addr = emitInoutArg(subject, keep, /*keepObject=*/true);
  if (!addr)
    return std::nullopt;
  // Released when the match ends, after the write-back, which needs the
  // field's object.
  holdObjects("match.subj", std::move(keep), mark);
  pir::LocalId place = B.addLocal("match.place", Type::Ptr);
  B.store(place, addr);
  return place;
}

bool ModuleLowering::emitMemMatchSubject(ast::MatchStmt *node, const Val &dst,
                                         std::optional<pir::LocalId> &place) {
  if (!hasInoutArm(node)) {
    emitMemInit(node->getSubject(), dst);
    return true;
  }
  place = emitMatchPlace(node->getSubject());
  if (!place)
    return false;
  ast::Type *ty = node->getSubject()->getResolvedType();
  B.memCopy(dst, B.load(*place, "subj.addr"), Val::i64(memLayout(ty).Size));
  emitMemRetain(dst, ty);
  return true;
}

void ModuleLowering::bindInoutArm(const ast::MatchArm *arm,
                                  pir::LocalId binding, pir::LocalId place,
                                  ast::Type *placeTy, ast::Type *bindTy,
                                  int64_t offset, const Val &header) {
  CurrentScope->WriteBacks.push_back(
      {arm, place, binding, placeTy, bindTy, offset, header});
}

void ModuleLowering::emitArmBody(const ast::MatchArm *arm) {
  const auto &stmts = arm->getBody()->getStatements();
  for (size_t i = 0; i <= stmts.size(); ++i) {
    // n's last use is done: the subject is the arm's to use again (Sema),
    // so n goes back now, not when the arm ends (a never-used n holds what
    // the subject does).
    std::vector<Scope::WriteBack> &wbs = CurrentScope->WriteBacks;
    if (i == arm->bindingUses() && !wbs.empty() && wbs.back().Arm == arm) {
      if (i > 0 && !B.isTerminated())
        emitWriteBack(wbs.back());
      wbs.pop_back();
    }
    if (i < stmts.size())
      visit(stmts[i]);
  }
}

void ModuleLowering::emitWriteBack(const Scope::WriteBack &wb) {
  Val place = B.load(wb.Place, "subj.addr");
  if (!ast::isMemType(wb.PlaceTy)) {
    // A reference: the binding's object replaces the one there (taken
    // before the old one goes, which may be the same).
    Val box = B.load(wb.Binding, "arm.box");
    emitRetain(box);
    emitRelease(B.ptrLoad(place, Type::Box, "old.box"));
    B.ptrStore(place, box);
    return;
  }
  // A value held in memory: the whole value is built around the binding,
  // as a variant or an optional is made, then assigned like any value.
  MemLayout l = memLayout(wb.PlaceTy);
  Val part = B.localAddr(wb.Binding, "arm.addr");
  Val value = part;
  if (wb.Header) {
    pir::LocalId whole = B.addMemLocal("arm.value", l.Size, l.Align);
    value = B.localAddr(whole, "arm.value.addr");
    B.memZero(value, Val::i64(l.Size));
    B.ptrStore(value, wb.Header);
    Val at = B.ptrAdd(value, Val::i64(wb.Offset), "arm.part.addr");
    B.memCopy(at, part, Val::i64(memLayout(wb.BindTy).Size));
    part = at;
  }
  emitMemRetain(part, wb.BindTy);
  emitMemRelease(place, wb.PlaceTy);
  B.memCopy(place, value, Val::i64(l.Size));
}

} // namespace paykan::lowering
