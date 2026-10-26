// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// AST -> PIR lowering: values held in memory, fixed-size arrays (`T[n]`,
// docs/language/09-arrays.md) and tuples (docs/language/10-tuples.md).
//
// Such a value is never in an SSA value: a variable is a `mem` local of its
// bytes (an `inout` one a `ptr` local holding the address of the value it
// names), and an expression of its type is lowered to the address of its
// bytes.  Its elements are read and written in place: an array's at its
// address plus the index times the element's size, after a bounds check
// that panics (Paykan_panic_index), a tuple's at their offsets, laid out like
// a C struct's fields.  An element held in memory is an address again.
// Initialising or assigning a whole value copies its bytes (mem.copy).
//
// An element of a reference type is its box, and its slot owns a reference
// to it, as a variable does: a copy retains every reference, an assignment
// releases the ones it replaces, and a variable's are released when its
// scope ends.  A temporary (a call's result, a literal) owns its references:
// the place it initialises takes them over, and any other use releases them
// once it is done (MemTemps).  A field is in its object's bytes
// (field.addr); the object of a call-rooted one is released the same way.

#include "LoweringInternal.h"
#include "Names.h"

#include <cmath>

namespace paykan::lowering {

using pir::CmpPred;
using pir::Opcode;
using pir::Type;

namespace {

/// The innermost elements of a (nested) fixed-size array: their type, and
/// how many there are, one after another.
std::pair<ast::Type *, int64_t> innermostElements(ast::FixedArrayType *ft) {
  int64_t n = ft->getLength();
  while (auto *inner =
             ast::dyn_cast<ast::FixedArrayType>(ft->getElementType())) {
    ft = inner;
    n *= ft->getLength();
  }
  return {ft->getElementType(), n};
}

/// Whether a value of @p ty holds a reference: is one, or holds one (a
/// function value holds its closure's).
bool holdsRef(const ast::Type *ty) {
  if (ast::isRefType(ty) || ast::isFunctionValue(ty))
    return true;
  if (ast::isMemOptional(ty))
    return holdsRef(ast::cast<ast::OptionalType>(ty)->getInnerType());
  if (const auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty))
    return holdsRef(ft->getElementType());
  if (const auto *tt = ast::dyn_cast<ast::TupleType>(ty))
    for (const ast::Type *e : tt->getElementTypes())
      if (holdsRef(e))
        return true;
  // A tagged union holds each variant's fields.
  if (const auto *et = ast::dyn_cast<ast::EnumType>(ty))
    for (size_t i = 0; i < et->getNumVariants(); ++i)
      if (const ast::StructType *payload = et->getPayload(i);
          payload && holdsRef(payload))
        return true;
  return false;
}

int64_t alignTo(int64_t n, int64_t align) {
  return (n + align - 1) / align * align;
}

/// Up to this many slots are visited one by one; more in a loop.
constexpr int64_t kUnrolledSlots = 4;

/// Whether @p e is a literal whose value is all zero bits.
bool isZeroLiteral(const ast::Expr *e) {
  if (const auto *i = ast::dyn_cast<ast::IntegerLiteral>(e))
    return i->getValue() == 0;
  if (const auto *f = ast::dyn_cast<ast::FloatLiteral>(e))
    return f->getValue() == 0.0 && !std::signbit(f->getValue());
  if (const auto *b = ast::dyn_cast<ast::BoolLiteral>(e))
    return !b->getValue();
  if (const auto *c = ast::dyn_cast<ast::CharLiteral>(e))
    return c->getValue() == 0;
  return false;
}

} // namespace

bool ModuleLowering::memHoldsRefs(const ast::Type *ty) {
  return ast::isMemType(ty) && holdsRef(ty);
}

ModuleLowering::MemLayout ModuleLowering::memLayout(ast::Type *ty) {
  if (ast::Type *shape = memShapeOf(ty); shape != ty)
    return memLayout(shape);
  if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty)) {
    MemLayout elem = memLayout(ft->getElementType());
    return {elem.Size * ft->getLength(), elem.Align};
  }
  if (auto *tt = ast::dyn_cast<ast::TupleType>(ty)) {
    MemLayout l;
    for (ast::Type *e : tt->getElementTypes()) {
      MemLayout el = memLayout(e);
      l.Size = alignTo(l.Size, el.Align) + el.Size;
      l.Align = std::max(l.Align, el.Align);
    }
    return {alignTo(l.Size, l.Align), l.Align};
  }
  Type t = toPIRType(ty);
  return {pir::storeSize(t), pir::storeAlign(t)};
}

