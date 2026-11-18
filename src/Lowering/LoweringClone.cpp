// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// AST -> PIR lowering: the builtin `Cloneable` (#369), the explicit copy of a
// move-only value.
//
// `x.clone()` of a value with no `clone` of its own is the copy of a
// Copyable value, a new `Str` (its `toString`, which copies), and for a
// struct, a tuple, a fixed-size array or an optional, a copy of its bytes
// whose unique references are each replaced by their own clone: a `shared`
// reference or a handle is the same object in the clone (the copy retained
// it).  The `clone` an empty `impl Cloneable for C {}` derives builds a new
// object of C from the fields of `self` the same way, field by field, with
// a `weak` field a new weak reference to the same object and an `atomic`
// field a new cell holding the value the original's has now.  A unique
// object reached through a field is cloned by its own `clone`, through its
// vtable: each class's makes an object of that class.

#include "LoweringInternal.h"
#include "Names.h"

namespace paykan::lowering {

using namespace names;
using pir::Type;

Val ModuleLowering::emitCloneObject(const Val &obj, ast::Type *refTy) {
  ast::Type *inner = ast::stripOptional(refTy);
  // A `Str` copies through its `toString` (PaykanString_toString).
  if (inner == ASTCtx.getStrTy()) {
    const pir::Function &toString = declareRuntime(kPaykanObjectToString);
    const int slot = ASTCtx.getObjTy()->getVTableIndex(kMethodToString);
    return B.vcall(obj, "", static_cast<uint32_t>(slot), toString.Sig, {},
                   "clone");
  }
  auto *ct = ast::cast<ast::ClassType>(inner);
  ast::MethodDecl *md = ct->findMethod(kMethodClone);
  const int slot = ct->getVTableIndex(kMethodClone);
  if (!md || slot < 0) {
    reportInternalError("a clone of '" + ct->getName() +
                        "', which has no 'clone'");
    return Val();
  }
  pir::Signature sig = slotSignature(ct, md);
  std::string className;
  if (!getOrCreateClass(ct)->IsExtern)
    className = ct->getName();
  else if (!checkImportedSlot(ct, static_cast<uint32_t>(slot), sig))
    return Val();
  return B.vcall(obj, className, static_cast<uint32_t>(slot), sig, {}, "clone");
}

Val ModuleLowering::emitCloneBox(const Val &box, ast::Type *refTy) {
  if (!ast::isa<ast::OptionalType>(refTy))
    return emitCloneObject(B.unbox(box, "clone.obj"), refTy);
  // An optional reference: None, or its object's clone.
  pir::LocalId result = B.addLocal("clone", Type::Box);
  pir::If *s =
      B.openIf(B.cmp(pir::CmpPred::Eq, box, Val::null(Type::Box), "isnone"),
               /*withElse=*/true);
  B.enter(*s->Then);
  B.store(result, Val::null(Type::Box));
  B.leave();
  B.enter(*s->Else);
  B.store(result, emitCloneObject(B.unbox(box, "clone.obj"), refTy));
  B.leave();
  return B.load(result, "clone");
}

void ModuleLowering::cloneRefSlot(const Val &slot, ast::Type *refTy) {
  // The slot holds a reference the copy retained: its clone replaces it.
  Val old = B.ptrLoad(slot, Type::Box, "orig");
  B.ptrStore(slot, emitCloneBox(old, refTy));
  emitRelease(old);
}

void ModuleLowering::cloneMemParts(const Val &addr, ast::Type *ty) {
  if (!addr)
    return;
  // A struct that writes its `clone` (one with a part that has none, such as
  // mem::Buffer's object): that clone replaces the copy, which lets go of
  // the references it retained.  Before the test below: a written clone
  // runs even for a struct that holds no references.
  if (auto *st = ast::dyn_cast<ast::StructType>(ty);
      st && st->findMethod(kMethodClone)) {
    pir::Function *fn = lookupTypeFunction(
        st->getName(), st->getName() + kNameSep + kMethodClone);
    if (!fn) {
      reportInternalError("no function for the 'clone' of struct '" +
                          st->getName() + "'");
      return;
    }
    Val slot = emitMemResultSlot(st);
    std::vector<Val> args{addr};
    if (fn->Sig.Ret != Type::Ptr)
      args.push_back(slot);
    B.call(fn->Name, fn->Sig, args, "clone");
    emitMemRelease(addr, st);
    B.memCopy(addr, slot, Val::i64(memLayout(st).Size));
    return;
  }
  // A function value is Copyable: its clone is the copy, which shares its
  // closure.
  if (!memHoldsRefs(ty) || ast::isFunctionValue(ty))
    return;
  // A part at @p at of type @p part, held with qualifier @p q.
  auto clonePart = [&](const Val &at, ast::Type *part, ast::Qualifier q) {
    // A cell's handle is Copyable: the clone shares the cell, already
    // retained with the bytes.
    if (ast::isa<ast::AtomicType>(part))
      return;
    if (ast::isCountedRef(ast::stripOptional(part))) {
      if (q.Form == ast::QualForm::Unique)
        cloneRefSlot(at, part);
      return;
    }
    if (ast::isMemType(part))
      cloneMemParts(at, part);
  };
  // An optional held in memory: its value's parts, when present.
  if (ast::isMemOptional(ty)) {
    pir::If *s = B.openIf(emitMemOptionalIsNone(addr, ty), /*withElse=*/true);
    B.enter(*s->Then);
    B.leave();
    B.enter(*s->Else);
    cloneMemParts(B.ptrAdd(addr, Val::i64(memOptionalPayloadOffset(ty)),
                           "opt.value.addr"),
                  ast::stripOptional(ty));
    B.leave();
    return;
  }
  // A struct's fields carry qualifiers; before the tuple case, which a
  // struct is too.
  if (auto *st = ast::dyn_cast<ast::StructType>(ty)) {
    for (size_t i = 0; i < st->getNumFields(); ++i) {
      ast::Type *part = st->getElementType(i);
      if (!ast::isCountedRef(ast::stripOptional(part)) && !memHoldsRefs(part))
        continue;
      const int64_t off = tupleElementOffset(st, i);
      clonePart(off == 0 ? addr : B.ptrAdd(addr, Val::i64(off), "field.addr"),
                part, st->findFieldQualifier(st->getFieldName(i)));
    }
    return;
  }
  if (auto *tt = ast::dyn_cast<ast::TupleType>(ty)) {
    for (size_t i = 0; i < tt->getArity(); ++i) {
      ast::Type *part = tt->getElementType(i);
      if (!ast::isCountedRef(ast::stripOptional(part)) && !memHoldsRefs(part))
        continue;
      const int64_t off = tupleElementOffset(tt, i);
      clonePart(off == 0 ? addr : B.ptrAdd(addr, Val::i64(off), "elem.addr"),
                part, tt->getElemQual(i));
    }
    return;
  }
  if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty)) {
    ast::Type *elem = ft->getElementType();
    const ast::Qualifier q = ft->getElemQual();
    forEachSlot(addr, 0, ft->getLength(), memLayout(elem).Size,
                [&](const Val &at) { clonePart(at, elem, q); });
    return;
  }
  // A tagged union is Cloneable only when Copyable: every reference in it
  // is held in a Copyable form, which the copy retained.
  if (ast::isPayloadEnum(ty))
    return;
  reportInternalError("a clone of a '" + ast::typeName(ty) +
                      "' held in memory");
}

