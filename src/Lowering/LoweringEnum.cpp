// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// AST -> PIR lowering: enums (docs/language/07-enums.md).
//
// An enum whose variants have no fields is its variant's index, an i64.  A
// tagged union (`enum E { A, B(x: int, s: Str) }`) is a value held in memory
// (LoweringMem.cpp), laid out as the tuple enumCarrier() gives: its tag, then
// each variant's fields, a struct, one after another.  A value has its
// variant's fields set and every other variant's all zeros, so a copy's
// retains and a release's releases are a tuple's (a null reference is
// skipped), and a container's type descriptor marks each reference in it
// nullable; all zeros is the first variant with zero fields, so the
// descriptor tells storage never set by the tag and that variant's words
// when those are no value (emitTypeDescriptor).  What is the enum's own:
//
//   * a variant is made in place (`E::B(1, s)`): zeros, the tag, then each
//     field initialised as a struct's is;
//   * a match switches on the tag; an arm's binding (`b: B { ... }`) is a
//     copy of the variant's fields, which with `inout` goes back to the
//     subject as variant B;
//   * its text and equality are the runtime's, through a layout of every
//     variant's name and fields (Runtime.h, "Enums").  An enum without
//     fields is written to a local first, so both kinds share them.

#include "LoweringInternal.h"
#include "Names.h"

#include <cassert>

