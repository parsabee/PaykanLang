// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// AST -> PIR lowering: expressions, arrays, tuples, subscripts.

#include "LoweringInternal.h"
#include "Names.h"

#include <cassert>
#include <cstring>

namespace paykan::lowering {

using namespace names;
using pir::CmpPred;
using pir::Opcode;
using pir::Type;

// -- Dispatch

Val ModuleLowering::ExprEmitter::visit(ast::Expr *node) {
  Val v = ast::ExprVisitor<ExprEmitter, Val>::visit(node);
  if (!v || !isPrimitiveBoxing(node))
    return v;
  return L.emitPrimitiveBox(v, node->getCoercedType());
}

// -- Literals

Val ModuleLowering::ExprEmitter::visitIntegerLiteral(
    ast::IntegerLiteral *node) {
  return Val::i64(node->getValue());
}

Val ModuleLowering::ExprEmitter::visitFloatLiteral(ast::FloatLiteral *node) {
  return Val::f64(node->getValue());
}

Val ModuleLowering::ExprEmitter::visitBoolLiteral(ast::BoolLiteral *node) {
  return Val::boolean(node->getValue());
}

Val ModuleLowering::ExprEmitter::visitCharLiteral(ast::CharLiteral *node) {
  return Val::chr(node->getValue());
}

Val ModuleLowering::ExprEmitter::visitNoneLiteral(ast::NoneLiteral *) {
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
  return Val::i64(node->getValue());
}

Val ModuleLowering::ExprEmitter::visitIdentifier(ast::Identifier *node) {
  if (node->getName() == kStdin)
    return L.externObject(kPaykanFileStdin);
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

Val ModuleLowering::ExprEmitter::visitArrayLiteralExpr(
    ast::ArrayLiteralExpr *node) {
  ast::Type *elemTy = nullptr;
  if (auto *at = ast::dyn_cast<ast::ArrayType>(node->getResolvedType()))
    elemTy = at->getElementType();

  bool isObjectArray = L.isObjectElementType(elemTy);
  size_t len = node->getNumElements();
  Val lenVal = Val::i64(static_cast<int64_t>(len));

  Val arr;
  if (isObjectArray) {
    arr = L.callRuntime(kPaykanArrayNewObj, {lenVal}, "arr");
    for (size_t i = 0; i < len; ++i) {
      auto *elemExpr = node->getElements()[i];
      Val v = L.emitAsShared(elemExpr);
      if (!v)
        return Val();
      L.callRuntime(kPaykanArraySetObj,
                    {arr, Val::i64(static_cast<int64_t>(i)), v});
      // set_obj retains the stored box; release the +1 temporary.
      L.emitRelease(v);
    }
  } else if (len > 0) {
    arr = emitPrimitiveArrayLiteral(node, len);
    if (!arr)
      return Val();
  } else {
    arr = L.callRuntime(kPaykanArrayNew, {lenVal}, "arr");
  }
  // Box the raw PaykanArray*.
  return L.emitSharedNew(arr, "arr.shared");
}

/// Compile-time slot bits of a constant operand (after promotion), if any.
static bool constantSlotBits(const Val &v, int64_t &out) {
  if (auto *i = std::get_if<int64_t>(&v.Op.V)) {
    out = *i;
    return true;
  }
  if (auto *d = std::get_if<double>(&v.Op.V)) {
    static_assert(sizeof(double) == sizeof(int64_t));
    std::memcpy(&out, d, sizeof(out));
    return true;
  }
  if (auto *b = std::get_if<bool>(&v.Op.V)) {
    out = *b ? 1 : 0;
    return true;
  }
  if (auto *c = std::get_if<char>(&v.Op.V)) {
    out = static_cast<unsigned char>(*c);
    return true;
  }
  return false;
}

Val ModuleLowering::ExprEmitter::emitPrimitiveArrayLiteral(
    ast::ArrayLiteralExpr *node, size_t len) {
  ast::Type *elemTy = nullptr;
  if (auto *at = ast::dyn_cast<ast::ArrayType>(node->getResolvedType()))
    elemTy = at->getElementType();
  Val lenVal = Val::i64(static_cast<int64_t>(len));

  // Constant fast path: every element a compile-time constant -> one static
  // data global and PaykanArray_new_from_data.
  std::vector<int64_t> words;
  words.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    auto *elemExpr = node->getElements()[i];
    if (!ast::isa<ast::IntegerLiteral>(elemExpr) &&
        !ast::isa<ast::FloatLiteral>(elemExpr) &&
        !ast::isa<ast::BoolLiteral>(elemExpr) &&
        !ast::isa<ast::CharLiteral>(elemExpr) &&
        !ast::isa<ast::EnumValueExpr>(elemExpr) &&
        !ast::isa<ast::UnaryExpr>(elemExpr)) {
      words.clear();
      break;
    }
    // Emitting a literal (or a folded unary on one) produces no instructions.
    Val v = visit(elemExpr);
    if (!v)
      return Val();
    v = L.promoteIntToFloat(v, elemTy);
    int64_t bits;
    if (!constantSlotBits(v, bits)) {
      words.clear();
      break;
    }
    words.push_back(bits);
  }

  if (!words.empty()) {
    std::string key;
    key.reserve(len * 5);
    for (int64_t w : words) {
      key += std::to_string(w);
      key += '|';
    }
    std::string global;
    auto it = L.InternedArrayData.find(key);
    if (it != L.InternedArrayData.end()) {
      global = it->second;
    } else {
      global = ".arr.data" + std::to_string(L.Mod.Datas.size());
      L.Mod.Datas.push_back({global, words});
      L.InternedArrayData[key] = global;
    }
    return L.callRuntime(kPaykanArrayNewFromData,
                         {lenVal, Val::symbol(global, Type::Ptr)}, "arr");
  }

  // Dynamic path: allocate then fill per element.
  Val arr = L.callRuntime(kPaykanArrayNew, {lenVal}, "arr");
  for (size_t i = 0; i < len; ++i) {
    auto *elemExpr = node->getElements()[i];
    Val v = visit(elemExpr);
    if (!v)
      return Val();
    v = L.promoteIntToFloat(v, elemTy);
    v = L.toSlotBits(v);
    L.callRuntime(kPaykanArraySet, {arr, Val::i64(static_cast<int64_t>(i)), v});
  }
  return arr;
}

Val ModuleLowering::ExprEmitter::visitSubscriptExpr(ast::SubscriptExpr *node) {
  Val idx = visit(node->getIndex());
  if (!idx)
    return Val();
  idx = L.coerceBoolToI64(idx, Type::I64);

  // Str vs. char[] receiver: decided from the receiver's own type.
  bool receiverIsStr = false;
  if (node->getResolvedType() == L.ASTCtx.getCharTy()) {
    ast::Type *recvTy = node->getArray()->getResolvedType();
    if (auto *id = ast::dyn_cast<ast::Identifier>(node->getArray());
        !recvTy && id && L.CurrentScope)
      recvTy = L.CurrentScope->lookupASTType(id->getName());
    receiverIsStr = !ast::dyn_cast<ast::ArrayType>(recvTy); // null-safe
  }

  // A call-rooted receiver is torn down once the element is copied out.
  Receiver recv = L.emitReceiver(node->getArray());
  if (!recv)
    return Val();
  const ExprValue &recvOwned = recv.Owned;

  if (receiverIsStr) {
    Val ch = L.callRuntime(kPaykanStringCharAt, {recv.Raw, idx}, "str.char_at");
    L.releaseIfOwned(recvOwned);
    return ch;
  }

  Val raw = L.callRuntime(kPaykanArrayGet, {recv.Raw, idx}, "elem.raw");
  ast::Type *elemTy = node->getResolvedType();
  if (!L.isObjectElementType(elemTy)) {
    L.releaseIfOwned(recvOwned);
    if (!elemTy)
      return raw;
    return L.fromSlotBits(raw, elemTy);
  }

  // Object element: the slot stores a box as bits.
  Val shared = L.B.cast(raw, Type::Box, "elem.shared");
  // A temporary array (`mk()[0]`) dies with this expression: retain the
  // element and hand the consumer the +1 box.
  if (L.exprAlreadyShared(node)) {
    L.emitRetain(shared);
    L.releaseIfOwned(recvOwned);
    return shared;
  }
  // Borrowed from a live array: unwrap to the raw object.
  return L.emitSharedGet(shared, "elem.obj");
}

Val ModuleLowering::ExprEmitter::visitTernaryExpr(ast::TernaryExpr *node) {
  Val cond = visit(node->getCondition());
  if (!cond)
    return Val();
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
  case ast::UnaryOpcode::Neg:
    if (auto *i = std::get_if<int64_t>(&operand.Op.V))
      return Val::i64(static_cast<int64_t>(0ULL - static_cast<uint64_t>(*i)));
    if (auto *d = std::get_if<double>(&operand.Op.V))
      return Val::f64(-*d);
    return L.B.unary(Opcode::Neg, operand,
                     operand.Ty == Type::F64 ? "fneg" : "neg");
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
                                                  const Val &rhs, bool isDiv) {
  // Divide / modulo by zero: runtime panic (sdiv/srem by zero is UB).
  Val isZero = L.B.cmp(CmpPred::Eq, rhs, Val::i64(0), "divzero.chk");
  pir::If *z = L.B.openIf(isZero, false);
  L.B.enter(*z->Then);
  L.callRuntime(kPaykanPanicDivByZero, {});
  L.B.emitUnreachable();
  L.B.leave();
  if (!isDiv)
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
    if (ast::isa<ast::OptionalType>(lt) || ast::isa<ast::OptionalType>(rt)) {
      Val eq = L.emitOptionalEquality(node);
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
    return L.B.binary(Opcode::Add, lhs, rhs, isFloat ? "fadd" : "add");
  case ast::BinaryOpcode::Sub:
    return L.B.binary(Opcode::Sub, lhs, rhs, isFloat ? "fsub" : "sub");
  case ast::BinaryOpcode::Mul:
    return L.B.binary(Opcode::Mul, lhs, rhs, isFloat ? "fmul" : "mul");
  case ast::BinaryOpcode::Div:
    if (isFloat)
      return L.B.binary(Opcode::Div, lhs, rhs, "fdiv");
    emitIntDivGuards(lhs, rhs, /*isDiv=*/true);
    return L.B.binary(Opcode::Div, lhs, rhs, "sdiv");
  case ast::BinaryOpcode::Mod: {
    if (isFloat)
      return L.B.binary(Opcode::Rem, lhs, rhs, "fmod");
    emitIntDivGuards(lhs, rhs, /*isDiv=*/false);
    // x % -1 is 0 for every x, but INT64_MIN % -1 traps: divide by 1 instead.
    Val isNegOne = L.B.cmp(CmpPred::Eq, rhs, Val::i64(-1));
    Val safe = L.B.select(isNegOne, Val::i64(1), rhs, "rem.divisor");
    return L.B.binary(Opcode::Rem, lhs, safe, "srem");
  }
  case ast::BinaryOpcode::Lt:
    return L.B.cmp(CmpPred::Lt, lhs, rhs, isFloat ? "flt" : "slt");
  case ast::BinaryOpcode::Gt:
    return L.B.cmp(CmpPred::Gt, lhs, rhs, isFloat ? "fgt" : "sgt");
  case ast::BinaryOpcode::Le:
    return L.B.cmp(CmpPred::Le, lhs, rhs, isFloat ? "fle" : "sle");
  case ast::BinaryOpcode::Ge:
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
    Val v = visit(argExpr);
    if (!v)
      return Val();
    ExprValue ev = L.classifyExpr(argExpr, v);
    if (ev.isOwned())
      ownedArgs.push_back(ev);
    if (L.exprAlreadyShared(argExpr) && v.Ty == Type::Box)
      v = L.emitSharedGet(v, "unboxed");
    // A boxed conversion source (`Str<Int>(x)`): format the boxed value.
    if (const char *unbox = conversionUnboxer(node->getCalleeName()))
      v = L.callRuntime(unbox, {v}, "conv.unboxed");
    // An optional argument to an `Obj` builtin: substitute the None
    // singleton for a null pointer so it prints as "None".
    if (v.Ty == Type::Obj)
      v = L.emitOptionalToObjRaw(argExpr, v);
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

bool ModuleLowering::isParseConversion(const std::string &callee) {
  return callee == kConvIntStr || callee == kConvFloatStr ||
         callee == kConvBoolStr || callee == kConvIntBoxStr ||
         callee == kConvFloatBoxStr || callee == kConvBoolBoxStr;
}

bool ModuleLowering::isStrConversion(const std::string &callee) {
  return callee == kConvStrInt || callee == kConvStrFloat ||
         callee == kConvStrBool || callee == kConvStrChar ||
         conversionUnboxer(callee) != nullptr;
}

const char *ModuleLowering::conversionUnboxer(const std::string &callee) {
  if (callee == kConvStrIntBox)
    return kPaykanIntValue;
  if (callee == kConvStrFloatBox)
    return kPaykanFloatValue;
  if (callee == kConvStrBoolBox)
    return kPaykanBoolValue; // 0 / 1, the i64 PaykanString_from_bool takes
  if (callee == kConvStrCharBox)
    return kPaykanCharValue;
  return nullptr;
}

// Inline numeric conversions `Target<Source>(value)` (#64).  This is the one
// place their semantics are defined; both backends inherit it, and nothing
// here is undefined behaviour in either (the C backend is strict C11):
//   int<float>  truncates toward zero; NaN, +-inf and values outside int64
//               panic (Paykan_panic_float_to_int) before the `ftoi`;
//   float<int>  nearest double (`itof`);
//   int<bool>   1 / 0;          bool<int>  `!= 0`;
//   int<char>   the char's byte code 0..255 (`cast` zero-extends);
//   char<int>   the byte with that code; outside 0..255 panics
//               (Paykan_panic_int_to_char).
// Returns an invalid Val when @p node is not a numeric conversion.
Val ModuleLowering::ExprEmitter::emitConversion(ast::CallExpr *node) {
  const std::string &name = node->getCalleeName();
  bool isNumeric = name == kConvIntFloat || name == kConvFloatInt ||
                   name == kConvIntBool || name == kConvBoolInt ||
                   name == kConvIntChar || name == kConvCharInt;
  if (!isNumeric || node->getNumArguments() != 1)
    return Val();
  Val v = visit(node->getArguments()[0]);
  if (!v)
    return Val();
  auto panicUnless = [&](const Val &ok, const char *panicFn) {
    Val bad = L.B.unary(Opcode::Not, ok, "conv.bad");
    pir::If *s = L.B.openIf(bad, false);
    L.B.enter(*s->Then);
    L.callRuntime(panicFn, {v});
    L.B.emitUnreachable();
    L.B.leave();
  };
  if (name == kConvIntFloat) {
    // [-2^63, 2^63) is exactly the doubles whose truncation fits in int64;
    // both compares are ordered, so NaN fails them.
    Val lo =
        L.B.cmp(CmpPred::Ge, v, Val::f64(-9223372036854775808.0), "conv.lo");
    Val hi =
        L.B.cmp(CmpPred::Lt, v, Val::f64(9223372036854775808.0), "conv.hi");
    panicUnless(L.B.select(lo, hi, Val::boolean(false), "conv.ok"),
                kPaykanPanicFloatToInt);
    return L.B.ftoi(v, "conv.int");
  }
  if (name == kConvFloatInt)
    return L.B.itof(v, "conv.float");
  if (name == kConvIntBool)
    return L.coerceBoolToI64(v, Type::I64);
  if (name == kConvBoolInt)
    return L.B.cmp(CmpPred::Ne, v, Val::i64(0), "conv.bool");
  if (name == kConvIntChar)
    return L.B.cast(v, Type::I64, "conv.code");
  // char<int>: range-check, then map the code 128..255 to the signed byte
  // with the same bits before the truncating cast, so the C backend's
  // `(int8_t)` conversion only ever sees an in-range value.
  Val lo = L.B.cmp(CmpPred::Ge, v, Val::i64(0), "conv.lo");
  Val hi = L.B.cmp(CmpPred::Le, v, Val::i64(255), "conv.hi");
  panicUnless(L.B.select(lo, hi, Val::boolean(false), "conv.ok"),
              kPaykanPanicIntToChar);
  Val high = L.B.cmp(CmpPred::Gt, v, Val::i64(127), "conv.high");
  Val wrapped = L.B.binary(Opcode::Sub, v, Val::i64(256), "conv.wrap");
  Val byte = L.B.select(high, wrapped, v, "conv.byte");
  return L.B.cast(byte, Type::Char, "conv.char");
}

Val ModuleLowering::ExprEmitter::visitCallExpr(ast::CallExpr *node) {
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
    for (size_t i = 0; i < node->getNumArguments(); ++i) {
      auto *argExpr = node->getArguments()[i];
      Type paramTy = i + 1 < superFn->Sig.Params.size()
                         ? superFn->Sig.Params[i + 1]
                         : Type::Void;
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
  for (size_t i = 0; i < node->getNumArguments(); ++i) {
    auto *arg = node->getArguments()[i];
    Type paramTy =
        i < callee->Sig.Params.size() ? callee->Sig.Params[i] : Type::Void;
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
  Val result = L.B.call(callee->Name, callee->Sig, args, "call");
  L.releaseAfterCall(keep);
  return result;
}

Val ModuleLowering::ExprEmitter::emitArrayPush(ast::MethodCallExpr *node,
                                               const Val &recv,
                                               ast::Type *elemTy) {
  if (node->getNumArguments() != 1)
    return Val();
  auto *argExpr = node->getArguments()[0];
  if (L.isObjectElementType(elemTy)) {
    Val argVal = L.emitAsShared(argExpr);
    if (!argVal)
      return Val();
    L.callRuntime(kPaykanArrayPushObj, {recv, argVal});
    // push_obj retains the stored box; drop the +1 temporary.
    L.emitRelease(argVal);
  } else {
    Val argVal = L.emitExpr(argExpr);
    if (!argVal)
      return Val();
    argVal = L.promoteIntToFloat(argVal, elemTy);
    argVal = L.toSlotBits(argVal);
    L.callRuntime(kPaykanArrayPush, {recv, argVal});
  }
  return Val();
}

Val ModuleLowering::ExprEmitter::emitArrayPop(const Val &recv,
                                              ast::Type *elemTy) {
  if (L.isObjectElementType(elemTy))
    return L.callRuntime(kPaykanArrayPopObj, {recv}, "mcall");
  Val raw = L.callRuntime(kPaykanArrayPop, {recv}, "mcall");
  return L.fromSlotBits(raw, elemTy);
}

Val ModuleLowering::ExprEmitter::visitMethodCallExpr(
    ast::MethodCallExpr *node) {
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
      if (id->getName() == kStdin)
        return L.ASTCtx.getFileTy();
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
  auto *arrTy = ast::dyn_cast<ast::ArrayType>(recvASTTy);

  // push / pop are direct runtime calls, not vtable slots.
  const std::string &mname = node->getMethodName();
  if (arrTy && (mname == kPush || mname == kPop)) {
    Val result = mname == kPush
                     ? emitArrayPush(node, recv, arrTy->getElementType())
                     : emitArrayPop(recv, arrTy->getElementType());
    L.releaseIfOwned(recvOwned);
    return result;
  }

  ast::ClassType *ct = nullptr;
  if (arrTy)
    ct = L.ASTCtx.getArrayTy();
  else if (auto *tupTy = ast::dyn_cast<ast::TupleType>(recvASTTy))
    ct = L.ASTCtx.getOrCreateSpecializedTupleType(tupTy);
  else
    ct = ast::dyn_cast<ast::ClassType>(recvASTTy);
  if (!ct)
    ct = L.getExprClassType(node->getReceiver());
  if (!ct) {
    for (auto *candidate :
         {L.ASTCtx.getStrTy(), L.ASTCtx.getFileTy(), L.ASTCtx.getObjTy()}) {
      if (candidate && candidate->findMethod(node->getMethodName())) {
        ct = candidate;
        break;
      }
    }
  }

  int vtableIdx = ct ? ct->getVTableIndex(node->getMethodName()) : -1;
  assert(vtableIdx >= 0 && "Sema should have verified method exists");
  ast::MethodDecl *method =
      ct ? ct->findMethod(node->getMethodName()) : nullptr;
  pir::Signature sig = method ? L.slotSignature(ct, method)
                              : pir::Signature{{Type::Obj}, Type::Box};

  bool isUserDefinedMethod = ct && !ct->isBuiltin();
  std::vector<Val> args;
  std::vector<ExprValue> ownedArgs;
  std::vector<Val> keep;
  for (size_t i = 0; i < node->getNumArguments(); ++i) {
    auto *argExpr = node->getArguments()[i];
    if (i + 1 < sig.Params.size() && sig.Params[i + 1] == Type::Ptr) {
      // `inout`: the argument's address
      Val p = L.emitInoutArg(argExpr, keep);
      if (!p)
        return Val();
      args.push_back(p);
      continue;
    }
    ast::Type *paramASTTy = (method && i < method->getParamTypes().size())
                                ? method->getParamTypes()[i]
                                : nullptr;
    // Ref-typed arguments to user methods and to the virtual `equals` use
    // the callee-consumes ABI (+1 box); builtin methods borrow.
    bool isClassParam =
        ast::isRefType(paramASTTy) &&
        (isUserDefinedMethod || node->getMethodName() == kMethodEquals);
    if (isClassParam) {
      Val v = L.emitAsShared(argExpr);
      if (!v)
        return Val();
      args.push_back(v);
      continue;
    }
    Val v = visit(argExpr);
    if (!v)
      return Val();
    if (ExprValue ev = L.classifyExpr(argExpr, v); ev.isOwned())
      ownedArgs.push_back(ev);
    Type paramTy = i + 1 < sig.Params.size() ? sig.Params[i + 1] : Type::Void;
    // A builtin method borrowing a ref argument (File.write(s)) takes the
    // raw object.
    if (paramTy == Type::Obj && v.Ty == Type::Box)
      v = L.emitSharedGet(v, "unboxed");
    args.push_back(L.coerceTo(v, paramTy));
  }

  // Virtual dispatch through the receiver's vtable.  User classes are named
  // (the backend knows their vtable layout); runtime classes use the
  // explicit-signature form.
  // A class of this module is named (the backend knows its vtable); a
  // runtime class or a class imported from another module (whose extern item
  // carries only the layout) uses the explicit-signature form.
  std::string className;
  if (isUserDefinedMethod && !L.getOrCreateClass(ct)->IsExtern)
    className = ct->getName();
  else if (isUserDefinedMethod &&
           !L.checkImportedSlot(ct, static_cast<uint32_t>(vtableIdx), sig))
    return Val();
  Val result = L.B.vcall(recv, className, static_cast<uint32_t>(vtableIdx), sig,
                         args, "mcall");
  L.releaseAfterCall(keep);
  for (const auto &ev : ownedArgs)
    L.releaseIfOwned(ev);
  L.releaseIfOwned(recvOwned);
  // An i64 slot result standing for a Paykan bool (`equals`) comes back as
  // the bool the expression has.
  if (result && result.Ty == Type::I64 && method && method->getReturnType() &&
      L.toPIRType(L.canonicalizeDeclType(method->getReturnType())) ==
          Type::Bool)
    result = L.B.cmp(CmpPred::Ne, result, Val::i64(0), "eq");
  return result;
}

// -- Subscript assignment

Val ModuleLowering::visitSubscriptAssignStmt(ast::SubscriptAssignStmt *node) {
  Receiver arr = emitReceiver(node->getArray());
  if (!arr)
    return Val();
  const Val &arrRaw = arr.Raw;

  Val idx = emitExpr(node->getIndex());
  if (!idx)
    return Val();
  idx = coerceBoolToI64(idx, Type::I64);

  ast::Type *elemTy = nullptr;
  if (ast::Type *arrTy = node->getArray()->getResolvedType())
    if (auto *at = ast::dyn_cast<ast::ArrayType>(arrTy))
      elemTy = at->getElementType();
  if (!elemTy)
    if (auto *id = ast::dyn_cast<ast::Identifier>(node->getArray()))
      if (CurrentScope)
        if (auto *st = CurrentScope->lookupASTType(id->getName()))
          if (auto *at = ast::dyn_cast<ast::ArrayType>(st))
            elemTy = at->getElementType();

  if (isObjectElementType(elemTy)) {
    Val val = emitAsShared(node->getValue());
    if (!val)
      return Val();
    callRuntime(kPaykanArraySetObj, {arrRaw, idx, val});
    // set_obj retains; drop the +1 temporary.
    emitRelease(val);
  } else {
    Val val = emitExpr(node->getValue());
    if (!val)
      return Val();
    val = promoteIntToFloat(val, elemTy);
    val = toSlotBits(val);
    callRuntime(kPaykanArraySet, {arrRaw, idx, val});
  }
  releaseIfOwned(arr.Owned);
  return Val();
}

// -- Tuples

unsigned char ModuleLowering::tupleElementKind(ast::Type *elemTy) const {
  if (isObjectElementType(elemTy))
    return kTupleSlotRef;
  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(elemTy)) {
    switch (bt->getTypeKind()) {
    case ast::BuiltinType::Float:
      return kTupleSlotFloat;
    case ast::BuiltinType::Bool:
      return kTupleSlotBool;
    case ast::BuiltinType::Char:
      return kTupleSlotChar;
    case ast::BuiltinType::Int:
    case ast::BuiltinType::Void:
      break;
    }
  }
  return kTupleSlotInt;
}

Val ModuleLowering::emitTupleKindsGlobal(ast::TupleType *tt) {
  std::string key;
  for (ast::Type *e : tt->getElementTypes())
    key += static_cast<char>('0' + tupleElementKind(e));
  auto it = InternedTupleKinds.find(key);
  if (it != InternedTupleKinds.end())
    return Val::symbol(it->second, Type::Ptr);
  std::string name =
      std::string(kTupleKindsGlobalName) + std::to_string(Mod.Bytes.size());
  std::vector<uint8_t> bytes;
  for (char c : key)
    bytes.push_back(static_cast<uint8_t>(c - '0'));
  Mod.Bytes.push_back({name, std::move(bytes)});
  InternedTupleKinds[key] = name;
  return Val::symbol(name, Type::Ptr);
}

Val ModuleLowering::toSlotBits(const Val &v) {
  int64_t bits;
  if (v.Ty != Type::I64 && constantSlotBits(v, bits))
    return Val::i64(bits);
  if (v.Ty == Type::F64)
    return B.cast(v, Type::I64, "f64.bits");
  if (v.Ty == Type::Bool || v.Ty == Type::Char)
    return B.cast(v, Type::I64, "bool.ext");
  return v;
}

Val ModuleLowering::fromSlotBits(const Val &bits, ast::Type *elemTy) {
  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(elemTy)) {
    switch (bt->getTypeKind()) {
    case ast::BuiltinType::Float:
      return B.cast(bits, Type::F64, "elem.f64");
    case ast::BuiltinType::Bool:
      return B.cast(bits, Type::Bool, "elem.bool");
    case ast::BuiltinType::Char:
      return B.cast(bits, Type::Char, "elem.char");
    case ast::BuiltinType::Int:
    case ast::BuiltinType::Void:
      break;
    }
  }
  return bits;
}

Val ModuleLowering::ExprEmitter::visitTupleLiteralExpr(
    ast::TupleLiteralExpr *node) {
  auto *tt = ast::dyn_cast<ast::TupleType>(node->getResolvedType());
  assert(tt && tt->getArity() == node->getNumElements() &&
         "Sema must resolve every tuple literal to its TupleType");
  Val tup = L.callRuntime(kPaykanTupleNew,
                          {Val::i64(static_cast<int64_t>(tt->getArity())),
                           L.emitTupleKindsGlobal(tt)},
                          "tup");
  for (size_t i = 0; i < tt->getArity(); ++i) {
    ast::Expr *elemExpr = node->getElements()[i];
    ast::Type *elemTy = tt->getElementType(i);
    Val idx = Val::i64(static_cast<int64_t>(i));
    if (L.isObjectElementType(elemTy)) {
      Val box = L.emitAsShared(elemExpr);
      if (!box)
        return Val();
      L.callRuntime(kPaykanTupleSetObj, {tup, idx, box});
      L.emitRelease(box);
    } else {
      Val v = visit(elemExpr);
      if (!v)
        return Val();
      v = L.promoteIntToFloat(v, elemTy);
      L.callRuntime(kPaykanTupleSet, {tup, idx, L.toSlotBits(v)});
    }
  }
  return L.emitSharedNew(tup, "tup.shared");
}

Val ModuleLowering::ExprEmitter::visitTupleIndexExpr(
    ast::TupleIndexExpr *node) {
  Receiver recv = L.emitReceiver(node->getTuple());
  if (!recv)
    return Val();
  const ExprValue &recvOwned = recv.Owned;

  Val bits = L.callRuntime(
      kPaykanTupleGet,
      {recv.Raw, Val::i64(static_cast<int64_t>(node->getIndex()))}, "elem.raw");
  ast::Type *elemTy = node->getResolvedType();
  if (!L.isObjectElementType(elemTy)) {
    L.releaseIfOwned(recvOwned);
    return L.fromSlotBits(bits, elemTy);
  }
  // Reference element: the slot's box, borrowed from the tuple; retained
  // first when the tuple was a temporary.
  Val box = L.B.cast(bits, Type::Box, "elem.shared");
  if (recvOwned.isOwned()) {
    L.emitRetain(box);
    L.releaseIfOwned(recvOwned);
  }
  return box;
}

Val ModuleLowering::visitDestructureStmt(ast::DestructureStmt *node) {
  auto *tt = ast::dyn_cast<ast::TupleType>(node->getValue()->getResolvedType());
  assert(tt && tt->getArity() == node->getNumTargets() &&
         "Sema must resolve the destructured value to a matching TupleType");

  Receiver val = emitReceiver(node->getValue());
  if (!val)
    return Val();
  const Val &raw = val.Raw;

  for (size_t i = 0; i < node->getNumTargets(); ++i) {
    const auto &target = node->getTargets()[i];
    if (target.isSkip())
      continue;
    ast::Type *elemTy = tt->getElementType(i);
    const std::string &name = target.getName();

    Val bits = callRuntime(
        kPaykanTupleGet, {raw, Val::i64(static_cast<int64_t>(i))}, "elem.raw");
    Val v;
    if (isObjectElementType(elemTy)) {
      // The binding becomes an owner of the element: +1 on the slot's box.
      v = B.cast(bits, Type::Box, "elem.shared");
      emitRetain(v);
    } else {
      v = fromSlotBits(bits, elemTy);
    }

    ast::Type *bindTy = elemTy;
    Scope *owner = target.DeclType ? nullptr : CurrentScope->findOwner(name);
    if (!owner) {
      if (target.DeclType)
        bindTy = canonicalizeDeclType(target.DeclType);
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
  releaseIfOwned(val.Owned);
  return Val();
}

} // namespace paykan::lowering
