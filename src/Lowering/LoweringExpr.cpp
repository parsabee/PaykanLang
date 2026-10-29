// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// AST -> PIR lowering: expressions, arrays, tuples, subscripts.

#include "LoweringInternal.h"
#include "Names.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

namespace paykan::lowering {

using namespace names;
using pir::CmpPred;
using pir::Opcode;
using pir::Type;

// -- Dispatch

Val ModuleLowering::ExprEmitter::visit(ast::Expr *node) {
  if (!ChainValues.empty()) {
    auto emitted = ChainValues.find(node);
    if (emitted != ChainValues.end()) {
      Val v = emitted->second;
      ChainValues.erase(emitted);
      return v;
    }
  }
  emitChainBelow(node);
  Val v = ast::ExprVisitor<ExprEmitter, Val>::visit(node);
  assert(!ChainValues.count(visitedFirst(node)) &&
         "a chain link's visitor emits the link it visits first");
  if (!v || !isOptionalWrap(node))
    return v;
  return L.emitSome(v, node->getCoercedType());
}

ast::Expr *ModuleLowering::ExprEmitter::visitedFirst(ast::Expr *e) {
  // As visitBinaryExpr: `==` and `!=` may compare in place.
  if (auto *b = ast::dyn_cast<ast::BinaryExpr>(e))
    return b->getOpcode() != ast::BinaryOpcode::Eq &&
                   b->getOpcode() != ast::BinaryOpcode::Ne
               ? b->getLHS()
               : nullptr;
  // As visitMethodCallExpr, branch by branch.
  if (auto *m = ast::dyn_cast<ast::MethodCallExpr>(e)) {
    ast::Expr *recv = m->getReceiver();
    ast::Type *ty = recv->getResolvedType();
    if (!ty)
      return nullptr;
    // A field's function value: in a struct's bytes, or in an object.
    if (m->isFieldCall())
      return L.getExprStructType(recv) ? L.memValueVisitsFirst(recv) : recv;
    if (ast::isa<ast::SliceType>(ty))
      return nullptr;
    if (ast::isa<ast::FixedArrayType>(ty))
      return ast::isa<ast::Identifier>(recv) ? nullptr
                                             : L.memValueVisitsFirst(recv);
    if (L.getExprStructType(recv) && m->getMethodName() != kMethodToString)
      return L.memValueVisitsFirst(recv);
    if ((ast::isa<ast::OptionalType>(ty) &&
         m->getMethodName() == kMethodToString) ||
        ast::isa<ast::TupleType>(ty) || ast::isa<ast::EnumType>(ty))
      return ast::isMemType(ty) ? L.memValueVisitsFirst(recv) : recv;
    // A builtin type's method, or a class's (emitReceiver).
    return recv;
  }
  // As lowerMemberAccessExpr: a field held in memory is its address; a
  // struct's field is read in place, in its bytes; a class's, in its object.
  if (auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e)) {
    if (ast::isMemType(ma->getResolvedType()))
      return L.memValueVisitsFirst(ma);
    return L.getExprStructType(ma->getReceiver())
               ? L.memValueVisitsFirst(ma->getReceiver())
               : ma->getReceiver();
  }
  // As visitTupleIndexExpr: an element in the tuple's bytes.
  if (auto *t = ast::dyn_cast<ast::TupleIndexExpr>(e))
    return L.memValueVisitsFirst(
        ast::isMemType(t->getResolvedType()) ? t : t->getTuple());
  // As visitSubscriptExpr: an element in a fixed-size array's or a slice's
  // bytes; a string's character, of the string (emitReceiver).
  if (auto *s = ast::dyn_cast<ast::SubscriptExpr>(e)) {
    ast::Type *arrTy = s->getArray()->getResolvedType();
    if (!ast::dyn_cast<ast::FixedArrayType>(arrTy) &&
        !ast::dyn_cast<ast::SliceType>(arrTy))
      return s->getArray();
    return L.memValueVisitsFirst(
        ast::isMemType(s->getResolvedType()) ? s : s->getArray());
  }
  // As visitCallValueExpr: the function value in place.
  if (auto *c = ast::dyn_cast<ast::CallValueExpr>(e))
    return L.memValueVisitsFirst(c->getCallee());
  return nullptr;
}

void ModuleLowering::ExprEmitter::emitChainBelow(ast::Expr *e) {
  std::vector<ast::Expr *> links;
  for (ast::Expr *x = visitedFirst(e);
       x && ast::chainOperand(x) && !ChainValues.count(x); x = visitedFirst(x))
    links.push_back(x);
  if (links.empty())
    return;
  // Each link is taken as the visitor of the one above it would take it.
  const bool wantAddress = std::exchange(WantAddress, false);
  for (auto it = links.rbegin(); it != links.rend(); ++it)
    ChainValues[*it] = visit(*it);
  WantAddress = wantAddress;
}

// -- Literals

Val ModuleLowering::ExprEmitter::visitIntegerLiteral(
    ast::IntegerLiteral *node) {
  // `1` flowing into an `f32` or `f16` slot is that float (Sema typed it),
  // rounded once, as `f32(n)` is: an `f32` straight from the integer (the
  // f64 of one beyond 2^53 is already rounded), an `f16` from the f64,
  // which is exact below its 65520 overflow.  A literal above INT64_MAX is
  // its unsigned value; in an integer slot every literal is its bits.
  ast::Type *ty = node->getResolvedType();
  const ast::BuiltinType::Kind k = L.builtinKind(ty);
  const bool wide = node->isAboveInt64Max();
  if (k == ast::BuiltinType::F32)
    return Val::f64(
        static_cast<double>(wide ? static_cast<float>(node->getUnsignedValue())
                                 : static_cast<float>(node->getValue())));
  if (ast::BuiltinType::isFloatKind(k))
    return L.canonicalize(
        Val::f64(wide ? static_cast<double>(node->getUnsignedValue())
                      : static_cast<double>(node->getValue())),
        ty);
  return Val::i64(node->getValue());
}

Val ModuleLowering::ExprEmitter::visitFloatLiteral(ast::FloatLiteral *node) {
  // Rounded to the `f32` or `f16` the literal has.
  return L.canonicalize(Val::f64(node->getValue()), node->getResolvedType());
}

Val ModuleLowering::ExprEmitter::visitBoolLiteral(ast::BoolLiteral *node) {
  return Val::boolean(node->getValue());
}

Val ModuleLowering::ExprEmitter::visitCharLiteral(ast::CharLiteral *node) {
  return Val::chr(node->getValue());
}

Val ModuleLowering::ExprEmitter::visitNoneLiteral(ast::NoneLiteral *node) {
  // `None` of an optional value type: the absent value.
  if (ast::isValueOptional(node->getResolvedType()))
    return L.B.none(L.toPIRType(node->getResolvedType()), "none");
  // The immortal None singleton, as a raw object.
  return L.externObject(kPaykanObjectNone);
}

Val ModuleLowering::ExprEmitter::visitStringLiteral(ast::StringLiteral *node) {
  // Every value use of a literal is a fresh PaykanString* over the interned
  // data: a tracked string temporary, owned like a concatenation's result
  // (boxed by a new owner, destroyed after a borrowing use).  Only the
  // runtime-call operand `@.strN` itself is a raw C string.
  return L.wrapStringLiteral(L.internString(node->getValue()),
                             node->getValue().size());
}

Val ModuleLowering::ExprEmitter::visitEnumValueExpr(ast::EnumValueExpr *node) {
  // `m::f` used as a value: a function value held in memory, its address;
  // a variant of a tagged union is held in memory too.
  if (node->getFunctionRef() || isVariantExpr(node))
    return L.emitMemAddress(node);
  return Val::i64(node->getValue());
}

Val ModuleLowering::ExprEmitter::visitLambdaExpr(ast::LambdaExpr *node) {
  return L.emitMemAddress(node);
}

Val ModuleLowering::emitFunctionValueCall(ast::Expr *callee,
                                          const std::vector<ast::Expr *> &args,
                                          ast::Type *resultTy) {
  auto *fnt = ast::dyn_cast<ast::FunctionType>(callee->getResolvedType());
  if (!fnt) {
    reportInternalError("call of a value that holds no function");
    return Val();
  }
  // The function value in place (a temporary's, or the object holding a
  // field's, kept until after the call).
  size_t mark = MemTemps.size();
  Val addr = emitMemAddress(callee);
  if (!addr)
    return Val();
  Val result = emitClosureCall(addr, fnt, args, resultTy);
  releaseMemTemps(mark);
  return result;
}

Val ModuleLowering::ExprEmitter::visitCallValueExpr(ast::CallValueExpr *node) {
  WantAddress = false; // a function type's result is never an `inout`
  return L.emitFunctionValueCall(node->getCallee(), node->getArguments(),
                                 node->getResolvedType());
}

