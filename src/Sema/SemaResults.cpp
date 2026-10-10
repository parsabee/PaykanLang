// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Borrowed results (docs/language/02-functions-and-calling.md, "Borrowed
// results"): `-> view T` returns a borrow of what the function was given as
// one, `self` or a `view` / `inout` parameter, never a new value, and
// `-> inout T` the storage of part of `self` or of an `inout` parameter.  A
// call's `view` result is a `view`, so what a caller may do with it follows
// the rules of a `view` local; an `inout` result is a place, as an `inout`
// local's is.

#include "Names.h"
#include "Sema.h"

namespace paykan {
namespace sema {

bool Sema::checkResultDecl(const ast::FuncDecl *fn, bool method) {
  switch (fn->getResultMode()) {
  case ast::ParamMode::Value:
    return true;
  case ast::ParamMode::Inout:
    if (fn->isView()) {
      error(fn->getLocation(), "'view fn " + fn->getName() +
                                   "' cannot return 'inout': it does not "
                                   "change 'self'");
      return false;
    }
    break;
  case ast::ParamMode::View:
    break;
  }
  if (!method && fn->getName() == names::kMain) {
    error(fn->getLocation(), "'main' cannot return a borrow");
    return false;
  }
  return true;
}

void Sema::noteBorrowResult(const ast::Expr *call, const std::string &callee,
                            const ast::ParamModes &modes,
                            const std::vector<ast::Expr *> &args,
                            const ast::Expr *receiver) {
  if (modes.Result == ast::ParamMode::Value)
    return;
  BorrowResult &r = BorrowResults[call];
  r.Callee = callee;
  r.Mode = modes.Result;
  r.From.clear();
  if (receiver)
    r.From.push_back(receiver);
  // An `inout` result can only be part of an `inout` argument.
  for (size_t i = 0; i < args.size(); ++i)
    if (modes.mode(i) == ast::ParamMode::Inout ||
        (modes.mode(i) == ast::ParamMode::View &&
         modes.Result == ast::ParamMode::View))
      r.From.push_back(args[i]);
}

bool Sema::isInoutResult(const ast::Expr *e) const {
  auto it = BorrowResults.find(e);
  return it != BorrowResults.end() && it->second.Mode == ast::ParamMode::Inout;
}

const Sema::BorrowResult *Sema::borrowResultOf(const ast::Expr *e) const {
  auto it = BorrowResults.find(placeBase(e));
  return it == BorrowResults.end() ? nullptr : &it->second;
}

std::string Sema::borrowedResultError(const ast::Expr *e) {
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e)) {
    std::string why = borrowedResultError(te->getTrueExpr());
    return why.empty() ? borrowedResultError(te->getFalseExpr()) : why;
  }
  const bool inout = CurrentResult.Mode == ast::ParamMode::Inout;
  const std::string what =
      inout ? "an 'inout' result must be part of 'self' or of an 'inout' "
              "parameter"
            : "a 'view' result must be part of 'self' or of a 'view' or "
              "'inout' parameter";
  // A borrow of what a call borrowed: each of those must be ours to lend.
  if (const BorrowResult *r = borrowResultOf(e)) {
    if (inout && r->Mode != ast::ParamMode::Inout)
      return "the result of '" + r->Callee +
             "' is a 'view'; it cannot be returned 'inout'";
    for (const ast::Expr *from : r->From)
      if (std::string why = borrowedResultError(from); !why.empty())
        return why;
    return "";
  }
  // The storage an `inout` result names is an `inout` argument's.
  if (inout)
    if (std::string why = inoutPlaceError(e, "returned 'inout'"); !why.empty())
      return why;
  const auto *id = ast::dyn_cast<ast::Identifier>(placeBase(e));
  if (!id)
    return what + ", not a new value";
  const std::string &name = id->getName();
  const Scope *owner = CurrentScope ? CurrentScope->findOwner(name) : nullptr;
  bool param = name == names::kSelf;
  for (const ast::Param &p : CurrentResult.Fn->getParams())
    param |= p.getName() == name;
  if (!owner || owner != CurrentResult.Params || !param)
    return "'" + name + "' is a local: " + what;
  if (name == names::kSelf || varKind(name) == VarKind::Inout ||
      (!inout && varKind(name) == VarKind::View))
    return "";
  return "'" + name + "' is a copy: " + what;
}

} // namespace sema
} // namespace paykan
