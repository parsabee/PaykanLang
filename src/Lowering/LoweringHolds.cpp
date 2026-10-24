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
// replace none of it.  One that runs code the module has may let go only of
// references of the classes that code stores into or lets go of (effectOf,
// further down): a read of storage the function never hands on whose slot
// is of another class is not replaced.  Nor is one of a field of `self` no
// code stores into once its object is made, whatever the call does
// (isFixedRead, at the end of this file).
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
// `inout` local holds the object for as long as it can be used, and a
// `match` with an arm's `n: inout T` until it ends.  A read with no such
// call while it is in use is read in place, as it was; one of storage the
// function never hands on that a call is made across which may let go of
// references, but not of its slot's, by a `stable` load, which the moves at
// last use count as held by the slot (docs/pir.md §6), as they counted the
// hold.

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
  /// A call is made that does @p effect: every read in use whose reference
  /// it may let go of takes one of its own.  A read of storage the function
  /// never hands on whose reference it cannot let go of, though it may let
  /// go of others (replaces), is read by a `stable` load (isStableRead).
  void call(const Effect &effect) {
    for (const ast::Expr *read : Live)
      if (replaces(effect, read))
        L.HeldReads.insert(read);
      else if (effect.Releases)
        L.StableReads.insert(read);
  }
  bool replaces(const Effect &effect, const ast::Expr *read) const;
  bool gives(ast::Expr *arg, Callee callee) const;
  Use variable(ast::Identifier *id) const;
  Use field(ast::MemberAccessExpr *ma);
  Use part(const ast::Expr *e, const Use &base);
  Use subscript(ast::SubscriptExpr *se);
  void binary(ast::BinaryExpr *be);
  Use callExpr(ast::CallExpr *ce, bool place);
  Use methodCall(ast::MethodCallExpr *mc, bool place);
  std::vector<const ast::Expr *> arguments(const std::vector<ast::Expr *> &args,
                                           Callee callee, bool given,
                                           const Effect &effect);
  static Use result(const ast::Expr *call, bool place,
                    std::vector<const ast::Expr *> owners);
};

// Given such a value, a call can also run what a closure in it runs.  A Str
// reaches nothing; neither do the primitives, the enums without fields and
// what holds only these.
bool ModuleLowering::reachesObject(const ast::Type *ty) {
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

// The variable a read is a part of the storage of (`h` of `h.b.s`, `xs` of
// `xs[i]`), or null (a call's result).
static const ast::Identifier *rootOf(const ast::Expr *e) {
  while (e) {
    if (const auto *id = ast::dyn_cast<ast::Identifier>(e))
      return id;
    if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e))
      e = ma->getReceiver();
    else if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(e))
      e = se->getArray();
    else if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(e))
      e = ti->getTuple();
    else
      return nullptr;
  }
  return nullptr;
}

// Whether a call that does @p effect may let go of the reference @p read
// borrows from its slot.  A field of `self` that no code stores into once
// its object is made keeps it, whatever the call does (isFixedRead).  Not
// knowing what it does, a call may let go of any other.  A read of
// storage the function never hands on (its `self`, an `inout`, `view` or
// slice parameter's) lives while that slot keeps its reference, which only
// letting go of a reference of the slot's class can take away: storing
// into the slot, or letting go of what holds it, whose class or value type
// then holds that class (dropEffect).  An `inout` name's slot may be of a
// superclass of its type: a `match` arm `n: inout T` names a field of a
// superclass as a T.  A read rooted in a variable the function may hand on
// at its last use keeps its reference whenever the call may let go of any
// (Effect::Releases): MoveAtLastUse keeps another variable's hand-off a
// copy while a borrowed read of it is in use across a call, and that
// variable's object would then die later than it does with the hold.
bool ModuleLowering::BorrowedReads::replaces(const Effect &effect,
                                             const ast::Expr *read) const {
  if (L.isFixedRead(read))
    return false;
  if (effect.Unknown)
    return true;
  const ast::Identifier *root = rootOf(read);
  if (!root || !L.CurrentScope->isStable(root->getName()))
    return effect.Releases;
  auto *ct = ast::dyn_cast<ast::ClassType>(L.slotType(read->getResolvedType()));
  if (!ct)
    return true;
  const bool name = ast::isa<ast::Identifier>(read);
  for (ast::ClassType *c = ct; c; c = name ? c->getSuperClass() : nullptr)
    if (effect.Types.count(L.slotType(c)))
      return true;
  return false;
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
    // A string's character: the string read is in use while the index is
    // computed.
    Use s = eval(se->getArray());
    within(holds(s), [&] { eval(se->getIndex()); });
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
        within(b.Owners,
               [&] { call(L.valueMethodEffect(lt, names::kMethodEquals)); });
      });
      return;
    }
    eval(lhs);
    eval(rhs);
    if (!none && ast::isRefType(lt))
      call(L.valueMethodEffect(lt, names::kMethodEquals));
    return;
  }
  const bool refs = lt && rt && ast::isRefType(lt) && ast::isRefType(rt);
  if (refs && (eq || be->getOpcode() == ast::BinaryOpcode::Add)) {
    // `a == b` is `a.equals(b)`, a Str's the runtime's; `a + b` joins two
    // strings in the runtime.  The left operand is in use until then.
    Use a = eval(lhs);
    within(holds(a), [&] {
      eval(rhs);
      if (eq)
        call(L.valueMethodEffect(lt, names::kMethodEquals));
    });
    return;
  }
  eval(lhs);
  eval(rhs);
}

