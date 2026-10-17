// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// AST -> PIR lowering: fixed-size arrays (`T[n]`,
// docs/language/05-arrays.md, "Fixed-size arrays").
//
// A fixed-size array is a value held in memory, never in an SSA value: a
// variable is a `mem` local of its bytes (an `inout` one a `ptr` local
// holding the address of the array it names), and an expression of a
// fixed-size array type is lowered to the address of its bytes.  Its elements
// are read and written in place, at the array's address plus the index times
// the element's size, after a bounds check that panics
// (Paykan_panic_index); a nested array's element is an address again.
// Initialising or assigning a whole array copies its bytes (mem.copy).
//
// An element of a reference type is its box, and its slot owns a reference
// to it, as a variable does: a copy retains every element, an assignment
// releases the elements it replaces, and a variable's are released when its
// scope ends.  A temporary (a call's result, a literal) owns its elements:
// the place it initialises takes them over, and any other use releases them
// once it is done (FixedTemps).  A field is in its object's bytes
// (field.addr); the object of a call-rooted one is released the same way.

#include "LoweringInternal.h"
#include "Names.h"

namespace paykan::lowering {

using pir::CmpPred;
using pir::Opcode;
using pir::Type;

namespace {

/// The references an array of references holds: its elements' at every
/// depth, one after another.
int64_t fixedRefCount(ast::FixedArrayType *ft) {
  int64_t n = ft->getLength();
  while (auto *inner =
             ast::dyn_cast<ast::FixedArrayType>(ft->getElementType())) {
    ft = inner;
    n *= ft->getLength();
  }
  return n;
}

/// Up to this many references are retained or released one by one; more in
/// a loop.
constexpr int64_t kUnrolledRefs = 4;

} // namespace

bool ModuleLowering::fixedHoldsRefs(const ast::Type *ty) {
  const auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty);
  if (!ft)
    return false;
  while (const auto *inner =
             ast::dyn_cast<ast::FixedArrayType>(ft->getElementType()))
    ft = inner;
  return ast::isRefType(ft->getElementType());
}

ModuleLowering::FixedLayout ModuleLowering::fixedLayout(ast::Type *ty) {
  if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty)) {
    FixedLayout elem = fixedLayout(ft->getElementType());
    return {elem.Size * ft->getLength(), elem.Align};
  }
  Type t = toPIRType(ty);
  return {pir::storeSize(t), pir::storeAlign(t)};
}

ModuleLowering::FixedValue ModuleLowering::emitFixedValue(ast::Expr *e) {
  if (auto *id = ast::dyn_cast<ast::Identifier>(e)) {
    pir::LocalId local = CurrentScope->lookup(id->getName());
    if (CurrentScope->inoutType(id->getName()) != Type::Void)
      return {B.load(local, id->getName() + ".addr")};
    return {B.localAddr(local, id->getName() + ".addr")};
  }
  if (auto *se = ast::dyn_cast<ast::SubscriptExpr>(e))
    return {emitFixedElementAddress(se->getArray(), se->getIndex())};
  // A field, in its object: a call-rooted one (`mk().xs`) is kept until the
  // consumer is done with the address.
  if (auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e)) {
    Receiver recv = emitReceiver(ma->getReceiver(), "obj");
    ast::ClassType *ct = getExprClassType(ma->getReceiver());
    if (!recv || !ct)
      return {};
    getOrCreateClass(ct);
    if (recv.Owned.isOwned())
      FixedTemps.emplace_back(recv.Owned.V, nullptr);
    return {B.fieldAddr(recv.Raw, ct->getName(), ma->getFieldName(),
                        ma->getFieldName() + ".addr")};
  }
  // A call's result, in the temporary the call wrote.
  if (ast::isa<ast::CallExpr>(e) || ast::isa<ast::MethodCallExpr>(e))
    return {emitExpr(e), /*Temp=*/true};
  if (auto *lit = ast::dyn_cast<ast::ArrayLiteralExpr>(e)) {
    FixedLayout l = fixedLayout(lit->getResolvedType());
    pir::LocalId tmp = B.addMemLocal("lit", l.Size, l.Align);
    Val addr = B.localAddr(tmp, "lit.addr");
    emitFixedInit(lit, addr);
    return {addr, /*Temp=*/true};
  }
  reportInternalError("a fixed-size array value that is not a variable, a "
                      "field, an element, a literal or a call");
  return {};
}

