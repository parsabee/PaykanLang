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
  return mode == ast::ParamMode::Inout ? Type::Ptr
                                       : toPIRType(canonicalizeDeclType(ty));
}

void ModuleLowering::declareParam(const std::string &name,
                                  const pir::Value &arg, ast::Type *astTy) {
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
  if (auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(arg)) {
    ast::Expr *recvExpr = ma->getReceiver();
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
  CurrentScope->declareInout(name, local, toPIRType(astTy), astTy);
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