// The arguments @p args of a call, evaluated in order, then the call, which
// does @p effect when it may replace borrowed storage at all: through what
// else it is given (@p given: a receiver, a closure), or what an argument
// gives.  Each argument's holds are in use while the later ones are
// computed; a reference the callee borrows, the place of an `inout`
// argument and a value read where it is stay in use through the call.  The
// result is what the `inout` arguments' storage is held by, which an `inout`
// result may be part of.
std::vector<const ast::Expr *>
ModuleLowering::BorrowedReads::arguments(const std::vector<ast::Expr *> &args,
                                         Callee callee, bool given,
                                         const Effect &effect) {
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
  within(through, [&] {
    if (mayReplace)
      call(effect);
  });
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
    arguments(args, Callee::Program, /*given=*/true, Effect::unknown());
    return {};
  }
  const ast::FuncDecl *native = L.localNative(name);
  std::vector<const ast::Expr *> owners =
      arguments(args, native ? Callee::Native : Callee::Program,
                /*given=*/false,
                native ? L.nativeEffect(*native, args) : L.calleeEffect(name));
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
    arguments(mc->getArguments(), Callee::Program, /*given=*/true,
              Effect::unknown());
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
          reachesObject(st) || (recv->isChangedByCall() && r.Borrowed),
          L.effectOf(L.bodyOf(st->getName() + names::kNameSep + name)));
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
    within(holds(r),
           [&] { call(L.valueMethodEffect(rt, names::kMethodToString)); });
    return {};
  }
  // A class's method: the receiver is in use until it returns.
  Use r = eval(recv);
  auto *ct = ast::dyn_cast<ast::ClassType>(rt);
  std::vector<const ast::Expr *> owners = holds(r);
  within(holds(r), [&] {
    std::vector<const ast::Expr *> places = arguments(
        mc->getArguments(),
        ct && ct->isBuiltin() ? Callee::Builtin : Callee::Program,
        reachesObject(rt), ct ? L.methodEffect(ct, name) : Effect::unknown());
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
    arguments(cv->getArguments(), Callee::Program, /*given=*/true,
              Effect::unknown());
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
    // With an `n: inout T` arm, the subject is evaluated as its storage,
    // once, and read there (emitMatchPlace).
    if (hasInoutArm(x))
      borrow(x->getSubject());
    else
      eval(x->getSubject());
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

// -- What a call does
//
// A call lets go of a reference its caller's storage holds by storing into
// the slot holding it, or by letting go of what holds the slot (the last
// reference to the object it is in, a value it is part of).  What else it
// lets go of is its own: a variable's or a temporary's reference to an
// object its caller's storage still holds, or to one it made, and with no
// variables at module level it reaches nothing else.  So what a function
// does is read from its body (EffectScan): the classes of the slots it may
// store into, with what letting go of their values may let go of in turn
// (dropEffect), and what the calls it makes do; or not known, for anything
// it does that is not recognized.  Effect::Releases is the same reading
// without the types: whether it may let go of any reference at all.

void ModuleLowering::Effect::add(const Effect &other) {
  Unknown = Unknown || other.Unknown;
  Releases = Releases || other.Releases;
  Types.insert(other.Types.begin(), other.Types.end());
}

/// The read of a function's body for effectOf.
struct ModuleLowering::EffectScan {
  ModuleLowering &L;
  /// The function read: a call of it from its own body does what the rest
  /// of the body does.
  const ast::FuncDecl *Fn;
  /// Storage the function does not own, as a store through a name for it
  /// lets go of: a value of type Value (null when not known), from a slot
  /// of type Slot or, for a class, of one of its superclasses (a `match`
  /// arm `n: inout T` names a field of a superclass as a T).
  struct Place {
    ast::Type *Value;
    ast::Type *Slot;
  };
  /// Its names for such storage: `self` (a struct's), its `inout`
  /// parameters and locals, its `match` arms' `n: inout T`, each with every
  /// place it stood for (the scan does not follow scopes).
  std::unordered_map<std::string, std::vector<Place>> NotOwned;
  Effect Out;

  void unknown() { Out.add(Effect::unknown()); }
  /// A call that does @p effect.
  void runs(const Effect &effect) { Out.add(effect); }
  void runs(const ast::FuncDecl *callee) {
    if (callee != Fn)
      Out.add(L.effectOf(callee));
  }
  /// A slot holding a value of type @p ty is stored into.
  void drop(ast::Type *ty) {
    Out.Releases = Out.Releases || holdsReference(ty);
    Out.add(L.dropEffect(ty));
  }
  void store(const std::string &name);
  void storeField(ast::Expr *recv, const std::string &field);
  /// Whether a value of type @p ty holds a reference that storing another
  /// releases.
  static bool holdsReference(const ast::Type *ty) {
    return !ty || ast::isRefType(ty) || memHoldsRefs(ty);
  }
  /// Whether the arguments @p args give a call something to reach an object
  /// through, or storage holding a reference to change.
  static bool gives(const std::vector<ast::Expr *> &args) {
    return std::any_of(args.begin(), args.end(), [](const ast::Expr *a) {
      return reachesObject(a->getResolvedType()) ||
             (a->isChangedByCall() && holdsReference(a->getResolvedType()));
    });
  }
  void exprs(const std::vector<ast::Expr *> &es) {
    for (ast::Expr *e : es)
      expr(e);
  }
  void stmt(ast::Stmt *s);
  void expr(ast::Expr *e);
  void call(ast::CallExpr *ce);
  void methodCall(ast::MethodCallExpr *mc);
  /// The static type of @p e, as Sema recorded it (the function read is not
  /// the one being lowered, whose scope knows other names), or null.
  ast::Type *typeOf(const ast::Expr *e) const {
    ast::Type *ty = e->getResolvedType();
    return ty ? L.canonicalizeDeclType(ty) : nullptr;
  }
  /// The type of the field @p field of what @p recv evaluates to, or null.
  ast::Type *fieldType(const ast::Expr *recv, const std::string &field) const {
    ast::Type *ty = typeOf(recv);
    if (auto *st = ast::dyn_cast<ast::StructType>(ty))
      return st->findField(field);
    if (auto *ct = ast::dyn_cast<ast::ClassType>(ty))
      return ct->findField(field);
    return nullptr;
  }
};

void ModuleLowering::EffectScan::store(const std::string &name) {
  for (const Place &p : NotOwned.at(name)) {
    drop(p.Value);
    for (auto *c = ast::dyn_cast<ast::ClassType>(L.slotType(p.Slot)); c;
         c = c->getSuperClass())
      Out.Types.insert(L.slotType(c));
  }
}

void ModuleLowering::EffectScan::storeField(ast::Expr *recv,
                                            const std::string &field) {
  ast::Type *rt = typeOf(recv);
  if (auto *ct = ast::dyn_cast<ast::ClassType>(rt);
      ct && ct->isWeakField(field)) {
    // A weak reference: letting it go destroys nothing.
    Out.Releases = true;
    return;
  }
  if (auto *st = ast::dyn_cast<ast::StructType>(rt))
    if (ast::Type *elem = L.bufferElementOf(st, field)) {
      // A buffer: letting go of its last reference drops its values.
      Out.Releases = true;
      Out.Types.insert(L.slotType(st->findField(field)));
      Out.add(L.dropEffect(elem));
      return;
    }
  drop(fieldType(recv, field));
}

void ModuleLowering::EffectScan::stmt(ast::Stmt *s) {
  if (!s || Out.Unknown)
    return;
  if (auto *x = ast::dyn_cast<ast::CompoundStmt>(s)) {
    for (ast::Stmt *t : x->getStatements())
      stmt(t);
  } else if (auto *x = ast::dyn_cast<ast::ReturnStmt>(s)) {
    expr(x->getReturnValue());
  } else if (auto *x = ast::dyn_cast<ast::ExprStmt>(s)) {
    expr(x->getExpr());
  } else if (auto *x = ast::dyn_cast<ast::DeclStmt>(s)) {
    auto *vd = ast::dyn_cast<ast::VarDecl>(x->getDecl());
    if (!vd)
      return unknown();
    if (vd->getMode() == ast::ParamMode::Inout)
      NotOwned[vd->getName()].push_back({vd->getType(), vd->getType()});
    expr(vd->getInitExpr());
  } else if (auto *x = ast::dyn_cast<ast::AssignStmt>(s)) {
    if (NotOwned.count(x->getVarName()))
      store(x->getVarName());
    expr(x->getValue());
  } else if (auto *x = ast::dyn_cast<ast::MemberAssignStmt>(s)) {
    storeField(x->getReceiver(), x->getFieldName());
    expr(x->getReceiver());
    expr(x->getValue());
  } else if (auto *x = ast::dyn_cast<ast::SubscriptAssignStmt>(s)) {
    ast::Type *arr = typeOf(x->getArray());
    ast::Type *elem = nullptr;
    if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(arr))
      elem = ft->getElementType();
    else if (auto *st = ast::dyn_cast<ast::SliceType>(arr))
      elem = st->getElementType();
    drop(elem);
    expr(x->getArray());
    expr(x->getIndex());
    expr(x->getValue());
  } else if (auto *x = ast::dyn_cast<ast::DestructureStmt>(s)) {
    for (const ast::DestructureStmt::Target &t : x->getTargets())
      if (!t.isSkip() && NotOwned.count(t.getName())) {
        Out.Releases = true;
        store(t.getName());
      }
    expr(x->getValue());
  } else if (auto *x = ast::dyn_cast<ast::IfStmt>(s)) {
    expr(x->getCondition());
    stmt(x->getThenBranch());
    stmt(x->getElseBranch());
  } else if (auto *x = ast::dyn_cast<ast::WhileStmt>(s)) {
    expr(x->getCondition());
    stmt(x->getBody());
  } else if (auto *x = ast::dyn_cast<ast::ForStmt>(s)) {
    expr(x->getStart());
    expr(x->getEnd());
    expr(x->getStep());
    stmt(x->getBody());
  } else if (auto *x = ast::dyn_cast<ast::MatchStmt>(s)) {
    expr(x->getSubject());
    for (ast::MatchArm *arm : x->getArms()) {
      // A store through the binding replaces the bound value (the arm's
      // copy, of its binding's type), and its write-back replaces whatever
      // the subject's slot holds then, of the subject's type.
      if (arm->hasBinding() && arm->getMode() == ast::ParamMode::Inout) {
        ast::Type *subject = typeOf(x->getSubject());
        std::vector<Place> &places = NotOwned[arm->getBinding()];
        places.push_back({arm->getBindType(), subject});
        places.push_back({subject, subject});
      }
      stmt(arm->getBody());
    }
  } else if (!ast::isa<ast::BreakStmt>(s) && !ast::isa<ast::ContinueStmt>(s)) {
    unknown();
  }
}

void ModuleLowering::EffectScan::expr(ast::Expr *e) {
  if (!e || Out.Unknown || ast::isa<ast::Identifier>(e) ||
      ast::isa<ast::IntegerLiteral>(e) || ast::isa<ast::FloatLiteral>(e) ||
      ast::isa<ast::BoolLiteral>(e) || ast::isa<ast::CharLiteral>(e) ||
      ast::isa<ast::NoneLiteral>(e) || ast::isa<ast::StringLiteral>(e) ||
      ast::isa<ast::EnumValueExpr>(e) ||
      ast::isa<ast::LambdaExpr>(e)) // a lambda's body runs when it is called
    return;
  if (auto *x = ast::dyn_cast<ast::UnaryExpr>(e))
    return expr(x->getOperand());
  if (auto *x = ast::dyn_cast<ast::BinaryExpr>(e)) {
    expr(x->getLHS());
    expr(x->getRHS());
    // Values that reach objects are compared by the objects' `equals`;
    // `== None` compares a reference with null.
    if (ast::isa<ast::NoneLiteral>(x->getLHS()) ||
        ast::isa<ast::NoneLiteral>(x->getRHS()))
      return;
    ast::Type *lt = typeOf(x->getLHS());
    if (x->getOpcode() == ast::BinaryOpcode::Eq ||
        x->getOpcode() == ast::BinaryOpcode::Ne)
      runs(L.valueMethodEffect(lt, names::kMethodEquals));
    else if (reachesObject(lt))
      unknown();
    return;
  }
  if (auto *x = ast::dyn_cast<ast::TernaryExpr>(e)) {
    expr(x->getCondition());
    expr(x->getTrueExpr());
    expr(x->getFalseExpr());
  } else if (auto *x = ast::dyn_cast<ast::MemberAccessExpr>(e)) {
    expr(x->getReceiver());
  } else if (auto *x = ast::dyn_cast<ast::SubscriptExpr>(e)) {
    expr(x->getArray());
    expr(x->getIndex());
  } else if (auto *x = ast::dyn_cast<ast::TupleIndexExpr>(e)) {
    expr(x->getTuple());
  } else if (auto *x = ast::dyn_cast<ast::ArrayLiteralExpr>(e)) {
    exprs(x->getElements());
  } else if (auto *x = ast::dyn_cast<ast::TupleLiteralExpr>(e)) {
    exprs(x->getElements());
  } else if (auto *x = ast::dyn_cast<ast::CallExpr>(e)) {
    call(x);
  } else if (auto *x = ast::dyn_cast<ast::MethodCallExpr>(e)) {
    methodCall(x);
  } else {
    unknown(); // a call through a function value
  }
}

void ModuleLowering::EffectScan::call(ast::CallExpr *ce) {
  const std::string &name = ce->getCalleeName();
  const std::vector<ast::Expr *> &args = ce->getArguments();
  exprs(args);
  // `Str(s)`, a variant made in place, a conversion or a parse: the
  // runtime's, given numbers and strings.
  ast::Type *target = nullptr;
  ast::Type *source = nullptr;
  if ((L.IdentityCtors.count(name) && args.size() == 1) || ce->getVariantEnum())
    return;
  if (L.FunctionTable.count(name) || L.conversionTypes(name, target, source)) {
    if (gives(args))
      unknown();
    return;
  }
  if (ce->calleeIsValue())
    return unknown();
  if (name == names::kMethodSuper) {
    // The superclass's `__init__`, on this object.
    auto it = L.BodyOwners.find(Fn);
    auto *ct = it == L.BodyOwners.end()
                   ? nullptr
                   : ast::dyn_cast<ast::ClassType>(it->second);
    ast::ClassType *super = ct ? ct->getSuperClass() : nullptr;
    if (!super || super->isBuiltin())
      return unknown();
    Effect init = L.effectOf(
        L.bodyOf(super->getName() + names::kNameSep + names::kMethodInit));
    init.Releases = true;
    return runs(init);
  }
  if (const ast::FuncDecl *native = L.localNative(name))
    return runs(L.nativeEffect(*native, args));
  if (!gives(args))
    return;
  if (const ast::FuncDecl *fn = L.bodyOf(name))
    return runs(fn);
  runs(L.calleeEffect(name));
}

void ModuleLowering::EffectScan::methodCall(ast::MethodCallExpr *mc) {
  ast::Expr *recv = mc->getReceiver();
  const std::string &name = mc->getMethodName();
  expr(recv);
  exprs(mc->getArguments());
  if (mc->isFieldCall()) // a function value in a field
    return unknown();
  ast::Type *rt = typeOf(recv);
  if (ast::dyn_cast<ast::SliceType>(rt) ||
      ast::dyn_cast<ast::FixedArrayType>(rt))
    return; // `len()`
  if (auto *st = ast::dyn_cast<ast::StructType>(rt);
      st && name != names::kMethodToString)
    return runs(L.bodyOf(st->getName() + names::kNameSep + name));
  // The builtin text, hash or equality of a value (which calls its objects'
  // methods), or a Str's method (the runtime's).
  auto *ct = ast::dyn_cast<ast::ClassType>(rt);
  if (!ct || !reachesObject(ct)) {
    if (name == names::kMethodToString || name == names::kMethodEquals)
      runs(L.valueMethodEffect(rt, name));
    else if (reachesObject(rt))
      unknown();
    return;
  }
  // A class's method: an `impl` one is the one the call runs.
  const ast::MethodDecl *md = ct->findMethod(name);
  if (md && md->isImpl()) {
    ast::ClassType *owner = ct->implOwner(name);
    return runs(owner ? L.bodyOf(owner->getName() + names::kNameSep + name)
                      : nullptr);
  }
  runs(L.methodEffect(ct, name));
}

const ast::FuncDecl *ModuleLowering::bodyOf(const std::string &name) const {
  auto it = Bodies.find(name);
  return it == Bodies.end() ? nullptr : it->second;
}

const ModuleLowering::Effect &
ModuleLowering::effectOf(const ast::FuncDecl *fn) {
  static const Effect kUnknown = Effect::unknown();
  if (!fn || !fn->getBody())
    return kUnknown;
  if (auto it = Effects.find(fn); it != Effects.end())
    return it->second;
  Effects[fn] = Effect::unknown(); // while it is read: a call of it from
                                   // another one
  EffectScan scan{*this, fn, {}, {}};
  auto owner = BodyOwners.find(fn);
  auto *st = owner == BodyOwners.end()
                 ? nullptr
                 : ast::dyn_cast<ast::StructType>(owner->second);
  scan.NotOwned[names::kSelf].push_back({st, st});
  for (const ast::Param &p : fn->getParams())
    if (p.Mode == ast::ParamMode::Inout)
      scan.NotOwned[p.getName()].push_back({p.ParamType, p.ParamType});
  scan.stmt(fn->getBody());
  Effect &effect = Effects[fn];
  effect = std::move(scan.Out);
  return effect;
}

ModuleLowering::Effect ModuleLowering::calleeEffect(const std::string &name) {
  if (const ast::FuncDecl *fn = bodyOf(name))
    return effectOf(fn);
  if (std::optional<Effect> ctor = constructorEffect(name))
    return *ctor;
  return Effect::unknown();
}

std::optional<ModuleLowering::Effect>
ModuleLowering::constructorEffect(const std::string &name) {
  ast::StructType *st = ASTCtx.lookupStructType(name);
  if (!st && !ASTCtx.lookupClassType(name))
    return std::nullopt;
  // A new object, or the caller's storage zeroed: the stores of its
  // `__init__` into it let go of what the `__init__` puts there before
  // (and of nothing else in the slots they fill first); a struct's default
  // constructor only fills them.
  Effect e;
  if (const ast::FuncDecl *init =
          bodyOf(name + names::kNameSep + names::kMethodInit))
    e = effectOf(init);
  else if (!st || st->getInitMethod())
    return Effect::unknown(); // compiled in another module
  e.Releases = true;
  return e;
}

bool ModuleLowering::isExactClass(const ast::ClassType *ct) {
  return (ct->isBuiltin() && ct->getName() == names::kString) ||
         ct->getName().find('<') != std::string::npos;
}

// The main module of a program is imported by no module (an import of it
// would close a cycle, which Sema rejects), and no other module's template
// can name its classes: every class derived from one of them is a class it
// defines, a declaration of its own or an instantiation of a template, its
// own or another module's (whose superclass is never a type parameter).
bool ModuleLowering::isClosedClass(const ast::ClassType *ct) const {
  return Canonical.empty() && PL.MainWorld == World::Closed &&
         OwnTypes.count(ct->getName());
}

void ModuleLowering::indexSubclasses(
    const std::vector<ast::ClassDecl *> &classes) {
  if (!Canonical.empty() || PL.MainWorld != World::Closed)
    return;
  for (const ast::ClassDecl *cls : classes) {
    ast::ClassType *ct = ASTCtx.lookupClassType(cls->getName());
    for (ast::ClassType *c = ct ? ct->getSuperClass() : nullptr; c;
         c = c->getSuperClass())
      Subclasses[c].push_back(ct);
  }
}

ast::Type *ModuleLowering::slotType(ast::Type *ty) {
  ty = canonicalizeDeclType(ty);
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    return canonicalizeDeclType(ot->getInnerType());
  return ty;
}

const ModuleLowering::Effect &ModuleLowering::dropEffect(ast::Type *ty) {
  if (auto it = DropEffects.find(ty); it != DropEffects.end())
    return it->second;
  Effect e;
  std::unordered_set<const ast::Type *> seen;
  std::vector<ast::Type *> work = {ty};
  while (!work.empty() && !e.Unknown) {
    ast::Type *t = slotType(work.back());
    work.pop_back();
    if (!t) {
      e = Effect::unknown();
      break;
    }
    if (!seen.insert(t).second)
      continue;
    if (ast::isa<ast::BuiltinType>(t)) {
      continue; // a number holds nothing
    } else if (auto *et = ast::dyn_cast<ast::EnumType>(t)) {
      for (size_t i = 0; i < et->getNumVariants(); ++i)
        if (ast::StructType *p = et->getPayload(i))
          work.push_back(p);
    } else if (auto *st = ast::dyn_cast<ast::StructType>(t)) {
      for (size_t i = 0; i < st->getNumFields(); ++i) {
        ast::Type *elem = bufferElementOf(st, st->getFieldName(i));
        if (elem)
          e.Types.insert(slotType(st->getElementType(i)));
        work.push_back(elem ? elem : st->getElementType(i));
      }
    } else if (auto *tt = ast::dyn_cast<ast::TupleType>(t)) {
      work.insert(work.end(), tt->getElementTypes().begin(),
                  tt->getElementTypes().end());
    } else if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(t)) {
      work.push_back(ft->getElementType());
    } else if (auto *ct = ast::dyn_cast<ast::ClassType>(t);
               ct && (isExactClass(ct) || isClosedClass(ct))) {
      // An object that dies lets go of what its fields hold (the object a
      // weak one names stays), its destructor runs no other code; one of a
      // closed class may be of a class derived from it.
      e.Types.insert(ct);
      std::vector<ast::ClassType *> classes = {ct};
      if (auto sub = Subclasses.find(ct); sub != Subclasses.end())
        classes.insert(classes.end(), sub->second.begin(), sub->second.end());
      for (ast::ClassType *k : classes)
        for (ast::ClassType *c = k; c && !c->isBuiltin();
             c = c->getSuperClass())
          for (const auto &[name, fieldTy] : c->getFields())
            if (!c->isWeakField(name))
              work.push_back(fieldTy);
    } else {
      // An object of a class a subclass of which may hold anything, a
      // function value's closure, or a type not known here.
      e = Effect::unknown();
    }
  }
  e.Releases = e.Unknown || !e.Types.empty();
  return DropEffects[ty] = std::move(e);
}