Val ModuleLowering::ExprEmitter::visitIdentifier(ast::Identifier *node) {
  // A function used as a value: a function value held in memory.
  if (node->isFunctionRef())
    return L.emitMemAddress(node);
  // A value held in memory, or a slice: the address of its bytes.
  if (ast::Type *t = L.CurrentScope->lookupASTType(node->getName());
      ast::isMemType(t) || ast::dyn_cast<ast::SliceType>(t))
    return L.emitMemAddress(node);
  // A `weak` variable: a fresh +1 box of its object, or null once the
  // object is gone (exprProducesFreshBox).
  if (L.CurrentScope->isWeak(node->getName()))
    return L.loadWeakVar(node->getName());
  pir::LocalId local = L.CurrentScope->lookup(node->getName());
  Val val = L.B.load(local, node->getName());
  auto *astTy = L.CurrentScope->lookupASTType(node->getName());
  // A read its statement may replace the variable's value under
  // (noteHeldReads): the box, with a reference of its own, a fresh +1 one
  // (exprProducesFreshBox).
  auto held = [&](const Val &box) {
    L.emitRetain(box, /*lend=*/false, astTy);
    return box;
  };
  // An `inout` parameter: read the caller's storage (a box: borrowed, and
  // unwrapped like an owned variable's).
  if (Type t = L.CurrentScope->inoutType(node->getName()); t != Type::Void) {
    Val v = L.B.ptrLoad(val, t, node->getName(),
                        t == Type::Box && L.isStableRead(node));
    if (t == Type::Box && L.isHeldRead(node))
      return held(v);
    return t == Type::Box ? L.emitSharedGet(v, node->getName() + ".obj") : v;
  }
  // Owned ref vars store a box — unwrap to the underlying object.
  if (ast::isRefType(astTy) && L.CurrentScope->isOwned(node->getName())) {
    if (L.isHeldRead(node))
      return held(val);
    val = L.emitSharedGet(val, node->getName() + ".obj");
  }
  return val;
}

Val ModuleLowering::ExprEmitter::visitMemberAccessExpr(
    ast::MemberAccessExpr *node) {
  return L.lowerMemberAccessExpr(node);
}

// -- Arrays

// A literal's value is the address of its temporary (emitMemValue).
Val ModuleLowering::ExprEmitter::visitArrayLiteralExpr(
    ast::ArrayLiteralExpr *node) {
  return L.emitMemAddress(node);
}

Val ModuleLowering::ExprEmitter::visitSubscriptExpr(ast::SubscriptExpr *node) {
  // An element of a fixed-size array or a slice, read in place (one held
  // in memory is its address).
  if (ast::Type *arrTy = node->getArray()->getResolvedType();
      ast::dyn_cast<ast::FixedArrayType>(arrTy) ||
      ast::dyn_cast<ast::SliceType>(arrTy)) {
    if (ast::isMemType(node->getResolvedType()))
      return L.emitMemAddress(node);
    size_t mark = L.MemTemps.size();
    Val addr = L.emitFixedElementAddress(node->getArray(), node->getIndex());
    if (!addr)
      return addr;
    // A temporary's element, or a held one, is a +1 box
    // (exprProducesFreshBox).
    return L.ownElementRead(
        node,
        L.emitElementLoad(addr, node->getResolvedType(), L.isStableRead(node)),
        mark);
  }
  // A string's character: the string is read first, then the index is
  // computed (a read a call there may replace holds a reference of its own).
  Receiver recv = L.emitReceiver(node->getArray());
  if (!recv)
    return Val();
  Val idx = visit(node->getIndex());
  if (!idx)
    return Val();
  idx = L.coerceBoolToI64(idx, Type::I64);
  Val ch = L.callRuntime(kPaykanStringCharAt, {recv.Raw, idx}, "str.char_at");
  L.releaseIfOwned(recv.Owned);
  return ch;
}

Val ModuleLowering::ExprEmitter::visitTernaryExpr(ast::TernaryExpr *node) {
  // A value held in memory: the address of a temporary (emitMemValue).
  if (ast::isMemType(node->getResolvedType()))
    return L.emitMemAddress(node);
  Val cond = visit(node->getCondition());
  if (!cond)
    return Val();
  // Both branches void (a statement, `if c then f() else g();`): one runs
  // for its effect, and there is no value.
  if (node->getTrueExpr()->getResolvedType() == L.ASTCtx.getVoidTy()) {
    pir::If *s = L.B.openIf(cond, true);
    L.B.enter(*s->Then);
    visit(node->getTrueExpr());
    L.B.leave();
    L.B.enter(*s->Else);
    visit(node->getFalseExpr());
    L.B.leave();
    return Val();
  }
  bool isClassResult =
      node->getResolvedType() && ast::isRefType(node->getResolvedType());
  Type resultTy =
      isClassResult ? Type::Box : L.toPIRType(node->getResolvedType());

  pir::LocalId tmp =
      L.B.addLocal("tern", resultTy == Type::Void ? Type::I64 : resultTy);
  pir::If *s = L.B.openIf(cond, true);
  L.B.enter(*s->Then);
  Val t = isClassResult ? L.emitAsShared(node->getTrueExpr())
                        : visit(node->getTrueExpr());
  if (!t)
    return Val();
  if (!isClassResult && resultTy == Type::Void) {
    // Sema left no type: take the branch's.
    L.B.function()->Locals[tmp].Ty = t.Ty;
  }
  L.B.store(tmp, L.coerceTo(t, L.B.localType(tmp)));
  L.B.leave();
  L.B.enter(*s->Else);
  Val f = isClassResult ? L.emitAsShared(node->getFalseExpr())
                        : visit(node->getFalseExpr());
  if (!f)
    return Val();
  L.B.store(tmp, L.coerceTo(f, L.B.localType(tmp)));
  L.B.leave();
  return L.B.load(tmp, "tern");
}

Val ModuleLowering::ExprEmitter::visitUnaryExpr(ast::UnaryExpr *node) {
  Val operand = visit(node->getOperand());
  if (!operand)
    return Val();
  switch (node->getOpcode()) {
  case ast::UnaryOpcode::Neg: {
    Val neg;
    if (auto *i = std::get_if<int64_t>(&operand.Op.V))
      neg = Val::i64(static_cast<int64_t>(0ULL - static_cast<uint64_t>(*i)));
    else if (auto *d = std::get_if<double>(&operand.Op.V))
      neg = Val::f64(-*d);
    else
      neg = L.B.unary(Opcode::Neg, operand,
                      operand.Ty == Type::F64 ? "fneg" : "neg");
    // `-(-128)` as an i8 wraps to -128; a float's negation is exact.
    ast::Type *ty = node->getResolvedType();
    if (ast::BuiltinType::isIntegerKind(L.builtinKind(ty)))
      neg = L.canonicalize(neg, ty);
    return neg;
  }
  case ast::UnaryOpcode::Not:
    if (auto *b = std::get_if<bool>(&operand.Op.V))
      return Val::boolean(!*b);
    return L.B.unary(Opcode::Not, operand, "not");
  case ast::UnaryOpcode::BitNot: {
    // The complement within the width (docs/design/bitwise.md, "Narrow
    // types and masks"): `xor` with the width's ones for a `u8` or a
    // `u16`, the 64-bit complement and its `mask 32` for a `u32`.
    ast::Type *ty = node->getResolvedType();
    if (auto *c = std::get_if<int64_t>(&operand.Op.V))
      return L.canonicalize(Val::i64(~*c), ty);
    const unsigned bits = ast::BuiltinType::integerBits(L.builtinKind(ty));
    if (bits < 32)
      return L.B.binary(Opcode::Xor, operand,
                        Val::i64(static_cast<int64_t>((1ULL << bits) - 1)),
                        "compl");
    return L.canonicalize(
        L.B.binary(Opcode::Xor, operand, Val::i64(-1), "compl"), ty);
  }
  case ast::UnaryOpcode::Count:
    break;
  }
  assert(false && "unknown UnaryOpcode");
  return Val();
}

Val ModuleLowering::ExprEmitter::emitShortCircuit(ast::BinaryExpr *node,
                                                  bool isAnd) {
  Val lhs = visit(node->getLHS());
  if (!lhs)
    return Val();
  pir::LocalId tmp = L.B.addLocal(isAnd ? "and" : "or", Type::Bool);
  pir::If *s = L.B.openIf(lhs, true);
  // and: lhs ? rhs : false        or: lhs ? true : rhs
  L.B.enter(*s->Then);
  if (isAnd) {
    Val rhs = visit(node->getRHS());
    if (!rhs)
      return Val();
    L.B.store(tmp, rhs);
  } else {
    L.B.store(tmp, Val::boolean(true));
  }
  L.B.leave();
  L.B.enter(*s->Else);
  if (isAnd) {
    L.B.store(tmp, Val::boolean(false));
  } else {
    Val rhs = visit(node->getRHS());
    if (!rhs)
      return Val();
    L.B.store(tmp, rhs);
  }
  L.B.leave();
  return L.B.load(tmp, isAnd ? "and" : "or");
}

Val ModuleLowering::ExprEmitter::emitIntDivGuards(const Val &lhs,
                                                  const Val &rhs, bool isDiv,
                                                  bool checkOverflow) {
  // Divide / modulo by zero: runtime panic (sdiv/srem by zero is UB).
  Val isZero = L.B.cmp(CmpPred::Eq, rhs, Val::i64(0), "divzero.chk");
  pir::If *z = L.B.openIf(isZero, false);
  L.B.enter(*z->Then);
  L.callRuntime(kPaykanPanicDivByZero, {});
  L.B.emitUnreachable();
  L.B.leave();
  // Only `int` reaches INT64_MIN: a narrower type's quotient wraps.
  if (!isDiv || !checkOverflow)
    return rhs;
  // INT64_MIN / -1 overflows: panic like the divide-by-zero case.
  Val isMin = L.B.cmp(CmpPred::Eq, lhs, Val::i64(INT64_MIN));
  Val isNegOne = L.B.cmp(CmpPred::Eq, rhs, Val::i64(-1));
  Val overflows =
      L.B.select(isMin, isNegOne, Val::boolean(false), "divovf.chk");
  pir::If *o = L.B.openIf(overflows, false);
  L.B.enter(*o->Then);
  L.callRuntime(kPaykanPanicDivOverflow, {});
  L.B.emitUnreachable();
  L.B.leave();
  return rhs;
}

