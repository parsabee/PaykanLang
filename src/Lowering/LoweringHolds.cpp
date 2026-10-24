// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// AST -> PIR lowering: reads of storage the function does not own that a
// call may replace while their value is in use (docs/pir.md §7, "Reads a
// call may replace").
//
// A value read from an object's storage has no reference of its own: a
// field's (`h.b.s`, `self.child`), or an element's or a field's of a value
// held in an object's field (`h.a.xs[0]`, `h.a.p.name`).  Nor has one read
// from what an `inout` parameter or local, a value passed by address (a
// `view` one, a slice) or a struct method's `self` names, which is the
// caller's.  The object, or the caller, keeps it.  But a call made while
// the value is in use may replace it: one given an object reaches whatever
// the object reaches, and runs whatever a closure in it holds, so it may
// assign the field the value is in, or the field holding the object it is
// in, and the value is freed before it is used.  So may a call given such
// storage to change, as an `inout` argument or as the `self` of a struct's
// method that may change it.  A call given nothing but values that hold no
// object (numbers, strings, and arrays, tuples and structs of them) can
// replace none of it.
//
// So the lowering walks each statement's expressions in the order it
// evaluates them, knowing how long each value stays in use: an operand
// until its operator, a receiver until its method returns, an argument the
// callee borrows until the call returns, the place an assignment writes
// until the value is stored, an array's storage until its element is read.
// A read in use where a call that may replace it is made takes a reference
// of its own (ModuleLowering::HeldReads): a box read is retained after its
// load and owned like a call's result; for a value held in memory, its
// address is in use, and the object it is in is held instead (`h.a` of
// `h.a.xs`), as the object of a call-rooted field is (MemTemps).  An
// `inout` local or a `match` arm's `n: inout T` holds the object for as
// long as it can be used.  A read with no such call while it is in use is
// read in place, as it was.

#include "LoweringInternal.h"
#include "Names.h"

#include <algorithm>

namespace paykan::lowering {

using pir::Type;

/// The walk over one statement's expressions (noteBorrowedReads).
struct ModuleLowering::BorrowedReads {
  ModuleLowering &L;
  /// The reads in use at this point of the statement, each one's hold
  /// keeping a value or the storage of one alive.
  std::vector<const ast::Expr *> Live;

  /// What an evaluated expression leaves to its user.
  struct Use {
    /// A box borrowed from storage the function does not own: the read to
    /// hold to keep it alive (the expression's own, or the one `Str(s)`
    /// passes on).
    const ast::Expr *Read = nullptr;
    /// A value held in memory, or a place: it is in storage the function
    /// does not own, and these reads' holds keep that storage alive.
    bool Borrowed = false;
    std::vector<const ast::Expr *> Owners;
  };

  /// How a call takes the arguments it borrows.
  enum class Callee {
    /// A function, a method, a constructor or a closure of the program: a
    /// reference is passed as a box of its own.
    Program,
    /// A native function of this module: it reads `x.f` where it is.
    Native,
    /// A builtin method: it reads every reference where it is.
    Builtin,
  };

  explicit BorrowedReads(ModuleLowering &l) : L(l) {}

