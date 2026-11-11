// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Exclusivity of local borrows (docs/language/02-basics.md,
// "Local borrows"): while an `inout` local is live, the variable it names
// cannot be used except through it, and while a `view` local of a variable
// is live, the variable cannot change.  A borrow is live from its
// declaration to the last statement of its block that uses it; a loop that
// uses it is one statement, so the borrow stays live for the whole loop.
//
// A borrow of a borrow (`y: inout = x;` with `x: inout = k;`) borrows what
// that one names too, so `k` stays borrowed while `y` is live.  A `match`
// arm's `n: view T` borrows the subject in the same way, from the start of
// the arm, and a borrow of a call's borrowed result (`v: view = b.peek();`)
// borrows what the call borrowed: its receiver and the arguments to its
// `view` / `inout` parameters.  A lambda's body and a call through a
// function value use the names they hold, no others; a lambda capturing a
// borrowed variable uses it (captureVariable).  A name is a use only where
// it means the borrow: a binder of the same name inside a statement (a
// lambda's parameter, a `for` loop's variable, a `match` arm's binding, a
// declaration in a block, from there to the block's end) starts another
// variable, which the names in its reach are.

#include "Names.h"
#include "Sema.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace paykan {
namespace sema {

namespace {

bool mentions(const ast::Stmt *s, const std::string &name);

/// An expression `mentions` passes over, and the start (line, column) of
/// an `await` whose earlier `await`s' operands it passes over too: what
/// they name was read before that wait (Sema::usedAfterWait).
const ast::Expr *MentionsSkip = nullptr;
std::optional<std::pair<size_t, size_t>> MentionsWaitStart;

/// Whether @p e is an `await` that is over before MentionsWaitStart.
bool awaitedBefore(const ast::Expr *e) {
  const auto *u = ast::dyn_cast<ast::UnaryExpr>(e);
  if (!MentionsWaitStart || !u || u->getOpcode() != ast::UnaryOpcode::Await)
    return false;
  const ast::SourceLocation loc = u->getLocation();
  return std::pair<size_t, size_t>{loc.getLineEnd(), loc.getColumnEnd()} <=
         *MentionsWaitStart;
}

/// The expressions @p e is made of, of the kinds `mentions` knows (a
/// lambda's body is another function's).
template <typename F> void eachPart(const ast::Expr *e, F f) {
  auto all = [&](const std::vector<ast::Expr *> &es) {
    for (const ast::Expr *x : es)
      f(x);
  };
  switch (e->getKind()) {
  case ast::ASTNode::NK_UnaryExpr:
    f(ast::cast<ast::UnaryExpr>(e)->getOperand());
    break;
  case ast::ASTNode::NK_BinaryExpr:
    f(ast::cast<ast::BinaryExpr>(e)->getLHS());
    f(ast::cast<ast::BinaryExpr>(e)->getRHS());
    break;
  case ast::ASTNode::NK_CallExpr:
    all(ast::cast<ast::CallExpr>(e)->getArguments());
    break;
  case ast::ASTNode::NK_CallValueExpr:
    f(ast::cast<ast::CallValueExpr>(e)->getCallee());
    all(ast::cast<ast::CallValueExpr>(e)->getArguments());
    break;
  case ast::ASTNode::NK_MethodCallExpr:
    f(ast::cast<ast::MethodCallExpr>(e)->getReceiver());
    all(ast::cast<ast::MethodCallExpr>(e)->getArguments());
    break;
  case ast::ASTNode::NK_TernaryExpr:
    f(ast::cast<ast::TernaryExpr>(e)->getCondition());
    f(ast::cast<ast::TernaryExpr>(e)->getTrueExpr());
    f(ast::cast<ast::TernaryExpr>(e)->getFalseExpr());
    break;
  case ast::ASTNode::NK_MemberAccessExpr:
    f(ast::cast<ast::MemberAccessExpr>(e)->getReceiver());
    break;
  case ast::ASTNode::NK_ArrayLiteralExpr:
    all(ast::cast<ast::ArrayLiteralExpr>(e)->getElements());
    break;
  case ast::ASTNode::NK_SubscriptExpr:
    f(ast::cast<ast::SubscriptExpr>(e)->getArray());
    f(ast::cast<ast::SubscriptExpr>(e)->getIndex());
    break;
  case ast::ASTNode::NK_TupleLiteralExpr:
    all(ast::cast<ast::TupleLiteralExpr>(e)->getElements());
    break;
  case ast::ASTNode::NK_TupleIndexExpr:
    f(ast::cast<ast::TupleIndexExpr>(e)->getTuple());
    break;
  default:
    break;
  }
}

/// The first `await` in @p e, if it has one.
const ast::UnaryExpr *awaitIn(const ast::Expr *e) {
  if (!e)
    return nullptr;
  if (const auto *u = ast::dyn_cast<ast::UnaryExpr>(e);
      u && u->getOpcode() == ast::UnaryOpcode::Await)
    return u;
  const ast::UnaryExpr *found = nullptr;
  eachPart(e, [&](const ast::Expr *x) {
    if (!found)
      found = awaitIn(x);
  });
  return found;
}

/// Whether @p target is @p e or a part of it.
bool holds(const ast::Expr *e, const ast::Expr *target) {
  if (!e)
    return false;
  if (e == target)
    return true;
  bool found = false;
  eachPart(e, [&](const ast::Expr *x) { found = found || holds(x, target); });
  return found;
}

/// Whether @p e uses the name @p name, as the variable it means where @p e
/// is: not where a binder inside @p e of the same name hides it.  A kind it
/// does not know counts as a use, which can only keep a borrow live longer.
bool mentions(const ast::Expr *e, const std::string &name) {
  if (!e || e == MentionsSkip || awaitedBefore(e))
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
    // A number, a `bool` or a `char` on the left is read before the right
    // is evaluated: before a wait there.
    if (MentionsSkip && b->getLHS()->getResolvedType() &&
        ast::isa<ast::BuiltinType>(b->getLHS()->getResolvedType()) &&
        holds(b->getRHS(), MentionsSkip))
      return mentions(b->getRHS(), name);
    return mentions(b->getLHS(), name) || mentions(b->getRHS(), name);
  }
  case ast::ASTNode::NK_CallExpr: {
    // `f(3)` uses `f` when it is a function value.
    const auto *c = ast::cast<ast::CallExpr>(e);
    return c->getCalleeName() == name || any(c->getArguments());
  }
  case ast::ASTNode::NK_CallValueExpr: {
    const auto *c = ast::cast<ast::CallValueExpr>(e);
    return mentions(c->getCallee(), name) || any(c->getArguments());
  }
  case ast::ASTNode::NK_LambdaExpr: {
    // What its body names (a borrow itself is never captured), unless a
    // parameter of the same name hides it there.
    const auto *l = ast::cast<ast::LambdaExpr>(e);
    for (const ast::Param &p : l->getParams())
      if (p.getName() == name)
        return false;
    return mentions(l->getBody(), name);
  }
  case ast::ASTNode::NK_MethodCallExpr: {
    const auto *m = ast::cast<ast::MethodCallExpr>(e);
    return mentions(m->getReceiver(), name) || any(m->getArguments());
  }
  case ast::ASTNode::NK_TernaryExpr: {
    const auto *t = ast::cast<ast::TernaryExpr>(e);
    // The condition is over before a branch runs, and one branch does.
    if (MentionsSkip && holds(t->getTrueExpr(), MentionsSkip))
      return mentions(t->getTrueExpr(), name);
    if (MentionsSkip && holds(t->getFalseExpr(), MentionsSkip))
      return mentions(t->getFalseExpr(), name);
    return mentions(t->getCondition(), name) ||
           mentions(t->getTrueExpr(), name) ||
           mentions(t->getFalseExpr(), name);
  }
  case ast::ASTNode::NK_MemberAccessExpr:
    return mentions(ast::cast<ast::MemberAccessExpr>(e)->getReceiver(), name);
  case ast::ASTNode::NK_ArrayLiteralExpr: {
    const auto *al = ast::cast<ast::ArrayLiteralExpr>(e);
    // `[v; n]` may evaluate `v` for each element (whether it does is
    // known once `v` is typed): it runs again, as a loop does, so what an
    // `await` in it awaited is read after that wait.
    if (al->isRepeat() && al->getLength() > 1 && MentionsSkip &&
        holds(al->getElements().front(), MentionsSkip)) {
      const ast::Expr *skip = std::exchange(MentionsSkip, nullptr);
      auto start = std::exchange(MentionsWaitStart, std::nullopt);
      const bool used = any(al->getElements());
      MentionsSkip = skip;
      MentionsWaitStart = start;
      return used;
    }
    return any(al->getElements());
  }
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

/// Whether @p s declares a variable named @p name in its block: a local, or
/// a destructuring target with a type (a fresh declaration).  @p before is
/// then what @p s evaluates before the variable exists.
bool declares(const ast::Stmt *s, const std::string &name,
              const ast::Expr *&before) {
  if (const auto *ds = ast::dyn_cast<ast::DeclStmt>(s)) {
    const auto *vd = ast::dyn_cast<ast::VarDecl>(ds->getDecl());
    if (!vd || vd->getName() != name)
      return false;
    before = vd->getInitExpr();
    return true;
  }
  if (const auto *d = ast::dyn_cast<ast::DestructureStmt>(s))
    for (const auto &t : d->getTargets())
      if (t.DeclType && t.getName() == name) {
        before = d->getValue();
        return true;
      }
  return false;
}

/// The statements of a block, in order: a declaration of @p name there
/// hides the variable outside from its initializer on.
bool mentions(const std::vector<ast::Stmt *> &stmts, const std::string &name) {
  for (const ast::Stmt *s : stmts) {
    const ast::Expr *before = nullptr;
    if (declares(s, name, before))
      return mentions(before, name);
    if (mentions(s, name))
      return true;
  }
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
  case ast::ASTNode::NK_LockStmt: {
    const auto *l = ast::cast<ast::LockStmt>(s);
    return mentions(l->getCell(), name) ||
           (l->getBinding()->getName() != name && mentions(l->getBody(), name));
  }
  case ast::ASTNode::NK_ForStmt: {
    // The range and the step are evaluated outside the loop's variable; the
    // body names that variable by its name.
    const auto *f = ast::cast<ast::ForStmt>(s);
    return mentions(f->getStart(), name) || mentions(f->getEnd(), name) ||
           mentions(f->getStep(), name) ||
           (f->getVar()->getName() != name && mentions(f->getBody(), name));
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
    // An arm's binding of the same name hides it in the arm's body.
    for (const ast::MatchArm *arm : m->getArms())
      if (arm->getBinding() != name && mentions(arm->getBody(), name))
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
  StmtCursor.emplace_back(&stmts, 0);
  struct PopCursor {
    Sema &S;
    ~PopCursor() { S.StmtCursor.pop_back(); }
  } popCursor{*this};
  for (size_t i = 0; i < stmts.size(); ++i) {
    StmtCursor.back().second = i;
    const bool checked = visit(stmts[i]);
    if (!checked)
      ok = false;
    // Nothing after a `return` runs: its path's moves join nothing (the
    // prototype's move analysis; `break` and `continue` say so themselves).
    if (ast::isa<ast::ReturnStmt>(stmts[i]))
      FlowEnded = true;
    if (const auto *ds = ast::dyn_cast<ast::DeclStmt>(stmts[i]))
      if (const auto *vd = ast::dyn_cast<ast::VarDecl>(ds->getDecl()))
        if (checked && vd->getMode() != ast::ParamMode::Value) {
          // Live until the last later statement that uses it.  (One that
          // was refused borrows nothing.)
          size_t end = i;
          for (size_t j = i + 1; j < stmts.size(); ++j)
            if (mentions(stmts[j], vd->getName()))
              end = j;
          if (vd->getMode() == ast::ParamMode::View) {
            bool written = false;
            std::string why =
                reachOf(vd->getInitExpr(), /*through=*/false, written);
            ViewLocalReach[varKey(vd->getName())] = {std::move(why), written};
          }
          if (end > i &&
              !startBorrow(vd->getInitExpr(), vd->getName(),
                           vd->getMode() == ast::ParamMode::Inout, &stmts, end))
            ok = false;
        }
    // Borrows whose last use was this statement end here.
    std::erase_if(Borrows, [&](const Borrow &b) {
      return b.Block == &stmts && b.End <= i;
    });
  }
  return ok;
}

std::vector<std::vector<const ast::VarDecl *>>
viewHoldEnds(const std::vector<ast::Stmt *> &stmts) {
  struct Local {
    const ast::VarDecl *Decl;
    size_t End;
  };
  std::vector<Local> locals;
  for (size_t i = 0; i < stmts.size(); ++i) {
    const auto *ds = ast::dyn_cast<ast::DeclStmt>(stmts[i]);
    const auto *vd = ds ? ast::dyn_cast<ast::VarDecl>(ds->getDecl()) : nullptr;
    if (!vd || vd->getMode() == ast::ParamMode::Value)
      continue;
    size_t end = i;
    for (size_t j = i + 1; j < stmts.size(); ++j)
      if (mentions(stmts[j], vd->getName()))
        end = j;
    locals.push_back({vd, end});
  }
  // A borrow of this borrow may lean on its hold; a value computed from it
  // (`w: view = v.n + 1`) does not.
  auto borrowsThrough = [](const ast::Expr *init, const std::string &name) {
    for (;;) {
      if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(init))
        init = ma->getReceiver();
      else if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(init))
        init = se->getArray();
      else if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(init))
        init = ti->getTuple();
      else
        break;
    }
    if (const auto *id = ast::dyn_cast<ast::Identifier>(init))
      return id->getName() == name;
    return (ast::isa<ast::CallExpr>(init) ||
            ast::isa<ast::MethodCallExpr>(init) ||
            ast::isa<ast::TernaryExpr>(init)) &&
           mentions(init, name);
  };
  for (size_t k = locals.size(); k-- > 0;)
    for (size_t m = k + 1; m < locals.size(); ++m)
      if (borrowsThrough(locals[m].Decl->getInitExpr(),
                         locals[k].Decl->getName()))
        locals[k].End = std::max(locals[k].End, locals[m].End);
  std::vector<std::vector<const ast::VarDecl *>> ends(stmts.size());
  for (const Local &l : locals)
    if (l.End + 1 < stmts.size() && !ast::isa<ast::ReturnStmt>(stmts[l.End]) &&
        !ast::isa<ast::BreakStmt>(stmts[l.End]) &&
        !ast::isa<ast::ContinueStmt>(stmts[l.End]))
      ends[l.End].push_back(l.Decl);
  return ends;
}

