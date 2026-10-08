// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The ownership prototype's checks (docs/design/ownership-proto.md): `view`
// and `inout` value parameters, their arguments, overrides, and `let`.  Only
// run with Sema::setOwnership(true); without it the syntax is an error.

#include "Names.h"
#include "Sema.h"

namespace paykan {
namespace sema {

bool Sema::requireOwnership(ast::SourceLocation loc, const char *what) {
  if (Ownership)
    return true;
  error(loc, std::string("'") + what + "' needs --ownership (prototype)");
  return false;
}

bool Sema::checkOwnershipSyntax(const ast::FuncDecl *fn) {
  for (const ast::Param &p : fn->getParams())
    if (p.Qual != ast::Qualifier::None)
      return requireOwnership(fn->getLocation(), ast::qualifierName(p.Qual));
  return true;
}

bool Sema::isValueType(const ast::Type *ty) {
  if (!ty || ast::isa<ast::EnumType>(ty) || ast::isa<ast::OptionalType>(ty))
    return ty != nullptr;
  const auto *bt = ast::dyn_cast<ast::BuiltinType>(ty);
  return bt && bt->getTypeKind() != ast::BuiltinType::Void;
}

bool Sema::checkParamQualifiers(const ast::FuncDecl *fn,
                                const std::vector<ast::Type *> &paramTys) {
  bool ok = true;
  const auto &params = fn->getParams();
  for (size_t i = 0; i < params.size() && i < paramTys.size(); ++i) {
    ast::Type *ty = paramTys[i];
    if (params[i].Qual == ast::Qualifier::None || !ty ||
        ast::isa<ast::PoisonType>(ty) || isValueType(ty))
      continue;
    error(fn->getLocation(), std::string("'") +
                                 ast::qualifierName(params[i].Qual) +
                                 "' applies only to value types (int, float, "
                                 "bool, char, enums and optionals); '" +
                                 typeName(ty) + "' is a reference");
    ok = false;
  }
  return ok;
}

void Sema::declareQualifiedParams(const ast::FuncDecl *fn) {
  for (const ast::Param &p : fn->getParams())
    if (p.Qual != ast::Qualifier::None)
      setVarKind(p.getName(), p.Qual);
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

bool Sema::checkReassignable(const std::string &name, ast::SourceLocation loc) {
  VarKind k = varKind(name);
  if (k.Let) {
    error(loc,
          "'" + name + "' is declared with 'let' and cannot be reassigned");
    return false;
  }
  if (k.Qual == ast::Qualifier::View) {
    error(loc, "cannot assign to 'view' parameter '" + name + "'");
    return false;
  }
  return true;
}

bool Sema::checkInoutArgs(const ast::FuncDecl *fn,
                          const std::vector<ast::Expr *> &args,
                          const std::string &callee) {
  if (!fn)
    return true;
  bool ok = true;
  const auto &params = fn->getParams();
  for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
    const ast::Param &p = params[i];
    const ast::Expr *arg = args[i];
    if (p.Qual != ast::Qualifier::Inout || !isValueType(p.ParamType) ||
        !arg->getResolvedType())
      continue;
    const std::string param = "'inout' parameter '" + p.getName() + "'";
    const std::string argN =
        "argument " + std::to_string(i + 1) + " of '" + callee + "'";
    std::string msg;
    // The callee changes the caller's storage: a variable, field or element.
    const auto *id = ast::dyn_cast<ast::Identifier>(arg);
    const auto *elem = ast::dyn_cast<ast::SubscriptExpr>(arg);
    if (!id && !elem && !ast::isa<ast::MemberAccessExpr>(arg)) {
      msg = argN + " must be a variable, field or element: parameter '" +
            p.getName() + "' is 'inout'";
    } else if (elem &&
               !ast::isa<ast::ArrayType>(elem->getArray()->getResolvedType())) {
      msg = "a character of a string cannot be passed to " + param;
    } else if (std::string argTy = typeName(arg->getResolvedType());
               argTy != typeName(p.ParamType)) {
      // The callee reads and writes the storage as the parameter's type, so
      // no conversion (int -> float) can happen on the way.  (A value that
      // does not convert at all was reported by the call's own check.)
      if (isAssignable(p.ParamType, arg->getResolvedType())) {
        msg = argN + " has type '";
        msg += argTy;
        msg += "', but " + param;
        msg += " has type '" + typeName(p.ParamType) + "'";
      }
    } else if (id && varKind(id->getName()).Let) {
      msg = "'" + id->getName() +
            "' is declared with 'let' and cannot be passed to " + param;
    } else if (id && varKind(id->getName()).Qual == ast::Qualifier::View) {
      msg = "'view' parameter '" + id->getName() + "' cannot be passed to " +
            param;
    }
    if (!msg.empty()) {
      error(arg->getLocation(), msg);
      ok = false;
    }
  }
  return ok;
}

bool Sema::checkMethodInoutArgs(const ast::MethodCallExpr *call,
                                const ast::MethodDecl *method) {
  auto it = MethodSources.find(method);
  return it == MethodSources.end() ||
         checkInoutArgs(it->second, call->getArguments(),
                        call->getMethodName());
}

bool Sema::checkOverrideQualifiers(const ast::FuncDecl *method,
                                   const ast::MethodDecl *base) {
  // `inout` decides how the argument is passed (by address), so the vtable
  // slot needs it to agree; `view` is kept for the callers' checks.
  bool ok = true;
  const auto &params = method->getParams();
  for (size_t i = 0; i < params.size() && i < base->getNumParams(); ++i) {
    ast::Qualifier want = base->getParamQualifier(i);
    if (params[i].Qual == want)
      continue;
    std::string msg = "override of '" + method->getName() + "' must ";
    msg += want == ast::Qualifier::None
               ? std::string("not add '") + ast::qualifierName(params[i].Qual) +
                     "' to"
               : std::string("keep '") + ast::qualifierName(want) + "' on";
    error(method->getLocation(),
          msg + " parameter '" + params[i].getName() + "'");
    ok = false;
  }
  return ok;
}

} // namespace sema
} // namespace paykan