namespace {

/// The values of @p a and @p b, read unsigned, when both are constants.
std::optional<std::pair<uint64_t, uint64_t>> foldInts(const Val &a,
                                                      const Val &b) {
  const auto *x = std::get_if<int64_t>(&a.Op.V);
  const auto *y = std::get_if<int64_t>(&b.Op.V);
  if (!x || !y)
    return std::nullopt;
  return std::pair{static_cast<uint64_t>(*x), static_cast<uint64_t>(*y)};
}

/// Whether the shift count @p count is below 64 by its form: `e & k` with
/// a literal `k` of 0 to 63, or `e % k` with a literal `k` of 1 to 64.  A
/// masked count and the rotate idiom need no overshift guard.
bool countBelow64(const ast::Expr *count) {
  const auto *b = ast::dyn_cast<ast::BinaryExpr>(count);
  if (!b)
    return false;
  auto literal = [](const ast::Expr *e, uint64_t lo, uint64_t hi) {
    const auto *il = ast::dyn_cast<ast::IntegerLiteral>(e);
    return il && !il->isAboveInt64Max() && il->getValue() >= 0 &&
           static_cast<uint64_t>(il->getValue()) >= lo &&
           static_cast<uint64_t>(il->getValue()) <= hi;
  };
  if (b->getOpcode() == ast::BinaryOpcode::BitAnd)
    return literal(b->getLHS(), 0, 63) || literal(b->getRHS(), 0, 63);
  if (b->getOpcode() == ast::BinaryOpcode::Mod)
    return literal(b->getRHS(), 1, 64);
  return false;
}

} // namespace

Val ModuleLowering::ExprEmitter::emitShift(bool left, const Val &value,
                                           const Val &count,
                                           const ast::Expr *countExpr,
                                           ast::Type *resultTy) {
  // A shift by the width or more is 0 (docs/design/bitwise.md, "How each
  // layer gives 0"); PIR's `shl` and `shr` read the count mod 64.  A `<<`
  // on a `u8`, `u16` or `u32` can set bits at and above the width, which
  // its mask clears; a `>>` of a zero-extended value stays so.
  const unsigned width = ast::BuiltinType::integerBits(L.builtinKind(resultTy));
  const Opcode op = left ? Opcode::Shl : Opcode::Shr;
  const char *name = left ? "shl" : "shr";
  auto finish = [&](const Val &v) {
    return left ? L.canonicalize(v, resultTy) : v;
  };
  if (const auto *c = std::get_if<int64_t>(&count.Op.V)) {
    // A constant count: 0 at or above the width (the value is evaluated
    // all the same), a plain shift below it, and two constants fold.
    const auto n = static_cast<uint64_t>(*c);
    if (n >= width)
      return Val::i64(0);
    if (const auto *v = std::get_if<int64_t>(&value.Op.V)) {
      const auto x = static_cast<uint64_t>(*v);
      return finish(Val::i64(static_cast<int64_t>(left ? x << n : x >> n)));
    }
    return finish(L.B.binary(op, value, count, name));
  }
  Val shifted = L.B.binary(op, value, count, name);
  // Any other count: 0 from 64 on, by a guard.  From the width to 63 the
  // mask clears a `<<` and a `>>` gives 0 already, so one comparison with
  // 64 covers every width; unsigned, so a count of 2^63 or more is no
  // negative one.
  if (!countBelow64(countExpr)) {
    Val inRange = L.B.ucmp(CmpPred::Lt, count, Val::i64(64), "shift.ok");
    shifted = L.B.select(inRange, shifted, Val::i64(0), name);
  }
  return finish(shifted);
}

Val ModuleLowering::ExprEmitter::visitBinaryExpr(ast::BinaryExpr *node) {
  if (node->getOpcode() == ast::BinaryOpcode::And)
    return emitShortCircuit(node, true);
  if (node->getOpcode() == ast::BinaryOpcode::Or)
    return emitShortCircuit(node, false);

  // Reference-typed equality lowers to the virtual `equals` method through a
  // synthesized MethodCallExpr (one evaluation per operand, the method-call
  // ABI for receiver teardown and the consumed `Obj` argument).
  if (node->getOpcode() == ast::BinaryOpcode::Eq ||
      node->getOpcode() == ast::BinaryOpcode::Ne) {
    ast::Type *lt = node->getLHS()->getResolvedType();
    ast::Type *rt = node->getRHS()->getResolvedType();
    // A function value compares as an optional does (emitOptionalEquality):
    // against None, or as the same code and closure.
    if (ast::isa<ast::OptionalType>(lt) || ast::isa<ast::OptionalType>(rt) ||
        ast::isa<ast::TupleType>(lt) || ast::isFunctionValue(lt) ||
        ast::isFunctionValue(rt) || ast::isPayloadEnum(lt)) {
      Val eq = ast::isa<ast::TupleType>(lt)
                   ? L.emitTupleEquals(node->getLHS(), node->getRHS())
               : ast::isPayloadEnum(lt)
                   ? L.emitEnumEquals(node->getLHS(), node->getRHS())
                   : L.emitOptionalEquality(node);
      if (!eq)
        return Val();
      if (node->getOpcode() == ast::BinaryOpcode::Ne)
        eq = L.B.unary(Opcode::Not, eq, "ne");
      return eq;
    }
    if (lt && rt && ast::isRefType(lt) && ast::isRefType(rt)) {
      auto *call = L.ASTCtx.make<ast::MethodCallExpr>(
          node->getLocation(), node->getLHS(), L.ASTCtx.intern(kMethodEquals),
          std::vector<ast::Expr *>{node->getRHS()});
      call->setResolvedType(L.ASTCtx.getBoolTy());
      Val eq = visit(call);
      if (!eq)
        return Val();
      if (node->getOpcode() == ast::BinaryOpcode::Ne)
        eq = L.B.unary(Opcode::Not, eq, "ne");
      return eq;
    }
  }

  Val lhs = visit(node->getLHS());
  Val rhs = visit(node->getRHS());
  if (!lhs || !rhs)
    return Val();

  // Implicit int -> float promotion.
  if (lhs.Ty == Type::F64 && rhs.Ty == Type::I64)
    rhs = L.promoteIntToFloat(rhs, L.ASTCtx.getFloatTy());
  else if (rhs.Ty == Type::F64 && lhs.Ty == Type::I64)
    lhs = L.promoteIntToFloat(lhs, L.ASTCtx.getFloatTy());
  bool isFloat = lhs.Ty == Type::F64;

  // The operands' numeric type (one type, or `int` with `float`): an
  // `unsigned` divides, takes the remainder and orders unsigned; a narrower
  // type's arithmetic result is wrapped or rounded back to its form
  // (docs/design/sized-types.md).
  const ast::BuiltinType::Kind kind =
      L.builtinKind(node->getLHS()->getResolvedType());
  const bool isUnsigned64 = kind == ast::BuiltinType::U64;
  ast::Type *resultTy = node->getResolvedType();
  auto canon = [&](const Val &v) { return L.canonicalize(v, resultTy); };

  // A char orders by its code, 0..255: both are zero-extended, as `int<char>`
  // is, since PIR's `cmp` on two chars orders their bytes signed.
  const ast::BinaryOpcode op = node->getOpcode();
  if (lhs.Ty == Type::Char && rhs.Ty == Type::Char &&
      (op == ast::BinaryOpcode::Lt || op == ast::BinaryOpcode::Gt ||
       op == ast::BinaryOpcode::Le || op == ast::BinaryOpcode::Ge)) {
    lhs = L.B.cast(lhs, Type::I64, "lhs.code");
    rhs = L.B.cast(rhs, Type::I64, "rhs.code");
  }

  switch (op) {
  case ast::BinaryOpcode::Add:
    if ((lhs.Ty == Type::Box || lhs.Ty == Type::Obj) &&
        (rhs.Ty == Type::Box || rhs.Ty == Type::Obj)) {
      // Classify each operand once; the concat borrows both and returns a
      // brand-new owned string.
      ExprValue lhsEV = L.classifyExpr(node->getLHS(), lhs);
      ExprValue rhsEV = L.classifyExpr(node->getRHS(), rhs);
      if (L.exprAlreadyShared(node->getLHS()) && lhs.Ty == Type::Box)
        lhs = L.emitSharedGet(lhs, "lhs.obj");
      if (L.exprAlreadyShared(node->getRHS()) && rhs.Ty == Type::Box)
        rhs = L.emitSharedGet(rhs, "rhs.obj");
      Val result = L.callRuntime(kPaykanStringConcat, {lhs, rhs}, "concat");
      L.releaseIfOwned(lhsEV);
      L.releaseIfOwned(rhsEV);
      L.trackStringTemp(result);
      return result;
    }
    return canon(L.B.binary(Opcode::Add, lhs, rhs, isFloat ? "fadd" : "add"));
  case ast::BinaryOpcode::Sub:
    return canon(L.B.binary(Opcode::Sub, lhs, rhs, isFloat ? "fsub" : "sub"));
  case ast::BinaryOpcode::Mul:
    return canon(L.B.binary(Opcode::Mul, lhs, rhs, isFloat ? "fmul" : "mul"));
  case ast::BinaryOpcode::Div:
    if (isFloat)
      return canon(L.B.binary(Opcode::Div, lhs, rhs, "fdiv"));
    emitIntDivGuards(lhs, rhs, /*isDiv=*/true,
                     /*checkOverflow=*/kind == ast::BuiltinType::Int);
    if (isUnsigned64)
      return L.B.binary(Opcode::UDiv, lhs, rhs, "udiv");
    return canon(L.B.binary(Opcode::Div, lhs, rhs, "sdiv"));
  case ast::BinaryOpcode::Mod: {
    if (isFloat)
      return canon(L.B.binary(Opcode::Rem, lhs, rhs, "fmod"));
    emitIntDivGuards(lhs, rhs, /*isDiv=*/false, /*checkOverflow=*/false);
    if (isUnsigned64)
      return L.B.binary(Opcode::URem, lhs, rhs, "urem");
    // x % -1 is 0 for every x, but INT64_MIN % -1 traps: divide by 1 instead.
    Val isNegOne = L.B.cmp(CmpPred::Eq, rhs, Val::i64(-1));
    Val safe = L.B.select(isNegOne, Val::i64(1), rhs, "rem.divisor");
    return canon(L.B.binary(Opcode::Rem, lhs, safe, "srem"));
  }
  case ast::BinaryOpcode::Lt:
    if (isUnsigned64)
      return L.B.ucmp(CmpPred::Lt, lhs, rhs, "ult");
    return L.B.cmp(CmpPred::Lt, lhs, rhs, isFloat ? "flt" : "slt");
  case ast::BinaryOpcode::Gt:
    if (isUnsigned64)
      return L.B.ucmp(CmpPred::Gt, lhs, rhs, "ugt");
    return L.B.cmp(CmpPred::Gt, lhs, rhs, isFloat ? "fgt" : "sgt");
  case ast::BinaryOpcode::Le:
    if (isUnsigned64)
      return L.B.ucmp(CmpPred::Le, lhs, rhs, "ule");
    return L.B.cmp(CmpPred::Le, lhs, rhs, isFloat ? "fle" : "sle");
  case ast::BinaryOpcode::Ge:
    if (isUnsigned64)
      return L.B.ucmp(CmpPred::Ge, lhs, rhs, "uge");
    return L.B.cmp(CmpPred::Ge, lhs, rhs, isFloat ? "fge" : "sge");
  case ast::BinaryOpcode::Eq:
    return L.B.cmp(CmpPred::Eq, lhs, rhs, isFloat ? "feq" : "eq");
  case ast::BinaryOpcode::Ne:
    return L.B.cmp(CmpPred::Ne, lhs, rhs, isFloat ? "fne" : "ne");
  // Of two zero-extended values, zero-extended: no mask
  // (docs/design/bitwise.md).
  case ast::BinaryOpcode::BitAnd:
    if (auto c = foldInts(lhs, rhs))
      return Val::i64(static_cast<int64_t>(c->first & c->second));
    return L.B.binary(Opcode::And, lhs, rhs, "and");
  case ast::BinaryOpcode::BitOr:
    if (auto c = foldInts(lhs, rhs))
      return Val::i64(static_cast<int64_t>(c->first | c->second));
    return L.B.binary(Opcode::Or, lhs, rhs, "or");
  case ast::BinaryOpcode::BitXor:
    if (auto c = foldInts(lhs, rhs))
      return Val::i64(static_cast<int64_t>(c->first ^ c->second));
    return L.B.binary(Opcode::Xor, lhs, rhs, "xor");
  case ast::BinaryOpcode::Shl:
  case ast::BinaryOpcode::Shr:
    return emitShift(op == ast::BinaryOpcode::Shl, lhs, rhs, node->getRHS(),
                     resultTy);
  case ast::BinaryOpcode::And:
  case ast::BinaryOpcode::Or:
  case ast::BinaryOpcode::Count:
    break;
  }
  assert(false && "unknown BinaryOpcode");
  return Val();
}

