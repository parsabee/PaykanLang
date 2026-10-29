// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Borrowed results (docs/language/03-functions.md, "Borrowed
// results"): `-> view T` returns a borrow of what the function was given as
// one, `self` or a `view` / `inout` parameter, never a new value, and
// `-> inout T` the storage of part of `self` or of an `inout` parameter,
// which its caller keeps alive: not storage in an object reached through a
// field of another one.  A call's `view` result is a `view`, so what a
// caller may do with it follows the rules of a `view` local; an `inout`
// result is a place, as an `inout` local's is.

#include "Names.h"
#include "Sema.h"

#include <algorithm>

namespace paykan {
namespace sema {

namespace {

/// The place @p e as it is written, for a diagnostic: `self.c`, `n.t.0`,
/// `self.xs[2]`, `a[i]`, `self.slot().c`, `(if ... then a else b).c` (an
/// index other than a number or a name, a call's arguments, or a
/// condition, as `...`).
std::string placeText(const ast::Expr *e) {
  if (const auto *id = ast::dyn_cast<ast::Identifier>(e))
    return id->getName();
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e))
    return "(if ... then " + placeText(te->getTrueExpr()) + " else " +
           placeText(te->getFalseExpr()) + ")";
  if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e))
    return placeText(ma->getReceiver()) + "." + ma->getFieldName();
  if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(e))
    return placeText(ti->getTuple()) + "." + std::to_string(ti->getIndex());
  if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(e)) {
    std::string index = "...";
    if (const auto *lit = ast::dyn_cast<ast::IntegerLiteral>(se->getIndex()))
      index = std::to_string(lit->getValue());
    else if (const auto *id = ast::dyn_cast<ast::Identifier>(se->getIndex()))
      index = id->getName();
    return placeText(se->getArray()) + "[" + index + "]";
  }
  if (const auto *mc = ast::dyn_cast<ast::MethodCallExpr>(e))
    return placeText(mc->getReceiver()) + "." + mc->getMethodName() +
           (mc->getArguments().empty() ? "()" : "(...)");
  if (const auto *ce = ast::dyn_cast<ast::CallExpr>(e))
    return Sema::displayName(ce->getCalleeName()) +
           (ce->getArguments().empty() ? "()" : "(...)");
  return "...";
}

} // namespace

bool Sema::holdsPlace(ast::Type *ty, ast::Type *place) {
  if (typesEqual(ty, place))
    return true;
  if (const auto *at = ast::dyn_cast<ast::FixedArrayType>(ty))
    return holdsPlace(at->getElementType(), place);
  // A struct is a tuple with names.
  if (const auto *tt = ast::dyn_cast<ast::TupleType>(ty))
    return std::any_of(tt->getElementTypes().begin(),
                       tt->getElementTypes().end(),
                       [&](ast::Type *e) { return holdsPlace(e, place); });
  return false;
}

bool Sema::reachesPlace(ast::Type *ty, ast::Type *place) {
  if (const auto *ct = ast::dyn_cast<ast::ClassType>(ty)) {
    if (ct->isBuiltin())
      return false;
    auto returnsPlace = [&](const ast::MethodDecl *m) {
      return m->getParamModes().Result == ast::ParamMode::Inout &&
             holdsPlace(m->getReturnType(), place);
    };
    if (std::any_of(ct->getVTable().begin(), ct->getVTable().end(),
                    returnsPlace))
      return true;
    for (const ast::ClassType *c = ct; c; c = c->getSuperClass()) {
      for (const auto &field : c->getFields())
        if (holdsPlace(field.second, place))
          return true;
      if (std::any_of(c->getImplMethods().begin(), c->getImplMethods().end(),
                      returnsPlace))
        return true;
    }
    return false;
  }
  if (const auto *at = ast::dyn_cast<ast::FixedArrayType>(ty))
    return reachesPlace(at->getElementType(), place);
  if (const auto *tt = ast::dyn_cast<ast::TupleType>(ty))
    return std::any_of(tt->getElementTypes().begin(),
                       tt->getElementTypes().end(),
                       [&](ast::Type *e) { return reachesPlace(e, place); });
  return false;
}

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
                            const std::vector<ast::Type *> &paramTys,
                            ast::Type *resultTy,
                            const std::vector<ast::Expr *> &args,
                            const ast::Expr *receiver) {
  if (modes.Result == ast::ParamMode::Value)
    return;
  // The lowering reads it from the call: a value held in memory is then used
  // in place, not copied out into a temporary.
  if (modes.Result == ast::ParamMode::Inout)
    const_cast<ast::Expr *>(call)->setInoutResult(true);
  BorrowResult &r = BorrowResults[call];
  r.Callee = callee;
  r.Mode = modes.Result;
  r.From.clear();
  r.PastFrom.clear();
  r.Receiver = receiver;
  // A class's method returns part of its object (or of an argument); a
  // struct's, part of its storage or of an object held there that can hold
  // a place of the result's type.
  if (receiver) {
    ast::Type *recvTy = receiver->getResolvedType();
    const auto *ct = ast::dyn_cast<ast::ClassType>(recvTy);
    r.From.push_back(receiver);
    r.PastFrom.push_back(ct ? !ct->isBuiltin()
                            : reachesPlace(recvTy, resultTy));
  }
  // An `inout` result can only be part of an `inout` argument: of the
  // storage passed as the parameter's type (a slice's, an array), or of an
  // object a reference held there refers to.
  for (size_t i = 0; i < args.size(); ++i)
    if (modes.mode(i) == ast::ParamMode::Inout ||
        (modes.mode(i) == ast::ParamMode::View &&
         modes.Result == ast::ParamMode::View)) {
      r.From.push_back(args[i]);
      r.PastFrom.push_back(i < paramTys.size() &&
                           reachesPlace(paramTys[i], resultTy));
    }
}