ModuleLowering::Effect ModuleLowering::methodEffect(ast::ClassType *ct,
                                                    const std::string &name) {
  const ast::MethodDecl *md = ct->findMethod(name);
  if (md && md->isImpl()) {
    ast::ClassType *owner = ct->implOwner(name);
    return effectOf(owner ? bodyOf(owner->getName() + names::kNameSep + name)
                          : nullptr);
  }
  // A virtual method: the one its vtable names, the closest class's from it
  // up that defines it, for an exact class; for a closed class, that one or
  // an override of a class derived from it, as an object of one has that
  // class's vtable (or, while a superclass's `__init__` runs, the
  // superclass's, through a reference of the superclass's type only).  A
  // pure one's trap stops the program.
  Effect e = Effect::unknown();
  if (!isExactClass(ct) && !isClosedClass(ct))
    return e;
  auto implementation = [&](ast::ClassType *c, const ast::FuncDecl *fn) {
    const ast::MethodDecl *m = c->findMethod(name);
    return m && m->isPure() ? Effect() : effectOf(fn);
  };
  for (ast::ClassType *c = ct; c; c = c->getSuperClass()) {
    if (c->isBuiltin()) {
      e = Effect(); // `Obj`'s or `Str`'s: the runtime's, which runs no code
      break;
    }
    if (const ast::FuncDecl *fn =
            bodyOf(c->getName() + names::kNameSep + name)) {
      e = implementation(c, fn);
      break;
    }
    if (!OwnTypes.count(c->getName()))
      break; // another module's class, compiled there
  }
  if (auto sub = Subclasses.find(ct); sub != Subclasses.end())
    for (ast::ClassType *c : sub->second)
      if (const ast::FuncDecl *fn =
              bodyOf(c->getName() + names::kNameSep + name))
        e.add(implementation(c, fn));
  e.Releases = true;
  return e;
}