Val ModuleLowering::emitMemClone(const Val &src, ast::Type *ty,
                                 const std::string &name) {
  Val copy = copyToTemporary(src, ty, name); // its references retained
  cloneMemParts(copy, ty);
  return copy;
}

void ModuleLowering::emitDerivedClone(pir::Function *fn, ast::ClassType *ct) {
  FunctionStateGuard fnState(*this, fn, ct, ct);
  Val self(pir::Operand::value(fn->Params[0]), Type::Obj);
  const std::string &cls = ct->getName();
  Val obj = B.newObject(cls, "clone.obj");
  Val shared = emitSharedNew(obj, "clone");
  for (auto &[fname, fty] : allFieldsInOrder(ct)) {
    const ast::Qualifier q = ct->findFieldQualifier(fname);
    // A value held in memory: its bytes, its references retained, then its
    // unique ones cloned.
    if (ast::isMemType(fty)) {
      MemLayout l = memLayout(fty);
      Val to = B.fieldAddr(obj, cls, fname, fname + ".to");
      B.memCopy(to, B.fieldAddr(self, cls, fname, fname + ".from"),
                Val::i64(l.Size));
      emitMemRetain(to, fty);
      // (A struct with a `__deinit__` in it is no value yet: its `clone`,
      // which cloneMemParts calls, makes the one the clone holds.)
      emitMemMovedOut(to, fty);
      cloneMemParts(to, fty);
      continue;
    }
    // A `weak` field: a new weak reference to the same object.
    if (ct->isWeakField(fname)) {
      Val strong = B.weakLoad(
          B.fieldLoad(self, cls, fname, Type::Weak, fname + ".weak"), fname);
      B.fieldStore(obj, cls, fname, B.weakNew(strong, fname + ".weak"));
      emitRelease(strong);
      continue;
    }
    // An `atomic` field: a cell of its own, holding the value it has now.
    if (auto *at = ast::dyn_cast<ast::AtomicType>(fty)) {
      Val cell = B.fieldLoad(self, cls, fname, Type::Box, fname);
      if (ast::isa<ast::FixedArrayType>(at->getInnerType())) {
        B.fieldStore(obj, cls, fname,
                     callRuntime(names::kPaykanAtomicArrayClone,
                                 {emitSharedGet(cell, fname + ".obj")},
                                 fname + ".cell"));
        continue;
      }
      Val v = emitAtomicLoad(at->getInnerType(), cell, fname + ".value");
      B.fieldStore(obj, cls, fname,
                   emitAtomicNew(at->getInnerType(), v, fname + ".cell"));
      continue;
    }
    if (ast::isCountedRef(ast::stripOptional(fty))) {
      Val box = B.fieldLoad(self, cls, fname, Type::Box, fname);
      if (q.Form == ast::QualForm::Unique) {
        B.fieldStore(obj, cls, fname, emitCloneBox(box, fty));
      } else {
        // `shared`, `atomic shared`, `locked`: the same object.
        emitRetain(box, /*lend=*/false, fty);
        B.fieldStore(obj, cls, fname, box);
      }
      continue;
    }
    // A number, a bool, a char, a function value held in place.
    Type pt = toPIRType(fty);
    B.fieldStore(obj, cls, fname, B.fieldLoad(self, cls, fname, pt, fname));
  }
  B.emitRet(shared);
}

