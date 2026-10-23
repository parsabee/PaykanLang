// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// AST -> PIR lowering: expressions, arrays, tuples, subscripts.

#include "LoweringInternal.h"
#include "Names.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <utility>

namespace paykan::lowering {

using namespace names;
using pir::CmpPred;
using pir::Opcode;
using pir::Type;

// -- Dispatch

Val ModuleLowering::ExprEmitter::visit(ast::Expr *node) {
  Val v = ast::ExprVisitor<ExprEmitter, Val>::visit(node);
  if (!v || !isOptionalWrap(node))
    return v;
  return L.emitSome(v, node->getCoercedType());
}

// -- Literals

Val ModuleLowering::ExprEmitter::visitIntegerLiteral(
    ast::IntegerLiteral *node) {
  // `1` flowing into an `f32` or `f16` slot is that float (Sema typed it).
  ast::Type *ty = node->getResolvedType();
  if (ast::BuiltinType::isFloatKind(L.builtinKind(ty)))
    return L.canonicalize(Val::f64(static_cast<double>(node->getValue())), ty);
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
  // An `inout` parameter: read the caller's storage (a box: borrowed, and
  // unwrapped like an owned variable's).
  if (Type t = L.CurrentScope->inoutType(node->getName()); t != Type::Void) {
    Val v = L.B.ptrLoad(val, t, node->getName());
    return t == Type::Box ? L.emitSharedGet(v, node->getName() + ".obj") : v;
  }
  // Owned ref vars store a box — unwrap to the underlying object.
  auto *astTy = L.CurrentScope->lookupASTType(node->getName());
  if (ast::isRefType(astTy) && L.CurrentScope->isOwned(node->getName()))
    val = L.emitSharedGet(val, node->getName() + ".obj");
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
    Val elem = L.emitElementLoad(addr, node->getResolvedType());
    // A temporary's element outlives it as a +1 box (exprProducesFreshBox).
    if (L.MemTemps.size() > mark) {
      if (elem.Ty == Type::Box)
        L.emitRetain(elem);
      L.releaseMemTemps(mark);
    }
    return elem;
  }
  // A string's character.
  Val idx = visit(node->getIndex());
  if (!idx)
    return Val();
  idx = L.coerceBoolToI64(idx, Type::I64);
  Receiver recv = L.emitReceiver(node->getArray());
  if (!recv)
    return Val();
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
  case ast::UnaryOpcode::Count:
    break;
  }
  assert(false && "unknown UnaryOpcode");
  return Val();
}

