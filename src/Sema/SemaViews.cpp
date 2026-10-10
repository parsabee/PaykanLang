// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// What cannot change through a `view`
// (docs/language/02-functions-and-calling.md, "A `view` stays a `view`"): a
// `view` parameter, a `view` local and a `match` arm's name for either,
// `self` in a `view fn` (docs/language/04-classes.md, "Methods that don't
// change self"), and everything reached through them.  None of those may be
// assigned, have a field or element assigned, be passed to an `inout` parameter
// or have a method called on it that is not a `view fn`.
//
// While objects, strings and arrays are references, a copy of one would
// change the original, so a `view` stays one: it can only be passed on to a
// `view` parameter, a `view` that shares what it holds is never stored nor
// returned except as a `view` (`-> view T`, which part of `self` is in a
// `view fn`), and a `match` arm's name for a `view` is one too.

#include "Names.h"
#include "Sema.h"

namespace paykan {
namespace sema {

const ast::Expr *Sema::placeBase(const ast::Expr *e) {
  for (;;) {
    if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e))
      e = ma->getReceiver();
    else if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(e))
      e = se->getArray();
    else if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(e))
      e = ti->getTuple();
    else
      return e;
  }
}

const ast::Identifier *Sema::placeRoot(const ast::Expr *e) {
  return ast::dyn_cast<ast::Identifier>(placeBase(e));
}

std::string Sema::frozenPlace(const ast::Expr *e, bool borrows) {
  if (const BorrowResult *r = borrowResultOf(e))
    if (r->Mode == ast::ParamMode::View)
      return "the result of '" + r->Callee + "' is a 'view'";
  const ast::Identifier *root = placeRoot(e);
  if (!root)
    return "";
  const std::string &name = root->getName();
  if (name == names::kSelf && CurrentClassCtx &&
      !CurrentClassCtx->MethodName.empty()) {
    if (CurrentClassCtx->ViewMethod)
      return "'self' is read-only in 'view fn " + CurrentClassCtx->MethodName +
             "'";
    // Here a `view fn` would fail: the method changes `self`.
    if (CurrentMethodUse)
      CurrentMethodUse->ChangesSelf = true;
  }
  switch (varKind(name)) {
  case VarKind::View:
    return "'" + name + "' is a 'view' parameter";
  case VarKind::ViewLocal:
    return "'" + name + "' is a 'view' local";
  case VarKind::ViewBinding:
    return "'" + name + "' is bound to a 'view'";
  case VarKind::Plain:
  case VarKind::Let:
  case VarKind::Inout:
    break;
  }
  return borrows ? viewedBy(name) : "";
}

bool Sema::checkChangeable(const ast::Expr *e, const std::string &what,
                           ast::SourceLocation loc) {
  std::string why = frozenPlace(e);
  if (why.empty())
    return true;
  error(loc, why + "; " + what);
  return false;
}

bool Sema::checkMethodReceiver(const ast::MethodCallExpr *call,
                               const ast::MethodDecl *method) {
  if (method->isView())
    return true;
  // `self.m()` in a method that may change `self`: whether that changes
  // `self` depends on m, which the warning works out once every body is
  // checked.
  const auto *id = ast::dyn_cast<ast::Identifier>(call->getReceiver());
  if (id && id->getName() == names::kSelf && CurrentMethodUse &&
      CurrentClassCtx && !CurrentClassCtx->ViewMethod) {
    CurrentMethodUse->SelfCalls.push_back(call->getMethodName());
    return true;
  }
  return checkChangeable(call->getReceiver(),
                         "'" + call->getMethodName() + "' is not a 'view fn'",
                         call->getLocation());
}

bool Sema::sharesStorage(const ast::Type *ty) {
  if (!ty || ast::isa<ast::PoisonType>(ty) || ast::isa<ast::EnumType>(ty) ||
      ast::isa<ast::BuiltinType>(ty))
    return false;
  if (const auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    return sharesStorage(ot->getInnerType());
  return true;
}

std::string Sema::viewOf(const ast::Expr *e, bool borrows) {
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e)) {
    std::string why = viewOf(te->getTrueExpr(), borrows);
    return why.empty() ? viewOf(te->getFalseExpr(), borrows) : why;
  }
  return frozenPlace(e, borrows);
}

bool Sema::checkViewArgs(const ast::ParamModes &modes,
                         const std::vector<ast::Expr *> &args,
                         const std::string &callee) {
  bool ok = true;
  for (size_t i = 0; i < args.size(); ++i) {
    // An `inout` parameter takes no `view` either (checkInoutArgs).  An
    // argument of the wrong type was reported by the call's own check.
    if (modes.mode(i) != ast::ParamMode::Value || !args[i]->getResolvedType())
      continue;
    // A viewed variable of a value type is passed as a copy, which cannot
    // change it.
    std::string msg =
        viewOf(args[i], sharesStorage(args[i]->getResolvedType()));
    if (msg.empty())
      continue;
    msg += "; it can only be passed to a 'view' parameter, and parameter ";
    msg += std::to_string(i + 1);
    msg += " of '";
    msg += callee;
    msg += "' is not one";
    error(args[i]->getLocation(), msg);
    ok = false;
  }
  return ok;
}

bool Sema::checkViewNotStored(const ast::Expr *e, const ast::Type *ty) {
  // A value type is copied, and the copy is the holder's own.
  if (!sharesStorage(ty))
    return true;
  std::string msg = viewOf(e);
  if (msg.empty())
    return true;
  msg += "; it cannot be stored, only read or passed to a 'view' parameter";
  error(e->getLocation(), msg);
  return false;
}

bool Sema::checkViewNotReturned(const ast::Expr *e, const ast::Type *ty) {
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e))
    return checkViewNotReturned(te->getTrueExpr(), ty) &&
           checkViewNotReturned(te->getFalseExpr(), ty);
  if (!sharesStorage(ty))
    return true;
  // Part of `self` is a `view` in a `view fn`, so it is returned as one
  // (`-> view T`): as a copy, its caller could change it.  Another method
  // may return it, and so hands out a way to change `self`.  A `view`
  // belongs to the caller.
  const ast::Identifier *root = placeRoot(e);
  if (root && root->getName() == names::kSelf && CurrentClassCtx &&
      !CurrentClassCtx->MethodName.empty()) {
    if (!CurrentClassCtx->ViewMethod) {
      if (CurrentMethodUse)
        CurrentMethodUse->ChangesSelf = true;
      return true;
    }
    const std::string &name = CurrentClassCtx->MethodName;
    std::string msg = "'self' is read-only in 'view fn " + name +
                      "', so part of it can only be returned as a 'view'";
    // An override returns its result as the method it overrides does.  An
    // instance of a generic class (`Box<Str>`) knows the result's type only
    // with its parameters substituted.
    const ast::ClassType *cls = CurrentClassCtx->ClassType;
    if (cls->getSuperClass() && cls->getSuperClass()->findMethod(name))
      msg += "; the method it overrides returns a copy, so return a new value";
    else if (cls->getName().find('<') != std::string::npos)
      msg += ": write 'view' before its result type";
    else
      msg += ": write '-> view " + typeName(CurrentReturnType) + "'";
    error(e->getLocation(), msg);
    return false;
  }
  std::string msg = viewOf(e);
  if (msg.empty())
    return true;
  msg += "; it cannot be returned";
  error(e->getLocation(), msg);
  return false;
}

} // namespace sema
} // namespace paykan
