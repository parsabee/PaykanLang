// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The concurrency prototype's checks (docs/design/concurrency.md): what a
// `parallel for` body may read and write, what an async call takes across
// to its task (the send check), and assignments to `atomic` variables.

#include "Names.h"
#include "NativeABI.h"
#include "Sema.h"

namespace paykan::sema {

namespace {

std::string at(ast::SourceLocation loc) {
  return std::to_string(loc.getLineStart()) + ":" +
         std::to_string(loc.getColumnStart());
}

/// A value of plain parts: a number, bool, char or enum.
bool isPlainValue(ast::Type *ty) {
  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(ty))
    return bt->getTypeKind() != ast::BuiltinType::Void;
  return ast::isa<ast::EnumType>(ty);
}

} // namespace

bool Sema::isOuterScope(Scope *owner) const {
  if (ParallelStack.empty() || !owner)
    return false;
  for (Scope *s = owner; s; s = s->Parent)
    if (s == ParallelStack.back().Boundary)
      return false; // bound by the loop, or inside its body
  return true;
}

bool Sema::checkParallelRead(const std::string &name, ast::Type *ty,
                             ast::SourceLocation loc) {
  if (!isOuterScope(CurrentScope->findOwner(name)))
    return true;
  if (isPlainValue(ty) || ast::isa<ast::AtomicType>(ty))
    return true;
  // A handle: a `locked` cell or an `atomic const` object.
  if (const ast::Qualifier q = qualifierOfVar(name);
      q.Form == ast::QualForm::Locked || q.Form == ast::QualForm::AtomicConst)
    return true;
  ast::Type *elem = nullptr;
  if (auto *a = ast::dyn_cast<ast::ArrayType>(ty))
    elem = a->getElementType();
  else if (auto *f = ast::dyn_cast<ast::FixedArrayType>(ty))
    elem = f->getElementType();
  else if (auto *s = ast::dyn_cast<ast::SliceType>(ty))
    elem = s->getElementType();
  if (elem && isPlainValue(elem))
    return true;
  error(loc, "'" + name + "' (a '" + typeName(ty) +
                 "') cannot be used in a 'parallel for' body: an iteration "
                 "uses only values, arrays of values and 'atomic' variables "
                 "of the enclosing function");
  return false;
}

bool Sema::checkParallelAssign(ast::AssignStmt *node, ast::Type *varTy) {
  const std::string &name = node->getVarName();
  ast::Expr *value = node->getValue();
  ast::Type *valTy = resolveExprType(value, varTy);
  if (!valTy)
    return false;
  // `s += e`, `s -= e` or `s *= e` on a number: a reduction.
  auto *bt = ast::dyn_cast<ast::BuiltinType>(varTy);
  auto *be = ast::dyn_cast<ast::BinaryExpr>(value);
  auto *lhs = be ? ast::dyn_cast<ast::Identifier>(be->getLHS()) : nullptr;
  const bool reduction = bt && bt->isNumeric() && lhs &&
                         lhs->getName() == name &&
                         (be->getOpcode() == ast::BinaryOpcode::Add ||
                          be->getOpcode() == ast::BinaryOpcode::Sub ||
                          be->getOpcode() == ast::BinaryOpcode::Mul);
  if (!reduction) {
    error(node->getLocation(),
          "cannot assign to '" + name +
              "' in a 'parallel for' body: a variable of the enclosing "
              "function changes only by a reduction ('" +
              name + " += e', '" + name + " -= e', '" + name +
              " *= e') or, if 'atomic', by its "
              "operations");
    return false;
  }
  if (!checkAssignable(varTy, valTy, value)) {
    error(node->getLocation(), "cannot assign value of type '" +
                                   typeName(valTy) + "' to variable '" + name +
                                   "' of type '" + typeName(varTy) + "'");
    return false;
  }
  ParallelStack.back().Loop->addReduction(Ctx.intern(name), be->getOpcode());
  return true;
}

