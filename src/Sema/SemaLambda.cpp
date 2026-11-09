// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Semantic analysis: function values (docs/language/14-function-types.md).
// A lambda `fn(x: int) -> int { ... }` is checked as a function body is,
// in a scope of its own whose parent is the scope it is written in: a name
// used inside that is bound outside is captured, by copy, and declared in
// the lambda's scope as a read-only Captured variable (so a nested lambda
// captures the copy), CapturedWeak for a `weak` one (which a nested lambda
// captures weakly too).  A named function used as a value has the function
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
  // Its qualifiers are part of the type (#368): a caller through the value
  // gives what the function takes and holds what it returns.
  std::vector<ast::Qualifier> quals = sig.ParamQuals;
  quals.resize(sig.ParamTypes.size());
  return Ctx.getFunctionType(sig.ParamTypes, std::move(modes),
                             sig.ReturnType ? sig.ReturnType : Ctx.getVoidTy(),
                             std::move(quals), sig.ResultQual);
}

bool Sema::checkFunctionValue(const std::string &name, const FunctionSig &sig,
                              ast::SourceLocation loc) {
  const std::string shown = displayName(name);
  const ast::ClassType *ct = Ctx.lookupClassType(name);
  if (ct && ct->isAbstract()) {
    // No lambda can construct it either.
    error(loc, "'" + shown +
                   "' is a constructor, not a function value, and its class "
                   "is abstract: it cannot be instantiated");
    return false;
  }
  if (ct || Ctx.lookupStructType(name)) {
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

bool Sema::errorGenericFunctionValue(const std::string &name,
                                     ast::SourceLocation loc) {
  error(loc, "'" + displayName(name) +
                 "' is a generic function; a generic function cannot be a "
                 "function value yet (wrap a call to it in a lambda)");
  return false;
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
    case VarKind::WeakView:
      why = "'" + name + "' is a 'weak' local of a 'view'";
      break;
    case VarKind::Plain:
    case VarKind::Let:
    case VarKind::ForVar:
    case VarKind::Captured:
    case VarKind::CapturedWeak:
    case VarKind::Weak:
      break;
    }
    if (!why.empty()) {
      error(loc, why + "; a function cannot capture a borrow (it may "
                       "outlive the call that holds it)");
      return false;
    }
    // A capture is a use of the variable, and a copy of the reference it
    // holds, which the function could change it through: neither while a
    // local borrow of it is live where the lambda is written.
    for (const Borrow &b : lc.OuterBorrows) {
      if (b.Root != name || b.RootScope != owner)
        continue;
      if (b.Inout) {
        error(loc, "'" + name + "' is borrowed by 'inout' local '" + b.By +
                       "' until '" + b.By +
                       "' is last used; a function cannot capture it");
        return false;
      }
      if (sharesStorage(ty)) {
        error(loc, "'" + name + "' is viewed by 'view' local '" + b.By +
                       "' until '" + b.By +
                       "' is last used; a function cannot capture it (it "
                       "could change it through its copy)");
        return false;
      }
    }
    // A capture is a copy (the copy rule, #369): a move-only value is
    // captured through a `weak` local of it, or from a `shared` form.
    if (kind != VarKind::Weak && kind != VarKind::CapturedWeak &&
        kind != VarKind::Captured && name != names::kSelf &&
        !isCopyablePart(ty, qualifierOfVar(name))) {
      const ast::Qualifier q = qualifierOfVar(name);
      noteArchetypeCopy(loc, ty, "to capture it");
      error(
          loc,
          "cannot capture '" + name + "' (a unique '" + qualifiedName(q, ty) +
              "'): a function captures by copy" +
              (ast::isRefType(ast::stripOptional(ty))
                   ? "; declare it '" +
                         qualifiedName(
                             ast::Qualifier{ast::QualForm::Shared, q.Mut}, ty) +
                         "' (and then, to keep no owner, capture a 'weak' "
                         "local of it)"
                   : std::string()));
      return false;
    }
    // An `atomic` cell's handle is not captured yet.
    if (ast::isa<ast::AtomicType>(ty) && kind != VarKind::Captured) {
      error(loc, "cannot capture '" + name + "' (an '" + typeName(ty) +
                     "' cell) yet: pass it to the function as an argument");
      return false;
    }
    if (name == names::kSelf) {
      const ClassContext *outer = lc.Outer;
      // A method borrows its receiver, and a capture is a copy: the
      // function would be a second owner of a unique object (#369).
      // Reported, the capture goes on: the method's other checks (what
      // `__init__` has assigned yet) still run.
      if (outer && !outer->Struct && !outer->ViewMethod && !outer->SharedMethod)
        error(loc, "cannot capture 'self': a method borrows its receiver, "
                   "and a function captures by copy; a 'shared fn' may "
                   "capture it, or a 'weak' local of it ('me: weak mut = "
                   "self;')");
      if (outer && outer->Struct) {
        error(loc, "'self' is the receiver's storage in a method of struct '" +
                       outer->Struct->getName() +
                       "'; a function cannot capture it (copy it into a "
                       "variable first)");
        return false;
      }
      if (outer && outer->ViewMethod && !outer->SharedMethod) {
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
    // the object weakly too, and a call reads it as the variable reads.  So
    // is an outer lambda's weak capture, by a lambda inside it.
    const bool weak = kind == VarKind::Weak || kind == VarKind::CapturedWeak;
    lc.Node->addCapture(Ctx.intern(name), ty, weak);
    lc.Boundary->declare(name, ty);
    lc.Boundary->Kinds[name] = weak ? VarKind::CapturedWeak : VarKind::Captured;
    // The capture keeps the variable's form (the prototype): a `mut`
    // object stays writable through it, though the capture itself is a
    // read-only copy.
    if (auto q = owner->Quals.find(name); q != owner->Quals.end())
      lc.Boundary->Quals[name] = q->second;
    owner = lc.Boundary;
  }
  return true;
}

ast::Type *Sema::checkLambda(ast::LambdaExpr *node) {
  // The signature, as a function's: canonical types written back.
  std::vector<ast::Type *> paramTys;
  std::vector<ast::ParamMode> modes;
  std::vector<ast::Qualifier> paramQuals;
  bool ok = true;
  for (ast::Param &p : node->getMutableParams()) {
    FixedArrayAllowed = true;
    SliceAllowed = p.Mode != ast::ParamMode::Value;
    auto *pty =
        resolveValueType(p.ParamType, p.Loc, "parameter '" + p.getName() + "'");
    // The prototype's qualifiers: taken here too, as a function's are.  A
    // borrow's form a type argument gave (`view T` in a template's lambda,
    // #368) is dropped: the lambda's function type carries none, so its
    // callers may lend any form, which its body must not keep a copy of.
    if (ast::Qualifier q = takeQualifier(); !q.isUnique()) {
      if (!checkBorrowQualifier(q, p.Mode, "parameter '" + p.getName() + "'",
                                p.Loc))
        ok = false;
      p.Qual = p.Mode != ast::ParamMode::Value && q.FromArgument
                   ? ast::Qualifier{}
                   : q;
    }
    if (pty)
      p.ParamType = pty;
    else
      ok = false;
    paramTys.push_back(pty ? pty : Ctx.getPoisonTy());
    modes.push_back(p.Mode);
    paramQuals.push_back(p.Qual);
  }
  // `fn(x: int) => x * 2` has no result type yet: the body (one `return`)
  // is checked against none, and the result type is then its value's.
  const bool inferred = node->hasInferredResult();
  ast::Type *retTy = inferred ? nullptr : Ctx.getVoidTy();
  ast::Qualifier resultQual;
  if (!inferred && node->getReturnType()) {
    FixedArrayAllowed = true; // a result may be a fixed-size array
    retTy = resolveType(node->getReturnType(), node->getLocation(),
                        "the lambda's result");
    // Its qualifier is its function type's (#368).
    resultQual = takeQualifier();
    if (!retTy)
      return nullptr;
    node->setReturnType(retTy);
  }
  if (!ok)
    return nullptr;
  StringSet seen;
  for (const ast::Param &p : node->getParams()) {
    if (!checkBinderName(p.getName(), p.Loc, /*binds=*/true,
                         "a parameter name"))
      return nullptr;
    if (!seen.insert(p.getName()).second) {
      error(p.Loc, "duplicate parameter '" + p.getName() + "' in the lambda");
      return nullptr;
    }
  }
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
    lambdaCtx.FrontSuperCall = nullptr;
  }
  CurrentClassCtx = savedClassCtx ? &lambdaCtx : nullptr;
  CurrentReturnType = retTy;
  // What its `return`s flow into: the lambda's result, not the enclosing
  // function's.
  const ast::Qualifier savedResultQual =
      std::exchange(CurrentResultQual, resultQual);
  LoopDepth = 0;
  CurrentMethodUse = nullptr;
  // Its moves are its own: a capture is a read-only copy (the prototype).
  MovedMap savedMoves = std::exchange(MovedVars, {});
  std::vector<LoopMoves> savedLoopMoves = std::exchange(LoopMoveStack, {});
  const bool savedFlowEnded = std::exchange(FlowEnded, false);
  bool bodyOk;
  {
    ScopeGuard guard(*this);
    // The lambda's task handles are its own (the prototype).
    Scope *savedFunctionScope = std::exchange(FunctionScope, CurrentScope);
    std::unordered_map<std::string, ast::SourceLocation> savedTasks =
        std::exchange(TaskLocals, {});
    LambdaStack.push_back({node, CurrentScope, savedClassCtx, savedMethodUse,
                           Borrows, savedResult.Fn});
    for (size_t i = 0; i < node->getParams().size(); ++i) {
      const ast::Param &p = node->getParams()[i];
      CurrentScope->declare(p.getName(), paramTys[i]);
      if (p.Mode != ast::ParamMode::Value)
        CurrentScope->Kinds[p.getName()] =
            p.Mode == ast::ParamMode::View ? VarKind::View : VarKind::Inout;
      ast::Qualifier q = p.Qual;
      // (a shared form, a template argument\'s, is its own: #368)
      if (p.Mode == ast::ParamMode::Inout && q.Form == ast::QualForm::Unique)
        q.Mut = true; // the borrow's writes
      CurrentScope->Quals[p.getName()] = q;
    }
    CurrentResult = {ast::ParamMode::Value, nullptr, CurrentScope};
    bodyOk = visitBody(node->getBody()->getStatements());
    if (bodyOk && !FlowEnded)
      checkTasksAwaited(CurrentScope, nullptr, node->getLocation());
    FunctionScope = savedFunctionScope;
    TaskLocals = std::move(savedTasks);
    if (bodyOk && inferred)
      retTy = inferredResult(node);
    if (!retTy)
      bodyOk = false;
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
  // An inferred result takes the form its `return` gave it
  // (visitReturnStmt).
  if (inferred)
    resultQual = CurrentResultQual;
  CurrentResultQual = savedResultQual;
  LoopDepth = savedLoopDepth;
  MovedVars = std::move(savedMoves);
  LoopMoveStack = std::move(savedLoopMoves);
  FlowEnded = savedFlowEnded;
  CurrentMethodUse = savedMethodUse;
  CurrentClassCtx = savedClassCtx;
  if (!bodyOk)
    return nullptr;
  auto *fnt = Ctx.getFunctionType(paramTys, modes, retTy, std::move(paramQuals),
                                  resultQual);
  node->setResolvedType(fnt);
  return fnt;
}

ast::Type *Sema::inferredResult(ast::LambdaExpr *node) {
  ast::Type *ty =
      returnedType(node->getBody(), "the lambda's result", "fn(...) { ...; }");
  if (ty)
    node->setReturnType(ty);
  return ty;
}

ast::Type *Sema::checkFunctionValueCall(ast::FunctionType *fnt,
                                        const std::vector<ast::Expr *> &args,
                                        const std::string &what,
                                        const ast::Expr *call) {
  if (args.size() != fnt->getNumParams()) {
    error(call->getLocation(),
          "'" + what + "' has type '" + typeName(fnt) + "': it takes " +
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
  checkInoutArgs(call, modes, fnt->getParamTypes(), args, what);
  checkViewArgs(modes, fnt->getParamTypes(), args, what);
  // A by-value parameter owns what it is given: it converts into the
  // parameter's form (#368), under the copy rule (#369); a borrow takes the
  // form its parameter says.
  for (size_t i = 0; i < args.size(); ++i)
    if (modes.mode(i) == ast::ParamMode::Value &&
        !checkQualifiedFlow(fnt->getParamQual(i), args[i],
                            "parameter " + std::to_string(i + 1) + " of '" +
                                what + "'",
                            args[i]->getLocation()))
      ok = false;
  // (A function type's borrow carries no qualifier: getFunctionType.)
  if (ok && !checkBorrowQualifiers(modes, fnt->getParamQuals(), args, what))
    ok = false;
  if (!ok)
    return nullptr;
  // What the call gives is held as the function type says.
  if (ast::Qualifier q = fnt->getResultQual(); !q.isUnique()) {
    // A `() -> T` in a template's code gives a type argument's form.
    q.FromArgument = isArgumentForm(q, fnt->getReturnType());
    ValueCallResultQuals[call] = q;
  }
  return fnt->getReturnType();
}

} // namespace sema
} // namespace paykan