int64_t ModuleLowering::tupleElementOffset(const ast::TupleType *tt, size_t i) {
  int64_t offset = 0;
  for (size_t k = 0; k <= i; ++k) {
    MemLayout el = memLayout(tt->getElementType(k));
    offset = alignTo(offset, el.Align);
    if (k < i)
      offset += el.Size;
  }
  return offset;
}

ast::Type *ModuleLowering::memTypeOf(ast::Expr *e) {
  ast::Type *coerced = e->getCoercedType();
  if (coerced && ast::isMemType(coerced) &&
      (isMemOptionalWrap(e) || ast::isa<ast::NoneLiteral>(e)))
    return coerced;
  if (ast::isa<ast::NoneLiteral>(e) && !ast::isMemType(e->getResolvedType()))
    return functionValueShape(); // an optional function value's, by default
  return e->getResolvedType();
}

void ModuleLowering::emitMemOptionalWrap(ast::Expr *e, const Val &dst) {
  // The flag, then the value in place: its own initialisation, not wrapped
  // again.
  ast::Type *optTy = e->getCoercedType();
  B.memZero(dst, Val::i64(memLayout(optTy).Size));
  B.ptrStore(dst, Val::boolean(true));
  const ast::Expr *saved = std::exchange(UnwrappedExpr, e);
  emitMemInit(e, B.ptrAdd(dst, Val::i64(memOptionalPayloadOffset(optTy)),
                          "opt.value.addr"));
  UnwrappedExpr = saved;
}

