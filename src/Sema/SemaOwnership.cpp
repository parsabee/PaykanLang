// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The ownership prototype's checks (docs/design/ownership-proto.md): `view`
// and `inout` value parameters, their arguments, overrides, and `let`.  Only
// run with Sema::setOwnership(true); without it the syntax is an error.

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

} // namespace sema
} // namespace paykan