ModuleLowering::Effect
ModuleLowering::valueMethodEffect(ast::Type *ty, const std::string &method) {
  Effect e;
  std::vector<ast::Type *> work = {ty};
  while (!work.empty() && !e.Unknown) {
    ast::Type *t = slotType(work.back());
    work.pop_back();
    if (t && ast::isa<ast::BuiltinType>(t))
      continue;
    if (auto *et = ast::dyn_cast<ast::EnumType>(t)) {
      for (size_t i = 0; i < et->getNumVariants(); ++i)
        if (ast::StructType *p = et->getPayload(i))
          work.push_back(p);
    } else if (auto *tt = ast::dyn_cast<ast::TupleType>(t)) {
      // A struct too: the runtime compares and prints the fields.
      work.insert(work.end(), tt->getElementTypes().begin(),
                  tt->getElementTypes().end());
    } else if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(t)) {
      work.push_back(ft->getElementType());
    } else if (auto *ct = ast::dyn_cast<ast::ClassType>(t)) {
      e.add(methodEffect(ct, method));
    } else {
      e = Effect::unknown(); // a function value, or a type not known
    }
  }
  e.Releases = e.Unknown || reachesObject(ty);
  return e;
}

ModuleLowering::Effect
ModuleLowering::nativeEffect(const ast::FuncDecl &native,
                             const std::vector<ast::Expr *> &args) {
  const bool inout =
      std::any_of(args.begin(), args.end(),
                  [](const ast::Expr *a) { return a->isChangedByCall(); });
  for (const names::BufferFunction &f : names::kPaykanBufferFunctions) {
    if (native.getNativeSymbol() != f.Symbol)
      continue;
    Effect e;
    if (f.Drops) {
      // It drops values of the type its buffer holds, which memory's
      // Buffer<T> makes with the T it passes.
      if (native.getTypeArgs().empty())
        return Effect::unknown();
      e = dropEffect(native.getTypeArgs().front());
    }
    e.Releases = inout;
    return e;
  }
  // Any other changes what it is given to change, and nothing else
  // (docs/language/03-functions.md, "Native functions").
  return inout ? Effect::unknown() : Effect();
}

