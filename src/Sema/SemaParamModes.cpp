// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The rules of `view` and `inout` parameters, of any type
// (docs/language/03-functions.md, "Parameter modes"): overrides,
// and the arguments an `inout` parameter takes, none of them overlapping
// anything else the call borrows; and of local borrows
// (docs/language/02-basics.md, "Local borrows").  Assignments to a
// `view` parameter or local are checked with `let` locals'
// (Sema::checkReassignable).

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

/// Whether places @p a and @p b share storage: the same, or one a field
/// chain on the other (`b` and `b.n`, not `b.c` and `b.n`).
bool overlaps(const std::string &a, const std::string &b) {
  const std::string &shorter = a.size() < b.size() ? a : b;
  const std::string &longer = a.size() < b.size() ? b : a;
  return longer.compare(0, shorter.size(), shorter) == 0 &&
         (longer.size() == shorter.size() || longer[shorter.size()] == '.');
}

/// One place a call borrows: an argument's, or the receiver's.
struct CallUse {
  std::string Path;
  std::string What;  // "passed to 'inout' parameter 'm'", ...
  std::string Param; // the parameter's name; "" for the receiver
  bool Writes = false;
};

/// Why @p use cannot share storage with @p earlier in one call; "" if it can.
std::string callConflict(const CallUse &earlier, const CallUse &use) {
  if ((!earlier.Writes && !use.Writes) || !overlaps(earlier.Path, use.Path))
    return "";
  if (earlier.Path == use.Path) {
    if (earlier.Writes && use.Writes && !earlier.Param.empty())
      return "'" + use.Path + "' is passed to two 'inout' parameters ('" +
             earlier.Param + "' and '" + use.Param + "')";
    return "'" + use.Path + "' is " + earlier.What + "; it cannot also be " +
           use.What;
  }
  return "'" + use.Path + "' " +
         (use.Path.size() > earlier.Path.size() ? "is part of '"
                                                : "contains '") +
         earlier.Path + "', " + earlier.What + "; it cannot also be " +
         use.What;
}

/// The first reason any of @p mine, one argument's places, cannot share
/// storage with the earlier arguments' and the receiver's @p uses.
std::string conflictWith(const std::vector<std::vector<CallUse>> &uses,
                         const std::vector<CallUse> &mine) {
  for (const CallUse &use : mine)
    for (const std::vector<CallUse> &earlier : uses)
      for (const CallUse &e : earlier)
        if (std::string why = callConflict(e, use); !why.empty())
          return why;
  return "";
}

} // namespace

void Sema::declareParamKinds(const ast::FuncDecl *fn) {
  CurrentResult = {fn->getResultMode(), fn, CurrentScope};
  for (const ast::Param &p : fn->getParams())
    if (p.Mode != ast::ParamMode::Value)
      CurrentScope->Kinds[p.getName()] =
          p.Mode == ast::ParamMode::View ? VarKind::View : VarKind::Inout;
}