ModuleLowering::MemValue ModuleLowering::emitMemValue(ast::Expr *e) {
  // A tuple or a struct made the present value of its optional: the
  // optional, in a temporary.
  if (isMemOptionalWrap(e) && e != UnwrappedExpr) {
    MemLayout l = memLayout(e->getCoercedType());
    pir::LocalId tmp = B.addMemLocal("opt", l.Size, l.Align);
    Val addr = B.localAddr(tmp, "opt.addr");
    emitMemOptionalWrap(e, addr);
    return {addr, /*Temp=*/true};
  }
  // None of an optional held in memory: all zeros.
  if (ast::isa<ast::NoneLiteral>(e)) {
    MemLayout l = memLayout(memTypeOf(e));
    pir::LocalId tmp = B.addMemLocal("none", l.Size, l.Align);
    Val addr = B.localAddr(tmp, "none.addr");
    B.memZero(addr, Val::i64(l.Size));
    return {addr, /*Temp=*/true};
  }
  // A function value made here: a lambda or a named function, written into
  // a temporary.
  if (isFunctionValueExpr(e)) {
    MemLayout l = memLayout(functionValueShape());
    pir::LocalId tmp = B.addMemLocal("fn", l.Size, l.Align);
    Val addr = B.localAddr(tmp, "fn.addr");
    if (auto *le = ast::dyn_cast<ast::LambdaExpr>(e)) {
      emitLambdaInto(le, addr);
    } else {
      auto *fnt = ast::cast<ast::FunctionType>(e->getResolvedType());
      auto *ev = ast::dyn_cast<ast::EnumValueExpr>(e);
      emitFunctionRefInto(ev ? *ev->getFunctionRef()
                             : ast::cast<ast::Identifier>(e)->getFunctionRef(),
                          fnt, addr);
    }
    return {addr, /*Temp=*/true};
  }
  // A tagged union's variant made here: written into a temporary.
  if (isVariantExpr(e)) {
    MemLayout l = memLayout(e->getResolvedType());
    pir::LocalId tmp = B.addMemLocal("variant", l.Size, l.Align);
    Val addr = B.localAddr(tmp, "variant.addr");
    emitMemInit(e, addr);
    return {addr, /*Temp=*/true};
  }
  if (auto *id = ast::dyn_cast<ast::Identifier>(e)) {
    pir::LocalId local = CurrentScope->lookup(id->getName());
    Val addr = CurrentScope->inoutType(id->getName()) != Type::Void
                   ? B.load(local, id->getName() + ".addr")
                   : B.localAddr(local, id->getName() + ".addr");
    if (!isHeldRead(id))
      return {addr};
    // A read its statement may change the variable under (noteHeldReads):
    // a copy of its own, in a temporary.
    ast::Type *ty = CurrentScope->lookupASTType(id->getName());
    MemLayout l = memLayout(ty);
    pir::LocalId tmp = B.addMemLocal("held", l.Size, l.Align);
    Val copy = B.localAddr(tmp, "held.addr");
    B.memCopy(copy, addr, Val::i64(l.Size));
    emitMemRetain(copy, ty);
    return {copy, /*Temp=*/true};
  }
  // An element, or a struct's field, in the bytes of the value it is part
  // of, at its offset: the whole's address first, then each part's in it,
  // outward.  A loop, as a chain of parts (`s.a.b.c`, `t.0.1`) may be
  // thousands long (frontend::kMaxChain).
  if (wholeOf(e)) {
    std::vector<ast::Expr *> parts;
    ast::Expr *whole = e;
    for (; wholeOf(whole); whole = wholeOf(whole))
      parts.push_back(whole);
    Val addr = emitMemAddress(whole);
    for (auto it = parts.rbegin(); addr && it != parts.rend(); ++it)
      addr = emitPartAddress(*it, addr);
    return {addr};
  }
  // A field, in its object: a call-rooted one (`mk().xs`) is kept until the
  // consumer is done with the address.
  if (auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e)) {
    Receiver recv = emitReceiver(ma->getReceiver(), "obj");
    ast::ClassType *ct = getExprClassType(ma->getReceiver());
    if (!recv || !ct)
      return {};
    getOrCreateClass(ct);
    if (recv.Owned.isOwned())
      MemTemps.emplace_back(recv.Owned.V, nullptr);
    return {B.fieldAddr(recv.Raw, ct->getName(), ma->getFieldName(),
                        ma->getFieldName() + ".addr")};
  }
  // A call's result, in the temporary the call wrote, or for an `inout`
  // result the storage it names, which is not the consumer's to drop.
  if (ast::isa<ast::CallExpr>(e) || ast::isa<ast::MethodCallExpr>(e) ||
      ast::isa<ast::CallValueExpr>(e))
    return {emitExpr(e), /*Temp=*/!e->isInoutResult()};
  if (ast::isa<ast::ArrayLiteralExpr>(e) ||
      ast::isa<ast::TupleLiteralExpr>(e)) {
    MemLayout l = memLayout(e->getResolvedType());
    pir::LocalId tmp = B.addMemLocal("lit", l.Size, l.Align);
    Val addr = B.localAddr(tmp, "lit.addr");
    emitMemInit(e, addr);
    return {addr, /*Temp=*/true};
  }
  // A conditional: the chosen branch's value, in a temporary.
  if (auto *te = ast::dyn_cast<ast::TernaryExpr>(e)) {
    Val cond = emitExpr(te->getCondition());
    if (!cond)
      return {};
    MemLayout l = memLayout(e->getResolvedType());
    pir::LocalId tmp = B.addMemLocal("sel", l.Size, l.Align);
    Val addr = B.localAddr(tmp, "sel.addr");
    pir::If *s = B.openIf(cond, /*withElse=*/true);
    B.enter(*s->Then);
    emitMemInit(te->getTrueExpr(), addr);
    B.leave();
    B.enter(*s->Else);
    emitMemInit(te->getFalseExpr(), addr);
    B.leave();
    return {addr, /*Temp=*/true};
  }
  reportInternalError("a value held in memory that is not a variable, a "
                      "field, an element, a literal, a conditional or a call");
  return {};
}

ast::Expr *ModuleLowering::memValueVisitsFirst(ast::Expr *e) {
  // As emitMemValue, branch by branch.
  if (isMemOptionalWrap(e) || ast::isa<ast::NoneLiteral>(e) ||
      isFunctionValueExpr(e) || isVariantExpr(e) ||
      ast::isa<ast::Identifier>(e))
    return nullptr;
  if (wholeOf(e)) {
    ast::Expr *whole = e;
    while (wholeOf(whole))
      whole = wholeOf(whole);
    return memValueVisitsFirst(whole);
  }
  if (auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e))
    return ma->getReceiver();
  if (ast::isa<ast::CallExpr>(e) || ast::isa<ast::MethodCallExpr>(e) ||
      ast::isa<ast::CallValueExpr>(e))
    return e->isInoutResult() && ast::isMemType(e->getResolvedType()) ? nullptr
                                                                      : e;
  return nullptr;
}

