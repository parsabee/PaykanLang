// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// AST -> PIR lowering: the `inout` value parameters of the ownership
// prototype (docs/design/ownership-proto.md).
//
// An `inout` parameter (int, float, bool, char, an enum, or an optional) is a
// PIR `ptr`: the address of the caller's storage.  The callee keeps it in a
// ptr-typed local and reads and writes the caller's value through it
// (ptr.load / ptr.store).  An optional's storage holds a box (null for
// None), which the slot owns: a store releases the old box and keeps the
// new one's reference.  A caller passes a variable's `local.addr`, or, from
// inside another such parameter, the address it was given.  A field or an
// array element has no PIR address: it is copied into a temporary local
// (retained, for a box) whose address is passed, and the temporary's final
// value is stored back after the call.  Sema requires a changeable
// variable, field or element of exactly the parameter's type.

#include "LoweringInternal.h"
#include "Names.h"

namespace paykan::lowering {

using namespace names;
using pir::Type;

Type ModuleLowering::paramType(ast::Type *ty, ast::Qualifier q) {
  ast::Type *canon = canonicalizeDeclType(ty);
  Type t = toPIRType(canon);
  bool value = t == Type::I64 || t == Type::F64 || t == Type::Bool ||
               t == Type::Char || ast::isa<ast::OptionalType>(canon);
  return q == ast::Qualifier::Inout && value ? Type::Ptr : t;
}

void ModuleLowering::emitInoutStore(pir::LocalId local, const std::string &name,
                                    const Val &v) {
  Type t = toPIRType(CurrentScope->lookupASTType(name));
  B.ptrStore(B.load(local, name + ".addr"), coerceTo(v, t));
}

Val ModuleLowering::loadVarBox(const std::string &name) {
  Val v = B.load(CurrentScope->lookup(name), name);
  return v.Ty == Type::Ptr ? B.ptrLoad(v, Type::Box, name) : v;
}

void ModuleLowering::storeVarBox(pir::LocalId local, const Val &box) {
  if (B.localType(local) != Type::Ptr) {
    emitRelease(B.load(local, "old.box"));
    B.store(local, box);
    return;
  }
  Val addr = B.load(local, "addr");
  emitRelease(B.ptrLoad(addr, Type::Box, "old.box"));
  B.ptrStore(addr, box);
}

Val ModuleLowering::emitInoutArg(ast::Expr *arg, WriteBacks &after) {
  if (auto *id = ast::dyn_cast<ast::Identifier>(arg);
      id && CurrentScope->hasLocal(id->getName())) {
    pir::LocalId local = CurrentScope->lookup(id->getName());
    if (B.localType(local) == Type::Ptr) // passed on: the same storage
      return B.load(local, id->getName() + ".addr");
    return B.localAddr(local, id->getName() + ".addr");
  }

  Type t = toPIRType(canonicalizeDeclType(arg->getResolvedType()));
  bool box = t == Type::Box; // an optional: the temporary owns a reference
  pir::LocalId tmp = B.addLocal("inout", t);
  auto *member = ast::dyn_cast<ast::MemberAccessExpr>(arg);
  auto *elem = ast::dyn_cast<ast::SubscriptExpr>(arg);
  if (member) {
    // `obj.f`: the receiver is evaluated once, for the read and the store.
    Receiver r = emitReceiver(member->getReceiver(), "obj");
    ast::ClassType *ct = getExprClassType(member->getReceiver());
    if (!r || !ct)
      return Val();
    getOrCreateClass(ct);
    std::string cls = ct->getName();
    const std::string &field = member->getFieldName();
    Val v = B.fieldLoad(r.Raw, cls, field, t, field);
    if (box)
      emitRetain(v);
    B.store(tmp, v);
    after.push_back([this, r, cls, field, tmp, box] {
      if (box) // the field's reference goes; the temporary's takes its place
        emitRelease(B.fieldLoad(r.Raw, cls, field, Type::Box, "old.field"));
      B.fieldStore(r.Raw, cls, field, B.load(tmp, "inout"));
      releaseIfOwned(r.Owned);
    });
  } else if (elem &&
             ast::isa<ast::ArrayType>(elem->getArray()->getResolvedType())) {
    // `xs[i]`: the array and the index are evaluated once.
    Receiver r = emitReceiver(elem->getArray());
    if (!r)
      return Val();
    Val idx = emitExpr(elem->getIndex());
    if (!idx)
      return Val();
    idx = coerceBoolToI64(idx, Type::I64);
    ast::Type *elemTy = arg->getResolvedType();
    Val raw = callRuntime(kPaykanArrayGet, {r.Raw, idx}, "elem.raw");
    Val v =
        box ? B.cast(raw, Type::Box, "elem.shared") : fromSlotBits(raw, elemTy);
    if (box)
      emitRetain(v);
    B.store(tmp, v);
    after.push_back([this, r, idx, tmp, box] {
      Val v = B.load(tmp, "inout");
      if (box) { // set_obj retains what it stores: drop the temporary's
        callRuntime(kPaykanArraySetObj, {r.Raw, idx, v});
        emitRelease(v);
      } else {
        callRuntime(kPaykanArraySet, {r.Raw, idx, toSlotBits(v)});
      }
      releaseIfOwned(r.Owned);
    });
  } else {
    // Not a place (a caller Sema did not check, e.g. through a `.pkm`
    // interface): the callee changes a temporary.
    Val v = box ? emitAsShared(arg) : emitExpr(arg);
    if (!v)
      return Val();
    B.store(tmp, coerceTo(v, t));
    if (box)
      after.push_back([this, tmp] { emitRelease(B.load(tmp, "inout")); });
  }
  return B.localAddr(tmp, "inout.addr");
}

} // namespace paykan::lowering