  void statement(ast::Stmt *s);
  Use eval(ast::Expr *e, bool place = false);

private:
  static std::vector<const ast::Expr *> holds(const Use &u) {
    if (u.Read)
      return {u.Read};
    return u.Owners;
  }
  template <typename F>
  void within(const std::vector<const ast::Expr *> &reads, F &&f) {
    const size_t n = Live.size();
    Live.insert(Live.end(), reads.begin(), reads.end());
    f();
    Live.resize(n);
  }
  /// A call is made: when it may replace borrowed storage, every read in
  /// use takes a reference of its own.
  void call(bool mayReplace) {
    if (mayReplace)
      L.HeldReads.insert(Live.begin(), Live.end());
  }
  bool reachesObject(const ast::Type *ty) const;
  bool gives(ast::Expr *arg, Callee callee) const;
  Use variable(ast::Identifier *id) const;
  Use field(ast::MemberAccessExpr *ma);
  Use part(const ast::Expr *e, const Use &base);
  Use subscript(ast::SubscriptExpr *se);
  void binary(ast::BinaryExpr *be);
  Use callExpr(ast::CallExpr *ce, bool place);
  Use methodCall(ast::MethodCallExpr *mc, bool place);
  std::vector<const ast::Expr *> arguments(const std::vector<ast::Expr *> &args,
                                           Callee callee, bool given);
  static Use result(const ast::Expr *call, bool place,
                    std::vector<const ast::Expr *> owners);
};

// Whether a value of type @p ty may reach an object: given it, a call can
// change whatever the object reaches and run what a closure in it runs.  A
// Str reaches nothing; neither do the primitives, the enums without fields
// and what holds only these.
bool ModuleLowering::BorrowedReads::reachesObject(const ast::Type *ty) const {
  if (!ty || ast::isa<ast::BuiltinType>(ty))
    return !ty;
  if (const auto *ct = ast::dyn_cast<ast::ClassType>(ty))
    return !(ct->isBuiltin() && ct->getName() == names::kString);
  if (const auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    return reachesObject(ot->getInnerType());
  if (const auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty))
    return reachesObject(ft->getElementType());
  if (const auto *st = ast::dyn_cast<ast::SliceType>(ty))
    return reachesObject(st->getElementType());
  if (const auto *tt = ast::dyn_cast<ast::TupleType>(ty))
    return std::any_of(tt->getElementTypes().begin(),
                       tt->getElementTypes().end(),
                       [&](const ast::Type *e) { return reachesObject(e); });
  if (const auto *et = ast::dyn_cast<ast::EnumType>(ty)) {
    for (size_t i = 0; i < et->getNumVariants(); ++i)
      if (const ast::StructType *p = et->getPayload(i); p && reachesObject(p))
        return true;
    return false;
  }
  // A function value (its closure holds what it captured), or a type the
  // lowering does not know here.
  return true;
}

// Whether a call given @p arg may replace storage the function does not own:
// the argument reaches an object, or is such storage passed to an `inout`
// parameter.  A native function reads where they are the arguments it is not
// given to change (its `view` ones and the others, which C borrows), so only
// an `inout` argument lets it replace anything.
bool ModuleLowering::BorrowedReads::gives(ast::Expr *arg, Callee callee) const {
  if (callee == Callee::Native)
    return arg->isChangedByCall();
  if (reachesObject(arg->getResolvedType()))
    return true;
  if (!arg->isChangedByCall())
    return false;
  ast::Identifier *root = L.storageRoot(arg);
  return !root || L.CurrentScope->inoutType(root->getName()) != Type::Void;
}

ModuleLowering::BorrowedReads::Use
ModuleLowering::BorrowedReads::variable(ast::Identifier *id) const {
  // The function's own variables are another rule's (noteHeldReads), and a
  // read that rule holds is a reference or a copy of its own.
  if (id->isFunctionRef() || L.isHeldRead(id) ||
      !L.CurrentScope->hasLocal(id->getName()))
    return {};
  // The caller's storage: an `inout` parameter or local, a value passed by
  // address, a struct method's `self`.
  switch (L.CurrentScope->inoutType(id->getName())) {
  case Type::Box:
    return {id, false, {}};
  case Type::Mem:
    return {nullptr, true, {}};
  default:
    return {};
  }
}

ModuleLowering::BorrowedReads::Use
ModuleLowering::BorrowedReads::field(ast::MemberAccessExpr *ma) {
  ast::Expr *recv = ma->getReceiver();
  // A struct's field is part of the struct's storage.
  if (L.getExprStructType(recv))
    return part(ma, eval(recv));
  // A class's field is in its object, which a variable, `self` or a
  // temporary keeps alive; one read from storage the function does not own
  // (`h.a` of `h.a.xs`) is kept by its hold.
  Use r = eval(recv);
  if (ast::isMemType(ma->getResolvedType()))
    return {nullptr, true, holds(r)};
  // A box read from the object's slot is borrowed, unless the read is a
  // reference of its own (a `weak` field's, a temporary's field).
  if (ast::isRefType(ma->getResolvedType()) && !L.isWeakFieldAccess(ma) &&
      !L.exprProducesFreshBox(ma))
    return {ma, false, {}};
  return {};
}

// @p e, a part of the value held in memory whose evaluation left @p base: of
// the same storage when it is held in memory too, else read in place, a box
// borrowed when the storage is.  When a hold keeps the storage alive, which
// ends once the part is read (ownElementRead), the box takes a reference of
// its own.
ModuleLowering::BorrowedReads::Use
ModuleLowering::BorrowedReads::part(const ast::Expr *e, const Use &base) {
  if (ast::isMemType(e->getResolvedType()))
    return base;
  if (!base.Borrowed || !ast::isRefType(e->getResolvedType()))
    return {};
  if (std::any_of(base.Owners.begin(), base.Owners.end(),
                  [&](const ast::Expr *o) { return L.isHeldRead(o); })) {
    L.HeldReads.insert(e);
    return {};
  }
  return {e, false, {}};
}

ModuleLowering::BorrowedReads::Use
ModuleLowering::BorrowedReads::subscript(ast::SubscriptExpr *se) {
  ast::Type *arrTy = se->getArray()->getResolvedType();
  if (!ast::dyn_cast<ast::FixedArrayType>(arrTy) &&
      !ast::dyn_cast<ast::SliceType>(arrTy)) {
    // A string's character: the index is computed first, then the string is
    // read for it.
    eval(se->getIndex());
    eval(se->getArray());
    return {};
  }
  // The array's storage is in use while the index is computed.
  Use base = eval(se->getArray());
  within(base.Owners, [&] { eval(se->getIndex()); });
  return part(se, base);
}

void ModuleLowering::BorrowedReads::binary(ast::BinaryExpr *be) {
  ast::Expr *lhs = be->getLHS();
  ast::Expr *rhs = be->getRHS();
  ast::Type *lt = lhs->getResolvedType();
  ast::Type *rt = rhs->getResolvedType();
  const bool eq = be->getOpcode() == ast::BinaryOpcode::Eq ||
                  be->getOpcode() == ast::BinaryOpcode::Ne;
  // As ExprEmitter::visitBinaryExpr: values held in memory are compared in
  // place by the runtime, which calls their objects' `equals`; two optional
  // references are each a reference of their own, and so is the argument of
  // the `equals` another reference is the receiver of.
  if (eq &&
      (ast::isa<ast::OptionalType>(lt) || ast::isa<ast::OptionalType>(rt) ||
       ast::isa<ast::TupleType>(lt) || ast::isFunctionValue(lt) ||
       ast::isFunctionValue(rt) || ast::isPayloadEnum(lt))) {
    const bool none =
        ast::isa<ast::NoneLiteral>(lhs) || ast::isa<ast::NoneLiteral>(rhs);
    if (ast::isa<ast::TupleType>(lt) || ast::isPayloadEnum(lt) ||
        (!none && (ast::isFunctionValue(lt) || ast::isMemOptional(lt)))) {
      Use a = eval(lhs);
      within(a.Owners, [&] {
        Use b = eval(rhs);
        within(b.Owners, [&] { call(reachesObject(lt)); });
      });
      return;
    }
    eval(lhs);
    eval(rhs);
    if (!none && ast::isRefType(lt))
      call(reachesObject(lt));
    return;
  }
  const bool refs = lt && rt && ast::isRefType(lt) && ast::isRefType(rt);
  if (refs && (eq || be->getOpcode() == ast::BinaryOpcode::Add)) {
    // `a == b` is `a.equals(b)`, a Str's the runtime's; `a + b` joins two
    // strings in the runtime.  The left operand is in use until then.
    Use a = eval(lhs);
    within(holds(a), [&] {
      eval(rhs);
      call(eq && reachesObject(lt));
    });
    return;
  }
  eval(lhs);
  eval(rhs);
}

// The arguments @p args of a call, evaluated in order, then the call, which
// @p given says may replace borrowed storage through what else it is given
// (a receiver, a closure).  Each argument's holds are in use while the later
// ones are computed; a reference the callee borrows, the place of an `inout`
// argument and a value read where it is stay in use through the call.  The
// result is what the `inout` arguments' storage is held by, which an `inout`
// result may be part of.
std::vector<const ast::Expr *>
ModuleLowering::BorrowedReads::arguments(const std::vector<ast::Expr *> &args,
                                         Callee callee, bool given) {
  const size_t n = Live.size();
  std::vector<const ast::Expr *> through;
  std::vector<const ast::Expr *> places;
  bool mayReplace = given;
  for (ast::Expr *arg : args) {
    mayReplace = mayReplace || gives(arg, callee);
    const bool mem = ast::isMemType(arg->getResolvedType()) || isSliceArg(arg);
    Use u = eval(arg, /*place=*/arg->isChangedByCall());
    std::vector<const ast::Expr *> h = holds(u);
    // A reference the callee reads where it is: a builtin method's argument,
    // a native function's variable or `x.f` (emitBorrowedBox).
    auto borrowed = [&] {
      const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(arg);
      return callee == Callee::Builtin ||
             (callee == Callee::Native &&
              (ast::isa<ast::Identifier>(arg) ||
               (ma && ast::isa<ast::Identifier>(ma->getReceiver()))));
    };
    // By address, a value is read where it is through the call, unless a
    // parameter without a mode copies it when the call starts (a native
    // function reads every one where it is).
    const bool inPlace = arg->isChangedByCall() ||
                         (mem ? callee == Callee::Native ||
                                    arg->isViewedByCall() || isSliceArg(arg)
                              : u.Read && borrowed());
    if (!mem && !inPlace)
      continue; // a reference of its own, or a value
    Live.insert(Live.end(), h.begin(), h.end());
    if (inPlace)
      through.insert(through.end(), h.begin(), h.end());
    if (arg->isChangedByCall())
      places.insert(places.end(), h.begin(), h.end());
  }
  Live.resize(n);
  within(through, [&] { call(mayReplace); });
  return places;
}

// What a call leaves: nothing borrowed, but for an `inout` result used where
// it is (@p place, or a value held in memory), the storage of what it is part
// of.
ModuleLowering::BorrowedReads::Use
ModuleLowering::BorrowedReads::result(const ast::Expr *call, bool place,
                                      std::vector<const ast::Expr *> owners) {
  if (call->isInoutResult() &&
      (place || ast::isMemType(call->getResolvedType())))
    return {nullptr, true, std::move(owners)};
  return {};
}

ModuleLowering::BorrowedReads::Use
ModuleLowering::BorrowedReads::callExpr(ast::CallExpr *ce, bool place) {
  const std::string &name = ce->getCalleeName();
  const std::vector<ast::Expr *> &args = ce->getArguments();
  // `Str(s)` is `s`.
  if (L.IdentityCtors.count(name) && args.size() == 1)
    return eval(args[0], place);
  // A variant made in place, a conversion or a parse: no call of the
  // program, each argument used at once.
  ast::Type *target = nullptr;
  ast::Type *source = nullptr;
  if (ce->getVariantEnum() || L.FunctionTable.count(name) ||
      L.conversionTypes(name, target, source)) {
    for (ast::Expr *a : args)
      eval(a);
    return {};
  }
  // A function value's code and closure are read first, and `__super__`
  // is given `self`.
  if (ce->calleeIsValue() || name == names::kMethodSuper) {
    arguments(args, Callee::Program, /*given=*/true);
    return {};
  }
  std::vector<const ast::Expr *> owners =
      arguments(args, L.localNative(name) ? Callee::Native : Callee::Program,
                /*given=*/false);
  return result(ce, place, std::move(owners));
}

ModuleLowering::BorrowedReads::Use
ModuleLowering::BorrowedReads::methodCall(ast::MethodCallExpr *mc, bool place) {
  ast::Expr *recv = mc->getReceiver();
  ast::Type *rt = recv->getResolvedType();
  const std::string &name = mc->getMethodName();
  // A function value in a field: its code and closure are read first.
  if (mc->isFieldCall()) {
    eval(recv);
    arguments(mc->getArguments(), Callee::Program, /*given=*/true);
    return {};
  }
  // An array's or a slice's length, a builtin hash: no call of the program.
  if (ast::dyn_cast<ast::SliceType>(rt) ||
      ast::dyn_cast<ast::FixedArrayType>(rt) ||
      (name == names::kMethodHash &&
       (ast::isa<ast::BuiltinType>(rt) || !reachesObject(rt)))) {
    eval(recv);
    return {};
  }
  // A struct's method runs on the receiver's storage, in use until it
  // returns, which it may replace when it may change `self`.
  if (auto *st = L.getExprStructType(recv);
      st && name != names::kMethodToString) {
    Use r = eval(recv);
    std::vector<const ast::Expr *> owners = r.Owners;
    within(r.Owners, [&] {
      std::vector<const ast::Expr *> places = arguments(
          mc->getArguments(), Callee::Program,
          reachesObject(st) || (recv->isChangedByCall() && r.Borrowed));
      owners.insert(owners.end(), places.begin(), places.end());
    });
    return result(mc, place, std::move(owners));
  }
  // The builtin text of a value: the runtime's, which calls its objects'
  // `toString`.
  if (name == names::kMethodToString &&
      (ast::isa<ast::OptionalType>(rt) || ast::isa<ast::TupleType>(rt) ||
       ast::isa<ast::EnumType>(rt) || ast::isa<ast::BuiltinType>(rt))) {
    Use r = eval(recv);
    within(holds(r), [&] { call(reachesObject(rt)); });
    return {};
  }
  // A class's method: the receiver is in use until it returns.
  Use r = eval(recv);
  const auto *ct = ast::dyn_cast<ast::ClassType>(rt);
  std::vector<const ast::Expr *> owners = holds(r);
  within(holds(r), [&] {
    std::vector<const ast::Expr *> places =
        arguments(mc->getArguments(),
                  ct && ct->isBuiltin() ? Callee::Builtin : Callee::Program,
                  reachesObject(rt));
    owners.insert(owners.end(), places.begin(), places.end());
  });
  return result(mc, place, std::move(owners));
}

// Evaluate @p e (as a place, when its storage is what is used: an `inout`
// argument or local, a borrowed `match` subject) and say what it leaves.
ModuleLowering::BorrowedReads::Use
ModuleLowering::BorrowedReads::eval(ast::Expr *e, bool place) {
  if (!e)
    return {};
  // A variable as a place: its storage is the function's, or the caller's,
  // which the caller keeps.
  if (auto *id = ast::dyn_cast<ast::Identifier>(e))
    return place ? Use() : variable(id);
  if (auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e)) {
    // A box field as a place: the storage of the object it is in.
    if (place && !ast::isMemType(ma->getResolvedType())) {
      if (L.getExprStructType(ma->getReceiver()))
        return eval(ma->getReceiver());
      return {nullptr, true, holds(eval(ma->getReceiver()))};
    }
    return field(ma);
  }
  if (auto *se = ast::dyn_cast<ast::SubscriptExpr>(e))
    return subscript(se);
  if (auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(e))
    return part(ti, eval(ti->getTuple()));
  if (auto *be = ast::dyn_cast<ast::BinaryExpr>(e)) {
    binary(be);
    return {};
  }
  if (auto *ue = ast::dyn_cast<ast::UnaryExpr>(e)) {
    eval(ue->getOperand());
    return {};
  }
  if (auto *te = ast::dyn_cast<ast::TernaryExpr>(e)) {
    // The branch taken gives a reference or a copy of its own.
    eval(te->getCondition());
    eval(te->getTrueExpr());
    eval(te->getFalseExpr());
    return {};
  }
  if (auto *ce = ast::dyn_cast<ast::CallExpr>(e))
    return callExpr(ce, place);
  if (auto *mc = ast::dyn_cast<ast::MethodCallExpr>(e))
    return methodCall(mc, place);
  if (auto *cv = ast::dyn_cast<ast::CallValueExpr>(e)) {
    eval(cv->getCallee());
    arguments(cv->getArguments(), Callee::Program, /*given=*/true);
    return {};
  }
  // A literal's elements are stored in it as they come.
  if (auto *al = ast::dyn_cast<ast::ArrayLiteralExpr>(e))
    for (ast::Expr *x : al->getElements())
      eval(x);
  if (auto *tl = ast::dyn_cast<ast::TupleLiteralExpr>(e))
    for (ast::Expr *x : tl->getElements())
      eval(x);
  // A lambda copies what it captures.
  return {};
}