bool ModuleLowering::isBuiltinClone(const ast::MethodCallExpr *node) const {
  if (node->getMethodName() != kMethodClone || node->getNumArguments() != 0)
    return false;
  if (node->cloneIsCopy()) // a `shared` reference's: the copy
    return true;
  ast::Type *ty = node->getReceiver()->getResolvedType();
  if (!ty)
    return false;
  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty))
    return ct == ASTCtx.getStrTy();
  if (auto *st = ast::dyn_cast<ast::StructType>(ty))
    return st->findMethod(kMethodClone) == nullptr;
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    return !ast::isa<ast::ClassType>(ot->getInnerType()) ||
           ot->getInnerType() == ASTCtx.getStrTy() ||
           ast::cast<ast::ClassType>(ot->getInnerType())
               ->findMethod(kMethodClone);
  return true;
}

Val ModuleLowering::ExprEmitter::emitBuiltinClone(ast::MethodCallExpr *node) {
  ast::Expr *recv = node->getReceiver();
  ast::Type *ty = recv->getResolvedType();
  // A reference held `shared`, `atomic shared` or `locked` (#368): the
  // same object, a reference of the call's own.
  if (node->cloneIsCopy())
    return L.emitAsShared(recv);
  // Held in memory: a copy of its bytes, its unique references cloned, in
  // the temporary that is the call's result.
  if (ast::isMemType(ty)) {
    size_t mark = L.MemTemps.size();
    Val addr = L.emitMemAddress(recv);
    if (!addr)
      return Val();
    Val copy = L.emitMemClone(addr, L.memTypeOf(recv), "clone");
    L.releaseMemTemps(mark);
    return copy;
  }
  // A reference (a `Str`, or an optional object): its object's clone.
  if (ast::isCountedRef(ast::stripOptional(ty))) {
    Receiver receiver = L.emitReceiver(recv, "clone.recv");
    if (!receiver)
      return Val();
    Val result;
    if (ast::isa<ast::OptionalType>(ty)) {
      pir::LocalId out = L.B.addLocal("clone", Type::Box);
      pir::If *s = L.B.openIf(L.B.cmp(pir::CmpPred::Eq, receiver.Raw,
                                      Val::null(Type::Obj), "isnone"),
                              /*withElse=*/true);
      L.B.enter(*s->Then);
      L.B.store(out, Val::null(Type::Box));
      L.B.leave();
      L.B.enter(*s->Else);
      L.B.store(out, L.emitCloneObject(receiver.Raw, ty));
      L.B.leave();
      result = L.B.load(out, "clone");
    } else {
      result = L.emitCloneObject(receiver.Raw, ty);
    }
    L.releaseIfOwned(receiver.Owned);
    return result;
  }
  // A Copyable value: the clone is the copy.
  return visit(recv);
}

} // namespace paykan::lowering
