// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// `view fn` methods, and the places that cannot change
// (docs/language/04-classes.md, "Methods that don't change self"): a method
// may change `self` unless it is a `view fn`, and only a `view fn` may be
// called on a `view` parameter (of any type) or on `self` inside a `view
// fn`.  Nothing reached through one of those may be assigned or passed to an
// `inout` parameter either.  While classes are references the check follows
// the name: a copy of the reference in another variable can change the
// object.  A `let` local only cannot be reassigned: what it holds can change,
// but the variable itself is not passed to `inout`.

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
  if (varKind(name) == VarKind::View)
    return "'" + name + "' is a 'view' parameter";
  return "";
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