// What a buffer's field may hold is derived from the code that may store
// into it: the struct's own, as the field is private to it.  A store is an
// assignment (any other way to change a field names it, which is a use
// that is not an argument of a Buffer function), so every object it holds
// is a buffer of the type each store makes it with.
struct ModuleLowering::BufferUses {
  ModuleLowering &L;
  const ast::Type *Struct;
  const std::string &Field;
  /// The type of what the buffers stored hold, once one is seen.
  ast::Type *Elem;
  /// No other use of the field is seen.
  bool Ok;

  bool isField(const ast::Expr *e) const {
    const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e);
    return ma && ma->getFieldName() == Field && of(ma->getReceiver());
  }
  /// Whether @p recv is a value of the struct.
  bool of(const ast::Expr *recv) const {
    ast::Type *ty = recv->getResolvedType();
    return ty && L.slotType(ty) == Struct;
  }
  void stmt(ast::Stmt *s);
  void expr(ast::Expr *e);
  /// @p value is stored to the field: it must be `_new<T>(n)`, a new
  /// buffer of T's.
  void storeBuffer(ast::Expr *value);
};

ast::Type *ModuleLowering::bufferElementOf(ast::StructType *st,
                                           const std::string &field) {
  const std::string key = st->getName() + names::kNameSep + field;
  if (auto it = BufferElements.find(key); it != BufferElements.end())
    return it->second;
  BufferElements[key] = nullptr;
  if (!names::isTypePrivateName(field) || !st->getInitMethod() ||
      slotType(st->findField(field)) != slotType(ASTCtx.getObjTy()))
    return nullptr;
  // Every method of the struct is lowered here, so every store to the
  // field that code can make is among the bodies this module has.
  std::vector<ast::MethodDecl *> methods = st->getImplMethods();
  methods.push_back(st->getInitMethod());
  for (const ast::MethodDecl *md : methods)
    if (!bodyOf(st->getName() + names::kNameSep + md->getName()))
      return nullptr;
  BufferUses uses{*this, st, field, nullptr, true};
  for (const auto &[name, fn] : Bodies)
    uses.stmt(fn->getBody());
  return BufferElements[key] = uses.Ok ? uses.Elem : nullptr;
}

