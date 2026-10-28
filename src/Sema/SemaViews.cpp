// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// What cannot change through a `view`
// (docs/language/03-functions.md, "A `view` of a reference stays a `view`"): a
// `view` parameter, a `view` local and a `match` arm's name for either,
// `self` in a `view fn` (docs/language/05-classes.md, "Methods that don't
// change self"), and everything reached through them.  None of those may be
// assigned, have a field or element assigned, be passed to an `inout` parameter
// or have a method called on it that is not a `view fn`.
//
// While objects, strings and arrays are references, a copy of one would
// change the original, so a `view` stays one: it can only be passed on to a
// `view` parameter, a `view` that shares what it holds is never stored nor
// returned except as a `view` (`-> view T`, which part of `self` is in a
// `view fn`), nor made an element of an array or tuple literal, and a
// `match` arm's name for a `view` is one too.

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

std::string Sema::frozenPlace(const ast::Expr *e, bool borrows, bool copy) {
  if (const BorrowResult *r = borrowResultOf(e))
    if (r->Mode == ast::ParamMode::View)
      return "the result of '" + r->Callee + "' is a 'view'";
  // A field or element of a conditional's object is one of its branches'
  // (`(if c then a else b).n`): it cannot change where either branch's
  // could not.  Past an object on the way to the conditional, what changes
  // is that object, which a copy of the branch would name as well.
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(placeBase(e))) {
    for (const ast::Expr *p = e; p != te && !copy;) {
      ast::Type *t = p->getResolvedType();
      copy = t && ast::isa<ast::ClassType>(ast::stripOptional(t));
      if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(p))
        p = ma->getReceiver();
      else if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(p))
        p = se->getArray();
      else
        p = ast::cast<ast::TupleIndexExpr>(p)->getTuple();
    }
    std::string why = frozenPlace(te->getTrueExpr(), borrows, copy);
    return why.empty() ? frozenPlace(te->getFalseExpr(), borrows, copy) : why;
  }
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
  case VarKind::WeakView:
    return "'" + name + "' is a 'weak' local of a 'view'";
  case VarKind::Captured:
  case VarKind::CapturedWeak: {
    // The lambda's own copy, which its body must not change (the program
    // could not see it); what a captured reference names is an object,
    // changed through it as through any other reference, whether the copy
    // is the reference or holds it (a struct's or a tuple's field), a weak
    // one included.  A copy of it holds its references as the capture does,
    // so it may be stored, returned or passed on.
    if (copy)
      break;
    for (const ast::Expr *p = e;;) {
      ast::Type *t = p == root ? CurrentScope->lookup(name)
                               : const_cast<ast::Expr *>(p)->getResolvedType();
      if (t && ast::isa<ast::ClassType>(ast::stripOptional(t)))
        return "";
      if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(p))
        p = ma->getReceiver();
      else if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(p))
        p = se->getArray();
      else if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(p))
        p = ti->getTuple();
      else
        break;
    }
    return "'" + name + "' is captured by copy by the function";
  }
  case VarKind::Plain:
  case VarKind::Let:
  case VarKind::ForVar:
  case VarKind::Inout:
  case VarKind::Weak:
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
  const std::string what = "'" + call->getMethodName() + "' is not a 'view fn'";
  // A struct method's `self` is the receiver's storage: the method changes
  // the struct where it is, so the receiver must be a place that can
  // change (as an assignment to one of its fields must).  A `let` value
  // and a tuple's element are immutable.
  if (ast::isa<ast::StructType>(call->getReceiver()->getResolvedType())) {
    if (std::string let = letValueRoot(call->getReceiver()); !let.empty()) {
      error(call->getLocation(), "'" + let +
                                     "' is declared with 'let'; its value "
                                     "cannot change, and " +
                                     what);
      return false;
    }
    if (const auto *ti = tupleElementHolding(call->getReceiver())) {
      error(call->getLocation(),
            "cannot change a struct in element '." +
                std::to_string(ti->getIndex()) + "' of type '" +
                typeName(ti->getTuple()->getResolvedType()) +
                "': tuples are immutable, and " + what);
      return false;
    }
  }
  return checkChangeable(call->getReceiver(), what, call->getLocation());
}