Val ModuleLowering::emitMemAddress(ast::Expr *e) {
  MemValue v = emitMemValue(e);
  // The temporary is what emitMemValue built: a tuple or a struct made the
  // present value of its optional is the optional (memTypeOf), whose
  // references sit after the presence flag.
  ast::Type *ty = memTypeOf(e);
  if (v.Temp && v.Addr && memHoldsRefs(ty))
    MemTemps.emplace_back(v.Addr, ty);
  return v.Addr;
}

void ModuleLowering::releaseMemTemps(size_t mark) {
  while (MemTemps.size() > mark) {
    auto [v, ty] = MemTemps.back();
    MemTemps.pop_back();
    if (ty)
      emitMemRelease(v, ty);
    else
      emitRelease(v);
  }
}

void ModuleLowering::forEachMemRef(const Val &addr, ast::Type *ty,
                                   const std::function<void(Val)> &fn) {
  if (!addr || !holdsRef(ty))
    return;
  if (ast::Type *shape = memShapeOf(ty); shape != ty) {
    forEachMemRef(addr, shape, fn);
    return;
  }
  if (ast::isRefType(ty)) {
    fn(B.ptrLoad(addr, Type::Box, "elem"));
    return;
  }
  if (auto *tt = ast::dyn_cast<ast::TupleType>(ty)) {
    for (size_t i = 0; i < tt->getArity(); ++i) {
      if (!holdsRef(tt->getElementType(i)))
        continue;
      int64_t off = tupleElementOffset(tt, i);
      forEachMemRef(off == 0 ? addr
                             : B.ptrAdd(addr, Val::i64(off), "elem.addr"),
                    tt->getElementType(i), fn);
    }
    return;
  }
  const auto inner = innermostElements(ast::cast<ast::FixedArrayType>(ty));
  ast::Type *elemTy = inner.first;
  forEachSlot(addr, 0, inner.second, memLayout(elemTy).Size,
              [&](const Val &at) { forEachMemRef(at, elemTy, fn); });
}

void ModuleLowering::forEachSlot(const Val &base, int64_t from, int64_t to,
                                 int64_t stride,
                                 const std::function<void(Val)> &fn) {
  if (to - from <= kUnrolledSlots) {
    for (int64_t i = from; i < to; ++i)
      fn(i == 0 ? base : B.ptrAdd(base, Val::i64(i * stride), "elem.addr"));
    return;
  }
  pir::LocalId i = B.addLocal("i", Type::I64);
  B.store(i, Val::i64(from));
  pir::While *w = B.openWhile();
  B.enter(*w->CondBlock);
  B.setWhileCond(w, B.cmp(CmpPred::Lt, B.load(i, "i"), Val::i64(to), "more"));
  B.leave();
  B.enterLoopBody(*w->Body);
  Val iv = B.load(i, "i");
  Val off = B.binary(Opcode::Mul, iv, Val::i64(stride), "elem.off");
  fn(B.ptrAdd(base, off, "elem.addr"));
  B.store(i, B.binary(Opcode::Add, iv, Val::i64(1), "i.next"));
  B.leaveLoopBody();
}

void ModuleLowering::emitMemRetain(const Val &addr, ast::Type *ty) {
  forEachMemRef(addr, ty, [&](const Val &box) { emitRetain(box); });
}

void ModuleLowering::emitMemRelease(const Val &addr, ast::Type *ty) {
  forEachMemRef(addr, ty, [&](const Val &box) { emitRelease(box); });
}

ast::Expr *ModuleLowering::wholeOf(ast::Expr *e) {
  if (auto *se = ast::dyn_cast<ast::SubscriptExpr>(e)) {
    ast::Type *arrTy = se->getArray()->getResolvedType();
    return ast::isa<ast::FixedArrayType>(arrTy) ||
                   ast::isa<ast::SliceType>(arrTy)
               ? se->getArray()
               : nullptr;
  }
  if (auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(e))
    return ti->getTuple();
  if (auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e))
    return getExprStructType(ma->getReceiver()) ? ma->getReceiver() : nullptr;
  return nullptr;
}

Val ModuleLowering::emitPartAddress(ast::Expr *e, const Val &whole) {
  if (auto *se = ast::dyn_cast<ast::SubscriptExpr>(e))
    return elementAddressIn(se->getArray(), se->getIndex(), whole);
  if (auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(e))
    return tupleElementIn(ti->getTuple(), ti->getIndex(), whole);
  // A struct's field is in the struct's bytes, at its offset, like a
  // tuple's element.
  auto *ma = ast::cast<ast::MemberAccessExpr>(e);
  int idx =
      getExprStructType(ma->getReceiver())->fieldIndex(ma->getFieldName());
  assert(idx >= 0 && "Sema should have verified the field exists");
  return tupleElementIn(ma->getReceiver(), static_cast<size_t>(idx), whole);
}

