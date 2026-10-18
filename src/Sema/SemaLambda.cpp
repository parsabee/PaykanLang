// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Semantic analysis: function values (docs/language/15-function-types.md).
// A lambda `fn(x: int) -> int { ... }` is checked as a function body is,
// in a scope of its own whose parent is the scope it is written in: a name
// used inside that is bound outside is captured, by copy, and declared in
// the lambda's scope as a read-only Captured variable (so a nested lambda
// captures the copy).  A named function used as a value has the function
// type of its signature.  A call through a function value checks its
// arguments against the function type's parameters, modes included.

#include "Names.h"
#include "Sema.h"
#include "SemaInternal.h"

namespace paykan {
namespace sema {

ast::FunctionType *Sema::functionTypeOf(const FunctionSig &sig) {
  std::vector<ast::ParamMode> modes;
  modes.reserve(sig.ParamTypes.size());
  for (size_t i = 0; i < sig.ParamTypes.size(); ++i)
    modes.push_back(sig.Modes.mode(i));
  return Ctx.getFunctionType(sig.ParamTypes, std::move(modes),
                             sig.ReturnType ? sig.ReturnType : Ctx.getVoidTy());
}

bool Sema::checkFunctionValue(const std::string &name, const FunctionSig &sig,
                              ast::SourceLocation loc) {
  const std::string shown = displayName(name);
  if (Ctx.lookupClassType(name) || Ctx.lookupStructType(name)) {
    error(loc, "'" + shown +
                   "' is a constructor, not a function value (wrap it: "
                   "'fn(...) -> " +
                   shown + " { return " + shown + "(...); }')");
    return false;
  }
  // An import's entry is marked builtin too (it is not this module's to
  // re-export); a compiler builtin is never qualified.
  if (sig.IsBuiltin && name.find(names::kQualSep) == std::string::npos) {
    error(loc, "'" + shown +
                   "' is a builtin and cannot be a function value (wrap it "
                   "in a lambda)");
    return false;
  }
  if (sig.Modes.Result != ast::ParamMode::Value) {
    error(loc, "'" + shown + "' returns a borrow ('-> " +
                   ast::paramModeName(sig.Modes.Result) +
                   "'); it cannot be a function value yet");
    return false;
  }
  for (ast::Type *p : sig.ParamTypes)
    if (!p) {
      ++SuppressedFollowOns; // its declaration was reported
      return false;
    }
  return true;
}

bool Sema::captureVariable(const std::string &name, ast::Type *ty, Scope *owner,
                           ast::SourceLocation loc) {
  if (LambdaStack.empty() || !owner)
    return true;
  // Whether a scope is a boundary or one inside it.
  auto within = [](const Scope *s, const Scope *boundary) {
    for (; s; s = s->Parent)
      if (s == boundary)
        return true;
    return false;
  };
  // Outermost first: an inner lambda captures the outer lambda's copy.
  for (LambdaContext &lc : LambdaStack) {
    if (within(owner, lc.Boundary))
      continue;
    auto k = owner->Kinds.find(name);
    const VarKind kind = k == owner->Kinds.end() ? VarKind::Plain : k->second;
    std::string why;
    switch (kind) {
    case VarKind::View:
      why = "'" + name + "' is a 'view' parameter";
      break;
    case VarKind::Inout:
      why = "'" + name + "' is an 'inout' parameter or local";
      break;
    case VarKind::ViewLocal:
      why = "'" + name + "' is a 'view' local";
      break;
    case VarKind::ViewBinding:
      why = "'" + name + "' is bound to a 'view'";
      break;
    case VarKind::Plain:
    case VarKind::Let:
    case VarKind::Captured:
    case VarKind::Weak:
      break;
    }
    if (!why.empty()) {
      error(loc, why + "; a function cannot capture a borrow (it may "
                       "outlive the call that holds it)");
      return false;
    }
    if (name == names::kSelf) {
      const ClassContext *outer = lc.Outer;
      if (outer && outer->Struct) {
        error(loc, "'self' is the receiver's storage in a method of struct '" +
                       outer->Struct->getName() +
                       "'; a function cannot capture it (copy it into a "
                       "variable first)");
        return false;
      }
      if (outer && outer->ViewMethod) {
        error(loc, "'self' is read-only in 'view fn " + outer->MethodName +
                       "'; a function cannot capture it (its copy could "
                       "change the object)");
        return false;
      }
      lc.Node->setCapturesSelf(true);
      if (lc.OuterUse)
        lc.OuterUse->ChangesSelf = true; // the closure may change it later
    }
    // A `weak` variable is captured as a weak reference: the closure holds
    // the object weakly too, and a call reads it as the variable reads.
    lc.Node->addCapture(Ctx.intern(name), ty, kind == VarKind::Weak);
    lc.Boundary->declare(name, ty);
    lc.Boundary->Kinds[name] = VarKind::Captured;
    owner = lc.Boundary;
  }
  return true;
}

ast::Type *Sema::checkLambda(ast::LambdaExpr *node) {
  // The signature, as a function's: canonical types written back.
  std::vector<ast::Type *> paramTys;
  std::vector<ast::ParamMode> modes;
  bool ok = true;
  for (ast::Param &p : node->getMutableParams()) {
    FixedArrayAllowed = true;
    SliceAllowed = p.Mode != ast::ParamMode::Value;
    auto *pty = resolveType(p.ParamType, node->getLocation(),
                            "parameter '" + p.getName() + "'");
    if (pty == Ctx.getVoidTy()) {
      error(node->getLocation(),
            "parameter '" + p.getName() + "' cannot have type 'void'");
      pty = nullptr;
    }
    if (pty)
      p.ParamType = pty;
    else
      ok = false;
    paramTys.push_back(pty ? pty : Ctx.getPoisonTy());
    modes.push_back(p.Mode);
  }
  ast::Type *retTy = Ctx.getVoidTy();
  if (node->getReturnType()) {
    FixedArrayAllowed = true; // a result may be a fixed-size array
    retTy = resolveType(node->getReturnType(), node->getLocation(),
                        "the lambda's result");
    if (!retTy)
      return nullptr;
    node->setReturnType(retTy);
  }
  if (!ok)
    return nullptr;
  StringSet seen;
  for (const ast::Param &p : node->getParams()) {
    if (!checkBinderName(p.getName(), node->getLocation()))
      return nullptr;
    if (!seen.insert(p.getName()).second) {
      error(node->getLocation(),
            "duplicate parameter '" + p.getName() + "' in the lambda");
      return nullptr;
    }
  }
  auto *fnt = Ctx.getFunctionType(paramTys, modes, retTy);

  // The body: a function's, in its own scope under the enclosing one (so
  // the names it uses resolve, and the ones bound outside are captured).
  // It is not the enclosing method's body: `self` is a captured variable,
  // not the receiver, so the method's rules on it do not apply inside, and
  // neither do its loops.
  auto *savedRetTy = CurrentReturnType;
  const ResultContext savedResult = CurrentResult;
  const unsigned savedLoopDepth = LoopDepth;
  MethodUse *savedMethodUse = CurrentMethodUse;
  ClassContext *savedClassCtx = CurrentClassCtx;
  ClassContext lambdaCtx;
  if (savedClassCtx) {
    lambdaCtx = *savedClassCtx;
    lambdaCtx.MethodName.clear();
    lambdaCtx.ViewMethod = false;
    lambdaCtx.SuperInitRequired = false;
    lambdaCtx.SuperInitCalled = false;
  }
  CurrentClassCtx = savedClassCtx ? &lambdaCtx : nullptr;
  CurrentReturnType = retTy;
  LoopDepth = 0;
  CurrentMethodUse = nullptr;
  bool bodyOk;
  {
    ScopeGuard guard(*this);
    LambdaStack.push_back({node, CurrentScope, savedClassCtx, savedMethodUse});
    for (size_t i = 0; i < node->getParams().size(); ++i) {
      const ast::Param &p = node->getParams()[i];
      CurrentScope->declare(p.getName(), paramTys[i]);
      if (p.Mode != ast::ParamMode::Value)
        CurrentScope->Kinds[p.getName()] =
            p.Mode == ast::ParamMode::View ? VarKind::View : VarKind::Inout;
    }
    CurrentResult = {ast::ParamMode::Value, nullptr, CurrentScope};
    bodyOk = visitBody(node->getBody()->getStatements());
    if (bodyOk && retTy != Ctx.getVoidTy() &&
        !detail::blockAlwaysReturns(node->getBody()->getStatements())) {
      error(node->getLocation(),
            "the lambda does not always return a value (its result type is '" +
                typeName(retTy) + "')");
      bodyOk = false;
    }
    LambdaStack.pop_back();
  }
  CurrentReturnType = savedRetTy;
  CurrentResult = savedResult;
  LoopDepth = savedLoopDepth;
  CurrentMethodUse = savedMethodUse;
  CurrentClassCtx = savedClassCtx;
  if (!bodyOk)
    return nullptr;
  node->setResolvedType(fnt);
  return fnt;
}

ast::Type *Sema::checkFunctionValueCall(ast::FunctionType *fnt,
                                        const std::vector<ast::Expr *> &args,
                                        const std::string &what,
                                        ast::SourceLocation loc) {
  if (args.size() != fnt->getNumParams()) {
    error(loc, "'" + what + "' has type '" + typeName(fnt) + "': it takes " +
                   std::to_string(fnt->getNumParams()) + " argument(s), got " +
                   std::to_string(args.size()));
    return nullptr;
  }
  // The parameters by number, as a function type names none.
  ast::ParamModes modes;
  modes.Modes = fnt->getModes();
  for (size_t i = 0; i < fnt->getNumParams(); ++i)
    modes.Names.push_back(std::to_string(i + 1));
  bool ok = true;
  for (size_t i = 0; i < args.size(); ++i) {
    ast::Type *want = fnt->getParamType(i);
    ast::Type *argTy = resolveExprType(args[i], want);
    if (!argTy) {
      ok = false;
      continue;
    }
    if (!checkAssignable(want, argTy, args[i]) &&
        !diagnoseOptionalNarrowing(args[i]->getLocation(), want, argTy)) {
      error(args[i]->getLocation(),
            "argument " + std::to_string(i + 1) + " of '" + what +
                "' has type '" + typeName(argTy) + "', expected '" +
                typeName(want) + "'" + sliceArgNote(want, argTy));
      ok = false;
    }
  }
  if (!ok)
    return nullptr;
  checkInoutArgs(modes, fnt->getParamTypes(), args, what);
  checkViewArgs(modes, fnt->getParamTypes(), args, what);
  return fnt->getReturnType();
}

} // namespace sema
} // namespace paykan