namespace paykan::lowering {

using namespace names;
using pir::CmpPred;
using pir::Type;

namespace {

/// The variant the enum match arm @p arm names (its type-name stub).
std::string armVariant(const ast::MatchArm *arm) {
  if (auto *stub = ast::dyn_cast<ast::ClassType>(arm->getArmType()))
    return stub->getName();
  if (auto *et = ast::dyn_cast<ast::EnumType>(arm->getArmType()))
    return et->getName();
  return "";
}

} // namespace

// static
bool ModuleLowering::isVariantExpr(const ast::Expr *expr) {
  if (const auto *call = ast::dyn_cast<ast::CallExpr>(expr))
    return call->getVariantEnum() != nullptr;
  const auto *ev = ast::dyn_cast<ast::EnumValueExpr>(expr);
  return ev && !ev->getFunctionRef() &&
         ast::isPayloadEnum(ev->getResolvedType());
}

ast::TupleType *ModuleLowering::enumCarrier(ast::EnumType *et) {
  auto [it, added] = EnumCarriers.try_emplace(et, nullptr);
  if (added) {
    std::vector<ast::Type *> elems{ASTCtx.getIntTy()};
    for (size_t i = 0; i < et->getNumVariants(); ++i)
      if (ast::StructType *payload = et->getPayload(i))
        elems.push_back(payload);
    it->second = ASTCtx.getTupleType(std::move(elems));
  }
  return it->second;
}

int64_t ModuleLowering::variantPayloadOffset(ast::EnumType *et, size_t i) {
  size_t element = 1; // after the tag
  for (size_t k = 0; k < i; ++k)
    if (et->getPayload(k))
      ++element;
  return tupleElementOffset(enumCarrier(et), element);
}

void ModuleLowering::emitVariantInit(ast::CallExpr *call, const Val &dst) {
  ast::EnumType *et = call->getVariantEnum();
  const size_t idx = call->getVariantIndex();
  ast::StructType *payload = et->getPayload(idx);
  assert(payload && "Sema makes a variant without fields an EnumValueExpr");
  B.memZero(dst, Val::i64(memLayout(et).Size));
  B.ptrStore(dst, Val::i64(static_cast<int64_t>(idx)));
  Val fields =
      B.ptrAdd(dst, Val::i64(variantPayloadOffset(et, idx)), "variant.addr");
  for (size_t i = 0; i < call->getNumArguments(); ++i) {
    int64_t off = tupleElementOffset(payload, i);
    emitElementInit(call->getArguments()[i], payload->getElementType(i),
                    off == 0 ? fields
                             : B.ptrAdd(fields, Val::i64(off), "field.addr"));
  }
}

Val ModuleLowering::emitEnumLayout(ast::EnumType *et) {
  std::vector<int64_t> words;
  appendEnumLayout(words, et);
  return internLayout(InternedEnumLayouts, kEnumLayoutGlobalName,
                      std::move(words));
}

Val ModuleLowering::emitEnumToStringAt(const Val &addr, ast::EnumType *et) {
  return callRuntime(kPaykanEnumToString, {addr, emitEnumLayout(et)},
                     "enum.str");
}

Val ModuleLowering::emitEnumToString(ast::Expr *e) {
  auto *et = ast::cast<ast::EnumType>(e->getResolvedType());
  size_t mark = MemTemps.size();
  Val addr;
  if (ast::isPayloadEnum(et)) {
    addr = emitMemAddress(e);
  } else if (Val v = emitExpr(e)) {
    pir::LocalId tag = B.addLocal("enum", Type::I64);
    B.store(tag, v);
    addr = B.localAddr(tag, "enum.addr");
  }
  if (!addr)
    return Val();
  Val str = emitEnumToStringAt(addr, et);
  releaseMemTemps(mark);
  trackStringTemp(str);
  return str;
}

Val ModuleLowering::emitEnumEquals(ast::Expr *a, ast::Expr *b) {
  size_t mark = MemTemps.size();
  Val pa = emitMemAddress(a);
  Val pb = pa ? emitMemAddress(b) : Val();
  if (!pb)
    return Val();
  auto *et = ast::cast<ast::EnumType>(a->getResolvedType());
  Val eq =
      callRuntime(kPaykanEnumEquals, {pa, pb, emitEnumLayout(et)}, "enum.eq");
  releaseMemTemps(mark);
  return B.cmp(CmpPred::Ne, eq, Val::i64(0), "eq");
}

Val ModuleLowering::emitPayloadEnumMatch(ast::MatchStmt *node) {
  auto *et = ast::cast<ast::EnumType>(node->getSubject()->getResolvedType());
  // The subject, copied into a slot of the match's scope: its references are
  // held until the match ends, on every way out of it.  With a `b: inout B`
  // arm it is read from its storage, where b goes back (emitMatchPlace).
  ScopeGuard matchGuard(*this);
  MemLayout l = memLayout(et);
  pir::LocalId subj = B.addMemLocal("match.subj", l.Size, l.Align);
  Val addr = B.localAddr(subj, "match.subj.addr");
  std::optional<pir::LocalId> place;
  if (!emitMemMatchSubject(node, addr, place))
    return Val();
  CurrentScope->declare("match.subj", subj, et);

  pir::Switch *s = B.openSwitch(B.ptrLoad(addr, Type::I64, "enum.tag"));
  for (ast::MatchArm *arm : node->getArms()) {
    if (arm->isWildcard())
      continue;
    const int64_t idx = et->findVariant(armVariant(arm));
    assert(idx >= 0 && "Sema should have verified the variant exists");
    B.enter(B.addCase(s, Val::i64(idx)));
    if (!arm->hasBinding()) {
      emitMatchArmBody(arm);
      B.leave();
      continue;
    }
    {
      ScopeGuard armGuard(*this);
      const std::string &name = arm->getBinding();
      ast::StructType *payload = et->getPayload(static_cast<size_t>(idx));
      const int64_t off = variantPayloadOffset(et, static_cast<size_t>(idx));
      // The binding's own copy of the variant's fields; `b: inout B` writes
      // it back as variant B after its last use (bindInoutArm).
      MemLayout pl = memLayout(payload);
      pir::LocalId local = B.addMemLocal(name, pl.Size, pl.Align);
      if (arm->getMode() == ast::ParamMode::View)
        B.markView(local);
      Val own = B.localAddr(local, name + ".addr");
      B.memCopy(own, B.ptrAdd(addr, Val::i64(off), "variant.addr"),
                Val::i64(pl.Size));
      emitMemRetain(own, payload);
      CurrentScope->declare(name, local, payload);
      if (arm->getMode() == ast::ParamMode::Inout && place)
        bindInoutArm(arm, local, *place, et, payload, off, Val::i64(idx));
      emitArmBody(arm);
    }
    B.leave();
  }
  if (ast::MatchArm *wildcard = findWildcardArm(node)) {
    B.enter(B.addDefault(s));
    emitMatchArmBody(wildcard);
    B.leave();
  }
  return Val();
}

} // namespace paykan::lowering