bool Sema::sharesStorage(const ast::Type *ty) {
  // A tagged union shares what its variants' fields do.
  if (const auto *et = ty ? ast::dyn_cast<ast::EnumType>(ty) : nullptr) {
    for (size_t i = 0; i < et->getNumVariants(); ++i)
      if (const ast::StructType *p = et->getPayload(i); p && sharesStorage(p))
        return true;
    return false;
  }
  if (!ty || ast::isa<ast::PoisonType>(ty) || ast::isa<ast::BuiltinType>(ty))
    return false;
  if (const auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    return sharesStorage(ot->getInnerType());
  // A fixed-size array or a tuple is a value: a copy of it shares what its
  // elements do.
  if (const auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty))
    return sharesStorage(ft->getElementType());
  if (const auto *tt = ast::dyn_cast<ast::TupleType>(ty)) {
    for (const ast::Type *e : tt->getElementTypes())
      if (sharesStorage(e))
        return true;
    return false;
  }
  return true;
}

std::string Sema::viewOf(const ast::Expr *e, bool borrows) {
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e)) {
    std::string why = viewOf(te->getTrueExpr(), borrows);
    return why.empty() ? viewOf(te->getFalseExpr(), borrows) : why;
  }
  return frozenPlace(e, borrows, /*copy=*/true);
}