Val ModuleLowering::emitFixedElementAddress(ast::Expr *array,
                                            ast::Expr *index) {
  Val base = emitMemAddress(array);
  if (!base)
    return Val();
  return elementAddressIn(array, index, base);
}

Val ModuleLowering::elementAddressIn(ast::Expr *array, ast::Expr *index,
                                     const Val &base) {
  auto *ft = ast::dyn_cast<ast::FixedArrayType>(array->getResolvedType());
  ast::Type *elemTy = ft ? ft->getElementType()
                         : ast::cast<ast::SliceType>(array->getResolvedType())
                               ->getElementType();
  int64_t elemSize = memLayout(elemTy).Size;
  // Sema checked a constant index into a fixed-size array.
  if (auto *lit = ast::dyn_cast<ast::IntegerLiteral>(index); lit && ft)
    return lit->getValue() == 0
               ? base
               : B.ptrAdd(base, Val::i64(lit->getValue() * elemSize),
                          "elem.addr");
  Val idx = emitExpr(index);
  if (!idx)
    return Val();
  Val len = ft ? Val::i64(ft->getLength()) : emitSliceLength(array);
  Val neg = B.cmp(CmpPred::Lt, idx, Val::i64(0), "idx.neg");
  Val over = B.cmp(CmpPred::Ge, idx, len, "idx.over");
  pir::If *s = B.openIf(B.select(neg, Val::boolean(true), over, "idx.bad"),
                        /*withElse=*/false);
  B.enter(*s->Then);
  callRuntime(names::kPaykanPanicIndex, {idx, len});
  B.emitUnreachable();
  B.leave();
  Val off = B.binary(Opcode::Mul, idx, Val::i64(elemSize), "elem.off");
  return B.ptrAdd(base, off, "elem.addr");
}

Val ModuleLowering::emitTupleElementAddress(ast::Expr *tuple, size_t index) {
  Val base = emitMemAddress(tuple);
  if (!base)
    return Val();
  return tupleElementIn(tuple, index, base);
}

Val ModuleLowering::tupleElementIn(ast::Expr *tuple, size_t index,
                                   const Val &base) {
  int64_t off = tupleElementOffset(
      ast::cast<ast::TupleType>(tuple->getResolvedType()), index);
  return off == 0 ? base : B.ptrAdd(base, Val::i64(off), "elem.addr");
}

Val ModuleLowering::emitElementLoad(const Val &addr, ast::Type *ty,
                                    bool stable) {
  const bool ref = ast::isRefType(ty);
  return B.ptrLoad(addr, ref ? Type::Box : toPIRType(ty), "elem",
                   ref && stable);
}

Val ModuleLowering::ownElementRead(ast::Expr *node, const Val &elem,
                                   size_t mark) {
  if (elem.Ty == Type::Box && exprProducesFreshBox(node))
    emitRetain(elem);
  releaseMemTemps(mark);
  return elem;
}

