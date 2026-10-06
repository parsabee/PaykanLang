// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// AST -> PIR lowering: classes.  Layouts and vtables become pir::Class
// items; methods, the synthetic destructor and the constructor become
// ordinary functions; member access/assignment carry their retain/release.

#include "LoweringInternal.h"
#include "Names.h"

#include <cassert>

namespace paykan::lowering {

using namespace names;
using pir::CmpPred;
using pir::Type;

std::vector<std::pair<std::string, ast::Type *>>
ModuleLowering::allFieldsInOrder(ast::ClassType *ct) const {
  if (!ct)
    return {};
  auto fields = allFieldsInOrder(ct->getSuperClass());
  for (auto &[name, ty] : ct->getFields())
    fields.emplace_back(name, ty);
  return fields;
}

ast::Type *ModuleLowering::fieldASTType(ast::ClassType *ct,
                                        const std::string &name) const {
  for (auto &[fname, fty] : allFieldsInOrder(ct))
    if (fname == name)
      return fty;
  return nullptr;
}

ast::ClassType *ModuleLowering::getExprClassType(ast::Expr *expr) const {
  if (auto *id = ast::dyn_cast<ast::Identifier>(expr)) {
    if (CurrentScope) {
      auto *astTy = CurrentScope->lookupASTType(id->getName());
      if (auto *ct = ast::dyn_cast<ast::ClassType>(astTy)) {
        if (auto *canonical = ASTCtx.lookupClassType(ct->getName()))
          return canonical;
        return ct;
      }
    }
    if (id->getName() == kSelf && CurrentMethodClassType)
      return CurrentMethodClassType;
  }
  if (auto *mae = ast::dyn_cast<ast::MemberAccessExpr>(expr))
    return ast::dyn_cast<ast::ClassType>(mae->getResolvedType());
  if (auto *mce = ast::dyn_cast<ast::MethodCallExpr>(expr))
    return ast::dyn_cast<ast::ClassType>(mce->getResolvedType());
  if (auto *ce = ast::dyn_cast<ast::CallExpr>(expr))
    return ast::dyn_cast<ast::ClassType>(ce->getResolvedType());
  if (auto *se = ast::dyn_cast<ast::SubscriptExpr>(expr))
    return ast::dyn_cast<ast::ClassType>(se->getResolvedType());
  if (auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(expr))
    return ast::dyn_cast<ast::ClassType>(ti->getResolvedType());
  return nullptr;
}

// -- Signatures

pir::Signature ModuleLowering::methodSignature(ast::MethodDecl *md) {
  // Receiver first.  Runtime methods borrow ref-typed arguments as raw
  // objects, except the virtual `equals`, whose `other` is a consumed box
  // (user classes may override it); user methods consume every ref-typed
  // argument as a box.
  pir::Signature sig;
  sig.Params.push_back(Type::Obj);
  for (auto *pty : md->getParamTypes()) {
    Type t = toPIRType(canonicalizeDeclType(pty));
    if (t == Type::Void)
      t = Type::Box;
    sig.Params.push_back(t);
  }
  sig.Ret = md->getReturnType()
                ? toPIRType(canonicalizeDeclType(md->getReturnType()))
                : Type::Void;
  // The virtual `equals` returns int64_t in the runtime (every `*_equals`
  // implementation); a user override is emitted with the same return type and
  // call sites convert the result back to bool.
  if (md->getName() == kMethodEquals && sig.Ret == Type::Bool)
    sig.Ret = Type::I64;
  return sig;
}

pir::Signature ModuleLowering::slotSignature(ast::ClassType *ct,
                                             ast::MethodDecl *md) {
  // Through a runtime class's vtable, ref-typed arguments are borrowed as
  // raw objects (except `equals`'s consumed box); user classes consume
  // boxes.  The slot of an inherited runtime method keeps the runtime
  // convention whichever class the vtable belongs to.
  pir::Signature sig = methodSignature(md);
  bool runtimeMethod = false;
  for (ast::ClassType *c = ct; c; c = c->getSuperClass())
    if (c->findMethod(md->getName()) == md) {
      runtimeMethod = c->isBuiltin();
      break;
    }
  if (runtimeMethod && md->getName() != kMethodEquals)
    for (size_t i = 1; i < sig.Params.size(); ++i)
      if (sig.Params[i] == Type::Box)
        sig.Params[i] = Type::Obj;
  return sig;
}

pir::Signature ModuleLowering::constructorSignature(ast::ClassType *ct) {
  pir::Signature sig;
  if (auto *initMd = ct->findMethod(kMethodInit))
    for (auto *pty : initMd->getParamTypes()) {
      Type t = toPIRType(canonicalizeDeclType(pty));
      sig.Params.push_back(t == Type::Void ? Type::Box : t);
    }
  sig.Ret = Type::Box;
  return sig;
}

// -- Runtime method symbols

const char *ModuleLowering::builtinMethodSymbol(ast::ClassType *ct,
                                                const std::string &name) {
  if (ct == ASTCtx.getObjTy()) {
    if (name == kMethodDestroy)
      return kPaykanObjectDestroy;
    if (name == kMethodToString)
      return kPaykanObjectToString;
    if (name == kMethodEquals)
      return kPaykanObjectEquals;
    return nullptr;
  }
  if (ct == ASTCtx.getStrTy()) {
    if (name == kMethodDestroy)
      return kPaykanStringDestroy;
    if (name == kMethodToString)
      return kPaykanStringToString;
    if (name == kMethodEquals)
      return kPaykanStringEquals;
    if (name == kMethodLength)
      return kPaykanStringLength;
    return nullptr;
  }
  if (ct == ASTCtx.getFileTy()) {
    if (name == kMethodDestroy)
      return kPaykanFileDestroy;
    if (name == kMethodToString)
      return kPaykanFileToString;
    if (name == kMethodEquals)
      return kPaykanFileEquals;
    if (name == kMethodWrite)
      return kPaykanFileWrite;
    if (name == kMethodReadln)
      return kPaykanFileReadln;
    return nullptr;
  }
  return nullptr;
}

std::string
ModuleLowering::findConcreteMethodFuncName(ast::ClassType *ct,
                                           const std::string &name) {
  if (!ct)
    return "";
  if (ct->isBuiltin()) {
    if (const char *sym = builtinMethodSymbol(ct, name))
      return declareRuntime(sym).Name;
    return "";
  }
  std::string mangled = ct->getName() + kNameSep + name;
  if (lookupClassFunction(ct, mangled))
    return mangled;
  return findConcreteMethodFuncName(ct->getSuperClass(), name);
}

pir::Function *
ModuleLowering::lookupOwnMethodFunction(ast::ClassType *ct,
                                        const std::string &methodName) {
  return lookupClassFunction(ct, ct->getName() + kNameSep + methodName);
}

// -- Class items

pir::Class *ModuleLowering::getOrCreateClass(ast::ClassType *ct) {
  if (auto it = ClassByName.find(ct->getName()); it != ClassByName.end())
    return it->second;
  // A user superclass (local or imported) is declared first, so the item's
  // `: Super` names a class of this module; the runtime root `Obj` is not a
  // PIR class, so a class deriving from it is a root class.
  ast::ClassType *super = ct->getSuperClass();
  if (super && !super->isBuiltin())
    getOrCreateClass(super);
  Classes_.emplace_back();
  pir::Class &cls = Classes_.back();
  cls.Name = ct->getName();
  cls.Super = (super && !super->isBuiltin()) ? super->getName() : "";
  for (auto &[name, ty] : allFieldsInOrder(ct)) {
    Type t = toPIRType(ty);
    cls.Fields.push_back({name, t == Type::Void ? Type::Box : t});
  }
  auto origin = PL.ClassOrigins.find(ct->getName());
  if (origin != PL.ClassOrigins.end() && origin->second != Mod.Name) {
    cls.IsExtern = true;
    cls.Module = origin->second;
  }
  ClassByName[cls.Name] = &cls;
  return &cls;
}

Val ModuleLowering::emitIsExactType(const Val &rawObjPtr, ast::ClassType *ct) {
  Val loaded = B.vtableLoad(rawObjPtr, "vtable." + ct->getName());
  Val expected;
  const char *runtimeVT = nullptr;
  if (auto *elemTy = ASTCtx.getSpecializedArrayElemType(ct)) {
    // Specialized array type: the runtime vtable the array was created with.
    runtimeVT = isObjectElementType(elemTy) ? kPaykanArrayObjVtable
                                            : kPaykanArrayVtable;
  } else if (ct == ASTCtx.getObjTy()) {
    runtimeVT = kPaykanObjectVtable;
  } else if (ct == ASTCtx.getStrTy()) {
    runtimeVT = kPaykanStringVtable;
  } else if (ct == ASTCtx.getFileTy()) {
    runtimeVT = kPaykanFileVtable;
  } else if (ct == ASTCtx.getErrorTy()) {
    runtimeVT = kPaykanErrorVtable;
  } else if (ct == ASTCtx.getIntBoxTy()) {
    runtimeVT = kPaykanIntVtable;
  } else if (ct == ASTCtx.getFloatBoxTy()) {
    runtimeVT = kPaykanFloatVtable;
  } else if (ct == ASTCtx.getBoolBoxTy()) {
    runtimeVT = kPaykanBoolVtable;
  } else if (ct == ASTCtx.getCharBoxTy()) {
    runtimeVT = kPaykanCharVtable;
  } else if (ct == ASTCtx.getTupleTy() ||
             ASTCtx.getSpecializedTupleElemType(ct)) {
    runtimeVT = kPaykanTupleVtable;
  }
  if (runtimeVT) {
    // Runtime class: its vtable global is the type identity.
    expected = B.vtableAddrExtern(externVTable(runtimeVT),
                                  "vtable.expected." + ct->getName());
  } else {
    getOrCreateClass(ct);
    expected = B.vtableAddr(ct->getName(), "vtable.expected." + ct->getName());
  }
  return B.cmp(CmpPred::Eq, loaded, expected, "is." + ct->getName());
}

void ModuleLowering::declareClass(ast::ClassDecl *node) {
  auto *ct = ASTCtx.lookupClassType(node->getName());
  assert(ct && "ClassType must have been registered by Sema");
  pir::Class *cls = getOrCreateClass(ct);
  if (!cls->VTable.empty() || cls->IsExtern)
    return;
  // Vtable shape (targets are filled in by lowerClassDecl).
  for (auto *md : ct->getVTable())
    cls->VTable.push_back({md->getName(), "", slotSignature(ct, md)});

  // Method functions `Class.method(obj self, params...)`; `destroy` is the
  // synthetic destructor declared below.
  for (auto *funcDecl : node->getMethods()) {
    if (funcDecl->getName() == kMethodDestroy)
      continue;
    if (auto *md = ct->findMethod(funcDecl->getName())) {
      pir::Function *fn =
          getOrCreateFunction(node->getName() + kNameSep + funcDecl->getName(),
                              methodSignature(md));
      fn->Params[0].Name = kSelf;
      for (size_t i = 0; i < funcDecl->getParams().size(); ++i)
        fn->Params[i + 1].Name = funcDecl->getParams()[i].getName();
    }
  }
  pir::Function *dtor = getOrCreateFunction(
      node->getName() + kNameSep + kMethodDestroy, {{Type::Obj}, Type::Void});
  dtor->Params[0].Name = kSelf;
  // Constructor `Class(initParams...) -> box`.
  pir::Function *ctor =
      getOrCreateFunction(node->getName(), constructorSignature(ct));
  for (auto *m : node->getMethods())
    if (m->getName() == kMethodInit)
      for (size_t i = 0; i < m->getParams().size() && i < ctor->Params.size();
           ++i)
        ctor->Params[i].Name = m->getParams()[i].getName();
}

Val ModuleLowering::lowerClassDecl(ast::ClassDecl *node) {
  auto *ct = ASTCtx.lookupClassType(node->getName());
  assert(ct && "ClassType must have been registered by Sema");
  declareClass(node);
  pir::Class *cls = getOrCreateClass(ct);

  // 1. Method bodies.
  for (auto *funcDecl : node->getMethods()) {
    if (funcDecl->getName() == kMethodDestroy)
      continue;
    auto *md = ct->findMethod(funcDecl->getName());
    if (!md)
      continue;
    pir::Function *fn =
        FuncByName.at(node->getName() + kNameSep + funcDecl->getName());
    FunctionStateGuard fnState(*this, fn, md->getReturnType(), ct);
    {
      ScopeGuard guard(*this);
      // `self` is a raw object pointer, not ref-counted by the body.
      pir::LocalId selfLocal = B.addLocal(kSelf, Type::Obj);
      B.store(selfLocal, Val(pir::Operand::value(fn->Params[0]), Type::Obj));
      CurrentScope->declareUnowned(kSelf, selfLocal, ct);
      for (size_t i = 0; i < funcDecl->getParams().size(); ++i) {
        auto &p = funcDecl->getParams()[i];
        const pir::Value &arg = fn->Params[i + 1];
        auto *pty = md->getParamTypes()[i];
        pir::LocalId local = B.addLocal(p.getName(), arg.Ty);
        B.store(local, Val(pir::Operand::value(arg), arg.Ty));
        if (ast::isRefType(pty))
          CurrentScope->declare(p.getName(), local, pty);
        else
          CurrentScope->declare(p.getName(), local, nullptr);
      }
      emitBody(funcDecl->getBody());
    }
    emitImplicitReturn(fn->Sig);
  }

  // 2. The synthetic destructor (so the destroy slot names it).
  emitDestructor(node, ct);

  // 3. Vtable targets: the most-derived concrete function per slot.
  const auto &vtable = ct->getVTable();
  for (size_t i = 0; i < vtable.size(); ++i)
    cls->VTable[i].Target =
        findConcreteMethodFuncName(ct, vtable[i]->getName());

  // 4. The constructor: allocate, box BEFORE __init__ (so `self` used as a
  //    value inside __init__ recovers the caller's box), run __init__,
  //    return the box.
  auto *initMd = ct->findMethod(kMethodInit);
  pir::Function *ctorFn = FuncByName.at(node->getName());
  FunctionStateGuard fnState(*this, ctorFn, nullptr, nullptr);
  Val rawPtr = B.newObject(node->getName(), "obj");
  Val shared = emitSharedNew(rawPtr, node->getName() + ".shared");
  if (initMd) {
    pir::Function *initFn = lookupOwnMethodFunction(ct, kMethodInit);
    assert(initFn && "__init__ must have been declared with the class");
    std::vector<Val> initArgs = {rawPtr};
    for (auto &arg : ctorFn->Params)
      initArgs.push_back(Val(pir::Operand::value(arg), arg.Ty));
    B.call(initFn->Name, initFn->Sig, initArgs);
  }
  B.emitRet(shared);
  return Val();
}

// `void Class.destroy(obj self)`: the user `destroy` body (if any), then a
// release of every ref-typed field, then the struct is freed.
void ModuleLowering::emitDestructor(ast::ClassDecl *node, ast::ClassType *ct) {
  pir::Function *fn =
      FuncByName.at(node->getName() + kNameSep + kMethodDestroy);
  ast::FuncDecl *userDestroy = nullptr;
  for (auto *m : node->getMethods())
    if (m->getName() == kMethodDestroy) {
      userDestroy = m;
      break;
    }

  FunctionStateGuard fnState(*this, fn, nullptr, ct);
  Val self(pir::Operand::value(fn->Params[0]), Type::Obj);

  if (userDestroy && userDestroy->getBody()) {
    // The user body runs inside its own scope with `self` bound.
    ScopeGuard guard(*this);
    pir::LocalId selfLocal = B.addLocal(kSelf, Type::Obj);
    B.store(selfLocal, self);
    CurrentScope->declareUnowned(kSelf, selfLocal, ct);
    emitBody(userDestroy->getBody());
  }
  // A top-level `return` in the user body (its scope cleanup already
  // emitted before it) falls through to the field teardown instead of
  // leaving the destructor, exactly as the former block redirect did.
  // Returns nested in deeper blocks keep their meaning as plain exits.
  {
    auto &stmts = fn->Body.Stmts;
    if (!stmts.empty() && std::holds_alternative<pir::Return>(stmts.back()))
      stmts.pop_back();
  }

  // Release every ref-typed field (null-safe), then free the struct.
  for (auto &[fname, fty] : allFieldsInOrder(ct)) {
    if (!ast::isRefType(fty))
      continue;
    Val box = B.fieldLoad(self, ct->getName(), fname, Type::Box, fname);
    emitRelease(box);
  }
  B.freeObject(self);
  B.emitRetVoid();
}

// -- Member assignment / access

Val ModuleLowering::lowerMemberAssignStmt(ast::MemberAssignStmt *node) {
  Receiver recv = emitReceiver(node->getReceiver(), "obj");
  if (!recv)
    return Val();
  const Val &objPtr = recv.Raw;

  ast::ClassType *ct = getExprClassType(node->getReceiver());
  if (!ct)
    return Val();
  getOrCreateClass(ct);
  ast::Type *fieldASTTy = fieldASTType(ct, node->getFieldName());
  assert(fieldASTTy && "Sema should have verified field exists");

  if (fieldASTTy && ast::isRefType(fieldASTTy)) {
    // The slot owns a box: retain/steal the new one, release the old one.
    Val newShared;
    if (isNoneForOptional(node->getValue()))
      newShared = Val::null(Type::Box);
    if (auto *rhsId = ast::dyn_cast<ast::Identifier>(node->getValue())) {
      if (CurrentScope && CurrentScope->isOwned(rhsId->getName())) {
        newShared =
            B.load(CurrentScope->lookup(rhsId->getName()), rhsId->getName());
        emitRetain(newShared);
      }
    }
    if (!newShared) {
      Val rhs = emitExpr(node->getValue());
      if (!rhs)
        return Val();
      newShared = exprAlreadyShared(node->getValue())
                      ? takeSharedOwnership(node->getValue(), rhs)
                      : emitSharedNew(rhs, "field.shared");
    }
    newShared = emitOptionalToObj(node->getValue(), newShared);

    // Release the old value only if it is non-null.
    Val old = B.fieldLoad(objPtr, ct->getName(), node->getFieldName(),
                          Type::Box, "old.field");
    Val isNull = B.cmp(CmpPred::Eq, old, Val::null(Type::Box), "is.null");
    pir::If *s = B.openIf(isNull, true);
    B.enter(*s->Else);
    emitRelease(old);
    B.leave();
    B.fieldStore(objPtr, ct->getName(), node->getFieldName(), newShared);
  } else {
    Val rhs = emitExpr(node->getValue());
    if (!rhs)
      return Val();
    B.fieldStore(objPtr, ct->getName(), node->getFieldName(),
                 coerceTo(rhs, toPIRType(fieldASTTy)));
  }
  releaseIfOwned(recv.Owned);
  return Val();
}

Val ModuleLowering::lowerMemberAccessExpr(ast::MemberAccessExpr *node) {
  // A call-rooted receiver (`makeH().a`) is a fresh +1 box torn down after
  // the read; a ref-typed field is retained first so it survives.
  Receiver recv = emitReceiver(node->getReceiver(), "obj");
  if (!recv)
    return Val();
  const Val &objPtr = recv.Raw;

  ast::ClassType *ct = getExprClassType(node->getReceiver());
  if (!ct)
    return Val();
  getOrCreateClass(ct);
  ast::Type *fieldASTTy = fieldASTType(ct, node->getFieldName());
  assert(fieldASTTy && "Sema should have verified field exists");
  Type fieldTy = toPIRType(fieldASTTy);
  if (fieldTy == Type::Void)
    fieldTy = Type::Box;

  Val fieldVal = B.fieldLoad(objPtr, ct->getName(), node->getFieldName(),
                             fieldTy, node->getFieldName());
  if (recv.Owned.isOwned()) {
    if (ast::isRefType(fieldASTTy))
      emitRetain(fieldVal);
    releaseIfOwned(recv.Owned);
  }
  return fieldVal;
}

} // namespace paykan::lowering