void ModuleLowering::BufferUses::stmt(ast::Stmt *s) {
  if (!s || !Ok)
    return;
  if (auto *x = ast::dyn_cast<ast::CompoundStmt>(s)) {
    for (ast::Stmt *t : x->getStatements())
      stmt(t);
  } else if (auto *x = ast::dyn_cast<ast::ReturnStmt>(s)) {
    expr(x->getReturnValue());
  } else if (auto *x = ast::dyn_cast<ast::ExprStmt>(s)) {
    expr(x->getExpr());
  } else if (auto *x = ast::dyn_cast<ast::DeclStmt>(s)) {
    auto *vd = ast::dyn_cast<ast::VarDecl>(x->getDecl());
    Ok = vd != nullptr;
    if (vd)
      expr(vd->getInitExpr());
  } else if (auto *x = ast::dyn_cast<ast::AssignStmt>(s)) {
    expr(x->getValue());
  } else if (auto *x = ast::dyn_cast<ast::MemberAssignStmt>(s)) {
    expr(x->getReceiver());
    if (x->getFieldName() == Field && of(x->getReceiver()))
      storeBuffer(x->getValue());
    else
      expr(x->getValue());
  } else if (auto *x = ast::dyn_cast<ast::SubscriptAssignStmt>(s)) {
    expr(x->getArray());
    expr(x->getIndex());
    expr(x->getValue());
  } else if (auto *x = ast::dyn_cast<ast::DestructureStmt>(s)) {
    expr(x->getValue());
  } else if (auto *x = ast::dyn_cast<ast::IfStmt>(s)) {
    expr(x->getCondition());
    stmt(x->getThenBranch());
    stmt(x->getElseBranch());
  } else if (auto *x = ast::dyn_cast<ast::WhileStmt>(s)) {
    expr(x->getCondition());
    stmt(x->getBody());
  } else if (auto *x = ast::dyn_cast<ast::ForStmt>(s)) {
    expr(x->getStart());
    expr(x->getEnd());
    expr(x->getStep());
    stmt(x->getBody());
  } else if (auto *x = ast::dyn_cast<ast::MatchStmt>(s)) {
    expr(x->getSubject());
    for (ast::MatchArm *arm : x->getArms())
      stmt(arm->getBody());
  } else if (!ast::isa<ast::BreakStmt>(s) && !ast::isa<ast::ContinueStmt>(s)) {
    Ok = false;
  }
}

void ModuleLowering::BufferUses::storeBuffer(ast::Expr *value) {
  auto *ce = ast::dyn_cast<ast::CallExpr>(value);
  const ast::FuncDecl *native =
      ce ? L.localNative(ce->getCalleeName()) : nullptr;
  if (!native || native->getNativeSymbol() != names::kPaykanBufferNew ||
      native->getTypeArgs().empty()) {
    Ok = false;
    return;
  }
  ast::Type *elem = L.slotType(native->getTypeArgs().front());
  Ok = !Elem || Elem == elem;
  Elem = elem;
  for (ast::Expr *a : ce->getArguments())
    expr(a);
}

void ModuleLowering::BufferUses::expr(ast::Expr *e) {
  if (!e || !Ok)
    return;
  if (isField(e)) {
    Ok = false; // used otherwise than by a Buffer function
    return;
  }
  if (auto *x = ast::dyn_cast<ast::CallExpr>(e)) {
    const ast::FuncDecl *native = L.localNative(x->getCalleeName());
    const bool buffer =
        native && std::any_of(std::begin(names::kPaykanBufferFunctions),
                              std::end(names::kPaykanBufferFunctions),
                              [&](const names::BufferFunction &f) {
                                return native->getNativeSymbol() == f.Symbol;
                              });
    for (ast::Expr *a : x->getArguments()) {
      if (buffer && isField(a))
        expr(ast::cast<ast::MemberAccessExpr>(a)->getReceiver());
      else
        expr(a);
    }
  } else if (auto *x = ast::dyn_cast<ast::MethodCallExpr>(e)) {
    expr(x->getReceiver());
    for (ast::Expr *a : x->getArguments())
      expr(a);
  } else if (auto *x = ast::dyn_cast<ast::CallValueExpr>(e)) {
    expr(x->getCallee());
    for (ast::Expr *a : x->getArguments())
      expr(a);
  } else if (auto *x = ast::dyn_cast<ast::MemberAccessExpr>(e)) {
    expr(x->getReceiver());
  } else if (auto *x = ast::dyn_cast<ast::UnaryExpr>(e)) {
    expr(x->getOperand());
  } else if (auto *x = ast::dyn_cast<ast::BinaryExpr>(e)) {
    expr(x->getLHS());
    expr(x->getRHS());
  } else if (auto *x = ast::dyn_cast<ast::TernaryExpr>(e)) {
    expr(x->getCondition());
    expr(x->getTrueExpr());
    expr(x->getFalseExpr());
  } else if (auto *x = ast::dyn_cast<ast::SubscriptExpr>(e)) {
    expr(x->getArray());
    expr(x->getIndex());
  } else if (auto *x = ast::dyn_cast<ast::TupleIndexExpr>(e)) {
    expr(x->getTuple());
  } else if (auto *x = ast::dyn_cast<ast::ArrayLiteralExpr>(e)) {
    for (ast::Expr *el : x->getElements())
      expr(el);
  } else if (auto *x = ast::dyn_cast<ast::TupleLiteralExpr>(e)) {
    for (ast::Expr *el : x->getElements())
      expr(el);
  } else if (auto *x = ast::dyn_cast<ast::LambdaExpr>(e)) {
    stmt(x->getBody());
  }
}