// -- Calls

Val ModuleLowering::ExprEmitter::emitIdentityCtor(ast::CallExpr *node) {
  Val arg = visit(node->getArguments()[0]);
  if (!arg)
    return Val();
  return arg;
}

Val ModuleLowering::ExprEmitter::emitBuiltinCall(ast::CallExpr *node) {
  auto it = L.FunctionTable.find(node->getCalleeName());
  if (it == L.FunctionTable.end())
    return Val();
  const auto &info = it->second;
  const pir::Function &runtimeFn = L.declareRuntime(info.RuntimeName);
  const pir::Signature &sig = runtimeFn.Sig;

  std::vector<Val> args;
  // Owned argument temporaries the builtin borrows (string temps, fresh
  // boxes); torn down after the call.
  std::vector<ExprValue> ownedArgs;
  for (size_t i = 0; i < node->getNumArguments(); ++i) {
    auto *argExpr = node->getArguments()[i];
    // A tuple or an enum printed: its text.
    if (ast::isa<ast::TupleType>(argExpr->getResolvedType()) ||
        ast::isa<ast::EnumType>(argExpr->getResolvedType())) {
      Val v = ast::isa<ast::EnumType>(argExpr->getResolvedType())
                  ? L.emitEnumToString(argExpr)
                  : L.emitTupleToString(argExpr);
      if (!v)
        return Val();
      ownedArgs.push_back(ExprValue::owned(v));
      args.push_back(v);
      continue;
    }
    Val v = visit(argExpr);
    if (!v)
      return Val();
    // An optional value printed: its text, "None" or the value's.
    if (pir::isOptional(v.Ty)) {
      v = L.emitOptionalToString(v, argExpr->getResolvedType());
      ownedArgs.push_back(ExprValue::owned(v));
      args.push_back(v);
      continue;
    }
    ExprValue ev = L.classifyExpr(argExpr, v);
    if (ev.isOwned())
      ownedArgs.push_back(ev);
    if (L.exprAlreadyShared(argExpr) && v.Ty == Type::Box)
      v = L.emitSharedGet(v, "unboxed");
    // An optional reference printed: the None object for a null one, so it
    // prints as "None".
    if (v.Ty == Type::Obj &&
        ast::isa<ast::OptionalType>(argExpr->getResolvedType()))
      v = L.emitNoneObject(v);
    Type paramTy = i < sig.Params.size() ? sig.Params[i] : Type::Void;
    v = L.coerceBoolToI64(v, paramTy);
    args.push_back(v);
  }

  bool returnsOwnedString = isStrConversion(node->getCalleeName());

  Val result = L.B.call(runtimeFn.Name, sig, args, "call");
  for (const auto &ev : ownedArgs)
    L.releaseIfOwned(ev);
  if (returnsOwnedString)
    L.trackStringTemp(result);
  return result;
}

// The parse of the string object @p str into the primitive @p target, the
// core of `s.toInt()` & co.: the runtime parses the borrowed string into a
// local and says whether it is valid; the result is that value as the
// present `int?` & co., or None.  @p owned, a temporary @p str is part of,
// is released after the parse.  A sized integer is parsed as a 64-bit one
// (an unsigned one without a sign) and refused outside its range; a sized
// float is the runtime's value of the type, refused when finite text rounds
// to infinity in it.
Val ModuleLowering::ExprEmitter::emitParseOf(const Val &str,
                                             const ExprValue &owned,
                                             ast::BuiltinType::Kind target) {
  using ast::BuiltinType;
  const char *parse = kPaykanStringParseInt;
  Type payload = Type::I64;
  if (target == BuiltinType::Bool) {
    parse = kPaykanStringParseBool;
  } else if (target == BuiltinType::F32) {
    parse = kPaykanStringParseF32;
    payload = Type::F64;
  } else if (target == BuiltinType::F16) {
    parse = kPaykanStringParseF16;
    payload = Type::F64;
  } else if (target == BuiltinType::Float) {
    parse = kPaykanStringParseFloat;
    payload = Type::F64;
  } else if (BuiltinType::isUnsignedKind(target)) {
    parse = kPaykanStringParseUint;
  }
  pir::LocalId out = L.B.addLocal("parsed", payload);
  Val ok = L.callRuntime(parse, {str, L.B.localAddr(out, "parsed.addr")},
                         "parse.ok");
  L.releaseIfOwned(owned);
  Val v = L.B.load(out, "parsed");
  if (target == BuiltinType::Bool)
    v = L.B.cmp(CmpPred::Ne, v, Val::i64(0), "parsed.bool");
  Val valid = L.B.cmp(CmpPred::Ne, ok, Val::i64(0), "parse.valid");
  const unsigned bits = BuiltinType::integerBits(target);
  if (BuiltinType::isIntegerKind(target) && bits < 64) {
    // Within the type's range, else None.  The compares are signed: an
    // unsigned parse of 2^63 or more holds a negative int64, below 0.
    const bool isUnsigned = BuiltinType::isUnsignedKind(target);
    const int64_t lo =
        isUnsigned ? 0 : -static_cast<int64_t>(1ULL << (bits - 1));
    const int64_t hi = static_cast<int64_t>(
        (isUnsigned ? (1ULL << bits) : (1ULL << (bits - 1))) - 1);
    Val notLow = L.B.cmp(CmpPred::Ge, v, Val::i64(lo), "parse.lo");
    Val notHigh = L.B.cmp(CmpPred::Le, v, Val::i64(hi), "parse.hi");
    Val inRange = L.B.select(notLow, notHigh, Val::boolean(false), "parse.in");
    valid = L.B.select(valid, inRange, Val::boolean(false), "parse.valid");
    v = L.canonicalize(v, L.ASTCtx.getBuiltinType(target));
  }
  Val some = L.B.some(v, "some");
  Val none = L.B.none(some.Ty, "none");
  return L.B.select(valid, some, none, "parsed.opt");
}

bool ModuleLowering::isStrConversion(const std::string &callee) {
  return callee.rfind(std::string(kString) + "<", 0) == 0;
}

ast::MethodCallExpr *ModuleLowering::showCallOf(ast::CallExpr *ce) {
  if (!isStrConversion(ce->getCalleeName()) ||
      FunctionTable.count(ce->getCalleeName()) || ce->getNumArguments() != 1)
    return nullptr;
  auto [it, added] = ShowCalls.try_emplace(ce, nullptr);
  if (added) {
    // A fresh +1 box, as exprAlreadyShared and exprProducesFreshBox take
    // the call (a reference result, a callee FunctionTable does not hold):
    // what a method's `Str` result is.  In FunctionTable, the call would
    // be a string temporary, which the box is not.
    it->second = ASTCtx.make<ast::MethodCallExpr>(
        ce->getLocation(), ce->getArguments()[0],
        ASTCtx.intern(kMethodToString), std::vector<ast::Expr *>{});
    it->second->setResolvedType(ASTCtx.getStrTy());
  }
  return it->second;
}

