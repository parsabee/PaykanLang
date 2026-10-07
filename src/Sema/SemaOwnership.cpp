// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The ownership prototype's access checks (docs/design/ownership-proto.md):
// the kinds of variables, the access of expressions, changing operations,
// `let` and the `self` qualifiers.  Only run with
// Sema::setOwnership(true).

#include "Names.h"
#include "Sema.h"

namespace paykan {
namespace sema {

namespace {

/// "read-only 'x'", or "a read-only value" when nothing names it.
std::string readOnlyName(const std::string &blame) {
  return blame.empty() ? std::string("a read-only value")
                       : "read-only '" + blame + "'";
}

} // namespace

ast::Qualifier Sema::fieldQualifier(const ast::ClassType *ct,
                                    const std::string &field) const {
  for (; ct; ct = ct->getSuperClass()) {
    auto it = FieldQuals.find(ct->getName() + "." + field);
    if (it != FieldQuals.end())
      return it->second;
  }
  return ast::Qualifier::View;
}

Sema::VarKind Sema::varKind(std::string_view name) const {
  for (const Scope *s = CurrentScope; s; s = s->Parent) {
    if (!s->contains(name))
      continue;
    auto it = s->Kinds.find(name);
    return it == s->Kinds.end() ? VarKind{} : it->second;
  }
  return {};
}

void Sema::setVarKind(std::string_view name, ast::Qualifier q, bool isLet) {
  CurrentScope->Kinds[std::string(name)] = VarKind{q, isLet};
}

ast::Qualifier Sema::selfQualifier(const ast::FuncDecl *method) {
  if (method->getName() == names::kMethodInit)
    return ast::Qualifier::Mut;
  return method->hasExplicitSelf() ? method->getSelfQualifier()
                                   : ast::Qualifier::View;
}

void Sema::declareOwnershipParams(const ast::FuncDecl *fn, bool isMethod) {
  if (isMethod)
    setVarKind(names::kSelf, selfQualifier(fn));
  for (const ast::Param &p : fn->getParams())
    if (p.Qual != ast::Qualifier::View)
      setVarKind(p.getName(), p.Qual);
}

Sema::Access Sema::accessOf(const ast::Expr *e) {
  Access a;
  if (const auto *id = ast::dyn_cast<ast::Identifier>(e)) {
    a.Path = id->getName();
    if (id->getName() != names::kStdin &&
        varKind(id->getName()).Qual == ast::Qualifier::View) {
      a.Changeable = false;
      a.Blame = id->getName();
    }
    return a;
  }
  if (const auto *m = ast::dyn_cast<ast::MemberAccessExpr>(e)) {
    Access base = accessOf(m->getReceiver());
    if (!base.Path.empty())
      a.Path = base.Path + "." + m->getFieldName();
    if (!base.Changeable) {
      a.Changeable = false;
      a.Blame = base.Blame;
    } else if (const auto *ct = ast::dyn_cast<ast::ClassType>(
                   m->getReceiver()->getResolvedType());
               ct &&
               fieldQualifier(ct, m->getFieldName()) == ast::Qualifier::View) {
      a.Changeable = false; // a view field
      a.Blame = a.Path;
    }
    return a;
  }
  // An element follows its array's (or tuple's) access.
  if (const auto *s = ast::dyn_cast<ast::SubscriptExpr>(e)) {
    a = accessOf(s->getArray());
    a.Path.clear();
    return a;
  }
  if (const auto *t = ast::dyn_cast<ast::TupleIndexExpr>(e)) {
    a = accessOf(t->getTuple());
    a.Path.clear();
    return a;
  }
  if (const auto *t = ast::dyn_cast<ast::TernaryExpr>(e)) {
    a = accessOf(t->getTrueExpr());
    if (a.Changeable)
      a = accessOf(t->getFalseExpr());
    a.Path.clear();
    return a;
  }
  // A view result is read-only; own and mut results, constructors, builtins,
  // literals, operators, `cp` and `mv` give fresh (changeable) values.
  const ast::FuncDecl *fn = nullptr;
  if (const auto *c = ast::dyn_cast<ast::CallExpr>(e)) {
    if (const auto *sig = lookupFunction(c->getCalleeName()))
      fn = sig->Decl;
  } else if (const auto *mc = ast::dyn_cast<ast::MethodCallExpr>(e)) {
    if (const auto *ct = ast::dyn_cast<ast::ClassType>(
            mc->getReceiver()->getResolvedType())) {
      auto it = MethodSources.find(ct->findMethod(mc->getMethodName()));
      if (it != MethodSources.end())
        fn = it->second;
    }
  }
  if (fn && fn->getResultQualifier() == ast::Qualifier::View)
    a.Changeable = false;
  return a;
}

bool Sema::checkReassignable(const std::string &name, ast::SourceLocation loc) {
  if (!varKind(name).Let)
    return true;
  error(loc, "'" + name + "' is declared with 'let' and cannot be reassigned");
  return false;
}

bool Sema::requireChangeable(const ast::Expr *obj, const std::string &what,
                             ast::SourceLocation loc) {
  Access a = accessOf(obj);
  if (a.Changeable)
    return true;
  error(loc, "cannot change " + what + " through " + readOnlyName(a.Blame));
  return false;
}

bool Sema::checkMethodCallAccess(const ast::MethodCallExpr *call,
                                 ast::Type *recvTy,
                                 const ast::MethodDecl *method) {
  auto it = MethodSources.find(method);
  const ast::FuncDecl *fn = it == MethodSources.end() ? nullptr : it->second;
  const std::string &name = call->getMethodName();
  std::string what;
  if (ast::isa<ast::ArrayType>(recvTy) &&
      (name == names::kPush || name == names::kPop))
    what = "an array with '" + name + "'";
  else if (recvTy == Ctx.getFileTy() && name == names::kMethodWrite)
    what = "a file with '" + name + "'";
  else if (fn && selfQualifier(fn) == ast::Qualifier::Mut)
    what = "an object with '" + name + "'";
  return what.empty() ||
         requireChangeable(call->getReceiver(), what, call->getLocation());
}

} // namespace sema
} // namespace paykan