void ModuleLowering::emitMemInit(ast::Expr *e, const Val &dst) {
  if (isMemOptionalWrap(e) && e != UnwrappedExpr) {
    emitMemOptionalWrap(e, dst);
    return;
  }
  ast::Type *ty = e == UnwrappedExpr ? e->getResolvedType() : memTypeOf(e);
  // A tagged union's variant: its tag, then its fields, in place.
  if (auto *call = ast::dyn_cast<ast::CallExpr>(e);
      call && call->getVariantEnum()) {
    emitVariantInit(call, dst);
    return;
  }
  if (auto *ev = ast::dyn_cast<ast::EnumValueExpr>(e);
      ev && isVariantExpr(ev)) {
    B.memZero(dst, Val::i64(memLayout(ty).Size));
    B.ptrStore(dst, Val::i64(ev->getValue()));
    return;
  }
  if (auto *tl = ast::dyn_cast<ast::TupleLiteralExpr>(e)) {
    auto *tt = ast::cast<ast::TupleType>(ty);
    for (size_t i = 0; i < tl->getNumElements(); ++i) {
      int64_t off = tupleElementOffset(tt, i);
      emitElementInit(tl->getElements()[i], tt->getElementType(i),
                      off == 0 ? dst
                               : B.ptrAdd(dst, Val::i64(off), "elem.addr"));
    }
    return;
  }
  auto *lit = ast::dyn_cast<ast::ArrayLiteralExpr>(e);
  if (!lit) {
    // A temporary's references move here; any other value's are shared.
    size_t mark = MemTemps.size();
    MemValue src = emitMemValue(e);
    if (!src.Addr)
      return;
    B.memCopy(dst, src.Addr, Val::i64(memLayout(ty).Size));
    if (!src.Temp)
      emitMemRetain(dst, ty);
    releaseMemTemps(mark);
    return;
  }
  auto *ft = ast::cast<ast::FixedArrayType>(ty);
  ast::Type *elemTy = ft->getElementType();
  int64_t elemSize = memLayout(elemTy).Size;
  if (lit->isRepeat()) {
    emitFixedRepeat(lit->getElements().front(), ft, dst);
    return;
  }
  for (size_t i = 0; i < lit->getNumElements(); ++i)
    emitElementInit(
        lit->getElements()[i], elemTy,
        i == 0 ? dst
               : B.ptrAdd(dst, Val::i64(static_cast<int64_t>(i) * elemSize),
                          "elem.addr"));
}

void ModuleLowering::emitElementInit(ast::Expr *e, ast::Type *ty,
                                     const Val &dst) {
  if (ast::isMemType(ty)) {
    emitMemInit(e, dst);
    return;
  }
  if (ast::isRefType(ty)) { // the slot owns a reference
    if (Val box = emitAsShared(e))
      B.ptrStore(dst, box);
    return;
  }
  if (Val v = emitExpr(e))
    B.ptrStore(dst, coerceTo(v, toPIRType(ty)));
}

void ModuleLowering::emitFixedRepeat(ast::Expr *elem, ast::FixedArrayType *ft,
                                     const Val &dst) {
  ast::Type *elemTy = ft->getElementType();
  const int64_t elemSize = memLayout(elemTy).Size;
  if (ast::isa<ast::BuiltinType>(elemTy) && isZeroLiteral(elem)) {
    B.memZero(dst, Val::i64(memLayout(ft).Size));
    return;
  }
  // The value is evaluated once, into the first slot; the others copy it.
  const bool inMem = ast::isMemType(elemTy);
  const bool ref = ast::isRefType(elemTy);
  Val v;
  if (inMem) {
    emitMemInit(elem, dst);
  } else {
    v = ref ? emitAsShared(elem) : emitExpr(elem);
    if (!v)
      return;
    if (!ref)
      v = coerceTo(v, toPIRType(elemTy));
    B.ptrStore(dst, v);
  }
  forEachSlot(dst, 1, ft->getLength(), elemSize, [&](const Val &at) {
    if (inMem) {
      B.memCopy(at, dst, Val::i64(elemSize));
      emitMemRetain(at, elemTy);
      return;
    }
    B.ptrStore(at, v);
    if (ref) // each slot owns a reference
      emitRetain(v);
  });
}

pir::LocalId ModuleLowering::declareMemVar(const std::string &name,
                                           ast::Type *ty, ast::Expr *init) {
  MemLayout l = memLayout(ty);
  pir::LocalId local = B.addMemLocal(name, l.Size, l.Align);
  Val addr = B.localAddr(local, name + ".addr");
  if (init)
    emitMemInit(init, addr);
  else
    B.memZero(addr, Val::i64(l.Size));
  CurrentScope->declare(name, local, ty);
  return local;
}

void ModuleLowering::declareMemParam(const std::string &name,
                                     const pir::Value &arg, ast::Type *ty,
                                     ast::ParamMode mode) {
  Val addr(pir::Operand::value(arg), Type::Ptr);
  if (mode == ast::ParamMode::Value) {
    // The callee's own copy: what it changes, the caller's value keeps.
    MemLayout l = memLayout(ty);
    pir::LocalId local = B.addMemLocal(name, l.Size, l.Align);
    Val own = B.localAddr(local, name + ".addr");
    B.memCopy(own, addr, Val::i64(l.Size));
    emitMemRetain(own, ty);
    CurrentScope->declare(name, local, ty);
    return;
  }
  pir::LocalId local = B.addLocal(name, Type::Ptr);
  if (mode == ast::ParamMode::View)
    B.markView(local);
  B.store(local, addr);
  CurrentScope->declareInout(name, local, Type::Mem, ty);
  CurrentScope->markStable(name);
}

