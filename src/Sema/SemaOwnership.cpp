// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The ownership prototype's access checks (docs/design/ownership-proto.md):
// the kinds of variables, the access of expressions, changing operations,
// `mut` narrowing, `let` and the `self` qualifiers.  Only run with
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

/// How a `self` of kind @p q is written: `self`, `self: mut`.
std::string selfText(ast::Qualifier q) {
  return q == ast::Qualifier::View
             ? std::string(names::kSelf)
             : std::string(names::kSelf) + ": " + ast::qualifierName(q);
}

} // namespace

bool Sema::isSharedType(const ast::Type *ty) {
  if (const auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    ty = ot->getInnerType();
  return ty && (ast::isa<ast::ClassType>(ty) || ast::isa<ast::ArrayType>(ty) ||
                ast::isa<ast::TupleType>(ty));
}

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

const ast::FuncDecl *Sema::calleeDecl(const ast::CallExpr *call) const {
  const std::string &name = call->getCalleeName();
  const ast::ClassType *ct = nullptr;
  if (name == names::kMethodSuper) {
    ct =
        CurrentClassCtx ? CurrentClassCtx->ClassType->getSuperClass() : nullptr;
  } else if (const auto *sig = lookupFunction(name); sig && sig->Decl) {
    return sig->Decl;
  } else {
    ct = Ctx.lookupClassType(name);
  }
  if (!ct)
    return nullptr;
  auto it = MethodSources.find(ct->findMethod(names::kMethodInit));
  return it == MethodSources.end() ? nullptr : it->second;
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

bool Sema::checkMutSource(ast::Type *dst, const ast::Expr *src,
                          ast::SourceLocation loc) {
  if (!isSharedType(dst))
    return true;
  Access a = accessOf(src);
  if (a.Changeable)
    return true;
  error(loc, "cannot give 'mut' access to an object reached through " +
                 readOnlyName(a.Blame));
  return false;
}

bool Sema::checkMutArgs(const ast::FuncDecl *fn,
                        const std::vector<ast::Expr *> &args,
                        const std::string &callee) {
  if (!fn)
    return true;
  bool ok = true;
  const auto &params = fn->getParams();
  for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
    const ast::Param &p = params[i];
    const ast::Expr *arg = args[i];
    if (p.Qual != ast::Qualifier::Mut || !arg->getResolvedType())
      continue;
    if (isSharedType(p.ParamType)) {
      ok &= checkMutSource(p.ParamType, arg, arg->getLocation());
      continue;
    }
    // A `mut` value parameter is the caller's storage (inout): a changeable
    // variable, field or element.
    if (!ast::isa<ast::Identifier>(arg) &&
        !ast::isa<ast::MemberAccessExpr>(arg) &&
        !ast::isa<ast::SubscriptExpr>(arg)) {
      error(arg->getLocation(), "argument " + std::to_string(i + 1) + " of '" +
                                    callee +
                                    "' must be a variable, field or "
                                    "element: parameter '" +
                                    p.getName() + "' is 'mut'");
      ok = false;
      continue;
    }
    ok &= requireChangeable(arg, "'mut' parameter '" + p.getName() + "'",
                            arg->getLocation());
  }
  return ok;
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
  bool ok = what.empty() ||
            requireChangeable(call->getReceiver(), what, call->getLocation());
  return checkMutArgs(fn, call->getArguments(), name) && ok;
}

bool Sema::checkSelfQualifier(const ast::FuncDecl *method,
                              const ast::MethodDecl *base) {
  auto it = MethodSources.find(base);
  if (it == MethodSources.end())
    return true; // a builtin method: its `self` is a view
  ast::Qualifier want = selfQualifier(it->second);
  if (selfQualifier(method) == want)
    return true;
  error(method->getLocation(), "override of '" + method->getName() +
                                   "' must keep '" + selfText(want) + "'");
  return false;
}

} // namespace sema
} // namespace paykan