bool Sema::visitBody(const std::vector<ast::Stmt *> &stmts) {
  // Another function's borrows (one whose body is checked in the middle of
  // this one's, a generic instance) do not reach in.
  std::vector<Borrow> outer = std::exchange(Borrows, {});
  bool ok = visitStatements(stmts);
  Borrows = std::move(outer);
  return ok;
}

bool Sema::startBorrow(const ast::Expr *place, const std::string &by,
                       bool inout, const std::vector<ast::Stmt *> *block,
                       size_t end) {
  // An `inout` borrow of storage the frame does not own cannot be live
  // across an `await` (checkBorrowsAcrossWait): how the place as written
  // is reached, whatever it is rooted in.
  std::string reached;
  if (inout) {
    reached = reachedThrough(place);
    if (CurrentScope && CurrentScope->findOwner(by))
      InoutLocalReach[varKey(by)] = reached;
  }
  const size_t before = Borrows.size();
  borrowRoots(place, by, inout, block, end);
  if (reached.empty())
    return true;
  // A place rooted in no variable (a call's result, a conditional) keeps
  // nothing from being used, and is still borrowed across the wait.
  if (Borrows.size() == before)
    Borrows.push_back({"", nullptr, by, inout, block, end, "", "", {}});
  // The place itself waits: what it read before the wait may be another
  // object's by the time the borrow is used.
  const ast::UnaryExpr *wait = awaitIn(place);
  for (size_t i = before; i < Borrows.size(); ++i) {
    Borrows[i].Reached = reached;
    Borrows[i].Place = placeText(place);
    Borrows[i].At = place->getLocation();
    Borrows[i].AcrossWait = wait != nullptr;
  }
  if (!wait)
    return true;
  error(place->getLocation(),
        "'" + placeText(place) + "' is reached " + reached +
            "; it cannot be borrowed 'inout' across an 'await': another task "
            "may change it there");
  note(wait->getLocation(), "'" + by + "' is used after this 'await'");
  return false;
}

