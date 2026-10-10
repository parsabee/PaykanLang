// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Exclusivity of local borrows (docs/language/01-language-basics.md,
// "Local borrows"): while an `inout` local is live, the variable it names
// cannot be used except through it, and while a `view` local of a variable
// is live, the variable cannot change.  A borrow is live from its
// declaration to the last statement of its block that uses it; a loop that
// uses it is one statement, so the borrow stays live for the whole loop.
//
// A borrow of a borrow (`y: inout = x;` with `x: inout = k;`) borrows what
// that one names too, so `k` stays borrowed while `y` is live.  A `match`
// arm's `n: view T` borrows the subject in the same way, from the start of
// the arm.

#include "Sema.h"

#include <utility>
#include <vector>

namespace paykan {
namespace sema {

namespace {

bool mentions(const ast::Stmt *s, const std::string &name);

/// Whether @p e uses the name @p name.  A kind it does not know counts as a
/// use, which can only keep a borrow live longer.
bool mentions(const ast::Expr *e, const std::string &name) {
  if (!e)
    return false;
  auto any = [&](const std::vector<ast::Expr *> &es) {
    for (const ast::Expr *x : es)
      if (mentions(x, name))
        return true;
    return false;
  };
  switch (e->getKind()) {
  case ast::ASTNode::NK_Identifier:
    return ast::cast<ast::Identifier>(e)->getName() == name;
  case ast::ASTNode::NK_IntegerLiteral:
  case ast::ASTNode::NK_FloatLiteral:
  case ast::ASTNode::NK_BoolLiteral:
  case ast::ASTNode::NK_CharLiteral:
  case ast::ASTNode::NK_NoneLiteral:
  case ast::ASTNode::NK_StringLiteral:
  case ast::ASTNode::NK_EnumValueExpr:
    return false;
  case ast::ASTNode::NK_UnaryExpr:
    return mentions(ast::cast<ast::UnaryExpr>(e)->getOperand(), name);
  case ast::ASTNode::NK_BinaryExpr: {
    const auto *b = ast::cast<ast::BinaryExpr>(e);
    return mentions(b->getLHS(), name) || mentions(b->getRHS(), name);
  }
  case ast::ASTNode::NK_CallExpr:
    return any(ast::cast<ast::CallExpr>(e)->getArguments());
  case ast::ASTNode::NK_MethodCallExpr: {
    const auto *m = ast::cast<ast::MethodCallExpr>(e);
    return mentions(m->getReceiver(), name) || any(m->getArguments());
  }
  case ast::ASTNode::NK_TernaryExpr: {
    const auto *t = ast::cast<ast::TernaryExpr>(e);
    return mentions(t->getCondition(), name) ||
           mentions(t->getTrueExpr(), name) ||
           mentions(t->getFalseExpr(), name);
  }
  case ast::ASTNode::NK_MemberAccessExpr:
    return mentions(ast::cast<ast::MemberAccessExpr>(e)->getReceiver(), name);
  case ast::ASTNode::NK_ArrayLiteralExpr:
    return any(ast::cast<ast::ArrayLiteralExpr>(e)->getElements());
  case ast::ASTNode::NK_SubscriptExpr: {
    const auto *s = ast::cast<ast::SubscriptExpr>(e);
    return mentions(s->getArray(), name) || mentions(s->getIndex(), name);
  }
  case ast::ASTNode::NK_TupleLiteralExpr:
    return any(ast::cast<ast::TupleLiteralExpr>(e)->getElements());
  case ast::ASTNode::NK_TupleIndexExpr:
    return mentions(ast::cast<ast::TupleIndexExpr>(e)->getTuple(), name);
  default:
    return true;
  }
}

bool mentions(const std::vector<ast::Stmt *> &stmts, const std::string &name) {
  for (const ast::Stmt *s : stmts)
    if (mentions(s, name))
      return true;
  return false;
}

bool mentions(const ast::Stmt *s, const std::string &name) {
  if (!s)
    return false;
  switch (s->getKind()) {
  case ast::ASTNode::NK_CompoundStmt:
    return mentions(ast::cast<ast::CompoundStmt>(s)->getStatements(), name);
  case ast::ASTNode::NK_ReturnStmt:
    return mentions(ast::cast<ast::ReturnStmt>(s)->getReturnValue(), name);
  case ast::ASTNode::NK_AssignStmt: {
    const auto *a = ast::cast<ast::AssignStmt>(s);
    return a->getVarName() == name || mentions(a->getValue(), name);
  }
  case ast::ASTNode::NK_DeclStmt: {
    const auto *vd =
        ast::dyn_cast<ast::VarDecl>(ast::cast<ast::DeclStmt>(s)->getDecl());
    return !vd || vd->getName() == name || mentions(vd->getInitExpr(), name);
  }
  case ast::ASTNode::NK_ExprStmt:
    return mentions(ast::cast<ast::ExprStmt>(s)->getExpr(), name);
  case ast::ASTNode::NK_IfStmt: {
    const auto *i = ast::cast<ast::IfStmt>(s);
    return mentions(i->getCondition(), name) ||
           mentions(i->getThenBranch(), name) ||
           mentions(i->getElseBranch(), name);
  }
  case ast::ASTNode::NK_WhileStmt: {
    const auto *w = ast::cast<ast::WhileStmt>(s);
    return mentions(w->getCondition(), name) || mentions(w->getBody(), name);
  }
  case ast::ASTNode::NK_BreakStmt:
  case ast::ASTNode::NK_ContinueStmt:
    return false;
  case ast::ASTNode::NK_MemberAssignStmt: {
    const auto *m = ast::cast<ast::MemberAssignStmt>(s);
    return mentions(m->getReceiver(), name) || mentions(m->getValue(), name);
  }
  case ast::ASTNode::NK_SubscriptAssignStmt: {
    const auto *a = ast::cast<ast::SubscriptAssignStmt>(s);
    return mentions(a->getArray(), name) || mentions(a->getIndex(), name) ||
           mentions(a->getValue(), name);
  }
  case ast::ASTNode::NK_MatchStmt: {
    const auto *m = ast::cast<ast::MatchStmt>(s);
    if (mentions(m->getSubject(), name))
      return true;
    for (const ast::MatchArm *arm : m->getArms())
      if (mentions(arm->getBody(), name))
        return true;
    return false;
  }
  case ast::ASTNode::NK_DestructureStmt: {
    const auto *d = ast::cast<ast::DestructureStmt>(s);
    for (const auto &t : d->getTargets())
      if (!t.isSkip() && t.getName() == name)
        return true;
    return mentions(d->getValue(), name);
  }
  default:
    return true;
  }
}

} // namespace

bool Sema::visitStatements(const std::vector<ast::Stmt *> &stmts) {
  bool ok = true;
  for (size_t i = 0; i < stmts.size(); ++i) {
    if (!visit(stmts[i]))
      ok = false;
    if (const auto *ds = ast::dyn_cast<ast::DeclStmt>(stmts[i]))
      if (const auto *vd = ast::dyn_cast<ast::VarDecl>(ds->getDecl()))
        if (vd->getMode() != ast::ParamMode::Value) {
          // Live until the last later statement that uses it.
          size_t end = i;
          for (size_t j = i + 1; j < stmts.size(); ++j)
            if (mentions(stmts[j], vd->getName()))
              end = j;
          if (end > i)
            startBorrow(vd->getInitExpr(), vd->getName(),
                        vd->getMode() == ast::ParamMode::Inout, &stmts, end);
        }
    // Borrows whose last use was this statement end here.
    std::erase_if(Borrows, [&](const Borrow &b) {
      return b.Block == &stmts && b.End <= i;
    });
  }
  return ok;
}

bool Sema::visitBody(const std::vector<ast::Stmt *> &stmts) {
  // Another function's borrows (one whose body is checked in the middle of
  // this one's, a generic instance) do not reach in.
  std::vector<Borrow> outer = std::exchange(Borrows, {});
  bool ok = visitStatements(stmts);
  Borrows = std::move(outer);
  return ok;
}

void Sema::startBorrow(const ast::Expr *place, const std::string &by,
                       bool inout, const std::vector<ast::Stmt *> *block,
                       size_t end) {
  const ast::Identifier *root = placeRoot(place);
  if (!root)
    return; // a `view` of an expression borrows nothing
  const std::string &name = root->getName();
  const Scope *owner = CurrentScope ? CurrentScope->findOwner(name) : nullptr;
  if (!owner)
    return;
  std::vector<Borrow> added{{name, owner, by, inout, block, end}};
  // A borrow of a borrow borrows what that one names too.
  for (const Borrow &b : Borrows)
    if (b.By == name)
      added.push_back({b.Root, b.RootScope, by, inout, block, end});
  Borrows.insert(Borrows.end(), added.begin(), added.end());
}

bool Sema::visitArmBody(const ast::MatchArm *arm, const ast::Expr *subject) {
  const auto &stmts = arm->getBody()->getStatements();
  if (arm->hasBinding() && arm->getMode() != ast::ParamMode::Value) {
    // `n: view T` borrows the subject from the arm's start to n's last use.
    size_t uses = 0;
    for (size_t j = 0; j < stmts.size(); ++j)
      if (mentions(stmts[j], arm->getBinding()))
        uses = j + 1;
    if (uses > 0)
      startBorrow(subject, arm->getBinding(),
                  arm->getMode() == ast::ParamMode::Inout, &stmts, uses - 1);
  }
  bool ok = visitStatements(stmts);
  std::erase_if(Borrows, [&](const Borrow &b) { return b.Block == &stmts; });
  return ok;
}

const Sema::Borrow *Sema::activeBorrow(const std::string &name,
                                       bool inout) const {
  const Scope *owner = CurrentScope ? CurrentScope->findOwner(name) : nullptr;
  for (const Borrow &b : Borrows)
    if (b.Root == name && b.Inout == inout && b.RootScope == owner)
      return &b;
  return nullptr;
}

bool Sema::checkNotBorrowed(const std::string &name, ast::SourceLocation loc) {
  const Borrow *b = activeBorrow(name, /*inout=*/true);
  if (!b)
    return true;
  // One error for each line that uses it (`a = a + 1` reads and writes).
  if (BorrowedUsesReported.insert({name, loc.getLineStart()}).second)
    error(loc, "'" + name + "' is borrowed by 'inout' local '" + b->By +
                   "' until '" + b->By + "' is last used");
  return false;
}

std::string Sema::viewedBy(const std::string &name) const {
  const Borrow *b = activeBorrow(name, /*inout=*/false);
  if (!b)
    return "";
  return "'" + name + "' is viewed by 'view' local '" + b->By + "' until '" +
         b->By + "' is last used";
}

} // namespace sema
} // namespace paykan