bool Sema::checkViewArgs(const ast::ParamModes &modes,
                         const std::vector<ast::Type *> &paramTys,
                         const std::vector<ast::Expr *> &args,
                         const std::string &callee) {
  bool ok = true;
  for (size_t i = 0; i < args.size(); ++i) {
    // A `view` is passed on to a `view` parameter as it is: a `view T`
    // never becomes the optional of a `view T?` (#96).
    if (modes.mode(i) == ast::ParamMode::View && i < paramTys.size() &&
        ast::isa<ast::OptionalType>(paramTys[i]) &&
        args[i]->getResolvedType() &&
        !ast::isa<ast::OptionalType>(args[i]->getResolvedType())) {
      std::string msg = viewOf(args[i], /*borrows=*/false);
      if (!msg.empty()) {
        msg += "; a 'view' is not converted, and parameter ";
        msg += std::to_string(i + 1);
        msg += " of '";
        msg += callee;
        msg += "' is a 'view ";
        msg += typeName(paramTys[i]);
        msg += "'";
        error(args[i]->getLocation(), msg);
        ok = false;
      }
      continue;
    }
    // An `inout` parameter takes no `view` either (checkInoutArgs).  An
    // argument of the wrong type was reported by the call's own check.
    if (modes.mode(i) != ast::ParamMode::Value || !args[i]->getResolvedType())
      continue;
    // A value that shares nothing (an `int`, a struct or a tuple of them:
    // an element of a `view` array, a field of a `view fn`'s `self`, a
    // `view int` parameter) is passed as a copy, which cannot change what
    // it was read from, as assigning it copies it.  The place is still
    // looked at: part of `self` passed on counts as a possible change of
    // `self` for the 'never changes self' warning, as before.
    const bool shares = sharesStorage(args[i]->getResolvedType());
    std::string msg = viewOf(args[i], shares);
    if (msg.empty() || !shares)
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

bool Sema::checkViewLocalNotConverted(const std::string &name,
                                      ast::Type *declTy, const ast::Expr *init,
                                      ast::Type *initTy) {
  if (!ast::isa<ast::OptionalType>(declTy) ||
      ast::isa<ast::OptionalType>(initTy))
    return true;
  std::string why = viewOf(init, /*borrows=*/false);
  if (why.empty())
    return true;
  error(init->getLocation(), why + "; a 'view' is not converted, and '" + name +
                                 "' is a 'view " + typeName(declTy) + "'");
  return false;
}

bool Sema::checkViewNotStored(const ast::Expr *e, const ast::Type *ty) {
  // A value type is copied, and the copy is the holder's own; for another
  // instance, it may not be.
  if (!sharesStorage(ty)) {
    noteTypeArgumentCopy(e);
    return true;
  }
  std::string msg = viewOf(e);
  if (msg.empty())
    return true;
  msg += "; it cannot be stored, only read or passed to a 'view' parameter";
  error(e->getLocation(), msg);
  return false;
}

namespace {

/// Whether the type annotation @p ty names one of @p params.
bool namesTypeParam(const ast::Type *ty,
                    const std::vector<const std::string *> &params) {
  if (!ty)
    return false;
  if (const auto *ct = ast::dyn_cast<ast::ClassType>(ty))
    return std::any_of(params.begin(), params.end(), [&](const std::string *p) {
      return *p == ct->getName();
    });
  if (const auto *gt = ast::dyn_cast<ast::GenericType>(ty))
    return std::any_of(
        gt->getArgs().begin(), gt->getArgs().end(),
        [&](const ast::Type *a) { return namesTypeParam(a, params); });
  if (const auto *at = ast::dyn_cast<ast::ArrayType>(ty))
    return namesTypeParam(at->getElementType(), params);
  if (const auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty))
    return namesTypeParam(ft->getElementType(), params);
  if (const auto *st = ast::dyn_cast<ast::SliceType>(ty))
    return namesTypeParam(st->getElementType(), params);
  if (const auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    return namesTypeParam(ot->getInnerType(), params);
  if (const auto *fnt = ast::dyn_cast<ast::FunctionType>(ty)) {
    for (size_t i = 0; i < fnt->getNumParams(); ++i)
      if (namesTypeParam(fnt->getParamType(i), params))
        return true;
    return namesTypeParam(fnt->getReturnType(), params);
  }
  // A struct is named, not built from its fields.
  if (ast::isa<ast::StructType>(ty))
    return false;
  if (const auto *tt = ast::dyn_cast<ast::TupleType>(ty))
    return std::any_of(
        tt->getElementTypes().begin(), tt->getElementTypes().end(),
        [&](const ast::Type *e) { return namesTypeParam(e, params); });
  return false;
}

} // namespace

bool Sema::holdsTypeArgument(const ast::Expr *e) {
  // The template's fields, and its type parameters.
  const std::vector<ast::VarDecl *> *fields = nullptr;
  const std::vector<const std::string *> *params = nullptr;
  if (const InstantiationInfo *info =
          CurrentMethodUse->Class ? instantiationInfo(CurrentMethodUse->Class)
                                  : nullptr) {
    fields = &info->Template->getFields();
    params = &info->Template->getTypeParams();
  } else if (const StructInstantiationInfo *sinfo =
                 CurrentMethodUse->Struct
                     ? structInstantiationInfo(CurrentMethodUse->Struct)
                     : nullptr) {
    fields = &sinfo->Template->getFields();
    params = &sinfo->Template->getTypeParams();
  } else {
    return false; // not an instance
  }
  // The field of `self` the place starts with.
  const ast::MemberAccessExpr *field = nullptr;
  for (const ast::Expr *p = e;;) {
    if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(p)) {
      field = ma;
      p = ma->getReceiver();
    } else if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(p)) {
      p = se->getArray();
    } else if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(p)) {
      p = ti->getTuple();
    } else {
      const auto *root = ast::dyn_cast<ast::Identifier>(p);
      if (!root || root->getName() != names::kSelf)
        return false;
      break;
    }
  }
  if (!field)
    return true; // `self` itself
  for (const ast::VarDecl *f : *fields)
    if (f->getName() == field->getFieldName())
      return namesTypeParam(f->getType(), *params);
  return false; // a superclass's
}

void Sema::noteTypeArgumentCopy(const ast::Expr *e) {
  if (!CurrentMethodUse || !CurrentClassCtx || CurrentClassCtx->ViewMethod)
    return;
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e)) {
    noteTypeArgumentCopy(te->getTrueExpr());
    noteTypeArgumentCopy(te->getFalseExpr());
  } else if (holdsTypeArgument(e)) {
    CurrentMethodUse->CopiesTypeArgument = true;
  }
}

bool Sema::checkViewNotReturned(const ast::Expr *e, const ast::Type *ty) {
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e))
    return checkViewNotReturned(te->getTrueExpr(), ty) &&
           checkViewNotReturned(te->getFalseExpr(), ty);
  if (!sharesStorage(ty)) {
    // A copy, for this instance; for another, it may not be.
    noteTypeArgumentCopy(e);
    return true;
  }
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
    // with its parameters substituted.  (A struct's method overrides
    // nothing.)
    const ast::ClassType *cls = CurrentClassCtx->ClassType;
    if (cls && cls->getSuperClass() && cls->getSuperClass()->findMethod(name))
      msg += "; the method it overrides returns a copy, so return a new value";
    else if (cls && cls->getName().find('<') != std::string::npos)
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