void Sema::borrowRoots(const ast::Expr *place, const std::string &by,
                       bool inout, const std::vector<ast::Stmt *> *block,
                       size_t end) {
  // A borrowed result borrows what its call borrowed.
  if (const BorrowResult *r = borrowResultOf(place)) {
    for (const ast::Expr *from : r->From)
      borrowRoots(from, by, inout, block, end);
    return;
  }
  const ast::Identifier *root = placeRoot(place);
  if (!root)
    return; // a `view` of an expression borrows nothing
  const std::string &name = root->getName();
  const Scope *owner = CurrentScope ? CurrentScope->findOwner(name) : nullptr;
  if (!owner)
    return;
  std::vector<Borrow> added{{name, owner, by, inout, block, end, "", "", {}}};
  // A borrow of a borrow borrows what that one names too.
  for (const Borrow &b : Borrows)
    if (b.By == name && b.RootScope)
      added.push_back({b.Root, b.RootScope, by, inout, block, end, "", "", {}});
  Borrows.insert(Borrows.end(), added.begin(), added.end());
}

std::string Sema::reachedThrough(const ast::Expr *place, bool *stable) {
  bool written = false;
  std::string why = reachOf(place, /*through=*/false, written);
  if (stable)
    *stable = !why.empty() && !written;
  return why;
}