Val ModuleLowering::emitFixedAddress(ast::Expr *e) {
  FixedValue v = emitFixedValue(e);
  if (v.Temp && v.Addr && fixedHoldsRefs(e->getResolvedType()))
    FixedTemps.emplace_back(
        v.Addr, ast::cast<ast::FixedArrayType>(e->getResolvedType()));
  return v.Addr;
}

void ModuleLowering::releaseFixedTemps(size_t mark) {
  while (FixedTemps.size() > mark) {
    auto [v, ty] = FixedTemps.back();
    FixedTemps.pop_back();
    if (ty)
      emitFixedRelease(v, ty);
    else
      emitRelease(v);
  }
}

void ModuleLowering::forEachFixedRef(const Val &addr, ast::FixedArrayType *ty,
                                     const std::function<void(Val)> &fn) {
  if (!addr || !fixedHoldsRefs(ty))
    return;
  const int64_t n = fixedRefCount(ty);
  const int64_t size = pir::storeSize(Type::Box);
  if (n <= kUnrolledRefs) {
    for (int64_t i = 0; i < n; ++i)
      fn(B.ptrLoad(i == 0 ? addr
                          : B.ptrAdd(addr, Val::i64(i * size), "elem.addr"),
                   Type::Box, "elem"));
    return;
  }
  pir::LocalId i = B.addLocal("i", Type::I64);
  B.store(i, Val::i64(0));
  pir::While *w = B.openWhile();
  B.enter(*w->CondBlock);
  B.setWhileCond(w, B.cmp(CmpPred::Lt, B.load(i, "i"), Val::i64(n), "more"));
  B.leave();
  B.enterLoopBody(*w->Body);
  Val iv = B.load(i, "i");
  Val off = B.binary(Opcode::Mul, iv, Val::i64(size), "elem.off");
  fn(B.ptrLoad(B.ptrAdd(addr, off, "elem.addr"), Type::Box, "elem"));
  B.store(i, B.binary(Opcode::Add, iv, Val::i64(1), "i.next"));
  B.leaveLoopBody();
}

void ModuleLowering::emitFixedRetain(const Val &addr, ast::FixedArrayType *ty) {
  forEachFixedRef(addr, ty, [&](const Val &box) { emitRetain(box); });
}

void ModuleLowering::emitFixedRelease(const Val &addr,
                                      ast::FixedArrayType *ty) {
  forEachFixedRef(addr, ty, [&](const Val &box) { emitRelease(box); });
}

Val ModuleLowering::emitFixedElementAddress(ast::Expr *array,
                                            ast::Expr *index) {
  auto *ft = ast::cast<ast::FixedArrayType>(array->getResolvedType());
  Val base = emitFixedAddress(array);
  if (!base)
    return Val();
  int64_t elemSize = fixedLayout(ft->getElementType()).Size;
  // Sema checked a constant index.
  if (auto *lit = ast::dyn_cast<ast::IntegerLiteral>(index))
    return lit->getValue() == 0
               ? base
               : B.ptrAdd(base, Val::i64(lit->getValue() * elemSize),
                          "elem.addr");
  Val idx = emitExpr(index);
  if (!idx)
    return Val();
  Val neg = B.cmp(CmpPred::Lt, idx, Val::i64(0), "idx.neg");
  Val over = B.cmp(CmpPred::Ge, idx, Val::i64(ft->getLength()), "idx.over");
  pir::If *s = B.openIf(B.select(neg, Val::boolean(true), over, "idx.bad"),
                        /*withElse=*/false);
  B.enter(*s->Then);
  callRuntime(names::kPaykanPanicIndex, {idx, Val::i64(ft->getLength())});
  B.emitUnreachable();
  B.leave();
  Val off = B.binary(Opcode::Mul, idx, Val::i64(elemSize), "elem.off");
  return B.ptrAdd(base, off, "elem.addr");
}