void ModuleLowering::BorrowedReads::statement(ast::Stmt *s) {
  // A borrow that outlives the statement holds what its storage is in for
  // as long as it can be used: any later call may replace it.
  auto borrow = [&](ast::Expr *placeExpr) {
    Use u = eval(placeExpr, /*place=*/true);
    L.HeldReads.insert(u.Owners.begin(), u.Owners.end());
  };
  if (auto *x = ast::dyn_cast<ast::ReturnStmt>(s)) {
    eval(x->getReturnValue());
  } else if (auto *x = ast::dyn_cast<ast::AssignStmt>(s)) {
    eval(x->getValue());
  } else if (auto *x = ast::dyn_cast<ast::DeclStmt>(s)) {
    if (auto *vd = ast::dyn_cast<ast::VarDecl>(x->getDecl())) {
      if (vd->getMode() == ast::ParamMode::Inout)
        borrow(vd->getInitExpr());
      else
        eval(vd->getInitExpr());
    }
  } else if (auto *x = ast::dyn_cast<ast::ExprStmt>(s)) {
    eval(x->getExpr());
  } else if (auto *x = ast::dyn_cast<ast::IfStmt>(s)) {
    eval(x->getCondition());
  } else if (auto *x = ast::dyn_cast<ast::WhileStmt>(s)) {
    eval(x->getCondition());
  } else if (auto *x = ast::dyn_cast<ast::ForStmt>(s)) {
    eval(x->getStart());
    eval(x->getEnd());
    eval(x->getStep());
  } else if (auto *x = ast::dyn_cast<ast::MatchStmt>(s)) {
    eval(x->getSubject());
    for (ast::MatchArm *arm : x->getArms())
      if (arm->hasBinding() && arm->getMode() == ast::ParamMode::Inout) {
        borrow(x->getSubject());
        break;
      }
  } else if (auto *x = ast::dyn_cast<ast::DestructureStmt>(s)) {
    eval(x->getValue());
  } else if (auto *x = ast::dyn_cast<ast::MemberAssignStmt>(s)) {
    // The object or the struct written to is in use until the value is
    // stored.
    ast::Expr *recv = x->getReceiver();
    Use r = eval(recv);
    within(L.getExprStructType(recv) ? r.Owners : holds(r),
           [&] { eval(x->getValue()); });
  } else if (auto *x = ast::dyn_cast<ast::SubscriptAssignStmt>(s)) {
    // The array's storage is in use until the element is stored.
    Use base = eval(x->getArray());
    within(base.Owners, [&] {
      eval(x->getIndex());
      eval(x->getValue());
    });
  }
}

void ModuleLowering::noteBorrowedReads(ast::Stmt *s) {
  BorrowedReads(*this).statement(s);
}

} // namespace paykan::lowering