// How the storage @p p names (or, @p through, the storage a part of @p p is
// in) is reached when the frame does not own it.  @p written is set when a
// way there is one something else may write: any but a frozen object's and
// a `shared` one's, which nothing writes.  Every way is looked at: the
// first reason found is the one reported.
std::string Sema::reachOf(const ast::Expr *p, bool through, bool &written) {
  // Of several ways, one something may write is the one to report.
  std::string reason;
  bool reasonWritten = false;
  auto way = [&](const ast::Expr *x, bool part) {
    bool w = false;
    std::string why = reachOf(x, part, w);
    if (!why.empty() && (reason.empty() || (w && !reasonWritten))) {
      reason = std::move(why);
      reasonWritten = w;
    }
    written = written || w;
  };
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(p)) {
    way(te->getTrueExpr(), through);
    way(te->getFalseExpr(), through);
    return reason;
  }
  // A part of what a reference several owners hold names is in their
  // object.  The slot holding such a reference is where its place is.
  if (through) {
    const ast::Qualifier q = qualifierOf(p);
    if (q.Form != ast::QualForm::Unique) {
      if (!(q.Form == ast::QualForm::AtomicShared ||
            (q.Form == ast::QualForm::Shared && !q.Mut)))
        written = true;
      return std::string("through '") + q.form().spelling() + "' reference '" +
             placeText(p) + "'";
    }
  }
  if (const auto *id = ast::dyn_cast<ast::Identifier>(p)) {
    const std::string &name = id->getName();
    if (name == names::kSelf) {
      // A struct's method is given its receiver's storage as an `inout` or
      // a `view` parameter is, which its caller checked (or copied).
      if (CurrentClassCtx && CurrentClassCtx->Struct)
        return "";
      written = true;
      return "through 'self'";
    }
    switch (varKind(name)) {
    case VarKind::View:
      written = true;
      return "through 'view' parameter '" + name + "'";
    case VarKind::ViewLocal:
    case VarKind::ViewBinding: {
      // What it views: the frame's own storage is the frame's.
      auto local = ViewLocalReach.find(varKey(name));
      if (local != ViewLocalReach.end()) {
        written = written || local->second.second;
        return local->second.first;
      }
      written = true;
      return "through 'view' local '" + name + "'";
    }
    case VarKind::Inout: {
      // A local's is its initializer's; a parameter is the frame's own.
      auto local = InoutLocalReach.find(varKey(name));
      if (local == InoutLocalReach.end() || local->second.empty())
        return "";
      written = true;
      return local->second;
    }
    default:
      return "";
    }
  }
  if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(p))
    return reachOf(ma->getReceiver(), /*through=*/true, written);
  if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(p))
    return reachOf(se->getArray(), /*through=*/true, written);
  if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(p))
    return reachOf(ti->getTuple(), /*through=*/true, written);
  // A call's borrowed result is part of what the call borrowed: its
  // receiver and the arguments of its `view` and `inout` parameters.
  if (const BorrowResult *r = borrowResultOf(p)) {
    for (const ast::Expr *from : r->From)
      way(from, /*part=*/true);
    return reason;
  }
  // Any other `inout` result names storage another owner holds; any other
  // temporary is the frame's.
  if (p->isInoutResult()) {
    written = true;
    return "through the result of '" + placeText(p) + "'";
  }
  return "";
}

