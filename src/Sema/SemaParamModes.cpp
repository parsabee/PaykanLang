// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The rules of `view` and `inout` parameters
// (docs/language/02-functions-and-calling.md, "Parameter modes"), which
// apply to every type: overrides, and the arguments an `inout` parameter
// takes, none of them twice in one call.  Assignments to a `view` parameter
// are checked with `let` locals' (Sema::checkReassignable), and what can be
// done through one with the places that cannot change (SemaViewFn.cpp).

#include "Names.h"
#include "Sema.h"

namespace paykan {
namespace sema {

namespace {

/// The storage @p e names when it is a variable or a chain of fields on one
/// (`k`, `self.n`, `a.b.n`); "" for anything else.
std::string placePath(const ast::Expr *e) {
  if (const auto *id = ast::dyn_cast<ast::Identifier>(e))
    return id->getName();
  const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e);
  if (!ma)
    return "";
  std::string base = placePath(ma->getReceiver());
  return base.empty() ? "" : base + "." + ma->getFieldName();
}

} // namespace

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
  // Exclusivity: each place passed to an `inout` parameter of the call, and
  // its parameter.  The same variable, or the same chain of fields on one,
  // twice is an error; different chains that might reach the same object at
  // run time (`a.n` and `b.n`) are not checked.
  StringMap<size_t> places;
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
    } else if (!typesEqual(argTy, paramTys[i])) {
      // The callee reads and writes the storage as the parameter's type, so
      // no conversion can happen on the way: not int -> float, and not a
      // subclass to its base (the callee could store another subclass).
      msg = argN + " has type '" + typeName(argTy) + "', but ";
      msg += param + " has type '" + typeName(paramTys[i]) + "'";
    } else if (id && id->getName() == names::kSelf) {
      msg = "'self' cannot be passed to " + param +
            ": the method would no longer know its object";
    } else if (id && varKind(id->getName()) == VarKind::Let) {
      // The callee could reassign it.
      msg = "'" + id->getName() + "' is 'let'; it cannot be passed to " + param;
    } else if (std::string why = frozenPlace(arg); !why.empty()) {
      msg = std::move(why);
      msg += "; it cannot be passed to ";
      msg += param;
    } else if (std::string path = placePath(arg); !path.empty()) {
      auto [it, first] = places.try_emplace(path, i);
      if (!first)
        msg = "'" + path + "' is passed to two 'inout' parameters ('" +
              modes.name(it->second) + "' and '" + modes.name(i) + "')";
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