// -- Fields stored only at construction
//
// A class's field keeps the reference its object's `__init__` gives it
// (`self.f = v` in the body of the class's `__init__` or of a derived
// class's) for as long as the object lives when no other code stores into
// it: an assignment (a compound one too), an `inout` argument or local, a
// `match` with an arm's `n: inout T` (which writes its subject back), and an
// `-> inout` function's result, which its caller may write through, are
// stores; a lambda's body is code of its own, wherever it is written.  An
// `__init__` runs once, on an object no one else has yet, and is called by
// nothing but its constructor and a derived class's `__init__` (Sema), so
// its stores are made in its own frame, under any code that reads the
// object, and none is made while a call made by such code runs.  A native
// function stores only through its `inout` arguments (docs/language/
// 03-functions.md), and an object's destructor runs no code of the program.
//
// What code may store into a field depends on what may name it.  The main
// module of a program (World::Closed) is the only one naming its classes and
// the instances of its templates, and it lowers every function run on them:
// its own, the methods of its classes, and its copies of other modules'
// templates, whose bodies may name a type argument's fields; a template
// itself runs only as such a copy.  Any other module may be imported, and
// an importer may name its classes, so only a private field's stores are all
// known there: one its importers' code cannot name (an importer's copy of
// another module's template cannot either), stored by none of the module's
// code and none of its templates, which may run in an importer's copy, and
// whose fields are known there by name only.

struct ModuleLowering::FieldStoreScan {
  ModuleLowering &L;
  /// A template's body, whose types are not resolved: a store is known by
  /// its field's name only, and any argument may be an `inout` one.
  bool Template;
  /// The statements read are an `__init__`'s own (not a lambda's in it).
  bool Init = false;
  /// The function read returns `-> inout`: its caller may store into what
  /// it returns.
  bool InoutResult = false;

  /// Read @p fn's body: an `__init__`'s stores into `self` are its
  /// object's construction (a struct's, of its own storage).
  void function(const ast::FuncDecl *fn) {
    Init = fn->getName() == names::kMethodInit;
    InoutResult = fn->getResultMode() == ast::ParamMode::Inout;
    stmt(fn->getBody());
  }
  void stmt(ast::Stmt *s);
  void expr(ast::Expr *e);
  void exprs(const std::vector<ast::Expr *> &es) {
    for (ast::Expr *e : es)
      expr(e);
  }
  /// The arguments @p args of a call, which may change those passed to its
  /// `inout` parameters.
  void arguments(const std::vector<ast::Expr *> &args) {
    for (ast::Expr *a : args)
      if (Template)
        place(a);
      else
        expr(a);
  }
  /// The storage @p e names is stored into: its own field's, and when it
  /// is a value held in memory (a struct's field, a tuple's element, an
  /// array's), that of what holds it.
  void place(ast::Expr *e);
  /// The field @p name of what @p recv evaluates to is stored into.
  void field(const ast::Expr *recv, const std::string &name);
  /// A field of what @p recv evaluates to is stored into: an object's is
  /// in the object, and a struct's part of where the struct is held.
  void holder(ast::Expr *recv) {
    if (!Template && ast::dyn_cast<ast::ClassType>(typeOf(recv)))
      expr(recv);
    else
      place(recv);
  }
  ast::Type *typeOf(const ast::Expr *e) const {
    return L.canonicalizeDeclType(e->getResolvedType());
  }
  /// Evaluate @p e's parts.
  void parts(ast::Expr *e);
};

void ModuleLowering::FieldStoreScan::field(const ast::Expr *recv,
                                           const std::string &name) {
  ast::Type *ty = Template ? nullptr : typeOf(recv);
  if (ast::dyn_cast<ast::StructType>(ty))
    return; // a struct's field: no fixed read is part of a struct
  for (auto *c = ast::dyn_cast<ast::ClassType>(ty); c; c = c->getSuperClass())
    for (const auto &[f, fieldTy] : c->getFields())
      if (f == name) {
        L.StoredFields.Fields.insert(c->getName() + names::kNameSep + name);
        return;
      }
  L.StoredFields.Names.insert(name);
}

void ModuleLowering::FieldStoreScan::place(ast::Expr *e) {
  if (auto *x = ast::dyn_cast<ast::MemberAccessExpr>(e)) {
    field(x->getReceiver(), x->getFieldName());
    return holder(x->getReceiver());
  }
  if (auto *x = ast::dyn_cast<ast::SubscriptExpr>(e)) {
    expr(x->getIndex());
    return place(x->getArray());
  }
  if (auto *x = ast::dyn_cast<ast::TupleIndexExpr>(e))
    return place(x->getTuple());
  if (auto *x = ast::dyn_cast<ast::TernaryExpr>(e)) {
    expr(x->getCondition());
    place(x->getTrueExpr());
    return place(x->getFalseExpr());
  }
  // A variable, or a call's `inout` result: what its function returns is a
  // place stored into there.
  parts(e);
}

void ModuleLowering::FieldStoreScan::stmt(ast::Stmt *s) {
  if (!s || L.StoredFields.Unknown)
    return;
  if (auto *x = ast::dyn_cast<ast::CompoundStmt>(s)) {
    for (ast::Stmt *t : x->getStatements())
      stmt(t);
  } else if (auto *x = ast::dyn_cast<ast::ReturnStmt>(s)) {
    if (InoutResult && x->getReturnValue())
      place(x->getReturnValue());
    else
      expr(x->getReturnValue());
  } else if (auto *x = ast::dyn_cast<ast::ExprStmt>(s)) {
    expr(x->getExpr());
  } else if (auto *x = ast::dyn_cast<ast::DeclStmt>(s)) {
    auto *vd = ast::dyn_cast<ast::VarDecl>(x->getDecl());
    if (!vd) {
      L.StoredFields.Unknown = true;
    } else if (vd->getMode() == ast::ParamMode::Inout && vd->getInitExpr()) {
      place(vd->getInitExpr());
    } else {
      expr(vd->getInitExpr());
    }
  } else if (auto *x = ast::dyn_cast<ast::AssignStmt>(s)) {
    expr(x->getValue()); // a variable's own storage
  } else if (auto *x = ast::dyn_cast<ast::MemberAssignStmt>(s)) {
    ast::Expr *recv = x->getReceiver();
    // What an `__init__` gives its object (or a struct's `__init__` its
    // own storage).
    const auto *id = ast::dyn_cast<ast::Identifier>(recv);
    if (Init && id && id->getName() == names::kSelf) {
      expr(recv);
    } else {
      field(recv, x->getFieldName());
      holder(recv);
    }
    expr(x->getValue());
  } else if (auto *x = ast::dyn_cast<ast::SubscriptAssignStmt>(s)) {
    place(x->getArray());
    expr(x->getIndex());
    expr(x->getValue());
  } else if (auto *x = ast::dyn_cast<ast::DestructureStmt>(s)) {
    expr(x->getValue()); // into variables
  } else if (auto *x = ast::dyn_cast<ast::IfStmt>(s)) {
    expr(x->getCondition());
    stmt(x->getThenBranch());
    stmt(x->getElseBranch());
  } else if (auto *x = ast::dyn_cast<ast::WhileStmt>(s)) {
    expr(x->getCondition());
    stmt(x->getBody());
  } else if (auto *x = ast::dyn_cast<ast::ForStmt>(s)) {
    expr(x->getStart());
    expr(x->getEnd());
    expr(x->getStep());
    stmt(x->getBody());
  } else if (auto *x = ast::dyn_cast<ast::MatchStmt>(s)) {
    // An arm's `n: inout T` writes its copy back to the subject.
    const std::vector<ast::MatchArm *> &arms = x->getArms();
    if (std::any_of(arms.begin(), arms.end(), [](const ast::MatchArm *a) {
          return a->hasBinding() && a->getMode() == ast::ParamMode::Inout;
        }))
      place(x->getSubject());
    else
      expr(x->getSubject());
    for (ast::MatchArm *arm : arms) {
      expr(arm->getLiteralPattern());
      stmt(arm->getBody());
    }
  } else if (!ast::isa<ast::BreakStmt>(s) && !ast::isa<ast::ContinueStmt>(s)) {
    L.StoredFields.Unknown = true;
  }
}

