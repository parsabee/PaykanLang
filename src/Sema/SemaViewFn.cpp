// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Where a `view fn` may be declared (docs/language/04-classes.md, "Methods
// that don't change self"): only a method, never `__init__`, and a method and
// its overrides are all `view fn` or none is, so that a call through a base
// class knows what the method does to its object.

#include "Names.h"
#include "Sema.h"

namespace paykan {
namespace sema {

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

} // namespace sema
} // namespace paykan