Val ModuleLowering::ExprEmitter::emitShortCircuit(ast::BinaryExpr *node,
                                                  bool isAnd) {
  pir::LocalId tmp = L.B.addLocal(isAnd ? "and" : "or", Type::Bool);
  Val lhs = visit(node->getLHS());
  if (!lhs)
    return Val();
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

  switch (node->getOpcode()) {
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

// `int<Str>(s)`, `float<Str>(s)`, `bool<Str>(s)` and `i8<Str>(s)` & co.: the
// runtime parses the borrowed string into a local and says whether it is
// valid; the result is that value as the present `int?` & co., or None.  A
// sized integer is parsed as a 64-bit one and refused outside its range; a
// sized float is rounded.
Val ModuleLowering::ExprEmitter::emitParse(ast::CallExpr *node) {
  using ast::BuiltinType;
  const std::string &name = node->getCalleeName();
  ast::Type *target = nullptr;
  ast::Type *source = nullptr;
  if (!L.conversionTypes(name, target, source) ||
      source != L.ASTCtx.getStrTy() || node->getNumArguments() != 1)
    return Val();
  const BuiltinType::Kind kind = L.builtinKind(target);
  const char *parse = nullptr;
  Type payload = Type::I64;
  if (kind == BuiltinType::Bool) {
    parse = kPaykanStringParseBool;
  } else if (BuiltinType::isFloatKind(kind)) {
    parse = kPaykanStringParseFloat;
    payload = Type::F64;
  } else if (kind == BuiltinType::U64) {
    parse = kPaykanStringParseUint;
  } else if (BuiltinType::isIntegerKind(kind)) {
    parse = kPaykanStringParseInt;
  }
  if (!parse)
    return Val();
  ast::Expr *argExpr = node->getArguments()[0];
  Val str = visit(argExpr);
  if (!str)
    return Val();
  ExprValue ev = L.classifyExpr(argExpr, str);
  if (L.exprAlreadyShared(argExpr) && str.Ty == Type::Box)
    str = L.emitSharedGet(str, "parse.str");
  pir::LocalId out = L.B.addLocal("parsed", payload);
  Val ok = L.callRuntime(parse, {str, L.B.localAddr(out, "parsed.addr")},
                         "parse.ok");
  L.releaseIfOwned(ev);
  Val v = L.B.load(out, "parsed");
  if (kind == BuiltinType::Bool)
    v = L.B.cmp(CmpPred::Ne, v, Val::i64(0), "parsed.bool");
  Val valid = L.B.cmp(CmpPred::Ne, ok, Val::i64(0), "parse.valid");
  const unsigned bits = BuiltinType::integerBits(kind);
  if (BuiltinType::isIntegerKind(kind) && bits < 64) {
    // Within the type's range, else None.
    const bool isUnsigned = BuiltinType::isUnsignedKind(kind);
    const int64_t lo =
        isUnsigned ? 0 : -static_cast<int64_t>(1ULL << (bits - 1));
    const int64_t hi = static_cast<int64_t>(
        (isUnsigned ? (1ULL << bits) : (1ULL << (bits - 1))) - 1);
    Val notLow = L.B.cmp(CmpPred::Ge, v, Val::i64(lo), "parse.lo");
    Val notHigh = L.B.cmp(CmpPred::Le, v, Val::i64(hi), "parse.hi");
    Val inRange = L.B.select(notLow, notHigh, Val::boolean(false), "parse.in");
    valid = L.B.select(valid, inRange, Val::boolean(false), "parse.valid");
  }
  v = L.canonicalize(v, target);
  Val some = L.B.some(v, "some");
  Val none = L.B.none(some.Ty, "none");
  return L.B.select(valid, some, none, "parsed.opt");
}

bool ModuleLowering::isStrConversion(const std::string &callee) {
  return callee.rfind(std::string(kString) + "<", 0) == 0;
}

// Inline numeric conversions `Target<Source>(value)` (#64).  This is the one
// place their semantics are defined; every backend inherits it, and nothing
// here is undefined behaviour in C or LLVM IR (--emit-c is strict C11):
//   int<float>  truncates toward zero; NaN, +-inf and values outside int64
//               panic (Paykan_panic_float_to_int) before the `ftoi`;
//   float<int>  nearest double (`itof`);
//   int<bool>   1 / 0;          bool<int>  `!= 0`;
//   int<char>   the char's byte code 0..255 (`cast` zero-extends);
//   char<int>   the byte with that code; outside 0..255 panics
//               (Paykan_panic_int_to_char).
// Returns an invalid Val when @p node is not a numeric conversion.
Val ModuleLowering::ExprEmitter::emitConversion(ast::CallExpr *node) {
  using ast::BuiltinType;
  ast::Type *target = nullptr;
  ast::Type *source = nullptr;
  if (!L.conversionTypes(node->getCalleeName(), target, source) ||
      target == L.ASTCtx.getStrTy() || source == L.ASTCtx.getStrTy() ||
      node->getNumArguments() != 1)
    return Val(); // `Str<T>` is a runtime call, `T<Str>` emitParse
  const BuiltinType::Kind tk = L.builtinKind(target);
  const BuiltinType::Kind sk = L.builtinKind(source);
  Val v = visit(node->getArguments()[0]);
  if (!v)
    return Val();
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
    // The doubles whose truncation fits the target: [-2^63, 2^63) for
    // `int` (no double lies strictly between -2^63 - 1 and -2^63), (lo - 1,
    // hi) for the others; both compares are ordered, so NaN fails them.
    const unsigned bits = BuiltinType::integerBits(tk);
    const bool isUnsigned = BuiltinType::isUnsignedKind(tk);
    const double hi = std::ldexp(1.0, isUnsigned ? bits : bits - 1);
    Val lo = tk == BuiltinType::Int
                 ? L.B.cmp(CmpPred::Ge, v, Val::f64(-hi), "conv.lo")
                 : L.B.cmp(CmpPred::Gt, v,
                           Val::f64(isUnsigned ? -1.0 : -hi - 1.0), "conv.lo");
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
    if (tk == BuiltinType::U64)
      return L.B.ftou(v, "conv.uint");
    return L.B.ftoi(v, "conv.int"); // in range: already canonical
  }
  if (BuiltinType::isFloatKind(tk) && BuiltinType::isIntegerKind(sk)) {
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
  if (tk == BuiltinType::Int && sk == BuiltinType::Bool)
    return L.coerceBoolToI64(v, Type::I64);
  if (tk == BuiltinType::Bool && sk == BuiltinType::Int)
    return L.B.cmp(CmpPred::Ne, v, Val::i64(0), "conv.bool");
  if (tk == BuiltinType::Int && sk == BuiltinType::Char)
    return L.B.cast(v, Type::I64, "conv.code");
  if (tk != BuiltinType::Char || sk != BuiltinType::Int)
    return Val();
  // char<int>: range-check, then map the code 128..255 to the signed byte
  // with the same bits before the truncating cast, so C's `(int8_t)`
  // conversion only ever sees an in-range value.
  Val lo = L.B.cmp(CmpPred::Ge, v, Val::i64(0), "conv.lo");
  Val hi = L.B.cmp(CmpPred::Le, v, Val::i64(255), "conv.hi");
  panicUnless(L.B.select(lo, hi, Val::boolean(false), "conv.ok"),
              kPaykanPanicIntToChar, {v});
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
  if (Val conv = emitConversion(node))
    return conv;
  if (Val parsed = emitParse(node))
    return parsed;

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
        Val v = L.emitAsShared(argExpr);
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
    L.releaseAfterCall(keep);
    L.releaseMemTemps(mark);
    return result;
  }
  size_t fixedMark = L.MemTemps.size();
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
      args.push_back(p);
      continue;
    }
    if (paramTy == Type::Ptr) { // `inout`: the argument's address
      Val p = L.emitInoutArg(arg, keep);
      if (!p)
        return Val();
      args.push_back(p);
      continue;
    }
    if (paramTy == Type::Box) {
      // Callee-consumes ABI: a +1 box for every expression form.
      Val v = L.emitAsShared(arg);
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
  L.releaseAfterCall(keep);
  L.releaseMemTemps(fixedMark);
  if (result && result.Ty == Type::Ptr && !wantAddress) // an `inout` result
    return L.loadInoutResult(result, node->getResolvedType());
  return result;
}

Val ModuleLowering::ExprEmitter::visitMethodCallExpr(
    ast::MethodCallExpr *node) {
  const bool wantAddress = std::exchange(WantAddress, false);
  ast::Type *recvResolved = node->getReceiver()->getResolvedType();
  // `obj.cb(args)` with `cb` a field of function type: the field's closure,
  // borrowed for the call (the receiver, kept for the call, holds it).
  if (node->isFieldCall()) {
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
  // `n.hash()` on an int, bool or char (the builtin `Hashable`): the
  // runtime's hash of its value, the char's code or the bool's 0 or 1.
  if (recvResolved && ast::isa<ast::BuiltinType>(recvResolved) &&
      node->getMethodName() == kMethodHash) {
    Val v = visit(node->getReceiver());
    if (!v)
      return Val();
    if (v.Ty != Type::I64)
      v = L.B.cast(v, Type::I64, "hash.ext");
    const pir::Function &rt = L.declareRuntime(kPaykanIntHash);
    return L.B.call(rt.Name, rt.Sig, {v}, "hash");
  }
  // `s.hash()` on a Str: the runtime's hash of its bytes.  Str is final, so
  // the call is direct; a fresh temporary receiver is released after it.
  if (recvResolved == L.ASTCtx.getStrTy() &&
      node->getMethodName() == kMethodHash) {
    Receiver receiver = L.emitReceiver(node->getReceiver());
    if (!receiver)
      return Val();
    const pir::Function &rt = L.declareRuntime(kPaykanStringHash);
    Val h = L.B.call(rt.Name, rt.Sig, {receiver.Raw}, "hash");
    L.releaseIfOwned(receiver.Owned);
    return h;
  }
  // A struct's method, called directly on the receiver's storage
  // (LoweringStruct.cpp); its `toString()` is the builtin one, below.
  if (auto *st = L.getExprStructType(node->getReceiver());
      st && node->getMethodName() != kMethodToString)
    return emitStructMethodCall(node, st, wantAddress);
  // `o.toString()` on an optional (the builtin `Show`): "None", or the
  // value's text, as a fresh box like any method's `Str` result.
  if (ast::isa<ast::OptionalType>(recvResolved) &&
      node->getMethodName() == kMethodToString) {
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
    Val str = L.B.vcall(L.emitNoneObject(receiver.Raw), "", 1, toString.Sig, {},
                        "str");
    L.releaseIfOwned(receiver.Owned);
    return str;
  }
  // `t.toString()` on a tuple, a struct or an enum (the builtin `Show`): its
  // text, as a fresh box like any method's `Str` result.
  if (ast::isa<ast::TupleType>(recvResolved) ||
      ast::isa<ast::EnumType>(recvResolved)) {
    Val str = ast::isa<ast::EnumType>(recvResolved)
                  ? L.emitEnumToString(node->getReceiver())
                  : L.emitTupleToString(node->getReceiver());
    if (!str)
      return Val();
    return L.emitSharedNew(str, "str.shared");
  }
  // `n.toString()` on an int, float, bool or char (the builtin `Show`): its
  // `Str` conversion, as a fresh box like any method's `Str` result.
  if (auto *bt = recvResolved ? ast::dyn_cast<ast::BuiltinType>(recvResolved)
                              : nullptr) {
    Val v = visit(node->getReceiver());
    if (!v)
      return Val();
    const pir::Function &rt =
        L.declareRuntime(L.toStringRuntime(bt->getTypeKind()));
    Val str = L.B.call(rt.Name, rt.Sig,
                       {L.coerceBoolToI64(v, rt.Sig.Params[0])}, "str");
    return L.emitSharedNew(str, "str.shared");
  }

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
  size_t fixedMark = L.MemTemps.size();
  if (!emitMethodArgs(node, method, sig, isUserDefinedMethod, args, ownedArgs,
                      keep))
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
  L.releaseAfterCall(keep);
  L.releaseMemTemps(fixedMark);
  for (const auto &ev : ownedArgs)
    L.releaseIfOwned(ev);
  L.releaseIfOwned(recvOwned);
  if (fixedSlot)
    return fixedSlot;
  // An i64 slot result standing for a Paykan bool (`equals`) comes back as
  // the bool the expression has.
  if (result && result.Ty == Type::I64 && method && method->getReturnType() &&
      L.toPIRType(L.canonicalizeDeclType(method->getReturnType())) ==
          Type::Bool)
    result = L.B.cmp(CmpPred::Ne, result, Val::i64(0), "eq");
  if (result && result.Ty == Type::Ptr && !wantAddress) // an `inout` result
    return L.loadInoutResult(result, node->getResolvedType());
  return result;
}

bool ModuleLowering::ExprEmitter::emitMethodArgs(
    ast::MethodCallExpr *node, const ast::MethodDecl *method,
    const pir::Signature &sig, bool userDefined, std::vector<Val> &args,
    std::vector<ExprValue> &ownedArgs, std::vector<Val> &keep) {
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
      args.push_back(p);
      continue;
    }
    if (at < sig.Params.size() && sig.Params[at] == Type::Ptr) {
      // `inout`: the argument's address
      Val p = L.emitInoutArg(argExpr, keep);
      if (!p)
        return false;
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
      Val v = L.emitAsShared(argExpr);
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
  Val elem = L.emitElementLoad(addr, node->getResolvedType());
  // A temporary's element outlives it as a +1 box (exprProducesFreshBox).
  if (L.MemTemps.size() > mark) {
    if (elem.Ty == Type::Box)
      L.emitRetain(elem);
    L.releaseMemTemps(mark);
  }
  return elem;
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
    if (ast::isRefType(elemTy))
      emitRetain(v); // the binding becomes an owner of the element
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