// Inline numeric conversions `Target<Source>(value)` (#64), the pairs of
// the families (docs/language/04-strings-and-conversions.md, "Between
// numbers").  This is the one place their semantics are defined; every
// backend inherits it, and nothing here is undefined behaviour in C or LLVM
// IR (--emit-c is strict C11):
//   T<T>         the value itself;
//   int<float>   & co. (a signed target) truncate toward zero; NaN, +-inf
//                and values outside the target panic
//                (Paykan_panic_float_to_int, _to_type) before the `ftoi`;
//   float<int>   & co. the nearest value, rounded once: `itof` / `utof`,
//                then the target's rounding; `f32<int>` and `f32<unsigned>`
//                round in the runtime (Paykan_int_to_f32, _uint_to_f32), as
//                the f64 an `itof` gives is already rounded beyond 2^53;
//   integer from an integer: the low bits of the value; from a `bool`, 1
//                or 0; from a `char`, its byte code 0..255 (`cast`
//                zero-extends), then the low bits (`i8`);
//   bool<int>    & co. `!= 0`;
//   char<int>    & co. the byte with that code; outside 0..255 panics
//                (Paykan_panic_int_to_char for `int`, Paykan_panic_to_char
//                naming the source for the rest), which a `u8` never is.
// Returns an invalid Val when @p node is not a numeric conversion.
Val ModuleLowering::ExprEmitter::emitConversion(ast::CallExpr *node) {
  using ast::BuiltinType;
  ast::Type *target = nullptr;
  ast::Type *source = nullptr;
  if (!L.conversionTypes(node->getCalleeName(), target, source) ||
      target == L.ASTCtx.getStrTy() || source == L.ASTCtx.getStrTy() ||
      node->getNumArguments() != 1)
    return Val(); // `Str<T>` is a runtime call; nothing converts a `Str`
  const BuiltinType::Kind tk = L.builtinKind(target);
  const BuiltinType::Kind sk = L.builtinKind(source);
  Val v = visit(node->getArguments()[0]);
  if (!v || tk == sk)
    return v; // a number, a bool and a char are already canonical
  auto panicUnless = [&](const Val &ok, const char *panicFn,
                         const std::vector<Val> &args) {
    Val bad = L.B.unary(Opcode::Not, ok, "conv.bad");
    pir::If *s = L.B.openIf(bad, false);
    L.B.enter(*s->Then);
    L.callRuntime(panicFn, args);
    L.B.emitUnreachable();
    L.B.leave();
  };
  if (BuiltinType::isIntegerKind(tk) && BuiltinType::isFloatKind(sk)) {
    // A signed target (Sema has no float source for an unsigned one).  The
    // doubles whose truncation fits it: [-2^63, 2^63) for `int` (no double
    // lies strictly between -2^63 - 1 and -2^63), (lo - 1, hi) for the
    // others; both compares are ordered, so NaN fails them.
    const double hi = std::ldexp(1.0, BuiltinType::integerBits(tk) - 1);
    Val lo = tk == BuiltinType::Int
                 ? L.B.cmp(CmpPred::Ge, v, Val::f64(-hi), "conv.lo")
                 : L.B.cmp(CmpPred::Gt, v, Val::f64(-hi - 1.0), "conv.lo");
    Val inRange =
        L.B.select(lo, L.B.cmp(CmpPred::Lt, v, Val::f64(hi), "conv.hi"),
                   Val::boolean(false), "conv.ok");
    if (tk == BuiltinType::Int) {
      panicUnless(inRange, kPaykanPanicFloatToInt, {v});
    } else {
      const std::string name = ast::typeName(target);
      panicUnless(inRange, kPaykanPanicFloatToType,
                  {v, L.internString(name),
                   Val::i64(static_cast<int64_t>(name.size()))});
    }
    return L.B.ftoi(v, "conv.int"); // in range: already canonical
  }
  if (BuiltinType::isFloatKind(tk) && BuiltinType::isIntegerKind(sk)) {
    if (tk == BuiltinType::F32 &&
        (sk == BuiltinType::Int || sk == BuiltinType::U64)) {
      // Rounded once, to the f32 nearest the integer (new-F3c-2).
      if (auto *c = std::get_if<int64_t>(&v.Op.V))
        return Val::f64(sk == BuiltinType::U64
                            ? static_cast<double>(
                                  static_cast<float>(static_cast<uint64_t>(*c)))
                            : static_cast<double>(static_cast<float>(*c)));
      return L.callRuntime(sk == BuiltinType::U64 ? kPaykanUintToF32
                                                  : kPaykanIntToF32,
                           {v}, "f32");
    }
    // Exact in an f64 for any narrower source, so one rounding to `f32`;
    // an `f16` overflows from 65520, far below 2^53.
    Val f;
    if (sk == BuiltinType::U64)
      f = L.B.utof(v, "conv.float");
    else
      f = L.promoteIntToFloat(v, L.ASTCtx.getFloatTy());
    return L.canonicalize(f, target);
  }
  if (BuiltinType::isIntegerKind(tk) && BuiltinType::isIntegerKind(sk))
    return L.canonicalize(v, target); // the low bits of the value
  if (BuiltinType::isFloatKind(tk) && BuiltinType::isFloatKind(sk))
    return L.canonicalize(v, target); // rounded to the target's precision
  if (BuiltinType::isIntegerKind(tk) && sk == BuiltinType::Bool)
    return L.coerceBoolToI64(v, Type::I64); // 1 and 0 fit every width
  if (BuiltinType::isIntegerKind(tk) && sk == BuiltinType::Char)
    return L.canonicalize(L.B.cast(v, Type::I64, "conv.code"), target);
  if (tk == BuiltinType::Bool && BuiltinType::isIntegerKind(sk))
    return L.B.cmp(CmpPred::Ne, v, Val::i64(0), "conv.bool");
  if (tk != BuiltinType::Char || !BuiltinType::isIntegerKind(sk))
    return Val();
  // char<S>: range-check (on the canonical slot, where an `unsigned` of 2^63
  // or more is negative), then map the code 128..255 to the signed byte
  // with the same bits before the truncating cast, so C's `(int8_t)`
  // conversion only ever sees an in-range value.  A `u8` is in range.
  if (sk != BuiltinType::U8) {
    Val lo = L.B.cmp(CmpPred::Ge, v, Val::i64(0), "conv.lo");
    Val hi = L.B.cmp(CmpPred::Le, v, Val::i64(255), "conv.hi");
    Val ok = L.B.select(lo, hi, Val::boolean(false), "conv.ok");
    if (sk == BuiltinType::Int) {
      panicUnless(ok, kPaykanPanicIntToChar, {v});
    } else {
      const std::string name = ast::typeName(source);
      panicUnless(ok, kPaykanPanicToChar,
                  {v, Val::i64(BuiltinType::isUnsignedKind(sk) ? 1 : 0),
                   L.internString(name),
                   Val::i64(static_cast<int64_t>(name.size()))});
    }
  }
  Val high = L.B.cmp(CmpPred::Gt, v, Val::i64(127), "conv.high");
  Val wrapped = L.B.binary(Opcode::Sub, v, Val::i64(256), "conv.wrap");
  Val byte = L.B.select(high, wrapped, v, "conv.byte");
  return L.B.cast(byte, Type::Char, "conv.char");
}