bool Sema::checkAtomicAssign(ast::AssignStmt *node, ast::AtomicType *at) {
  const std::string &name = node->getVarName();
  ast::Expr *value = node->getValue();
  ast::Type *inner = at->getInnerType();
  // `x += e` and `x -= e`: one atomic add.
  if (auto *be = ast::dyn_cast<ast::BinaryExpr>(value)) {
    auto *lhs = ast::dyn_cast<ast::Identifier>(be->getLHS());
    const bool add = be->getOpcode() == ast::BinaryOpcode::Add;
    const bool sub = be->getOpcode() == ast::BinaryOpcode::Sub;
    if (lhs && lhs->getName() == name && (add || sub)) {
      ast::Type *rhsTy = resolveExprType(be->getRHS(), inner);
      if (!rhsTy)
        return false;
      if (!checkAssignable(inner, rhsTy, be->getRHS())) {
        error(node->getLocation(), "cannot add a '" + typeName(rhsTy) +
                                       "' to '" + name + "', an '" +
                                       typeName(at) + "'");
        return false;
      }
      lhs->setResolvedType(inner);
      be->setResolvedType(inner);
      node->setAtomicKind(add ? ast::AssignStmt::AtomicKind::Add
                              : ast::AssignStmt::AtomicKind::Sub);
      return true;
    }
  }
  ast::Type *valTy = resolveExprType(value, inner);
  if (!valTy)
    return false;
  if (!checkAssignable(inner, valTy, value)) {
    error(node->getLocation(), "cannot store a '" + typeName(valTy) + "' in '" +
                                   name + "', an '" + typeName(at) + "'");
    return false;
  }
  node->setAtomicKind(ast::AssignStmt::AtomicKind::Store);
  return true;
}

bool Sema::checkSendArgs(const ast::CallExpr *call, const FunctionSig &sig) {
  if (!ParallelStack.empty()) {
    error(call->getLocation(),
          "a task cannot be started in a 'parallel for' body");
    return false;
  }
  const std::vector<ast::Expr *> &args = call->getArguments();
  bool ok = true;
  for (size_t i = 0; i < args.size() && i < sig.ParamTypes.size(); ++i) {
    ast::Type *p = sig.ParamTypes[i];
    // A value is copied, a cell shared.
    if (ast::isa<ast::AtomicType>(p) || pirType(Ctx, p) != pir::Type::Box)
      continue;
    // A handle (`locked`, `atomic const`) is copied; a `shared` reference
    // never crosses; a unique or `const` one crosses by `move`, or as a
    // temporary of the caller's.
    const ast::Qualifier q = qualifierOf(args[i]);
    auto *id = ast::dyn_cast<ast::Identifier>(args[i]);
    auto *u = ast::dyn_cast<ast::UnaryExpr>(args[i]);
    if (u && u->getOpcode() == ast::UnaryOpcode::Move)
      id = ast::dyn_cast<ast::Identifier>(u->getOperand());
    if (q.Form == ast::QualForm::Shared) {
      error(args[i]->getLocation(),
            "'" + placeText(u ? u->getOperand() : args[i]) + "' is '" +
                q.spelling() +
                "': it never crosses to a task (a 'shared' reference is "
                "aliased on its own thread)");
      ok = false;
    } else if (q.Form == ast::QualForm::Locked ||
               q.Form == ast::QualForm::AtomicConst) {
      continue; // a handle: another reference to the same cell or object
    } else if (id && !u) {
      error(args[i]->getLocation(), "cannot copy '" + id->getName() +
                                        "' (a unique '" + typeName(p) +
                                        "') into a task: write 'move " +
                                        id->getName() + "' to transfer it");
      ok = false;
    }
  }
  return ok;
}

ast::Type *Sema::checkMove(ast::UnaryExpr *node) {
  auto *id = ast::dyn_cast<ast::Identifier>(node->getOperand());
  if (!id) {
    error(node->getLocation(), "'move' takes a variable: a local or a "
                               "by-value parameter");
    return nullptr;
  }
  const std::string &name = id->getName();
  if (name == names::kSelf) {
    error(node->getLocation(),
          "cannot move 'self': a method borrows its receiver");
    return nullptr;
  }
  switch (varKind(name)) {
  case VarKind::Plain:
  case VarKind::Let:
  case VarKind::ForVar: // an integer: read as it is
    break;
  case VarKind::Inout:
    error(node->getLocation(), "cannot move '" + name +
                                   "': it is an 'inout' parameter, the "
                                   "caller's storage");
    return nullptr;
  case VarKind::View:
  case VarKind::ViewLocal:
  case VarKind::ViewBinding:
    error(node->getLocation(),
          "cannot move '" + name + "': it is a 'view', a borrow");
    return nullptr;
  case VarKind::Weak:
  case VarKind::WeakView:
    error(node->getLocation(), "cannot move '" + name +
                                   "': a 'weak' variable holds no strong "
                                   "reference");
    return nullptr;
  case VarKind::Captured:
  case VarKind::CapturedWeak:
    error(node->getLocation(),
          "cannot move '" + name + "': the function captures it by copy");
    return nullptr;
  }
  ast::Type *ty = resolveExprType(id); // live, and not moved already
  if (!ty)
    return nullptr;
  if (auto depth = DeclLoopDepth.find(name);
      depth != DeclLoopDepth.end() && depth->second < LoopDepth) {
    error(node->getLocation(),
          "'" + name +
              "' is moved in a loop it is declared outside of: the "
              "next iteration would read it after the move");
    return nullptr;
  }
  MovedVars[name] = node->getLocation();
  return ty;
}