bool Sema::isInoutResult(const ast::Expr *e) const {
  auto it = BorrowResults.find(e);
  return it != BorrowResults.end() && it->second.Mode == ast::ParamMode::Inout;
}

const Sema::BorrowResult *Sema::borrowResultOf(const ast::Expr *e) const {
  auto it = BorrowResults.find(placeBase(e));
  return it == BorrowResults.end() ? nullptr : &it->second;
}

std::string Sema::borrowedResultError(const ast::Expr *e, bool holder) {
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e)) {
    std::string why = borrowedResultError(te->getTrueExpr(), holder);
    return why.empty() ? borrowedResultError(te->getFalseExpr(), holder) : why;
  }
  const bool inout = CurrentResult.Mode == ast::ParamMode::Inout;
  const std::string what =
      inout ? "an 'inout' result must be part of 'self' or of an 'inout' "
              "parameter"
            : "a 'view' result must be part of 'self' or of a 'view' or "
              "'inout' parameter";
  // A field or element of a conditional's object, `(if c then a else b).n`,
  // is in what one of its branches holds: each must hold what may be
  // returned.  Whether it is an `inout` place is checked once, of the whole
  // place: a conditional's struct, tuple or array is a temporary, and a
  // field of its object is in the object (frozenPlace looks at each
  // branch).
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(placeBase(e))) {
    if (inout && !holder)
      if (std::string why = inoutPlaceError(e, "returned 'inout'");
          !why.empty())
        return why;
    std::string why = borrowedResultError(te->getTrueExpr(), /*holder=*/true);
    return why.empty() ? borrowedResultError(te->getFalseExpr(), true) : why;
  }
  // A borrow of what a call borrowed: each of those must be ours to lend,
  // and the place in the call's `inout` result one an `inout` could name
  // (not an element of a tuple or a character of a string).
  if (const BorrowResult *r = borrowResultOf(e)) {
    if (inout && r->Mode != ast::ParamMode::Inout)
      return "the result of '" + r->Callee +
             "' is a 'view'; it cannot be returned 'inout'";
    if (inout && !holder)
      if (std::string why = inoutPlaceError(e, "returned 'inout'");
          !why.empty())
        return why;
    for (const ast::Expr *from : r->From) {
      // `self` is not returned when it is the receiver: what the call
      // returns is part of it.
      const auto *id = ast::dyn_cast<ast::Identifier>(from);
      if (from == r->Receiver && id && id->getName() == names::kSelf)
        continue;
      if (std::string why = borrowedResultError(from); !why.empty())
        return why;
    }
    return "";
  }
  // The storage an `inout` result names is an `inout` argument's.
  if (inout && !holder)
    if (std::string why = inoutPlaceError(e, "returned 'inout'"); !why.empty())
      return why;
  const auto *id = ast::dyn_cast<ast::Identifier>(placeBase(e));
  if (!id)
    return what + ", not a new value";
  const std::string &name = id->getName();
  const Scope *owner = CurrentScope ? CurrentScope->findOwner(name) : nullptr;
  // A `match` arm's name for a `view` is part of the subject, which cannot
  // change while the arm runs.
  if (owner && varKind(name) == VarKind::ViewBinding)
    if (auto it = owner->ViewSubjects.find(name);
        it != owner->ViewSubjects.end())
      return borrowedResultError(it->second);
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