namespace {

/// Whether @p s is a loop: what is in it runs again.
bool isLoop(const ast::Stmt *s) {
  return ast::isa<ast::WhileStmt>(s) || ast::isa<ast::ForStmt>(s);
}

/// How the statement @p s leaves its block for good.
enum class Leaves { No, Return, Break, Continue };
Leaves leaves(const ast::Stmt *s) {
  if (ast::isa<ast::ReturnStmt>(s))
    return Leaves::Return;
  if (ast::isa<ast::BreakStmt>(s))
    return Leaves::Break;
  if (ast::isa<ast::ContinueStmt>(s))
    return Leaves::Continue;
  return Leaves::No;
}

} // namespace

bool Sema::usedAfterWait(const Borrow &b, const ast::UnaryExpr *wait) const {
  // The statement of each block being checked, from the borrow's own in.
  auto level = std::find_if(StmtCursor.begin(), StmtCursor.end(),
                            [&](const auto &c) { return c.first == b.Block; });
  if (level == StmtCursor.end() || level->second > b.End)
    return false;
  // An `inout` arm's binding goes back to the subject after its last use,
  // or on the way out of the arm: after any wait before that.
  if (b.WritesBack)
    return true;
  // What runs after the wait, from the statement it is in outwards, until
  // the path leaves: a `return` ends it, a `break` or a `continue` takes it
  // to the loop around.
  Leaves out = Leaves::No;
  for (auto at = std::prev(StmtCursor.end());; --at) {
    const std::vector<ast::Stmt *> &stmts = *at->first;
    const ast::Stmt *stmt = stmts[at->second];
    if (std::next(at) == StmtCursor.end()) {
      // The statement the `await` is in.  A `while`'s condition runs
      // again; of any other, what the `await` awaits is read before the
      // wait, and so is what an `await` before this one awaited.
      bool after = false;
      if (ast::isa<ast::WhileStmt>(stmt)) {
        after = mentions(stmt, b.By);
      } else {
        MentionsSkip = wait->getOperand();
        MentionsWaitStart = {wait->getLocation().getLineStart(),
                             wait->getLocation().getColumnStart()};
        after = mentions(stmt, b.By);
        MentionsSkip = nullptr;
        MentionsWaitStart.reset();
      }
      if (after)
        return true;
      out = leaves(stmt);
    } else if (isLoop(stmt)) {
      // A loop around the wait: over on a `break`, and otherwise run
      // again, where whatever it does with the borrow comes after.
      const bool over = out == Leaves::Break;
      if (out != Leaves::Return)
        out = Leaves::No;
      if (!over && mentions(stmt, b.By))
        return true;
    }
    // (A branch not taken and a condition before are not after.)
    if (out == Leaves::Return)
      return false;
    // What follows in the block, to where the path leaves it.
    if (out == Leaves::No)
      for (size_t j = at->second + 1; j < stmts.size(); ++j) {
        // A declaration of the name hides the borrow from there on.
        const ast::Expr *before = nullptr;
        if (at != level && declares(stmts[j], b.By, before)) {
          if (mentions(before, b.By))
            return true;
          break;
        }
        if (mentions(stmts[j], b.By))
          return true;
        out = leaves(stmts[j]);
        if (out == Leaves::Return)
          return false;
        if (out != Leaves::No)
          break;
      }
    if (at == level)
      return false;
  }
}

