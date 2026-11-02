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
    // A value is copied, a handle shared.
    if (ast::isa<ast::AtomicType>(p) || pirType(Ctx, p) != pir::Type::Box)
      continue;
    // A reference crosses by `move`, or as a temporary of the caller's.
    auto *id = ast::dyn_cast<ast::Identifier>(args[i]);
    auto *u = ast::dyn_cast<ast::UnaryExpr>(args[i]);
    if (u && u->getOpcode() == ast::UnaryOpcode::Move)
      id = ast::dyn_cast<ast::Identifier>(u->getOperand());
    if (id && SharedVars.count(id->getName())) {
      error(args[i]->getLocation(),
            "'" + id->getName() +
                "' is 'shared': it never crosses to a task (a 'shared' "
                "reference is aliased on its own thread)");
      ok = false;
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

} // namespace paykan::sema