Val ModuleLowering::ExprEmitter::visitCallExpr(ast::CallExpr *node) {
  // Wanted as an address only by this call, not by the arguments' calls.
  const bool wantAddress = std::exchange(WantAddress, false);
  // `E::B(x, y)`: a tagged union's variant, held in memory (LoweringEnum.cpp).
  if (node->getVariantEnum())
    return L.emitMemAddress(node);
  // A call through a function value held by a variable.
  if (node->calleeIsValue()) {
    auto *id = L.ASTCtx.make<ast::Identifier>(
        node->getLocation(), L.ASTCtx.intern(node->getCalleeName()));
    id->setResolvedType(L.CurrentScope->lookupASTType(node->getCalleeName()));
    return L.emitFunctionValueCall(id, node->getArguments(),
                                   node->getResolvedType());
  }
  if (ast::MethodCallExpr *show = L.showCallOf(node))
    return visitMethodCallExpr(show);
  if (Val conv = emitConversion(node))
    return conv;

  if (L.IdentityCtors.count(node->getCalleeName()) &&
      node->getNumArguments() == 1)
    return emitIdentityCtor(node);

  if (L.FunctionTable.count(node->getCalleeName()))
    return emitBuiltinCall(node);

  // __super__(args): call the base class __init__ with self.
  if (node->getCalleeName() == kMethodSuper) {
    ast::ClassType *cls = L.CurrentMethodClassType;
    ast::ClassType *superClass = cls ? cls->getSuperClass() : nullptr;
    if (!superClass) {
      L.reportInternalError("'__super__' outside a subclass method");
      return Val();
    }
    pir::Function *superFn = L.lookupOwnMethodFunction(superClass, kMethodInit);
    if (!superFn) {
      if (!superClass->findMethod(kMethodInit))
        return Val();
      L.reportInternalError("no function for '" + superClass->getName() + "." +
                            kMethodInit + "' called by '" + cls->getName() +
                            "." + kMethodSuper + "'");
      return Val();
    }
    Val selfVal = L.B.load(L.CurrentScope->lookup(kSelf), kSelf);
    std::vector<Val> initArgs = {selfVal};
    std::vector<Val> keep;
    size_t fixedMark = L.MemTemps.size();
    for (size_t i = 0; i < node->getNumArguments(); ++i) {
      auto *argExpr = node->getArguments()[i];
      if (L.isSliceArg(argExpr)) {
        if (!L.emitSliceArg(argExpr, initArgs))
          return Val();
        continue;
      }
      Type paramTy = initArgs.size() < superFn->Sig.Params.size()
                         ? superFn->Sig.Params[initArgs.size()]
                         : Type::Void;
      if (ast::isMemType(argExpr->getResolvedType())) {
        Val p = L.emitMemAddress(argExpr); // passed by address
        if (!p)
          return Val();
        initArgs.push_back(p);
        continue;
      }
      if (paramTy == Type::Ptr) { // `inout`: the argument's address
        Val p = L.emitInoutArg(argExpr, keep);
        if (!p)
          return Val();
        initArgs.push_back(p);
        continue;
      }
      // The base __init__ consumes ref-typed parameters: pass a +1 box.
      if (paramTy == Type::Box) {
        Val v = L.emitAsShared(argExpr, hasOwnershipBit(superFn->Sig,
                                                        pir::lends(*superFn),
                                                        initArgs.size()));
        if (!v)
          return Val();
        initArgs.push_back(v);
        continue;
      }
      Val v = visit(argExpr);
      if (!v)
        return Val();
      initArgs.push_back(L.coerceTo(v, paramTy));
    }
    L.B.call(superFn->Name, superFn->Sig, initArgs);
    L.releaseAfterCall(keep);
    L.releaseMemTemps(fixedMark);
    return Val();
  }

  // User-defined function or class constructor: declared up front, or
  // imported.  A miss is a compiler bug.
  pir::Function *callee = L.lookupFunction(node->getCalleeName());
  if (!callee) {
    L.reportInternalError("call to undeclared function '" +
                          node->getCalleeName() + "'");
    return Val();
  }

  std::vector<Val> args;
  std::vector<Val> keep;
  // A native function of this module: C borrows what the caller holds.
  if (const ast::FuncDecl *native = L.localNative(node->getCalleeName())) {
    size_t mark = L.MemTemps.size();
    for (size_t i = 0; i < node->getNumArguments(); ++i) {
      ast::Expr *arg = node->getArguments()[i];
      Type paramTy = callee->Sig.Params[i];
      Val v;
      if (ast::isMemType(arg->getResolvedType()))
        v = L.emitMemAddress(arg); // a value held in memory: its address
      else if (paramTy == Type::Ptr)
        v = L.emitInoutArg(arg, keep);
      else if (paramTy == Type::Box)
        v = L.emitBorrowedBox(arg, keep);
      else if (Val e = visit(arg))
        v = L.coerceTo(e, paramTy);
      if (!v)
        return Val();
      args.push_back(v);
    }
    Val memResult;
    if (ast::isMemType(node->getResolvedType()))
      memResult = L.emitMemResultSlot(node->getResolvedType());
    Val result = L.emitNativeCall(*native, args, memResult);
    // A runtime function that never returns (`_panic`'s): nothing after
    // its call runs, so nothing its arguments hold is released after it.
    if (isNoReturnRuntimeFunction(native->getNativeSymbol())) {
      L.MemTemps.resize(mark);
      return result;
    }
    L.releaseAfterCall(keep);
    L.releaseMemTemps(mark);
    return result;
  }
  size_t fixedMark = L.MemTemps.size();
  InoutPlaces places;
  for (auto *arg : node->getArguments()) {
    if (L.isSliceArg(arg)) {
      if (!L.emitSliceArg(arg, args))
        return Val();
      continue;
    }
    Type paramTy = args.size() < callee->Sig.Params.size()
                       ? callee->Sig.Params[args.size()]
                       : Type::Void;
    if (ast::isMemType(arg->getResolvedType())) {
      Val p = L.emitMemAddress(arg); // passed by address
      if (!p)
        return Val();
      if (arg->isChangedByCall() && node->isInoutResult())
        places.emplace_back(p, arg->getResolvedType());
      args.push_back(p);
      continue;
    }
    if (paramTy == Type::Ptr) { // `inout`: the argument's address
      Val p = L.emitInoutArg(arg, keep);
      if (!p)
        return Val();
      if (node->isInoutResult())
        places.emplace_back(p, arg->getResolvedType());
      args.push_back(p);
      continue;
    }
    if (paramTy == Type::Box) {
      // Callee-consumes ABI: a +1 box for every expression form.
      Val v = L.emitAsShared(
          arg, hasOwnershipBit(callee->Sig, pir::lends(*callee), args.size()));
      if (!v)
        return Val();
      args.push_back(v);
      continue;
    }
    Val v = visit(arg);
    if (!v)
      return Val();
    args.push_back(L.coerceTo(v, paramTy));
  }
  // A result held in memory: written into a temporary, whose address is the
  // call's value (an `inout` one is the address of its storage instead).
  if (ast::isMemType(node->getResolvedType()) && callee->Sig.Ret != Type::Ptr) {
    Val slot = L.emitMemResultSlot(node->getResolvedType());
    args.push_back(slot);
    L.B.call(callee->Name, callee->Sig, args);
    L.releaseAfterCall(keep);
    L.releaseMemTemps(fixedMark);
    return slot;
  }
  Val result = L.B.call(callee->Name, callee->Sig, args, "call");
  if (result && result.Ty == Type::Ptr) // an `inout` result
    return L.takeInoutResult(result, node, wantAddress, keep, fixedMark,
                             places);
  L.releaseAfterCall(keep);
  L.releaseMemTemps(fixedMark);
  return result;
}

Val ModuleLowering::ExprEmitter::visitMethodCallExpr(
    ast::MethodCallExpr *node) {
  const bool wantAddress = std::exchange(WantAddress, false);
  ast::Type *recvResolved = node->getReceiver()->getResolvedType();
  // Each kind of call in a function of its own, so that the frame a call
  // in a call's arguments recurses through holds only its kind's locals.
  // `obj.cb(args)` with `cb` a field of function type.
  if (node->isFieldCall())
    return emitFieldCall(node);
  // A fixed-size array's `len()`: its length.  An element receiver is still
  // evaluated, for its index and its bounds check.
  if (ast::dyn_cast<ast::SliceType>(recvResolved))
    return L.emitSliceLength(node->getReceiver());
  if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(recvResolved)) {
    size_t mark = L.MemTemps.size();
    if (!ast::isa<ast::Identifier>(node->getReceiver()) &&
        !L.emitMemAddress(node->getReceiver()))
      return Val();
    L.releaseMemTemps(mark);
    return Val::i64(ft->getLength());
  }
  // `x.hash()` on an int, bool, char or Str (the builtin `Hashable`).
  if (recvResolved && node->getMethodName() == kMethodHash &&
      (ast::isa<ast::BuiltinType>(recvResolved) ||
       recvResolved == L.ASTCtx.getStrTy()))
    return emitHashCall(node);
  // `s.toInt()` & co. on a Str: its parses.
  if (ast::BuiltinType::Kind target = ast::BuiltinType::Void;
      recvResolved == L.ASTCtx.getStrTy() &&
      ast::strParseKind(node->getMethodName(), target))
    return emitStrParseCall(node, target);
  // `u.toInt()` on an `unsigned`: its value as an `int?`.
  if (L.builtinKind(recvResolved) == ast::BuiltinType::U64 &&
      node->getMethodName() == kMethodToInt)
    return emitUnsignedToInt(node);
  // A struct's method, called directly on the receiver's storage
  // (LoweringStruct.cpp); its `toString()` is the builtin one, below.
  if (auto *st = L.getExprStructType(node->getReceiver());
      st && node->getMethodName() != kMethodToString)
    return emitStructMethodCall(node, st, wantAddress);
  // `o.toString()` on an optional (the builtin `Show`).
  if (ast::dyn_cast<ast::OptionalType>(recvResolved) &&
      node->getMethodName() == kMethodToString)
    return emitOptionalToStringCall(node);
  // `t.toString()` on a tuple, a struct or an enum, `n.toString()` on an
  // int, float, bool or char (the builtin `Show`).
  if (ast::dyn_cast<ast::TupleType>(recvResolved) ||
      ast::dyn_cast<ast::EnumType>(recvResolved) ||
      ast::dyn_cast<ast::BuiltinType>(recvResolved))
    return emitShowCall(node);
  return emitClassMethodCall(node, wantAddress);
}

// `obj.cb(args)` with `cb` a field of function type: the field's closure,
// borrowed for the call (the receiver, kept for the call, holds it)
// (visitMethodCallExpr).
Val ModuleLowering::ExprEmitter::emitFieldCall(ast::MethodCallExpr *node) {
  ast::Type *recvResolved = node->getReceiver()->getResolvedType();
  ast::Type *fieldTy = nullptr;
  if (auto *ct = ast::dyn_cast<ast::ClassType>(recvResolved))
    fieldTy = ct->findField(node->getMethodName());
  else if (auto *st = ast::dyn_cast<ast::StructType>(recvResolved))
    fieldTy = st->findField(node->getMethodName());
  auto *ma = L.ASTCtx.make<ast::MemberAccessExpr>(
      node->getLocation(), node->getReceiver(),
      L.ASTCtx.intern(node->getMethodName()));
  ma->setResolvedType(fieldTy);
  return L.emitFunctionValueCall(ma, node->getArguments(),
                                 node->getResolvedType());
}