std::string Sema::inoutPlaceError(const ast::Expr *place,
                                  const std::string &use) {
  // A struct's field is in the struct's own bytes: it is a place where the
  // struct is one (a variable, a field of an object or of another struct,
  // an `inout` result), not in a temporary, which dies with the statement,
  // and not in an array element (not yet, like the element itself).
  const ast::Expr *root = place; // the variable (or field) holding it
  for (const ast::Expr *e = place;;) {
    const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e);
    if (!ma || !ast::isa<ast::StructType>(ma->getReceiver()->getResolvedType()))
      break;
    const ast::Expr *recv = ma->getReceiver();
    if (ast::isa<ast::SubscriptExpr>(recv))
      return inoutPlaceError(recv, use);
    if (ast::isa<ast::TupleIndexExpr>(recv))
      return "a field of a struct held in a tuple cannot be " + use +
             ": tuples are immutable";
    if (!ast::isa<ast::Identifier>(recv) &&
        !ast::isa<ast::MemberAccessExpr>(recv) && !isInoutResult(recv))
      return "a field of a struct that is not a variable or a field (a "
             "temporary, which dies with the statement) cannot be " +
             use;
    e = root = recv;
  }
  if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(place)) {
    const ast::Type *arr = se->getArray()->getResolvedType();
    return ast::dyn_cast<ast::FixedArrayType>(arr) ||
                   ast::dyn_cast<ast::SliceType>(arr)
               ? "an array element cannot be " + use + " yet"
               : "a character of a string cannot be " + use;
  }
  const auto *id = ast::dyn_cast<ast::Identifier>(root);
  // A struct method's `self` is the receiver's storage, which an `inout`
  // may name (unless it is read-only, which frozenPlace reports below); a
  // class method's is its object.
  if (id && id->getName() == names::kSelf &&
      !(CurrentClassCtx && CurrentClassCtx->Struct))
    return "'self' cannot be " + use +
           ": the method would no longer know its object";
  if (id && varKind(id->getName()) == VarKind::Let)
    // It could be reassigned through the other name.
    return "'" + id->getName() + "' is declared with 'let' and cannot be " +
           use;
  if (id && varKind(id->getName()) == VarKind::ForVar)
    return "'" + id->getName() +
           "' is the variable of a 'for' loop and "
           "cannot be " +
           use;
  if (id && varKind(id->getName()) == VarKind::Captured)
    return "'" + id->getName() +
           "' is captured by copy by the function, and the copy is read-only; "
           "it cannot be " +
           use;
  // A weak reference is not a place a strong reference could be written to.
  if (id && (varKind(id->getName()) == VarKind::Weak ||
             varKind(id->getName()) == VarKind::WeakView))
    return "'" + id->getName() + "' is a 'weak' reference and cannot be " + use;
  if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(place))
    if (const auto *ct =
            ast::dyn_cast<ast::ClassType>(ma->getReceiver()->getResolvedType());
        ct && ct->isWeakField(ma->getFieldName()))
      return "field '" + ma->getFieldName() + "' of class '" + ct->getName() +
             "' is a 'weak' reference and cannot be " + use;
  std::string why = frozenPlace(place);
  if (!why.empty())
    why += "; it cannot be " + use;
  return why;
}

bool Sema::checkLocalBorrow(const ast::VarDecl *node) {
  const std::string &name = node->getName();
  switch (node->getMode()) {
  case ast::ParamMode::Value:
    return true;
  case ast::ParamMode::View:
    // A `view` local reads its initializer, which may be any expression: a
    // copy of a value type, the same object, string or array otherwise.
    CurrentScope->Kinds[name] = VarKind::ViewLocal;
    return true;
  case ast::ParamMode::Inout:
    break;
  }
  // An `inout` local is another name for a variable or a field, of exactly
  // its type: an assignment to it writes there.
  const ast::Expr *place = node->getInitExpr();
  const std::string local = "'inout' local '" + name + "'";
  ast::Type *placeTy = place ? place->getResolvedType() : nullptr;
  std::string msg;
  if (!placeTy || !node->getType())
    return false; // the initializer's error was reported
  if (!ast::isa<ast::Identifier>(place) &&
      !ast::isa<ast::MemberAccessExpr>(place) &&
      !ast::isa<ast::SubscriptExpr>(place) && !isInoutResult(place))
    msg = local + " must name a variable or a field";
  else if (!typesEqual(placeTy, node->getType()))
    msg = local + " has type '" + typeName(node->getType()) +
          "', but what it names has type '" + typeName(placeTy) + "'";
  else
    msg = inoutPlaceError(place, "named by " + local);
  CurrentScope->Kinds[name] = VarKind::Inout;
  if (msg.empty())
    return true;
  error(place->getLocation(), msg);
  return false;
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
  // So is how the result is returned (a borrow is not a copy).
  ast::ParamMode want = base->getParamModes().Result;
  if (method->getResultMode() != want) {
    error(method->getLocation(),
          want == ast::ParamMode::Value
              ? "override of '" + method->getName() +
                    "' cannot return a borrow: the method it overrides "
                    "returns a copy"
              : "override of '" + method->getName() + "' must return '" +
                    ast::paramModeName(want) +
                    "', like the method it overrides");
    ok = false;
  }
  return ok;
}

void Sema::borrowedPaths(const ast::Expr *e, std::vector<std::string> &out) {
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e)) {
    borrowedPaths(te->getTrueExpr(), out);
    borrowedPaths(te->getFalseExpr(), out);
  } else if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(e)) {
    borrowedPaths(se->getArray(), out);
  } else if (const BorrowResult *r = borrowResultOf(e)) {
    for (const ast::Expr *from : r->From)
      borrowedPaths(from, out);
  } else if (std::string path = placePath(e); !path.empty()) {
    out.push_back(std::move(path));
  }
}

