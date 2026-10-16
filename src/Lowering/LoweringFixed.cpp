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
// Initialising or assigning a whole array copies its bytes (mem.copy).  The
// elements are scalars (int, float, bool, char, an enum) or other such
// arrays, so no element is counted and nothing is released.

#include "LoweringInternal.h"
#include "Names.h"

namespace paykan::lowering {

using pir::CmpPred;
using pir::Opcode;
using pir::Type;

ModuleLowering::FixedLayout ModuleLowering::fixedLayout(ast::Type *ty) {
  if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty)) {
    FixedLayout elem = fixedLayout(ft->getElementType());
    return {elem.Size * ft->getLength(), elem.Align};
  }
  Type t = toPIRType(ty);
  return {pir::storeSize(t), pir::storeAlign(t)};
}

Val ModuleLowering::emitFixedAddress(ast::Expr *e) {
  if (auto *id = ast::dyn_cast<ast::Identifier>(e)) {
    pir::LocalId local = CurrentScope->lookup(id->getName());
    if (CurrentScope->inoutType(id->getName()) != Type::Void)
      return B.load(local, id->getName() + ".addr");
    return B.localAddr(local, id->getName() + ".addr");
  }
  if (auto *se = ast::dyn_cast<ast::SubscriptExpr>(e))
    return emitFixedElementAddress(se->getArray(), se->getIndex());
  if (auto *lit = ast::dyn_cast<ast::ArrayLiteralExpr>(e)) {
    FixedLayout l = fixedLayout(lit->getResolvedType());
    pir::LocalId tmp = B.addMemLocal("lit", l.Size, l.Align);
    Val addr = B.localAddr(tmp, "lit.addr");
    emitFixedInit(lit, addr);
    return addr;
  }
  reportInternalError("a fixed-size array value that is not a variable, an "
                      "element or a literal");
  return Val();
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
                        /*hasElse=*/false);
  B.enter(*s->Then);
  callRuntime(names::kPaykanPanicIndex, {idx, Val::i64(ft->getLength())});
  B.emitUnreachable();
  B.leave();
  Val off = B.binary(Opcode::Mul, idx, Val::i64(elemSize), "elem.off");
  return B.ptrAdd(base, off, "elem.addr");
}

void ModuleLowering::emitFixedInit(ast::Expr *e, const Val &dst) {
  auto *lit = ast::dyn_cast<ast::ArrayLiteralExpr>(e);
  if (!lit) {
    Val src = emitFixedAddress(e);
    if (src)
      B.memCopy(dst, src, Val::i64(fixedLayout(e->getResolvedType()).Size));
    return;
  }
  ast::Type *elemTy =
      ast::cast<ast::FixedArrayType>(lit->getResolvedType())->getElementType();
  int64_t elemSize = fixedLayout(elemTy).Size;
  for (size_t i = 0; i < lit->getNumElements(); ++i) {
    Val at = i == 0 ? dst
                    : B.ptrAdd(dst, Val::i64(static_cast<int64_t>(i) * elemSize),
                               "elem.addr");
    ast::Expr *elem = lit->getElements()[i];
    if (ast::isa<ast::FixedArrayType>(elemTy)) {
      emitFixedInit(elem, at);
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

void ModuleLowering::emitFixedAssign(const Val &dst, ast::Expr *value) {
  // A literal is built in a temporary first: its elements may read the
  // array it replaces (`xs = [xs[1], xs[0]];`).
  Val src = emitFixedAddress(value);
  if (src)
    B.memCopy(dst, src, Val::i64(fixedLayout(value->getResolvedType()).Size));
}

} // namespace paykan::lowering