// `x.hash()` (the builtin `Hashable`) (visitMethodCallExpr).
Val ModuleLowering::ExprEmitter::emitHashCall(ast::MethodCallExpr *node) {
  // `s.hash()` on a Str: the runtime's hash of its bytes.  Str is final, so
  // the call is direct; a fresh temporary receiver is released after it.
  if (node->getReceiver()->getResolvedType() == L.ASTCtx.getStrTy()) {
    Receiver receiver = L.emitReceiver(node->getReceiver());
    if (!receiver)
      return Val();
    const pir::Function &rt = L.declareRuntime(kPaykanStringHash);
    Val h = L.B.call(rt.Name, rt.Sig, {receiver.Raw}, "hash");
    L.releaseIfOwned(receiver.Owned);
    return h;
  }
  // `n.hash()` on an int, bool or char (the builtin `Hashable`): the
  // runtime's hash of its value, the char's code or the bool's 0 or 1.
  Val v = visit(node->getReceiver());
  if (!v)
    return Val();
  if (v.Ty != Type::I64)
    v = L.B.cast(v, Type::I64, "hash.ext");
  const pir::Function &rt = L.declareRuntime(kPaykanIntHash);
  return L.B.call(rt.Name, rt.Sig, {v}, "hash");
}

// `s.toInt()` & co. (visitMethodCallExpr): the parse of the receiver into
// @p target (emitParseOf).  Str is final, so the call is direct; a fresh
// temporary receiver is released after it.
Val ModuleLowering::ExprEmitter::emitStrParseCall(
    ast::MethodCallExpr *node, ast::BuiltinType::Kind target) {
  Receiver str = L.emitReceiver(node->getReceiver(), "parse.str");
  if (!str)
    return Val();
  return emitParseOf(str.Raw, str.Owned, target);
}

// `u.toInt()` on an `unsigned` (visitMethodCallExpr): the value as the
// present `int?`, or None from 2^63 on, which the slot's bits hold as a
// negative int64.
Val ModuleLowering::ExprEmitter::emitUnsignedToInt(ast::MethodCallExpr *node) {
  Val v = visit(node->getReceiver());
  if (!v)
    return Val();
  Val fits = L.B.cmp(CmpPred::Ge, v, Val::i64(0), "toint.fits");
  Val some = L.B.some(v, "some");
  Val none = L.B.none(some.Ty, "none");
  return L.B.select(fits, some, none, "toint.opt");
}

// `o.toString()` on an optional (the builtin `Show`): "None", or the value's
// text, as a fresh box like any method's `Str` result (visitMethodCallExpr).
Val ModuleLowering::ExprEmitter::emitOptionalToStringCall(
    ast::MethodCallExpr *node) {
  ast::Type *recvResolved = node->getReceiver()->getResolvedType();
  if (ast::isMemOptional(recvResolved)) {
    // A tuple's, a struct's or a tagged union's, in place: its presence
    // flag, then its layout's text.
    size_t mark = L.MemTemps.size();
    Val addr = L.emitMemAddress(node->getReceiver());
    if (!addr)
      return Val();
    ast::Type *value = ast::stripOptional(recvResolved);
    pir::LocalId text = L.B.addLocal("opt.str", Type::Box);
    pir::If *s = L.B.openIf(L.emitMemOptionalIsNone(addr, recvResolved),
                            /*withElse=*/true);
    L.B.enter(*s->Then);
    L.B.store(text,
              L.emitSharedNew(L.callRuntime(kPaykanStringNew,
                                            {L.internString(kNoneText),
                                             Val::i64(static_cast<int64_t>(
                                                 std::strlen(kNoneText)))},
                                            "opt.str"),
                              "str.shared"));
    L.B.leave();
    L.B.enter(*s->Else);
    Val payload =
        L.B.ptrAdd(addr, Val::i64(L.memOptionalPayloadOffset(recvResolved)),
                   "opt.value.addr");
    Val raw;
    if (auto *et = ast::dyn_cast<ast::EnumType>(value))
      raw = L.emitEnumToStringAt(payload, et);
    else
      raw = L.callRuntime(
          ast::isa<ast::StructType>(value) ? kPaykanStructToString
                                           : kPaykanTupleToString,
          {payload, L.emitTupleLayout(ast::cast<ast::TupleType>(value))},
          "tup.str");
    L.B.store(text, L.emitSharedNew(raw, "str.shared"));
    L.B.leave();
    Val str = L.B.load(text, "opt.str");
    L.releaseMemTemps(mark);
    return str;
  }
  if (ast::isValueOptional(recvResolved)) {
    Val v = visit(node->getReceiver());
    if (!v)
      return Val();
    return L.emitSharedNew(L.emitOptionalToString(v, recvResolved),
                           "str.shared");
  }
  Receiver receiver = L.emitReceiver(node->getReceiver());
  if (!receiver)
    return Val();
  const pir::Function &toString = L.declareRuntime(kPaykanObjectToString);
  Val str =
      L.B.vcall(L.emitNoneObject(receiver.Raw), "", 1, toString.Sig, {}, "str");
  L.releaseIfOwned(receiver.Owned);
  return str;
}

// `t.toString()` on a tuple, a struct or an enum, `n.toString()` on an int,
// float, bool or char (the builtin `Show`): its text, as a fresh box like any
// method's `Str` result (visitMethodCallExpr).
Val ModuleLowering::ExprEmitter::emitShowCall(ast::MethodCallExpr *node) {
  ast::Type *recvResolved = node->getReceiver()->getResolvedType();
  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(recvResolved)) {
    Val v = visit(node->getReceiver());
    if (!v)
      return Val();
    const pir::Function &rt =
        L.declareRuntime(L.toStringRuntime(bt->getTypeKind()));
    Val str = L.B.call(rt.Name, rt.Sig,
                       {L.coerceBoolToI64(v, rt.Sig.Params[0])}, "str");
    return L.emitSharedNew(str, "str.shared");
  }
  Val str = ast::isa<ast::EnumType>(recvResolved)
                ? L.emitEnumToString(node->getReceiver())
                : L.emitTupleToString(node->getReceiver());
  if (!str)
    return Val();
  return L.emitSharedNew(str, "str.shared");
}

// A method of a class, `Str`'s and `Obj`'s included: its slot, or an `impl`
// method directly (visitMethodCallExpr).
Val ModuleLowering::ExprEmitter::emitClassMethodCall(ast::MethodCallExpr *node,
                                                     bool wantAddress) {
  // A fresh temporary receiver is released after the call (the method
  // borrows it).
  Receiver receiver = L.emitReceiver(node->getReceiver());
  if (!receiver)
    return Val();
  Val recv = receiver.Raw;
  const ExprValue &recvOwned = receiver.Owned;

  // Resolve the receiver's AST type (identifiers, self, self.field, or the
  // expression's own resolved type).
  auto resolveReceiverASTType = [&]() -> ast::Type * {
    auto *recvExpr = node->getReceiver();
    if (auto *id = ast::dyn_cast<ast::Identifier>(recvExpr)) {
      if (L.CurrentScope)
        if (auto *t = L.CurrentScope->lookupASTType(id->getName()))
          return t;
      if (id->getName() == kSelf && L.CurrentMethodClassType)
        return L.CurrentMethodClassType;
    }
    if (auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(recvExpr)) {
      if (auto *rid = ast::dyn_cast<ast::Identifier>(ma->getReceiver())) {
        ast::Type *ownerTy = L.CurrentScope
                                 ? L.CurrentScope->lookupASTType(rid->getName())
                                 : nullptr;
        if (!ownerTy && rid->getName() == kSelf && L.CurrentMethodClassType)
          ownerTy = L.CurrentMethodClassType;
        if (auto *ownerCt = ast::dyn_cast<ast::ClassType>(ownerTy))
          for (auto &[fn, ft] : ownerCt->getFields())
            if (fn == ma->getFieldName())
              return ft;
      }
    }
    if (auto *resolved = recvExpr->getResolvedType())
      return L.canonicalizeDeclType(resolved);
    return nullptr;
  };

  ast::Type *recvASTTy = resolveReceiverASTType();
  const std::string &mname = node->getMethodName();

  ast::ClassType *ct = ast::dyn_cast<ast::ClassType>(recvASTTy);
  if (!ct)
    ct = L.getExprClassType(node->getReceiver());
  if (!ct) {
    for (auto *candidate : {L.ASTCtx.getStrTy(), L.ASTCtx.getObjTy()}) {
      if (candidate && candidate->findMethod(node->getMethodName())) {
        ct = candidate;
        break;
      }
    }
  }

  ast::MethodDecl *method =
      ct ? ct->findMethod(node->getMethodName()) : nullptr;
  // An `impl` method has no vtable slot: it is called directly.
  pir::Function *implFn =
      method && method->isImpl()
          ? L.lookupOwnMethodFunction(ct->implOwner(mname), mname)
          : nullptr;
  int vtableIdx = ct ? ct->getVTableIndex(node->getMethodName()) : -1;
  assert((implFn || vtableIdx >= 0) && "Sema should have verified the method");
  pir::Signature sig = implFn   ? implFn->Sig
                       : method ? L.slotSignature(ct, method)
                                : pir::Signature{{Type::Obj}, Type::Box};

  bool isUserDefinedMethod = ct && !ct->isBuiltin();
  std::vector<Val> args;
  std::vector<ExprValue> ownedArgs;
  std::vector<Val> keep;
  InoutPlaces places;
  size_t fixedMark = L.MemTemps.size();
  if (!emitMethodArgs(node, method, sig, isUserDefinedMethod, args, ownedArgs,
                      keep, places))
    return Val();

  // A fixed-size array result is written into a temporary, whose address is
  // the call's value.
  Val fixedSlot;
  if (ast::isMemType(node->getResolvedType()) && sig.Ret != Type::Ptr) {
    fixedSlot = L.emitMemResultSlot(node->getResolvedType());
    args.push_back(fixedSlot);
  }

  // Virtual dispatch through the receiver's vtable.  User classes are named
  // (the backend knows their vtable layout); runtime classes use the
  // explicit-signature form.
  // A class of this module is named (the backend knows its vtable); a
  // runtime class or a class imported from another module (whose extern item
  // carries only the layout) uses the explicit-signature form.
  Val result;
  if (implFn) {
    args.insert(args.begin(), recv);
    result = L.B.call(implFn->Name, sig, args, "mcall");
  } else {
    std::string className;
    if (isUserDefinedMethod && !L.getOrCreateClass(ct)->IsExtern)
      className = ct->getName();
    else if (isUserDefinedMethod &&
             !L.checkImportedSlot(ct, static_cast<uint32_t>(vtableIdx), sig))
      return Val();
    result = L.B.vcall(recv, className, static_cast<uint32_t>(vtableIdx), sig,
                       args, "mcall");
  }
  for (const auto &ev : ownedArgs)
    L.releaseIfOwned(ev);
  if (result && result.Ty == Type::Ptr) // an `inout` result
    return L.takeInoutResult(result, node, wantAddress, keep, fixedMark, places,
                             recvOwned);
  L.releaseAfterCall(keep);
  L.releaseMemTemps(fixedMark);
  L.releaseIfOwned(recvOwned);
  if (fixedSlot)
    return fixedSlot;
  // An i64 slot result standing for a Paykan bool (`equals`) comes back as
  // the bool the expression has.
  if (result && result.Ty == Type::I64 && method && method->getReturnType() &&
      L.toPIRType(L.canonicalizeDeclType(method->getReturnType())) ==
          Type::Bool)
    result = L.B.cmp(CmpPred::Ne, result, Val::i64(0), "eq");
  return result;
}