bool ModuleLowering::isSliceArg(const ast::Expr *arg) {
  return ast::dyn_cast<ast::SliceType>(arg->getCoercedType()) ||
         ast::dyn_cast<ast::SliceType>(arg->getResolvedType());
}

bool ModuleLowering::emitSliceArg(ast::Expr *arg, std::vector<Val> &args) {
  // A fixed-size array's bytes (a temporary is released after the call),
  // or the elements of the slice passed on.
  Val addr = emitMemAddress(arg);
  if (!addr)
    return false;
  args.push_back(addr);
  if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(arg->getResolvedType()))
    args.push_back(Val::i64(ft->getLength()));
  else
    args.push_back(emitSliceLength(arg));
  return true;
}

Val ModuleLowering::emitSliceLength(ast::Expr *e) {
  const std::string &name = ast::cast<ast::Identifier>(e)->getName();
  return B.load(CurrentScope->lookup(name + ".len"), name + ".len");
}

Val ModuleLowering::emitMemResultSlot(ast::Type *ty) {
  MemLayout l = memLayout(ty);
  pir::LocalId tmp = B.addMemLocal("res", l.Size, l.Align);
  return B.localAddr(tmp, "res.addr");
}

void ModuleLowering::emitMemAssign(const Val &dst, ast::Expr *value) {
  // A literal is built in a temporary first: its elements may read the
  // value it replaces (`xs = [xs[1], xs[0]];`, `t = (t.1, t.0);`).
  ast::Type *ty = memTypeOf(value);
  size_t mark = MemTemps.size();
  MemValue src = emitMemValue(value);
  if (!src.Addr)
    return;
  // The new references are taken before the old ones go (`xs = xs;`).
  if (!src.Temp)
    emitMemRetain(src.Addr, ty);
  emitMemRelease(dst, ty);
  B.memCopy(dst, src.Addr, Val::i64(memLayout(ty).Size));
  releaseMemTemps(mark);
}

// -- Tuples and structs: their layouts, text and equality

namespace {

/// Append @p name as a layout writes it (Runtime.h, "Structs"): its length,
/// then its bytes packed eight to a word, the first byte in the lowest eight
/// bits.
void appendName(std::vector<int64_t> &words, const std::string &name) {
  words.push_back(static_cast<int64_t>(name.size()));
  for (size_t i = 0; i < name.size(); i += 8) {
    uint64_t word = 0;
    for (size_t k = 0; k < 8 && i + k < name.size(); ++k)
      word |= static_cast<uint64_t>(static_cast<unsigned char>(name[i + k]))
              << (8 * k);
    words.push_back(static_cast<int64_t>(word));
  }
}

/// The kind of a layout entry for the tuple or struct @p t.
int64_t aggregateKind(const ast::TupleType *t) {
  return ast::isa<ast::StructType>(t) ? int64_t{names::kNativeKindStruct}
                                      : int64_t{names::kNativeKindTuple};
}

} // namespace