bool Sema::checkNotMoved(const std::string &name, ast::SourceLocation loc) {
  auto moved = MovedVars.find(name);
  if (moved == MovedVars.end())
    return true;
  error(loc, "'" + name + "' was moved at " + at(moved->second) +
                 "; it cannot be used after the move");
  return false;
}

// -- Qualifiers on places (docs/design/concurrency.md, "The qualifiers")

ast::Qualifier Sema::qualifierOfVar(std::string_view name) const {
  for (const Scope *s = CurrentScope; s; s = s->Parent) {
    if (!s->contains(name))
      continue;
    auto it = s->Quals.find(name);
    return it == s->Quals.end() ? ast::Qualifier{} : it->second;
  }
  return {};
}

ast::Qualifier Sema::qualifierOf(const ast::Expr *e) {
  if (!e)
    return {};
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e))
    return qualifierOf(te->getTrueExpr());
  if (const auto *u = ast::dyn_cast<ast::UnaryExpr>(e))
    return u->getOpcode() == ast::UnaryOpcode::Move
               ? qualifierOf(u->getOperand())
               : ast::Qualifier{};
  if (const auto *id = ast::dyn_cast<ast::Identifier>(e))
    return id->isFunctionRef() ? ast::Qualifier{}
                               : qualifierOfVar(id->getName());
  if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e)) {
    const ast::Qualifier recv = qualifierOf(ma->getReceiver());
    ast::Type *rt = ast::stripOptional(
        const_cast<ast::Expr *>(ma->getReceiver())->getResolvedType());
    ast::Qualifier q;
    if (const auto *ct = ast::dyn_cast<ast::ClassType>(rt))
      q = ct->findFieldQualifier(ma->getFieldName());
    else if (const auto *st = ast::dyn_cast<ast::StructType>(rt))
      q = st->findFieldQualifier(ma->getFieldName());
    // A field reached through a `const` reference reads as `const`.
    if (recv.isConst() &&
        (q.Form == ast::QualForm::Unique || q.Form == ast::QualForm::Shared))
      q.Const = true;
    return q;
  }
  if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(e))
    return qualifierOf(se->getArray()); // the elements carry the array's
  if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(e))
    return qualifierOf(ti->getTuple());
  if (const auto *ce = ast::dyn_cast<ast::CallExpr>(e)) {
    if (const FunctionSig *sig = lookupFunction(ce->getCalleeName()))
      return sig->ResultQual;
    return {};
  }
  if (const auto *mc = ast::dyn_cast<ast::MethodCallExpr>(e)) {
    ast::Type *rt = ast::stripOptional(
        const_cast<ast::Expr *>(mc->getReceiver())->getResolvedType());
    if (const auto *ct = ast::dyn_cast<ast::ClassType>(rt))
      if (const ast::MethodDecl *m = ct->findMethod(mc->getMethodName()))
        return m->getResultQual();
    return {};
  }
  return {};
}

std::string Sema::qualifiedName(ast::Qualifier q, ast::Type *ty) {
  std::string s = q.spelling();
  if (!s.empty() && ty)
    s += ' ';
  return s + (ty ? typeName(ty) : std::string());
}

bool Sema::checkQualifierApplies(ast::Qualifier q, ast::Type *inner,
                                 ast::SourceLocation loc) {
  if (q.isUnique() || !inner)
    return true;
  const bool ref = sharesStorage(inner);
  switch (q.Form) {
  case ast::QualForm::Unique: // `const`
    if (!ref) {
      error(loc, "'const' applies to objects; '" + typeName(inner) +
                     "' is a value, which 'let' fixes");
      return false;
    }
    return true;
  case ast::QualForm::Shared:
    if (!ref) {
      error(loc, "'shared' qualifies a reference; '" + typeName(inner) +
                     "' is a value");
      return false;
    }
    return true;
  case ast::QualForm::AtomicConst:
    if (!ref) {
      error(loc, "'atomic const' qualifies a reference; '" + typeName(inner) +
                     "' is a value (the prototype does not box values)");
      return false;
    }
    return true;
  case ast::QualForm::Locked:
    if (!ast::isRefType(inner)) {
      error(loc, "'locked' holds an object in the prototype, not a '" +
                     typeName(inner) + "' (wrap the value in a class)");
      return false;
    }
    return true;
  }
  return true;
}