bool ModuleLowering::ExprEmitter::emitMethodArgs(
    ast::MethodCallExpr *node, const ast::MethodDecl *method,
    const pir::Signature &sig, bool userDefined, std::vector<Val> &args,
    std::vector<ExprValue> &ownedArgs, std::vector<Val> &keep,
    InoutPlaces &places) {
  for (size_t i = 0; i < node->getNumArguments(); ++i) {
    auto *argExpr = node->getArguments()[i];
    // The PIR parameter this argument goes to: after the receiver, and two
    // for each slice.
    const size_t at = args.size() + 1;
    if (L.isSliceArg(argExpr)) {
      if (!L.emitSliceArg(argExpr, args))
        return false;
      continue;
    }
    if (ast::isMemType(argExpr->getResolvedType())) {
      Val p = L.emitMemAddress(argExpr); // passed by address
      if (!p)
        return false;
      if (argExpr->isChangedByCall() && node->isInoutResult())
        places.emplace_back(p, argExpr->getResolvedType());
      args.push_back(p);
      continue;
    }
    if (at < sig.Params.size() && sig.Params[at] == Type::Ptr) {
      // `inout`: the argument's address
      Val p = L.emitInoutArg(argExpr, keep);
      if (!p)
        return false;
      if (node->isInoutResult())
        places.emplace_back(p, argExpr->getResolvedType());
      args.push_back(p);
      continue;
    }
    ast::Type *paramASTTy = (method && i < method->getParamTypes().size())
                                ? method->getParamTypes()[i]
                                : nullptr;
    // Ref-typed arguments to user methods and to the virtual `equals` use
    // the callee-consumes ABI (+1 box); builtin methods borrow.
    bool isClassParam = ast::isRefType(paramASTTy) &&
                        (userDefined || node->getMethodName() == kMethodEquals);
    if (isClassParam) {
      // A method, a vtable slot or a struct's method: it lends.
      Val v = L.emitAsShared(argExpr,
                             hasOwnershipBit(sig, pir::lendsType(sig), at));
      if (!v)
        return false;
      args.push_back(v);
      continue;
    }
    Val v = visit(argExpr);
    if (!v)
      return false;
    if (ExprValue ev = L.classifyExpr(argExpr, v); ev.isOwned())
      ownedArgs.push_back(ev);
    Type paramTy = at < sig.Params.size() ? sig.Params[at] : Type::Void;
    // A builtin method borrowing a ref argument (File.write(s)) takes the
    // raw object.
    if (paramTy == Type::Obj && v.Ty == Type::Box)
      v = L.emitSharedGet(v, "unboxed");
    args.push_back(L.coerceTo(v, paramTy));
  }
  return true;
}

// -- Subscript assignment

Val ModuleLowering::visitSubscriptAssignStmt(ast::SubscriptAssignStmt *node) {
  // An element of a fixed-size array or a slice, written in place.
  ast::Type *arrTy = node->getArray()->getResolvedType();
  auto *ft = ast::dyn_cast<ast::FixedArrayType>(arrTy);
  auto *st = ast::dyn_cast<ast::SliceType>(arrTy);
  size_t mark = MemTemps.size();
  Val addr = emitFixedElementAddress(node->getArray(), node->getIndex());
  if (!addr)
    return Val();
  ast::Type *elemTy = ft ? ft->getElementType() : st->getElementType();
  if (ast::isMemType(elemTy)) {
    emitMemAssign(addr, node->getValue());
  } else if (ast::isRefType(elemTy)) {
    // The new reference is taken before the old one goes (`xs[0] =
    // xs[0];`).
    if (Val box = emitAsShared(node->getValue())) {
      Val old = B.ptrLoad(addr, Type::Box, "old");
      B.ptrStore(addr, box);
      emitRelease(old);
    }
  } else if (Val v = emitExpr(node->getValue())) {
    B.ptrStore(addr, coerceTo(v, toPIRType(elemTy)));
  }
  releaseMemTemps(mark);
  return Val();
}

// -- Tuples

// A literal's value is the address of its temporary (emitMemValue).
Val ModuleLowering::ExprEmitter::visitTupleLiteralExpr(
    ast::TupleLiteralExpr *node) {
  return L.emitMemAddress(node);
}

Val ModuleLowering::ExprEmitter::visitTupleIndexExpr(
    ast::TupleIndexExpr *node) {
  // An element held in memory is its address; any other is read in place.
  if (ast::isMemType(node->getResolvedType()))
    return L.emitMemAddress(node);
  size_t mark = L.MemTemps.size();
  Val addr = L.emitTupleElementAddress(node->getTuple(), node->getIndex());
  if (!addr)
    return addr;
  // A temporary's element, or a held one, is a +1 box
  // (exprProducesFreshBox).
  return L.ownElementRead(
      node,
      L.emitElementLoad(addr, node->getResolvedType(), L.isStableRead(node)),
      mark);
}

Val ModuleLowering::visitDestructureStmt(ast::DestructureStmt *node) {
  auto *tt = ast::dyn_cast<ast::TupleType>(node->getValue()->getResolvedType());
  assert(tt && tt->getArity() == node->getNumTargets() &&
         "Sema must resolve the destructured value to a matching TupleType");

  // The tuple's bytes, kept until every element is copied out (`a, b = (b,
  // a);` reads both before either is replaced).
  size_t mark = MemTemps.size();
  Val base = emitMemAddress(node->getValue());
  if (!base)
    return Val();

  for (size_t i = 0; i < node->getNumTargets(); ++i) {
    const auto &target = node->getTargets()[i];
    if (target.isSkip())
      continue;
    ast::Type *elemTy = tt->getElementType(i);
    const std::string &name = target.getName();
    int64_t off = tupleElementOffset(tt, i);
    Val at = off == 0 ? base : B.ptrAdd(base, Val::i64(off), "elem.addr");
    ast::Type *bindTy =
        target.DeclType ? canonicalizeDeclType(target.DeclType) : elemTy;
    Scope *owner = target.DeclType ? nullptr : CurrentScope->findOwner(name);

    // An element held in memory: a new variable's copy, or the variable's
    // bytes replaced.
    if (ast::isMemType(elemTy)) {
      const int64_t size = memLayout(elemTy).Size;
      if (!owner) {
        MemLayout l = memLayout(bindTy);
        pir::LocalId local = B.addMemLocal(name, l.Size, l.Align);
        Val dst = B.localAddr(local, name + ".addr");
        B.memCopy(dst, at, Val::i64(size));
        emitMemRetain(dst, elemTy);
        CurrentScope->declare(name, local, bindTy);
        continue;
      }
      pir::LocalId local = owner->lookup(name);
      Val dst = CurrentScope->inoutType(name) != Type::Void
                    ? B.load(local, name + ".addr")
                    : B.localAddr(local, name + ".addr");
      emitMemRetain(at, elemTy);
      emitMemRelease(dst, elemTy);
      B.memCopy(dst, at, Val::i64(size));
      continue;
    }

    Val v = emitElementLoad(at, elemTy);
    if (ast::isRefType(elemTy)) // the binding becomes an owner of the element
      emitRetain(v, /*lend=*/false, elemTy);
    if (!owner) {
      Type pirTy = toPIRType(bindTy);
      if (pirTy == Type::Void)
        pirTy = v.Ty;
      v = coerceTo(v, pirTy);
      pir::LocalId local = B.addLocal(name, pirTy);
      B.store(local, v);
      CurrentScope->declare(name, local, bindTy);
      continue;
    }

    // Re-assignment of an existing variable.
    pir::LocalId local = owner->lookup(name);
    ast::Type *varTy = CurrentScope->lookupASTType(name);
    if (varTy && ast::isRefType(varTy)) {
      if (CurrentScope->isWeak(name)) {
        storeWeakVar(name, v);
        continue;
      }
      if (!holdsBox(name)) {
        reportInternalError("destructuring into unowned variable '" + name +
                            "'");
        continue;
      }
      storeVarBox(name, v);
      continue;
    }
    if (!storeInout(name, local, v))
      B.store(local, coerceTo(v, B.localType(local)));
  }

  // Every element has been copied out (references retained): a temporary
  // tuple can die now.
  releaseMemTemps(mark);
  return Val();
}

} // namespace paykan::lowering
