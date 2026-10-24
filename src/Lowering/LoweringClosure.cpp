// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// AST -> PIR lowering: function values (docs/language/14-function-types.md,
// docs/design/function-types.md).
//
// A function value is a value held in place, laid out as the tuple
// `(int, Obj?)` (functionValueShape): the address of its code, kept as an
// i64, then the box of its closure, the object holding its captures, or
// null when it captures nothing.  So it is copied, retained, released,
// compared and described to the runtime as that tuple is, and an optional
// one is the same 16 bytes, None a zero code word.
//
// The code of a lambda is a function `fn.<module>.N.call(closure, params...)`
// and its closure an object of the class `fn.<module>.N`, one field per
// capture, whose vtable is an object's (its destroy releases the captures).
// A named function used as a value is the code `fn.ref.<module>.<f>.call`,
// which forwards to it, and no closure.  A call through a function value
// loads the code and the closure (kept for the call), then calls the code
// through its address (icall), the closure object first.
//
// A lambda expression writes its code and a new closure, a copy of each
// captured variable stored in it (a reference retained, a value held in
// memory copied with its references retained).  Its code reads the captures
// back into locals of their names, owned like by-value parameters, then
// runs the body as a function's.

#include "LoweringInternal.h"
#include "Names.h"

#include <cassert>