bool Sema::checkQualifiedFlow(ast::Qualifier dst, ast::Expr *src,
                              const std::string &what,
                              ast::SourceLocation loc) {
  ast::Type *ty = src ? src->getResolvedType() : nullptr;
  if (!ty || !sharesStorage(ty) || ast::isa<ast::PoisonType>(ty))
    return true; // a value is copied
  if (ast::isa<ast::NoneLiteral>(src))
    return true;
  const ast::Expr *inner = src;
  bool moved = false;
  if (const auto *u = ast::dyn_cast<ast::UnaryExpr>(src);
      u && u->getOpcode() == ast::UnaryOpcode::Move) {
    moved = true;
    inner = u->getOperand();
  }
  const bool place = ast::isa<ast::Identifier>(inner) ||
                     ast::isa<ast::MemberAccessExpr>(inner) ||
                     ast::isa<ast::SubscriptExpr>(inner) ||
                     ast::isa<ast::TupleIndexExpr>(inner);
  const ast::Qualifier sq = qualifierOf(src);
  // The conversions: a unique value into any form, a `const` one into the
  // other `const` forms, a handle or a `shared` reference only as it is.
  bool ok = sq.isUnique() || sq == dst;
  if (!ok && sq.Form == ast::QualForm::Unique && sq.Const)
    ok = dst.isConst() && dst.Form != ast::QualForm::Locked;
  if (!ok) {
    error(loc, "cannot store a '" + qualifiedName(sq, ty) + "' in " + what +
                   ", a '" + qualifiedName(dst, ty) +
                   "': a reference converts only from unique, or from "
                   "'const' into the other 'const' forms");
    return false;
  }
  // A unique (or `const`) value that becomes a cell, or a frozen object,
  // is made into one at this use.
  if (sq.Form == ast::QualForm::Unique && dst.Form != ast::QualForm::Unique &&
      dst.Form != ast::QualForm::Shared)
    src->setMaterialize(dst.Form);
  // The copy rule (--strict-ownership): a move-only value read from a
  // place is moved, never copied.  A `shared` reference and a handle are
  // Copyable.
  if (StrictOwnership && place && !moved && sq.Form == ast::QualForm::Unique) {
    const std::string text = placeText(inner);
    std::string msg =
        "cannot copy '" + text + "' (a unique '" + qualifiedName(sq, ty) + "')";
    if (ast::isa<ast::Identifier>(inner))
      msg +=
          ": write 'move " + text + "' to transfer it, or declare it '" +
          qualifiedName(ast::Qualifier{ast::QualForm::Shared, sq.Const}, ty) +
          "'";
    else
      msg += ": a field or element leaves by 'mem::replace' or 'mem::take', "
             "or is declared 'shared " +
             typeName(ty) + "'";
    error(loc, msg);
    return false;
  }
  return true;
}

std::string Sema::constPlace(const ast::Expr *e) {
  for (const ast::Expr *p = e;;) {
    if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(p)) {
      ast::Type *rt = ast::stripOptional(
          const_cast<ast::Expr *>(ma->getReceiver())->getResolvedType());
      ast::Qualifier q;
      if (const auto *ct = ast::dyn_cast<ast::ClassType>(rt))
        q = ct->findFieldQualifier(ma->getFieldName());
      else if (const auto *st = ast::dyn_cast<ast::StructType>(rt))
        q = st->findFieldQualifier(ma->getFieldName());
      if (q.isConst())
        return "field '" + ma->getFieldName() + "' is '" + q.spelling() + "'";
      p = ma->getReceiver();
    } else if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(p)) {
      p = se->getArray();
    } else if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(p)) {
      p = ti->getTuple();
    } else {
      break;
    }
  }
  if (const auto *id = ast::dyn_cast<ast::Identifier>(placeBase(e))) {
    const ast::Qualifier q = qualifierOfVar(id->getName());
    if (q.isConst())
      return "'" + id->getName() + "' is '" + q.spelling() + "'";
  }
  return "";
}

bool Sema::checkNotLocked(const ast::Expr *place, ast::SourceLocation loc) {
  if (LockOperand || !place)
    return true;
  if (qualifierOf(place).Form != ast::QualForm::Locked)
    return true;
  const std::string text = placeText(place);
  error(loc, "'" + text + "' is 'locked': reach its contents in 'lock " + text +
                 " as c: view { }' (or 'inout')");
  return false;
}

} // namespace paykan::sema