bool Sema::checkBorrowsAcrossWait(const ast::UnaryExpr *wait) {
  bool ok = true;
  for (Borrow &b : Borrows) {
    if (!b.Inout || b.Reached.empty() || b.AcrossWait)
      continue;
    if (!usedAfterWait(b, wait))
      continue;
    // Once for a borrow, whatever it borrows and however often it waits.
    for (Borrow &same : Borrows)
      if (same.By == b.By && same.Block == b.Block)
        same.AcrossWait = true;
    error(b.At, "'" + b.Place + "' is reached " + b.Reached +
                    "; it cannot be borrowed 'inout' across an 'await': "
                    "another task may change it there");
    note(wait->getLocation(), "'" + b.By + "' is used after this 'await'");
    ok = false;
  }
  return ok;
}

bool Sema::visitArmBody(ast::MatchArm *arm, const ast::Expr *subject) {
  const auto &stmts = arm->getBody()->getStatements();
  bool waits = true;
  if (arm->hasBinding() && arm->getMode() != ast::ParamMode::Value) {
    // `n: view T` borrows the subject from the arm's start to n's last use,
    // where an `inout` one goes back to the subject (the lowering).
    // A local that borrows the binding keeps it in use to its own last
    // use: a write through it is the binding's.
    std::vector<std::string> names{arm->getBinding()};
    size_t uses = 0;
    for (size_t j = 0; j < stmts.size(); ++j) {
      if (std::none_of(names.begin(), names.end(), [&](const std::string &n) {
            return mentions(stmts[j], n);
          }))
        continue;
      uses = j + 1;
      if (const auto *ds = ast::dyn_cast<ast::DeclStmt>(stmts[j]))
        if (const auto *vd = ast::dyn_cast<ast::VarDecl>(ds->getDecl());
            vd && vd->getMode() != ast::ParamMode::Value)
          names.push_back(vd->getName());
    }
    arm->setBindingUses(uses);
    if (uses > 0) {
      const bool inout = arm->getMode() == ast::ParamMode::Inout;
      const size_t before = Borrows.size();
      if (!startBorrow(subject, arm->getBinding(), inout, &stmts, uses - 1))
        waits = false;
      for (size_t i = before; inout && i < Borrows.size(); ++i)
        Borrows[i].WritesBack = true;
    }
  }
  // What a `view` binding views (one of a move-only part of a place is a
  // `view` too, whatever its arm says): the subject's storage.
  if (arm->hasBinding() && CurrentScope) {
    const VarKind kind = varKind(arm->getBinding());
    if (kind == VarKind::ViewLocal || kind == VarKind::ViewBinding) {
      bool written = false;
      std::string why = reachOf(subject, /*through=*/false, written);
      ViewLocalReach[varKey(arm->getBinding())] = {std::move(why), written};
    }
  }
  bool ok = visitStatements(stmts) && waits;
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
