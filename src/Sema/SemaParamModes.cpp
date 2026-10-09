// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The rules of `view` and `inout` parameters
// (docs/language/02-functions-and-calling.md, "Parameter modes"): the types
// a mode applies to, overrides, and the arguments an `inout` parameter
// takes.  Assignments to a `view` parameter are checked with `let` locals'
// (Sema::checkReassignable).

#include "Names.h"
#include "Sema.h"

namespace paykan {
namespace sema {

bool Sema::isValueType(const ast::Type *ty) {
  if (ast::isa<ast::EnumType>(ty))
    return true;
  const auto *bt = ast::dyn_cast<ast::BuiltinType>(ty);
  return bt && bt->getTypeKind() != ast::BuiltinType::Void;
}

bool Sema::checkParamModeTypes(const ast::FuncDecl *fn,
                               const std::vector<ast::Type *> &paramTys) {
  bool ok = true;
  const auto &params = fn->getParams();
  for (size_t i = 0; i < params.size() && i < paramTys.size(); ++i) {
    ast::Type *ty = paramTys[i];
    if (params[i].Mode == ast::ParamMode::Value || !ty ||
        ast::isa<ast::PoisonType>(ty) || isValueType(ty))
      continue;
    error(fn->getLocation(), std::string("'") +
                                 ast::paramModeName(params[i].Mode) +
                                 "' applies only to int, float, bool, char "
                                 "and enum parameters; '" +
                                 typeName(ty) + "' is not a value type");
    ok = false;
  }
  return ok;
}

bool Sema::checkTemplateParamModes(
    const ast::FuncDecl *fn, const std::vector<const std::string *> &tps) {
  bool ok = true;
  for (const ast::Param &p : fn->getParams()) {
    const auto *ct = ast::dyn_cast<ast::ClassType>(p.ParamType);
    if (p.Mode == ast::ParamMode::Value || !ct)
      continue;
    for (const std::string *tp : tps)
      if (ct->getName() == *tp) {
        error(fn->getLocation(), std::string("'") + ast::paramModeName(p.Mode) +
                                     "' does not apply to type parameter '" +
                                     *tp + "' yet");
        ok = false;
      }
  }
  return ok;
}

void Sema::declareParamKinds(const ast::FuncDecl *fn) {
  for (const ast::Param &p : fn->getParams())
    if (p.Mode != ast::ParamMode::Value)
      CurrentScope->Kinds[p.getName()] =
          p.Mode == ast::ParamMode::View ? VarKind::View : VarKind::Inout;
}

bool Sema::checkOverrideModes(const ast::FuncDecl *method,
                              const ast::MethodDecl *base) {
  // The mode decides how the argument is passed (`inout`: by address) and
  // what a caller may pass, through the base or the subclass alike.
  bool ok = true;
  const auto &params = method->getParams();
  for (size_t i = 0; i < params.size() && i < base->getNumParams(); ++i) {
    ast::ParamMode want = base->getParamModes().mode(i);
    if (params[i].Mode == want)
      continue;
    std::string msg = "override of '" + method->getName() + "' must ";
    msg += want == ast::ParamMode::Value
               ? std::string("not add '") + ast::paramModeName(params[i].Mode) +
                     "' to"
               : std::string("keep '") + ast::paramModeName(want) + "' on";
    error(method->getLocation(),
          msg + " parameter '" + params[i].getName() + "'");
    ok = false;
  }
  return ok;
}

bool Sema::checkInoutArgs(const ast::ParamModes &modes,
                          const std::vector<ast::Type *> &paramTys,
                          const std::vector<ast::Expr *> &args,
                          const std::string &callee) {
  bool ok = true;
  for (size_t i = 0; i < args.size() && i < paramTys.size(); ++i) {
    ast::Expr *arg = args[i];
    ast::Type *argTy = arg->getResolvedType();
    // An argument of the wrong type was reported by the call's own check.
    if (modes.mode(i) != ast::ParamMode::Inout || !argTy ||
        !isAssignable(paramTys[i], argTy))
      continue;
    const std::string param = "'inout' parameter '" + modes.name(i) + "'";
    const std::string argN =
        "argument " + std::to_string(i + 1) + " of '" + callee + "'";
    const auto *id = ast::dyn_cast<ast::Identifier>(arg);
    std::string msg;
    if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(arg)) {
      msg = ast::isa<ast::ArrayType>(se->getArray()->getResolvedType())
                ? "an array element cannot be passed to " + param + " yet"
                : "a character of a string cannot be passed to " + param;
    } else if (!id && !ast::isa<ast::MemberAccessExpr>(arg)) {
      msg = argN + " must be a variable or a field: parameter '" +
            modes.name(i) + "' is 'inout'";
    } else if (!id) {
      msg = "a field cannot be passed to " + param + " yet";
    } else if (!typesEqual(argTy, paramTys[i])) {
      // The callee reads and writes the storage as the parameter's type, so
      // no conversion (int -> float) can happen on the way.
      msg = argN + " has type '" + typeName(argTy) + "', but ";
      msg += param + " has type '" + typeName(paramTys[i]) + "'";
    } else if (id && varKind(id->getName()) == VarKind::View) {
      msg = "'view' parameter '" + id->getName() + "' cannot be passed to " +
            param;
    } else if (id && varKind(id->getName()) == VarKind::Let) {
      msg = "'" + id->getName() +
            "' is declared with 'let' and cannot be passed to " + param;
    }
    if (!msg.empty()) {
      error(arg->getLocation(), msg);
      ok = false;
    }
  }
  return ok;
}

} // namespace sema
} // namespace paykan