int Sema::inoutReach(const ast::Expr *e, const ast::Expr *&past,
                     const BorrowResult *&via) const {
  // A field of a class is in the object its receiver refers to; one of a
  // struct, a tuple's element and an array's are in the receiver's storage.
  if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e)) {
    const ast::Expr *recv = ma->getReceiver();
    const int left = inoutReach(recv, past, via);
    if (left < 0 || !ast::isa<ast::ClassType>(recv->getResolvedType()))
      return left;
    if (left == 0) {
      past = recv;
      via = nullptr;
    }
    return left - 1;
  }
  if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(e))
    return inoutReach(ti->getTuple(), past, via);
  if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(e))
    return inoutReach(se->getArray(), past, via);
  // A conditional's is either branch's: as far as the farther.
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e)) {
    int reach = inoutReach(te->getTrueExpr(), past, via);
    const ast::Expr *p = nullptr;
    const BorrowResult *v = nullptr;
    if (const int other = inoutReach(te->getFalseExpr(), p, v); other < reach) {
      reach = other;
      past = p;
      via = v;
    }
    return reach;
  }
  // A call's `inout` result is in what it borrowed, or in an object one
  // reference past it: as far from what the caller keeps alive as the
  // farthest of those.
  if (auto it = BorrowResults.find(e); it != BorrowResults.end()) {
    const BorrowResult &r = it->second;
    int reach = 1;
    for (size_t i = 0; i < r.From.size(); ++i) {
      const ast::Expr *p = nullptr;
      const BorrowResult *v = nullptr;
      int left = inoutReach(r.From[i], p, v);
      if (left < 0) {
        // What it borrowed is in a separate object, and so is the result.
        if (!v)
          v = &r;
      } else if (r.PastFrom[i]) {
        if (left == 0) {
          p = r.From[i];
          v = &r;
        }
        --left;
      }
      if (left < reach) {
        reach = left;
        past = p;
        via = v;
      }
    }
    return reach;
  }
  // `self` or an `inout` parameter: the objects a slice's array holds are
  // not kept alive.
  return ast::isa<ast::SliceType>(e->getResolvedType()) ? 0 : 1;
}

std::string Sema::separateObjectError(const ast::Expr *e) const {
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e)) {
    std::string why = separateObjectError(te->getTrueExpr());
    return why.empty() ? separateObjectError(te->getFalseExpr()) : why;
  }
  const ast::Expr *past = nullptr;
  const BorrowResult *via = nullptr;
  if (inoutReach(e, past, via) >= 0)
    return "";
  std::string msg =
      "'" + displayName(CurrentResult.Fn->getName()) + "' returns ";
  msg +=
      via ? "the 'inout' result of '" + via->Callee + "', which can point into "
          : std::string("an 'inout' reference into ");
  // The object `past` refers to, or one a value holds; a slice's are in
  // its caller's array.  A call's `inout` result is no object: the call
  // names the object its reference refers to, and its receiver or argument
  // where the result is.
  const ast::Identifier *root = placeRoot(past);
  const bool slice = root && ast::isa<ast::SliceType>(root->getResolvedType());
  std::string object = "that object";
  if (auto call = BorrowResults.find(past); call != BorrowResults.end()) {
    msg += "a separate object, the one referred to by the 'inout' result of '" +
           call->second.Callee + "', which can point into " +
           resultPlaceText(call->second);
  } else {
    object = "'" + placeText(past) + "'";
    if (slice)
      msg += "an object of the slice '" + root->getName() + "'";
    else if (ast::isa<ast::ClassType>(past->getResolvedType()))
      msg += object + ", a separate object";
    else
      msg += "an object " + object + " holds";
  }
  msg += ": an 'inout' result can only point into 'self' or into an "
         "'inout' parameter's storage";
  if (slice)
    msg += ", and its caller keeps no object in a slice alive";
  return msg + "; return a copy, or call an accessor on " + object +
         " directly";
}

std::string Sema::resultPlaceText(const BorrowResult &r) const {
  // The farthest of what it borrowed, as inoutReach counts it.
  size_t farthest = 0;
  int least = 0;
  for (size_t i = 0; i < r.From.size(); ++i) {
    const ast::Expr *past = nullptr;
    const BorrowResult *via = nullptr;
    const int left = inoutReach(r.From[i], past, via) - (r.PastFrom[i] ? 1 : 0);
    if (i == 0 || left < least) {
      farthest = i;
      least = left;
    }
  }
  const ast::Expr *from = r.From[farthest];
  std::string place = "'" + placeText(from) + "'";
  // A class-typed one's object is where it can point (as for `self.c`); a
  // value's, its storage or an object it holds.
  if (r.PastFrom[farthest] &&
      !ast::isa<ast::ClassType>(from->getResolvedType()))
    return "an object " + place + " holds";
  return place;
}

} // namespace sema
} // namespace paykan
