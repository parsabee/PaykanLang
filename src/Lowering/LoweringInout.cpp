// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// AST -> PIR lowering: `inout` parameters and locals
// (docs/language/02-functions-and-calling.md, "Parameter modes";
// docs/language/01-language-basics.md, "Local borrows").
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
// alive for as long as the local can be used.  A `match` arm's `n: inout T`
// is one for the subject (docs/language/07-match-statements.md, "Borrowing
// the subject").
//
// For a reference type the storage holds a box, and the `inout` is a mutable
// reference to it, not a share of it: it retains nothing and releases
// nothing at its end.  Reading it borrows the box, as reading a variable
// does; assigning it replaces the box like an assignment to the variable
// (release the old, store the new).  A `view` parameter is passed by value
// like any other.

#include "LoweringInternal.h"

namespace paykan::lowering {

using pir::Type;

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
    // A struct's field: its address in the struct's bytes, which a
    // variable's, a field's or an element's are (Sema takes no temporary's);
    // like a value held in memory passed by address, it outlives the call.
    if (auto *st = getExprStructType(recvExpr)) {
      int idx = st->fieldIndex(ma->getFieldName());
      assert(idx >= 0 && "Sema should have verified the field exists");
      return emitTupleElementAddress(recvExpr, static_cast<size_t>(idx));
    }
    ast::ClassType *ct = getExprClassType(recvExpr);
    if (!ct) {
      reportInternalError("an 'inout' field of a non-class receiver");
      return Val();
    }
    Val obj;
    if (ast::isa<ast::Identifier>(recvExpr) && !keepObject) {
      // A variable, or `self`: it holds the object for the whole call (the
      // callee cannot reassign the caller's variables).
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

void ModuleLowering::emitInoutBinding(const std::string &name, ast::Expr *place,
                                      ast::Type *astTy) {
  std::vector<Val> keep;
  Val addr = emitInoutArg(place, keep, /*keepObject=*/true);
  if (!addr)
    return;
  // A field's object stays alive as long as the borrow can be used: a hidden
  // owned local holds it, released with the scope.
  for (const Val &box : keep) {
    pir::LocalId hold = B.addLocal(name + ".obj", Type::Box);
    B.store(hold, box);
    CurrentScope->declare(name + ".obj", hold, ASTCtx.getObjTy());
  }
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
  Val addr = emitInoutArg(place, keep);
  releaseAfterCall(keep);
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

void ModuleLowering::releaseAfterCall(const std::vector<Val> &keep) {
  for (const Val &box : keep)
    emitRelease(box);
}

bool ModuleLowering::checkImportedSlot(ast::ClassType *ct, uint32_t slot,
                                       const pir::Signature &sig) {
  auto origin = PL.ClassOrigins.find(ct->getName());
  auto mod = origin == PL.ClassOrigins.end() ? PL.ByName.end()
                                             : PL.ByName.find(origin->second);
  const pir::Class *def =
      mod == PL.ByName.end() ? nullptr : mod->second->findClass(ct->getName());
  if (def && !def->IsExtern && slot < def->VTable.size() &&
      def->VTable[slot].Sig == sig)
    return true;
  reportInternalError("a call of slot " + std::to_string(slot) + " of '" +
                      ct->getName() +
                      "' does not match the class's definition in its module");
  return false;
}

} // namespace paykan::lowering