bool Sema::checkInoutArgs(const ast::ParamModes &modes,
                          const std::vector<ast::Type *> &paramTys,
                          const std::vector<ast::Expr *> &args,
                          const std::string &callee, const ast::Expr *receiver,
                          const ast::MethodDecl *method) {
  bool ok = true;
  // Exclusivity: what each `inout` argument names may not share storage
  // with anything else the call borrows, the receiver included: another
  // `inout` argument, a `view` one, or the receiver of a method that may
  // change it, or of a `view fn`.  Places are compared as written, so
  // different chains that might reach the same object at run time (`a.n`
  // and `b.n`) are not checked, and a plain parameter is a copy.
  std::vector<std::vector<CallUse>> uses;
  // A struct's method that may change `self` changes the receiver's
  // storage, which the lowering must know of (ast::Expr::isChangedByCall).
  if (receiver && method && !method->isView() &&
      ast::dyn_cast<ast::StructType>(receiver->getResolvedType()))
    const_cast<ast::Expr *>(receiver)->setChangedByCall(true);
  if (receiver && method) {
    std::vector<std::string> paths;
    borrowedPaths(receiver, paths);
    std::vector<CallUse> recv;
    recv.reserve(paths.size());
    for (std::string &path : paths)
      recv.push_back(
          {std::move(path),
           method->isView()
               ? "the receiver of 'view fn " + callee + "'"
               : "the receiver of '" + callee + "' (not a 'view fn')",
           "", !method->isView()});
    uses.push_back(std::move(recv));
  }
  for (size_t i = 0; i < args.size() && i < paramTys.size(); ++i) {
    ast::Expr *arg = args[i];
    ast::Type *argTy = arg->getResolvedType();
    // An argument of the wrong type was reported by the call's own check.
    if (!argTy || !isAssignable(paramTys[i], argTy))
      continue;
    if (modes.mode(i) == ast::ParamMode::View) {
      // The lowering keeps what the argument is part of alive for the call
      // (ast::Expr::isViewedByCall).
      arg->setViewedByCall(true);
      std::vector<std::string> paths;
      borrowedPaths(arg, paths);
      std::vector<CallUse> mine;
      mine.reserve(paths.size());
      for (std::string &path : paths)
        mine.push_back({std::move(path),
                        "passed to 'view' parameter '" + modes.name(i) + "'",
                        modes.name(i), false});
      std::string msg = conflictWith(uses, mine);
      uses.push_back(std::move(mine));
      if (!msg.empty()) {
        error(arg->getLocation(), msg);
        ok = false;
      }
      continue;
    }
    if (modes.mode(i) != ast::ParamMode::Inout)
      continue;
    const std::string param = "'inout' parameter '" + modes.name(i) + "'";
    const std::string argN =
        "argument " + std::to_string(i + 1) + " of '" + callee + "'";
    std::string msg;
    if (ast::isa<ast::SubscriptExpr>(arg)) {
      msg = inoutPlaceError(arg, "passed to " + param);
    } else if (!ast::isa<ast::Identifier>(arg) &&
               !ast::isa<ast::MemberAccessExpr>(arg) && !isInoutResult(arg)) {
      msg = argN + " must be a variable or a field: parameter '" +
            modes.name(i) + "' is 'inout'";
    } else if (!typesEqual(argTy, paramTys[i]) &&
               !ast::dyn_cast<ast::SliceType>(paramTys[i])) {
      // The callee reads and writes the storage as the parameter's type, so
      // no conversion can happen on the way: not int -> float, and not a
      // subclass to its base (the callee could store another subclass).  A
      // slice takes a fixed-size array of exactly its element type.
      msg = argN + " has type '" + typeName(argTy) + "', but ";
      msg += param + " has type '" + typeName(paramTys[i]) + "'";
    } else if (std::string why = inoutPlaceError(arg, "passed to " + param);
               !why.empty()) {
      msg = std::move(why);
    } else {
      arg->setChangedByCall(true);
      // An `inout` result passes on what its call borrowed.
      std::vector<std::string> paths;
      borrowedPaths(arg, paths);
      std::vector<CallUse> mine;
      mine.reserve(paths.size());
      for (std::string &path : paths)
        mine.push_back(
            {std::move(path), "passed to " + param, modes.name(i), true});
      msg = conflictWith(uses, mine);
      uses.push_back(std::move(mine));
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
