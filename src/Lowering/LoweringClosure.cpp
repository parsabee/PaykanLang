// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// AST -> PIR lowering: function values (docs/language/15-function-types.md,
// docs/design/function-types.md).
//
// A function value is a closure: an object of a class the lowering makes
// per lambda (`fn.N`), or per named function used as a value (`fn.ref.f`),
// holding one field per capture.  Its vtable is an object's -- destroy,
// toString and equals, the last two the runtime's defaults -- followed by
// `call`, whose signature is the function type's with the closure object
// first, as a method's.  So a call through a function value is a `vcall`
// of slot 3 with that signature and no class name (a call site knows only
// the type), a closure crosses modules as any object does, and the runtime
// destroys one through slot 0 as it does any object.
//
// A lambda expression allocates its class, stores a copy of each captured
// variable (a reference retained, a value held in memory copied with its
// references retained) and boxes the object: a fresh owned box, like a
// constructor's.  Its `call` reads the captures back into locals of their
// names, owned like by-value parameters, then runs the body as a function's.

#include "LoweringInternal.h"
#include "Names.h"

#include <cassert>

namespace paykan::lowering {

using namespace names;
using pir::Type;

namespace {

/// The name of the capture field @p i.
std::string captureField(size_t i) { return "c" + std::to_string(i); }

} // namespace

pir::Signature ModuleLowering::closureSignature(ast::FunctionType *fnt) {
  pir::Signature sig;
  sig.Params.push_back(Type::Obj);
  for (size_t i = 0; i < fnt->getNumParams(); ++i)
    appendParamTypes(sig.Params, fnt->getParamType(i), fnt->getMode(i));
  ast::Type *ret = fnt->getReturnType();
  sig.Ret = toPIRType(ret);
  // A result held in memory is written where a last parameter points.
  if (ast::isMemType(ret)) {
    sig.Params.push_back(Type::Ptr);
    sig.Ret = Type::Void;
  }
  return sig;
}

pir::Class *ModuleLowering::declareClosureClass(
    const std::string &name, const std::vector<ast::Type *> &captureTys,
    const std::vector<bool> &captureWeak, const pir::Signature &callSig) {
  Classes_.emplace_back();
  pir::Class &cls = Classes_.back();
  cls.Name = name;
  for (size_t i = 0; i < captureTys.size(); ++i) {
    Type t = captureWeak[i] ? Type::Weak : toPIRType(captureTys[i]);
    pir::Field f{captureField(i), t == Type::Void ? Type::Box : t};
    if (t == Type::Mem) {
      MemLayout l = memLayout(captureTys[i]);
      f.Size = l.Size;
      f.Align = l.Align;
    }
    cls.Fields.push_back(f);
  }
  ClassByName[name] = &cls;
  PL.ClassOrigins.try_emplace(name, Mod.Name);
  // The vtable: an object's three slots, then `call`.
  pir::Function *dtor = getOrCreateFunction(name + kNameSep + kMethodDestroy,
                                            {{Type::Obj}, Type::Void});
  dtor->Params[0].Name = kSelf;
  pir::Function *call =
      getOrCreateFunction(name + kNameSep + kClosureCall, callSig);
  call->Params[0].Name = kSelf;
  const pir::Function &toString = declareRuntime(kPaykanObjectToString);
  const pir::Function &equals = declareRuntime(kPaykanObjectEquals);
  cls.VTable = {{kMethodDestroy, dtor->Name, dtor->Sig},
                {kMethodToString, toString.Name, toString.Sig},
                {kMethodEquals, equals.Name, equals.Sig},
                {kClosureCall, call->Name, call->Sig}};
  return &cls;
}

void ModuleLowering::emitClosureDestructor(
    const std::string &name, const std::vector<ast::Type *> &captureTys,
    const std::vector<bool> &captureWeak) {
  pir::Function *fn = FuncByName.at(name + kNameSep + kMethodDestroy);
  FunctionStateGuard fnState(*this, fn, nullptr, nullptr);
  Val self(pir::Operand::value(fn->Params[0]), Type::Obj);
  for (size_t i = 0; i < captureTys.size(); ++i) {
    ast::Type *ty = captureTys[i];
    const std::string field = captureField(i);
    if (ast::isMemType(ty)) {
      if (memHoldsRefs(ty))
        emitMemRelease(B.fieldAddr(self, name, field, field + ".addr"), ty);
      continue;
    }
    if (!ast::isRefType(ty))
      continue;
    if (captureWeak[i]) {
      B.weakRelease(B.fieldLoad(self, name, field, Type::Weak, field));
      continue;
    }
    emitRelease(B.fieldLoad(self, name, field, Type::Box, field));
  }
  B.emitRetVoid();
}

const std::string &ModuleLowering::lowerLambda(ast::LambdaExpr *node) {
  if (auto it = LambdaClasses.find(node); it != LambdaClasses.end())
    return it->second;
  // Class names are global across the program's modules: the module's name
  // is part of it.
  const std::string name = std::string(kClosureClassPrefix) + Mod.Name +
                           kNameSep + std::to_string(NextClosureId++);
  auto *fnt = ast::cast<ast::FunctionType>(node->getResolvedType());
  std::vector<ast::Type *> captureTys;
  std::vector<bool> captureWeak;
  for (const auto &cap : node->getCaptures()) {
    captureTys.push_back(cap.Ty);
    captureWeak.push_back(cap.Weak);
  }
  declareClosureClass(name, captureTys, captureWeak, closureSignature(fnt));
  pir::Function *call = FuncByName.at(name + kNameSep + kClosureCall);
  nameParams(*call, 1, node->getParams());
  if (ast::isMemType(fnt->getReturnType()))
    call->Params.back().Name = "result";

  // The body: the captures as owned locals of their names (a copy of the
  // closure's copy, so the closure may be released during the call), then
  // the parameters, as a function's.  It sees no enclosing locals.
  {
    ast::Type *retTy = fnt->getReturnType() == ASTCtx.getVoidTy()
                           ? nullptr
                           : fnt->getReturnType();
    FunctionStateGuard fnState(*this, call, retTy, nullptr);
    Scope *savedScope = std::exchange(CurrentScope, nullptr);
    auto savedTemps = std::exchange(MemTemps, {});
    {
      ScopeGuard guard(*this);
      Val self(pir::Operand::value(call->Params[0]), Type::Obj);
      for (size_t i = 0; i < captureTys.size(); ++i) {
        ast::Type *ty = captureTys[i];
        const std::string &cname = node->getCaptures()[i].getName();
        const std::string field = captureField(i);
        if (ast::isMemType(ty)) {
          MemLayout l = memLayout(ty);
          pir::LocalId local = B.addMemLocal(cname, l.Size, l.Align);
          Val own = B.localAddr(local, cname + ".addr");
          B.memCopy(own, B.fieldAddr(self, name, field, field + ".addr"),
                    Val::i64(l.Size));
          emitMemRetain(own, ty);
          CurrentScope->declare(cname, local, ty);
        } else if (captureWeak[i]) {
          // A captured `weak` variable: the object, held strongly for the
          // call's duration (None once it is gone), as the variable reads.
          Val box = B.weakLoad(
              B.fieldLoad(self, name, field, Type::Weak, cname + ".weak"),
              cname);
          pir::LocalId local = B.addLocal(cname, Type::Box);
          B.store(local, box);
          CurrentScope->declare(cname, local, ty);
        } else if (ast::isRefType(ty)) {
          Val box = B.fieldLoad(self, name, field, Type::Box, cname);
          emitRetain(box);
          pir::LocalId local = B.addLocal(cname, Type::Box);
          B.store(local, box);
          CurrentScope->declare(cname, local, ty);
        } else {
          Type t = toPIRType(ty);
          Val v = B.fieldLoad(self, name, field, t, cname);
          pir::LocalId local = B.addLocal(cname, t);
          B.store(local, v);
          CurrentScope->declare(cname, local, nullptr);
        }
      }
      size_t at = 1;
      for (const ast::Param &p : node->getParams())
        at += declareParam(p.getName(), call->Params, at,
                           canonicalizeDeclType(p.ParamType), p.Mode);
      emitBody(node->getBody());
    }
    emitImplicitReturn(call->Sig);
    MemTemps = std::move(savedTemps);
    CurrentScope = savedScope;
  }
  emitClosureDestructor(name, captureTys, captureWeak);
  return LambdaClasses[node] = name;
}

Val ModuleLowering::emitLambdaValue(ast::LambdaExpr *node) {
  const std::string &name = lowerLambda(node);
  Val obj = B.newObject(name, "closure");
  // Each capture: the variable as an expression, copied into its field.
  for (size_t i = 0; i < node->getCaptures().size(); ++i) {
    const auto &cap = node->getCaptures()[i];
    const std::string field = captureField(i);
    auto *id = ASTCtx.make<ast::Identifier>(node->getLocation(),
                                            ASTCtx.intern(cap.getName()));
    id->setResolvedType(cap.Ty);
    if (ast::isMemType(cap.Ty)) {
      Val src = emitMemAddress(id);
      if (!src)
        return Val();
      Val dst = B.fieldAddr(obj, name, field, field + ".addr");
      B.memCopy(dst, src, Val::i64(memLayout(cap.Ty).Size));
      emitMemRetain(dst, cap.Ty);
    } else if (cap.Weak) {
      // A `weak` variable: the closure holds its object weakly too.
      Val box = emitAsShared(id); // +1, dropped once the weak one is taken
      if (!box)
        return Val();
      B.fieldStore(obj, name, field, B.weakNew(box, field + ".weak"));
      emitRelease(box);
    } else if (ast::isRefType(cap.Ty)) {
      Val box = emitAsShared(id); // +1, the field's own
      if (!box)
        return Val();
      B.fieldStore(obj, name, field, box);
    } else {
      Val v = emitExpr(id);
      if (!v)
        return Val();
      B.fieldStore(obj, name, field, coerceTo(v, toPIRType(cap.Ty)));
    }
  }
  return emitSharedNew(obj, "closure.shared");
}

const std::string &ModuleLowering::lowerFunctionRef(const std::string &fnName,
                                                    ast::FunctionType *fnt) {
  if (auto it = FunctionRefClasses.find(fnName); it != FunctionRefClasses.end())
    return it->second;
  const std::string name =
      std::string(kClosureRefPrefix) + Mod.Name + kNameSep + fnName;
  pir::Function *target = lookupFunction(fnName);
  if (!target) {
    reportInternalError("function '" + fnName +
                        "' used as a value is not declared");
    return FunctionRefClasses[fnName] = name;
  }
  declareClosureClass(name, {}, {}, closureSignature(fnt));
  pir::Function *call = FuncByName.at(name + kNameSep + kClosureCall);
  // `call` forwards its parameters: a box given to it is the callee's.
  {
    FunctionStateGuard fnState(*this, call, nullptr, nullptr);
    std::vector<Val> args;
    for (size_t i = 1; i < call->Params.size(); ++i)
      args.push_back(
          Val(pir::Operand::value(call->Params[i]), call->Params[i].Ty));
    Val result = B.call(target->Name, target->Sig, args, "call");
    if (call->Sig.Ret == Type::Void)
      B.emitRetVoid();
    else
      B.emitRet(result);
  }
  emitClosureDestructor(name, {}, {});
  return FunctionRefClasses[fnName] = name;
}

Val ModuleLowering::emitFunctionRefValue(const std::string &fnName,
                                         ast::FunctionType *fnt) {
  const std::string &name = lowerFunctionRef(fnName, fnt);
  Val obj = B.newObject(name, "closure");
  return emitSharedNew(obj, "closure.shared");
}

Val ModuleLowering::emitClosureCall(const Val &closure, ast::FunctionType *fnt,
                                    const std::vector<ast::Expr *> &argExprs,
                                    ast::Type *resultTy, bool wantAddress) {
  const pir::Signature sig = closureSignature(fnt);
  std::vector<Val> args;
  std::vector<Val> keep;
  size_t fixedMark = MemTemps.size();
  for (ast::Expr *arg : argExprs) {
    if (isSliceArg(arg)) {
      if (!emitSliceArg(arg, args))
        return Val();
      continue;
    }
    // The parameter after the closure object.
    Type paramTy = args.size() + 1 < sig.Params.size()
                       ? sig.Params[args.size() + 1]
                       : Type::Void;
    if (ast::isMemType(arg->getResolvedType())) {
      Val p = emitMemAddress(arg); // passed by address
      if (!p)
        return Val();
      args.push_back(p);
      continue;
    }
    if (paramTy == Type::Ptr) { // `inout`: the argument's address
      Val p = emitInoutArg(arg, keep);
      if (!p)
        return Val();
      args.push_back(p);
      continue;
    }
    if (paramTy == Type::Box) {
      // Callee-consumes ABI: a +1 box for every expression form.
      Val v = emitAsShared(arg);
      if (!v)
        return Val();
      args.push_back(v);
      continue;
    }
    Val v = emitExpr(arg);
    if (!v)
      return Val();
    args.push_back(coerceTo(v, paramTy));
  }
  if (ast::isMemType(resultTy)) {
    Val slot = emitMemResultSlot(resultTy);
    args.push_back(slot);
    B.vcall(closure, "", kClosureCallSlot, sig, args);
    releaseAfterCall(keep);
    releaseMemTemps(fixedMark);
    return slot;
  }
  Val result = B.vcall(closure, "", kClosureCallSlot, sig, args, "call");
  releaseAfterCall(keep);
  releaseMemTemps(fixedMark);
  (void)wantAddress; // a function type's result is never an `inout`
  return result;
}

} // namespace paykan::lowering
