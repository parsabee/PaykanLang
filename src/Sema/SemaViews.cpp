// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// What cannot change through a `view`
// (docs/language/02-functions-and-calling.md, "A `view` stays a `view`"): a
// `view` parameter, a `view` local and a `match` arm's name for either, and
// everything reached through them.  None of those may be assigned, have a field
// or element assigned, be passed to an `inout` parameter or have a method
// called on it that may change it.
//
// While objects, strings and arrays are references, a copy of one would
// change the original, so a `view` stays one: it can only be passed on to a
// `view` parameter, a `view` that shares what it holds is never stored, a
// `view` is never returned, and a `match` arm's name for a `view` is one too.

#include "Names.h"
#include "Sema.h"

namespace paykan {
namespace sema {

const ast::Identifier *Sema::placeRoot(const ast::Expr *e) {
  for (;;) {
    if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e))
      e = ma->getReceiver();
    else if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(e))
      e = se->getArray();
    else if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(e))
      e = ti->getTuple();
    else
      return ast::dyn_cast<ast::Identifier>(e);
  }
}

std::string Sema::frozenPlace(const ast::Expr *e, bool borrows) {
  const ast::Identifier *root = placeRoot(e);
  if (!root)
    return "";
  const std::string &name = root->getName();
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

bool Sema::readsOnly(ast::ClassType *ct, const ast::MethodDecl *method) {
  if (method->isView())
    return true;
  // An override of a method that only reads (`toString`, `equals`) only
  // reads too.
  for (ast::ClassType *c = ct ? ct->getSuperClass() : nullptr; c;
       c = c->getSuperClass())
    if (const ast::MethodDecl *base = c->findMethod(method->getName()))
      if (base->isView())
        return true;
  return false;
}

bool Sema::checkMethodReceiver(const ast::MethodCallExpr *call,
                               ast::ClassType *ct,
                               const ast::MethodDecl *method) {
  if (readsOnly(ct, method))
    return true;
  return checkChangeable(call->getReceiver(),
                         "'" + call->getMethodName() + "' may change it",
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
  if (!sharesStorage(ty))
    return true;
  std::string msg = viewOf(e);
  if (msg.empty())
    return true;
  msg += "; it cannot be returned";
  error(e->getLocation(), msg);
  return false;
}

} // namespace sema
} // namespace paykan
