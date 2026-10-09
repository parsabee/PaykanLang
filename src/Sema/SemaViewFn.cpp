// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// `view fn` methods, and the places that cannot change
// (docs/language/04-classes.md, "Methods that don't change self"): a method
// may change `self` unless it is a `view fn`, and only a `view fn` may be
// called on a `view` parameter (of any type) or on `self` inside a `view
// fn`.  Nothing reached through one of those may be assigned or passed to an
// `inout` parameter either.
//
// While objects, strings and arrays are references, a copy of one would
// change the original, so a `view` stays one: it can only be passed on to a
// `view` parameter, a `view` that shares what it holds is never stored, a
// `view` parameter is never returned, and a `match` arm's name for a `view`
// is one too.  A method may return part of `self`, which its caller does not
// see as a `view` yet (#216's `-> view T`).  A `let` local only cannot be
// reassigned: what it holds can change, but the variable itself is not passed
// to `inout`.

#include "Names.h"
#include "Sema.h"

#include <map>
#include <set>

namespace paykan {
namespace sema {

namespace {

/// The variable a place starts from: `c` for `c`, `c.a.n`, `c.xs[i]` and
/// `c.t.0`; null when the place starts from anything else (a call).
const ast::Identifier *rootVariable(const ast::Expr *e) {
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

/// The class, from @p ct up, that introduces method @p name: where a method
/// and all its overrides start.
ast::ClassType *introducingClass(ast::ClassType *ct, const std::string &name) {
  while (ct->getSuperClass() && ct->getSuperClass()->findMethod(name))
    ct = ct->getSuperClass();
  return ct;
}

} // namespace

std::string Sema::frozenPlace(const ast::Expr *e) {
  const ast::Identifier *root = rootVariable(e);
  if (!root)
    return "";
  const std::string &name = root->getName();
  if (name == names::kSelf && CurrentClassCtx &&
      !CurrentClassCtx->MethodName.empty()) {
    if (CurrentClassCtx->ViewMethod)
      return "'self' is read-only in 'view fn " + CurrentClassCtx->MethodName +
             "'";
    if (CurrentMethodUse)
      CurrentMethodUse->ChangesSelf = true;
    return "";
  }
  // A `let` local is not here: only the variable is fixed, not what it holds
  // (`p.x = 1`, `p.tick()`), and checkInoutArgs keeps it from `inout`.
  switch (varKind(name)) {
  case VarKind::View:
    return "'" + name + "' is a 'view' parameter";
  case VarKind::ViewBinding:
    return "'" + name + "' is bound to a 'view'";
  case VarKind::Plain:
  case VarKind::Let:
  case VarKind::Inout:
    break;
  }
  return "";
}

bool Sema::sharesStorage(const ast::Type *ty) {
  if (!ty || ast::isa<ast::PoisonType>(ty) || ast::isa<ast::EnumType>(ty) ||
      ast::isa<ast::BuiltinType>(ty))
    return false;
  if (const auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    return sharesStorage(ot->getInnerType());
  return true;
}

std::string Sema::viewOf(const ast::Expr *e) {
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e)) {
    std::string why = viewOf(te->getTrueExpr());
    return why.empty() ? viewOf(te->getFalseExpr()) : why;
  }
  return frozenPlace(e);
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
    std::string msg = viewOf(args[i]);
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
  // Part of `self` may be returned by any method; a `view` parameter belongs
  // to the caller and stays there.
  const ast::Identifier *root = rootVariable(e);
  if (!root || root->getName() == names::kSelf)
    return true;
  std::string msg = frozenPlace(e);
  if (msg.empty())
    return true;
  msg += "; it cannot be returned";
  error(e->getLocation(), msg);
  return false;
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
  const std::string what = "'" + call->getMethodName() + "' is not a 'view fn'";
  // `self.m()` in a method that may change `self`: whether that changes
  // `self` depends on m, which the warning works out once every body is
  // checked.
  const auto *id = ast::dyn_cast<ast::Identifier>(call->getReceiver());
  if (id && id->getName() == names::kSelf && CurrentMethodUse &&
      CurrentClassCtx && !CurrentClassCtx->ViewMethod) {
    CurrentMethodUse->SelfCalls.push_back(call->getMethodName());
    return true;
  }
  return checkChangeable(call->getReceiver(), what, call->getLocation());
}

bool Sema::checkViewFnDecl(const ast::FuncDecl *method,
                           const ast::MethodDecl *base) {
  if (method->isView() && method->getName() == names::kMethodInit) {
    error(method->getLocation(), "'" + std::string(names::kMethodInit) +
                                     "' cannot be a 'view fn': it sets up "
                                     "'self'");
    return false;
  }
  if (!base || base->isView() == method->isView())
    return true;
  if (base->isView())
    error(method->getLocation(),
          "override of '" + method->getName() +
              "' must be a 'view fn', like the method it overrides");
  else
    error(method->getLocation(),
          "override of '" + method->getName() +
              "' cannot be a 'view fn': the method it overrides may change "
              "'self'");
  return false;
}

bool Sema::rejectFreeViewFns(ast::TranslationUnit *tu) {
  bool ok = true;
  for (auto *fns : {&tu->getFuncDecls(), &tu->getGenericFuncDecls()})
    for (auto *fn : *fns)
      if (fn->isView()) {
        error(fn->getLocation(), "only a method can be a 'view fn': '" +
                                     fn->getName() +
                                     "' is a free function, with no 'self'");
        ok = false;
      }
  return ok;
}

void Sema::warnMissingViewFns() {
  // A method and its overrides form a family, keyed by the class that
  // introduces it: all of them are `view fn` or none is.  A family could be
  // one when no member changes `self` and every `self.m()` in its members
  // calls a family that could be one too (a fixed point, so that a getter
  // calling another getter is found in one compile).
  using Family = std::pair<ast::ClassType *, std::string>;
  std::map<Family, std::vector<const MethodUse *>> families;
  for (const MethodUse &use : MethodUses) {
    const std::string &name = use.Decl->getName();
    families[{introducingClass(use.Class, name), name}].push_back(&use);
  }
  std::set<Family> could;
  for (const auto &[family, members] : families) {
    // A family started outside this module (Obj's toString, an imported
    // class's method) keeps its marker: there is nothing to suggest.
    const auto *intro = family.first->findMethod(family.second);
    bool introducedHere = false;
    bool changes = false;
    for (const MethodUse *m : members) {
      introducedHere |= m->Class == family.first;
      changes |= m->ChangesSelf;
    }
    if (introducedHere && !changes && intro && !intro->isView())
      could.insert(family);
  }
  for (bool changed = true; changed;) {
    changed = false;
    for (auto it = could.begin(); it != could.end();) {
      bool stays = true;
      for (const MethodUse *m : families[*it])
        for (const std::string &callee : m->SelfCalls)
          if (!could.count({introducingClass(m->Class, callee), callee}))
            stays = false;
      if (stays) {
        ++it;
      } else {
        it = could.erase(it);
        changed = true;
      }
    }
  }
  // A generic class's instances share their template's locations: one
  // warning each.
  std::set<std::pair<unsigned, unsigned>> warned;
  for (const Family &family : could) {
    for (const MethodUse *m : families[family]) {
      if (m->Class != family.first)
        continue;
      ast::SourceLocation loc = m->Decl->getLocation();
      if (!warned.insert({loc.getLineStart(), loc.getColumnStart()}).second)
        continue;
      bool overridden = families[family].size() > 1;
      warning(loc, "'" + family.second +
                       "' never changes 'self': make it a 'view fn'" +
                       (overridden ? " (and its overrides)" : "") +
                       " so 'view' parameters can call it");
    }
  }
}

} // namespace sema
} // namespace paykan