void ModuleLowering::emitFixedInit(ast::Expr *e, const Val &dst) {
  auto *ft = ast::cast<ast::FixedArrayType>(e->getResolvedType());
  auto *lit = ast::dyn_cast<ast::ArrayLiteralExpr>(e);
  if (!lit) {
    // A temporary's references move here; any other array's are shared.
    size_t mark = FixedTemps.size();
    FixedValue src = emitFixedValue(e);
    if (!src.Addr)
      return;
    B.memCopy(dst, src.Addr, Val::i64(fixedLayout(ft).Size));
    if (!src.Temp)
      emitFixedRetain(dst, ft);
    releaseFixedTemps(mark);
    return;
  }
  ast::Type *elemTy = ft->getElementType();
  int64_t elemSize = fixedLayout(elemTy).Size;
  for (size_t i = 0; i < lit->getNumElements(); ++i) {
    Val at = i == 0
                 ? dst
                 : B.ptrAdd(dst, Val::i64(static_cast<int64_t>(i) * elemSize),
                            "elem.addr");
    ast::Expr *elem = lit->getElements()[i];
    if (ast::isa<ast::FixedArrayType>(elemTy)) {
      emitFixedInit(elem, at);
      continue;
    }
    if (ast::isRefType(elemTy)) { // the slot owns a reference
      Val box = emitAsShared(elem);
      if (!box)
        return;
      B.ptrStore(at, box);
      continue;
    }
    Val v = emitExpr(elem);
    if (!v)
      return;
    B.ptrStore(at, coerceTo(v, toPIRType(elemTy)));
  }
}

void ModuleLowering::declareFixedVar(const std::string &name,
                                     ast::FixedArrayType *ty, ast::Expr *init) {
  FixedLayout l = fixedLayout(ty);
  pir::LocalId local = B.addMemLocal(name, l.Size, l.Align);
  Val addr = B.localAddr(local, name + ".addr");
  if (init)
    emitFixedInit(init, addr);
  else
    B.memZero(addr, Val::i64(l.Size));
  CurrentScope->declare(name, local, ty);
}

void ModuleLowering::declareFixedParam(const std::string &name,
                                       const pir::Value &arg,
                                       ast::FixedArrayType *ty,
                                       ast::ParamMode mode) {
  Val addr(pir::Operand::value(arg), Type::Ptr);
  if (mode == ast::ParamMode::Value) {
    // The callee's own copy: what it changes, the caller's array keeps.
    FixedLayout l = fixedLayout(ty);
    pir::LocalId local = B.addMemLocal(name, l.Size, l.Align);
    Val own = B.localAddr(local, name + ".addr");
    B.memCopy(own, addr, Val::i64(l.Size));
    emitFixedRetain(own, ty);
    CurrentScope->declare(name, local, ty);
    return;
  }
  pir::LocalId local = B.addLocal(name, Type::Ptr);
  B.store(local, addr);
  CurrentScope->declareInout(name, local, Type::Mem, ty);
}

Val ModuleLowering::emitFixedResultSlot(ast::Type *ty) {
  FixedLayout l = fixedLayout(ty);
  pir::LocalId tmp = B.addMemLocal("res", l.Size, l.Align);
  return B.localAddr(tmp, "res.addr");
}

void ModuleLowering::emitFixedAssign(const Val &dst, ast::Expr *value) {
  // A literal is built in a temporary first: its elements may read the
  // array it replaces (`xs = [xs[1], xs[0]];`).
  auto *ft = ast::cast<ast::FixedArrayType>(value->getResolvedType());
  size_t mark = FixedTemps.size();
  FixedValue src = emitFixedValue(value);
  if (!src.Addr)
    return;
  // The new references are taken before the old ones go (`xs = xs;`).
  if (!src.Temp)
    emitFixedRetain(src.Addr, ft);
  emitFixedRelease(dst, ft);
  B.memCopy(dst, src.Addr, Val::i64(fixedLayout(ft).Size));
  releaseFixedTemps(mark);
}

} // namespace paykan::lowering