void ModuleLowering::FieldStoreScan::expr(ast::Expr *e) {
  if (!e || L.StoredFields.Unknown)
    return;
  // An `inout` argument, or the receiver of a struct's method that may
  // change `self`.
  if (e->isChangedByCall())
    return place(e);
  parts(e);
}

void ModuleLowering::FieldStoreScan::parts(ast::Expr *e) {
  if (ast::isa<ast::Identifier>(e) || ast::isa<ast::IntegerLiteral>(e) ||
      ast::isa<ast::FloatLiteral>(e) || ast::isa<ast::BoolLiteral>(e) ||
      ast::isa<ast::CharLiteral>(e) || ast::isa<ast::NoneLiteral>(e) ||
      ast::isa<ast::StringLiteral>(e) || ast::isa<ast::EnumValueExpr>(e))
    return;
  if (auto *x = ast::dyn_cast<ast::UnaryExpr>(e)) {
    expr(x->getOperand());
  } else if (auto *x = ast::dyn_cast<ast::BinaryExpr>(e)) {
    expr(x->getLHS());
    expr(x->getRHS());
  } else if (auto *x = ast::dyn_cast<ast::TernaryExpr>(e)) {
    expr(x->getCondition());
    expr(x->getTrueExpr());
    expr(x->getFalseExpr());
  } else if (auto *x = ast::dyn_cast<ast::MemberAccessExpr>(e)) {
    expr(x->getReceiver());
  } else if (auto *x = ast::dyn_cast<ast::SubscriptExpr>(e)) {
    expr(x->getArray());
    expr(x->getIndex());
  } else if (auto *x = ast::dyn_cast<ast::TupleIndexExpr>(e)) {
    expr(x->getTuple());
  } else if (auto *x = ast::dyn_cast<ast::ArrayLiteralExpr>(e)) {
    exprs(x->getElements());
  } else if (auto *x = ast::dyn_cast<ast::TupleLiteralExpr>(e)) {
    exprs(x->getElements());
  } else if (auto *x = ast::dyn_cast<ast::CallExpr>(e)) {
    arguments(x->getArguments());
  } else if (auto *x = ast::dyn_cast<ast::MethodCallExpr>(e)) {
    // A call on a box field cannot replace it; one on a value held in
    // memory may change it, and is marked so (isChangedByCall).
    expr(x->getReceiver());
    arguments(x->getArguments());
  } else if (auto *x = ast::dyn_cast<ast::CallValueExpr>(e)) {
    expr(x->getCallee());
    arguments(x->getArguments());
  } else if (auto *x = ast::dyn_cast<ast::LambdaExpr>(e)) {
    // Its body runs when it is called, after any `__init__` it is made in.
    const bool init = Init;
    const bool inout = InoutResult;
    Init = InoutResult = false;
    stmt(x->getBody());
    Init = init;
    InoutResult = inout;
  } else {
    L.StoredFields.Unknown = true;
  }
}

void ModuleLowering::scanFieldStores(ast::TranslationUnit *tu) {
  for (const ast::ClassDecl *cls : tu->getClassDecls())
    DeclaredClasses.insert(cls->getName());
  for (const auto &[name, fn] : Bodies)
    FieldStoreScan{*this, /*Template=*/false}.function(fn);
  // A program's main module runs its templates only as its own copies.
  if (Canonical.empty() && PL.MainWorld == World::Closed)
    return;
  auto templates = [&](const std::vector<ast::FuncDecl *> &fns) {
    for (const ast::FuncDecl *fn : fns)
      FieldStoreScan{*this, /*Template=*/true}.function(fn);
  };
  templates(tu->getGenericFuncDecls());
  for (const ast::ClassDecl *cd : tu->getGenericClassDecls())
    templates(cd->getMethods());
  for (const ast::StructDecl *sd : tu->getGenericStructDecls())
    templates(sd->getMethods());
  for (const ast::ImplDecl *im : tu->getImplDecls())
    if (!im->getTypeParams().empty())
      templates(im->getMethods());
}

bool ModuleLowering::isFixedField(ast::ClassType *ct,
                                  const std::string &field) {
  if (StoredFields.Unknown)
    return false;
  for (ast::ClassType *c = ct; c; c = c->getSuperClass())
    for (const auto &[f, fieldTy] : c->getFields()) {
      if (f != field)
        continue;
      if (!DeclaredClasses.count(c->getName()) || c->isWeakField(field) ||
          !ast::isRefType(canonicalizeDeclType(fieldTy)))
        return false;
      if (!(Canonical.empty() && PL.MainWorld == World::Closed) &&
          !names::isPrivateName(field))
        return false;
      return !StoredFields.Fields.count(c->getName() + names::kNameSep +
                                        field) &&
             !StoredFields.Names.count(field);
    }
  return false;
}

bool ModuleLowering::isFixedRead(const ast::Expr *read) {
  if (!CurrentMethodClassType || EmittingInit)
    return false;
  for (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(read); ma;
       ma = ast::dyn_cast<ast::MemberAccessExpr>(ma->getReceiver())) {
    auto *ct = ast::dyn_cast<ast::ClassType>(
        canonicalizeDeclType(ma->getReceiver()->getResolvedType()));
    if (!ct || !isFixedField(ct, ma->getFieldName()))
      return false;
    if (const auto *id = ast::dyn_cast<ast::Identifier>(ma->getReceiver()))
      return id->getName() == names::kSelf &&
             CurrentScope->isStable(names::kSelf) &&
             CurrentScope->inoutType(names::kSelf) == Type::Void;
  }
  return false;
}

} // namespace paykan::lowering