namespace paykan::lowering {

using namespace names;
using pir::CmpPred;
using pir::Type;

namespace {

/// The name of the capture field @p i.
std::string captureField(size_t i) { return "c" + std::to_string(i); }

/// The offsets of a function value's two words (functionValueShape).
constexpr int64_t kCodeOffset = 0;
constexpr int64_t kClosureOffset = 8;

} // namespace

ast::TupleType *ModuleLowering::functionValueShape() {
  return ASTCtx.getTupleType(
      {ASTCtx.getIntTy(), ASTCtx.getOptionalType(ASTCtx.getObjTy())});
}

ast::Type *ModuleLowering::memShapeOf(ast::Type *ty) {
  if (ast::isFunctionValue(ty))
    return functionValueShape();
  // An optional tuple or struct: its presence flag, then the value.
  if (ast::isMemOptional(ty))
    return ASTCtx.getTupleType(
        {ASTCtx.getBoolTy(), ast::cast<ast::OptionalType>(ty)->getInnerType()});
  // A tagged union: its tag, then each variant's fields.
  if (ast::isPayloadEnum(ty))
    return enumCarrier(ast::cast<ast::EnumType>(ty));
  return ty;
}

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

pir::Class *
ModuleLowering::declareClosureClass(const std::string &name,
                                    const std::vector<ast::Type *> &captureTys,
                                    const std::vector<bool> &captureWeak) {
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
  // An object's vtable: its own destroy, the runtime's toString and equals.
  pir::Function *dtor = getOrCreateFunction(name + kNameSep + kMethodDestroy,
                                            {{Type::Obj}, Type::Void});
  dtor->Params[0].Name = kSelf;
  const pir::Function &toString = declareRuntime(kPaykanObjectToString);
  const pir::Function &equals = declareRuntime(kPaykanObjectEquals);
  cls.VTable = {{kMethodDestroy, dtor->Name, dtor->Sig},
                {kMethodToString, toString.Name, toString.Sig},
                {kMethodEquals, equals.Name, equals.Sig}};
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
  if (auto it = LambdaCode.find(node); it != LambdaCode.end())
    return it->second;
  // Class and function names are global across the program's modules: the
  // module's name is part of them.
  const std::string name = std::string(kClosureClassPrefix) + Mod.Name +
                           kNameSep + std::to_string(NextClosureId++);
  auto *fnt = ast::cast<ast::FunctionType>(node->getResolvedType());
  std::vector<ast::Type *> captureTys;
  std::vector<bool> captureWeak;
  for (const auto &cap : node->getCaptures()) {
    captureTys.push_back(cap.Ty);
    captureWeak.push_back(cap.Weak);
  }
  const bool hasClosure = !captureTys.empty();
  if (hasClosure)
    declareClosureClass(name, captureTys, captureWeak);
  pir::Function *call = getOrCreateFunction(name + kNameSep + kClosureCall,
                                            closureSignature(fnt));
  call->Params[0].Name = hasClosure ? kSelf : "closure";
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
  if (hasClosure)
    emitClosureDestructor(name, captureTys, captureWeak);
  return LambdaCode[node] = name;
}

namespace {

/// Store the address of the function @p code as the code word at @p dst.
void storeCode(pir::Builder &B, const Val &dst, const std::string &code) {
  Val fn = B.fnAddr(code, "fn.code");
  B.ptrStore(dst, B.cast(fn, Type::I64, "fn.code.word"));
}

} // namespace

void ModuleLowering::emitLambdaInto(ast::LambdaExpr *node, const Val &dst) {
  const std::string &name = lowerLambda(node);
  storeCode(B, dst, name + kNameSep + kClosureCall);
  Val closureAddr = B.ptrAdd(dst, Val::i64(kClosureOffset), "fn.closure.addr");
  if (node->getCaptures().empty()) {
    B.ptrStore(closureAddr, Val::null(Type::Box));
    return;
  }
  Val obj = B.newObject(name, "closure");
  // Each capture: the variable as an expression, copied into its field.
  for (size_t i = 0; i < node->getCaptures().size(); ++i) {
    const auto &cap = node->getCaptures()[i];
    const std::string field = captureField(i);
    auto *id = ASTCtx.make<ast::Identifier>(node->getLocation(),
                                            ASTCtx.intern(cap.getName()));
    id->setResolvedType(cap.Ty);
    if (ast::isMemType(cap.Ty)) {
      size_t mark = MemTemps.size();
      Val src = emitMemAddress(id);
      if (!src)
        return;
      Val at = B.fieldAddr(obj, name, field, field + ".addr");
      B.memCopy(at, src, Val::i64(memLayout(cap.Ty).Size));
      emitMemRetain(at, cap.Ty);
      releaseMemTemps(mark);
    } else if (cap.Weak) {
      // A `weak` variable: the closure holds its object weakly too.
      Val box = emitAsShared(id); // +1, dropped once the weak one is taken
      if (!box)
        return;
      B.fieldStore(obj, name, field, B.weakNew(box, field + ".weak"));
      emitRelease(box);
    } else if (ast::isRefType(cap.Ty)) {
      Val box = emitAsShared(id); // +1, the field's own
      if (!box)
        return;
      B.fieldStore(obj, name, field, box);
    } else {
      Val v = emitExpr(id);
      if (!v)
        return;
      B.fieldStore(obj, name, field, coerceTo(v, toPIRType(cap.Ty)));
    }
  }
  B.ptrStore(closureAddr, emitSharedNew(obj, "closure.shared"));
}

const std::string &ModuleLowering::lowerFunctionRef(const std::string &fnName,
                                                    ast::FunctionType *fnt) {
  pir::Function *target = lookupFunction(fnName);
  if (!target) {
    reportInternalError("function '" + fnName +
                        "' used as a value is not declared");
    return FunctionRefCode[fnName] = std::string(kClosureRefPrefix) + Mod.Name +
                                     kNameSep + fnName + kNameSep +
                                     kClosureCall;
  }
  // One code per function in this module, however the function is spelled
  // (`square`, `lib::square`): two values of it compare equal.
  const std::string &key = target->Name;
  if (auto it = FunctionRefCode.find(key); it != FunctionRefCode.end())
    return it->second;
  const std::string name = std::string(kClosureRefPrefix) + Mod.Name +
                           kNameSep + key + kNameSep + kClosureCall;
  pir::Function *call = getOrCreateFunction(name, closureSignature(fnt));
  call->Params[0].Name = "closure";
  // The code forwards its parameters: a box given to it is the callee's.
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
  return FunctionRefCode[key] = name;
}

void ModuleLowering::emitFunctionRefInto(const std::string &fnName,
                                         ast::FunctionType *fnt,
                                         const Val &dst) {
  storeCode(B, dst, lowerFunctionRef(fnName, fnt));
  B.ptrStore(B.ptrAdd(dst, Val::i64(kClosureOffset), "fn.closure.addr"),
             Val::null(Type::Box));
}

Val ModuleLowering::emitFunctionValueIsNone(const Val &addr) {
  Val code = B.ptrLoad(addr, Type::I64, "fn.code.word");
  return B.cmp(CmpPred::Eq, code, Val::i64(0), "fn.isnone");
}

int64_t ModuleLowering::memOptionalPayloadOffset(ast::Type *optTy) {
  if (ast::isFunctionValue(optTy))
    return 0; // the value itself; None has no code
  return tupleElementOffset(ast::cast<ast::TupleType>(memShapeOf(optTy)), 1);
}

Val ModuleLowering::emitMemOptionalIsNone(const Val &addr, ast::Type *optTy) {
  if (ast::isFunctionValue(optTy))
    return emitFunctionValueIsNone(addr);
  Val present = B.ptrLoad(addr, Type::Bool, "opt.has");
  return B.unary(pir::Opcode::Not, present, "opt.isnone");
}

Val ModuleLowering::emitClosureCall(const Val &addr, ast::FunctionType *fnt,
                                    const std::vector<ast::Expr *> &argExprs,
                                    ast::Type *resultTy) {
  const pir::Signature sig = closureSignature(fnt);
  // The code and the closure, read before the arguments: an argument may
  // rebind the variable or the field that holds the function value
  // (`f(swap(f))`, `o.cb(reset(o))`).  The call keeps its own reference to
  // the closure, which would otherwise be freed before, or while, it runs.
  Val code =
      B.cast(B.ptrLoad(B.ptrAdd(addr, Val::i64(kCodeOffset), "fn.code.addr"),
                       Type::I64, "fn.code.word"),
             Type::Ptr, "fn.code");
  Val box =
      B.ptrLoad(B.ptrAdd(addr, Val::i64(kClosureOffset), "fn.closure.addr"),
                Type::Box, "fn.closure");
  emitRetain(box);
  std::vector<Val> args{emitSharedGet(box, "closure.obj")};
  std::vector<Val> keep;
  size_t fixedMark = MemTemps.size();
  for (ast::Expr *arg : argExprs) {
    if (isSliceArg(arg)) {
      if (!emitSliceArg(arg, args))
        return Val();
      continue;
    }
    Type paramTy =
        args.size() < sig.Params.size() ? sig.Params[args.size()] : Type::Void;
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
  Val result;
  if (ast::isMemType(resultTy)) {
    result = emitMemResultSlot(resultTy);
    args.push_back(result);
    B.icall(code, sig, args);
  } else {
    result = B.icall(code, sig, args, "call");
  }
  releaseAfterCall(keep);
  releaseMemTemps(fixedMark);
  emitRelease(box);
  return result;
}

Val ModuleLowering::emitMemOptionalMatch(ast::MatchStmt *node) {
  ast::Type *subjTy = node->getSubject()->getResolvedType();
  auto *optTy = ast::dyn_cast<ast::OptionalType>(subjTy);
  ast::Type *inner = optTy ? optTy->getInnerType() : subjTy;
  // The subject, copied into a slot of the match's scope: its references are
  // held until the match ends, on every way out of it.  With a `p: inout T`
  // arm it is read from its storage, where p goes back (emitMatchPlace).
  ScopeGuard matchGuard(*this);
  MemLayout l = memLayout(subjTy);
  pir::LocalId subj = B.addMemLocal("match.subj", l.Size, l.Align);
  Val addr = B.localAddr(subj, "match.subj.addr");
  std::optional<pir::LocalId> place;
  if (!emitMemMatchSubject(node, addr, place))
    return Val();
  CurrentScope->declare("match.subj", subj, subjTy);

  ast::MatchArm *noneArm = nullptr;
  ast::MatchArm *valueArm = nullptr;
  for (ast::MatchArm *arm : node->getArms())
    if (arm->isLiteral())
      noneArm = arm;
    else if (!arm->isWildcard() && !valueArm)
      valueArm = arm;
  ast::MatchArm *wildcard = findWildcardArm(node);
  auto emitArmOrDefault = [&](ast::MatchArm *arm) {
    if (arm)
      emitMatchArmBody(arm);
    else if (wildcard)
      emitMatchArmBody(wildcard);
  };

  Val isNone =
      optTy ? emitMemOptionalIsNone(addr, subjTy) : Val::boolean(false);
  pir::If *s = B.openIf(isNone, /*withElse=*/true);
  B.enter(*s->Then);
  emitArmOrDefault(noneArm);
  B.leave();
  B.enter(*s->Else);
  if (valueArm && valueArm->hasBinding()) {
    ScopeGuard armGuard(*this);
    const std::string &name = valueArm->getBinding();
    // The binding's own copy of the present value; `p: inout T` writes it
    // back after its last use (bindInoutArm): the whole of a function
    // value's (None has no code), the value after a presence flag of a
    // tuple's or a struct's, and the whole optional for an arm binding a
    // `T` that is that optional (ast::MatchArm::getBindType).
    ast::Type *bindTy =
        ast::isa<ast::OptionalType>(valueArm->getBindType()) ? subjTy : inner;
    MemLayout bl = memLayout(bindTy);
    pir::LocalId local = B.addMemLocal(name, bl.Size, bl.Align);
    Val own = B.localAddr(local, name + ".addr");
    int64_t off =
        optTy && bindTy == inner ? memOptionalPayloadOffset(subjTy) : 0;
    B.memCopy(own, off ? B.ptrAdd(addr, Val::i64(off), "opt.value.addr") : addr,
              Val::i64(bl.Size));
    emitMemRetain(own, bindTy);
    CurrentScope->declare(name, local, bindTy);
    if (valueArm->getMode() == ast::ParamMode::Inout && place)
      bindInoutArm(valueArm, local, *place, subjTy, bindTy, off,
                   off ? Val::boolean(true) : Val());
    emitArmBody(valueArm);
  } else {
    emitArmOrDefault(valueArm);
  }
  B.leave();
  return Val();
}

} // namespace paykan::lowering