void ModuleLowering::appendTupleLayout(std::vector<int64_t> &words,
                                       ast::TupleType *t) {
  auto *st = ast::dyn_cast<ast::StructType>(t);
  words.push_back(static_cast<int64_t>(t->getArity()));
  // A variant's fields are shown under the variant's name, `Move(x: 1)`.
  if (st && st->getVariantEnum())
    appendName(words,
               *st->getVariantEnum()->getVariants()[st->getVariantIndex()]);
  else if (st)
    appendName(words, st->getName());
  for (size_t i = 0; i < t->getArity(); ++i) {
    ast::Type *e = t->getElementType(i);
    const int64_t off = tupleElementOffset(t, i);
    if (st)
      appendName(words, st->getFieldName(i));
    if (auto *et = ast::dyn_cast<ast::EnumType>(e)) {
      words.push_back(names::kNativeKindEnum);
      words.push_back(off);
      appendEnumLayout(words, et);
      continue;
    }
    // An optional enum: the presence flag at the entry's offset, then a
    // whole entry for the value (Runtime.h, "Tuples"), as an optional
    // struct's is; an enum without fields is the payload of an inline
    // optional (8 bytes in).
    if (auto *et = ast::isa<ast::OptionalType>(e)
                       ? ast::dyn_cast<ast::EnumType>(ast::stripOptional(e))
                       : nullptr) {
      words.push_back(names::kNativeKindOptional);
      words.push_back(off);
      words.push_back(names::kNativeKindEnum);
      words.push_back(names::kNativeKindEnum);
      words.push_back(off + (ast::isPayloadEnum(et)
                                 ? memOptionalPayloadOffset(e)
                                 : int64_t{8}));
      appendEnumLayout(words, et);
      continue;
    }
    ast::TupleType *nested = ast::isFunctionValue(e)
                                 ? functionValueShape()
                                 : ast::dyn_cast<ast::TupleType>(e);
    words.push_back(nested ? aggregateKind(nested)
                           : int64_t{nativeTypeKind(e)});
    words.push_back(off);
    if (nested) {
      appendTupleLayout(words, nested);
    } else if (ast::isMemOptional(e)) {
      // An optional tuple or struct: the presence flag at the entry's
      // offset, then a whole entry for the value (Runtime.h, "Tuples").
      auto *value = ast::cast<ast::TupleType>(ast::stripOptional(e));
      words.push_back(aggregateKind(value));
      words.push_back(aggregateKind(value));
      words.push_back(off + memOptionalPayloadOffset(e));
      appendTupleLayout(words, value);
    } else if (ast::isa<ast::OptionalType>(e))
      words.push_back(ast::isValueOptional(e)
                          ? int64_t{nativeTypeKind(ast::stripOptional(e))}
                          : int64_t{names::kNativeKindObject});
  }
}

void ModuleLowering::appendEnumLayout(std::vector<int64_t> &words,
                                      ast::EnumType *et) {
  const auto &variants = et->getVariants();
  words.push_back(static_cast<int64_t>(variants.size()));
  for (size_t i = 0; i < variants.size(); ++i) {
    appendName(words, *variants[i]);
    ast::StructType *payload = et->getPayload(i);
    words.push_back(payload ? 1 : 0);
    if (!payload)
      continue;
    words.push_back(variantPayloadOffset(et, i));
    appendTupleLayout(words, payload);
  }
}

Val ModuleLowering::internLayout(
    std::unordered_map<std::string, std::string> &interned, const char *prefix,
    std::vector<int64_t> words) {
  std::string key;
  for (int64_t w : words) {
    key += std::to_string(w);
    key += '|';
  }
  auto [it, added] = interned.try_emplace(key);
  if (added) {
    it->second = prefix + std::to_string(interned.size() - 1);
    Mod.Datas.push_back({it->second, std::move(words)});
  }
  return Val::symbol(it->second, Type::Ptr);
}

Val ModuleLowering::emitTupleLayout(ast::TupleType *tt) {
  std::vector<int64_t> words;
  appendTupleLayout(words, tt);
  // A struct's and a tuple's are named apart, as they were.
  return internLayout(InternedTupleLayouts,
                      ast::isa<ast::StructType>(tt)
                          ? names::kStructLayoutGlobalName
                          : names::kTupleLayoutGlobalName,
                      std::move(words));
}

Val ModuleLowering::emitTupleToString(ast::Expr *e) {
  size_t mark = MemTemps.size();
  Val addr = emitMemAddress(e);
  if (!addr)
    return Val();
  auto *tt = ast::cast<ast::TupleType>(e->getResolvedType());
  Val str =
      callRuntime(ast::isa<ast::StructType>(tt) ? names::kPaykanStructToString
                                                : names::kPaykanTupleToString,
                  {addr, emitTupleLayout(tt)}, "tup.str");
  releaseMemTemps(mark);
  trackStringTemp(str);
  return str;
}

Val ModuleLowering::emitTupleEquals(ast::Expr *a, ast::Expr *b) {
  size_t mark = MemTemps.size();
  Val pa = emitMemAddress(a);
  Val pb = pa ? emitMemAddress(b) : Val();
  if (!pb)
    return Val();
  // A function value compares as its shape: the same code and closure.
  auto *tt = ast::cast<ast::TupleType>(memShapeOf(a->getResolvedType()));
  Val eq =
      callRuntime(ast::isa<ast::StructType>(tt) ? names::kPaykanStructEquals
                                                : names::kPaykanTupleEquals,
                  {pa, pb, emitTupleLayout(tt)}, "tup.eq");
  releaseMemTemps(mark);
  return B.cmp(CmpPred::Ne, eq, Val::i64(0), "eq");
}

} // namespace paykan::lowering
