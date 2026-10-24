// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// AST -> PIR lowering: `match` in its three modes (class / optional, value,
// enum).  A match on an enum or on integer, char or bool literals is a
// `switch`; one on classes, optionals, strings or floats an if-else chain.

#include "LoweringInternal.h"
#include "Names.h"

#include <cassert>

namespace paykan::lowering {

using namespace names;
using pir::CmpPred;
using pir::Type;

ast::MatchArm *ModuleLowering::findWildcardArm(ast::MatchStmt *node) {
  for (ast::MatchArm *arm : node->getArms())
    if (arm->isWildcard())
      return arm;
  return nullptr;
}

void ModuleLowering::emitMatchArmBody(ast::MatchArm *arm) {
  ScopeGuard armGuard(*this);
  emitBody(arm->getBody());
}

// if (check(0)) body(0) else if (check(1)) body(1) ... else default().
// Each check is evaluated only when every earlier one failed, exactly like
// the former block chain.
void ModuleLowering::emitMatchChain(size_t n,
                                    const std::function<Val(size_t)> &emitCheck,
                                    const std::function<void(size_t)> &emitBody,
                                    const std::function<void()> &emitDefault) {
  std::vector<pir::Block *> opened;
  for (size_t i = 0; i < n; ++i) {
    Val isMatch = emitCheck(i);
    pir::If *s = B.openIf(isMatch, /*withElse=*/true);
    B.enter(*s->Then);
    emitBody(i);
    B.leave();
    B.enter(*s->Else);
    opened.push_back(s->Else.get());
  }
  emitDefault();
  for (size_t i = 0; i < opened.size(); ++i)
    B.leave();
}

// switch (subj) { case c(0) { body(0) } ... default { the wildcard's } }.
// Of two arms naming one value the first is taken, as in a chain, so a
// later one is dropped.
void ModuleLowering::emitMatchSwitch(
    const Val &subj, const std::vector<std::pair<Val, ast::MatchArm *>> &arms,
    ast::MatchArm *wildcard) {
  pir::Switch *s = B.openSwitch(subj);
  std::vector<const pir::Operand *> seen;
  for (const auto &[value, arm] : arms) {
    bool again = false;
    for (const pir::Operand *o : seen)
      again |= pir::sameConstant(*o, value.Op);
    if (again)
      continue;
    seen.push_back(&value.Op);
    B.enter(B.addCase(s, value));
    emitMatchArmBody(arm);
    B.leave();
  }
  if (wildcard) {
    B.enter(B.addDefault(s));
    emitMatchArmBody(wildcard);
    B.leave();
  }
}

Val ModuleLowering::visitMatchStmt(ast::MatchStmt *node) {
  // An optional held in memory (a function value's, a tuple's, a struct's):
  // matched on its copy.
  if (ast::Type *t = node->getSubject()->getResolvedType();
      ast::isFunctionValue(t) || ast::isMemOptional(t))
    return emitMemOptionalMatch(node);
  // A tagged union: a switch on its tag (LoweringEnum.cpp).
  if (ast::isPayloadEnum(node->getSubject()->getResolvedType()))
    return emitPayloadEnumMatch(node);
  // A scope spanning the whole match: the subject box is a pending release
  // of this scope, so fall-through, `return`, `break` and `continue` all
  // release it.
  ScopeGuard matchGuard(*this);

  // 1. Evaluate the subject once.  A boxed subject (call result, field read)
  //    is owned for the duration of the match and released on every exit.
  //    With an `n: inout T` arm, it is read from the subject's storage,
  //    where n goes back (emitMatchPlace): a box, or the value of an
  //    optional primitive or enum, which an arm binding a `T` that is that
  //    optional binds (ast::MatchArm::getBindType).
  Val subjRaw;
  Val sharedSubj; // non-null iff we must release at the end
  std::optional<pir::LocalId> place;
  if (hasInoutArm(node)) {
    place = emitMatchPlace(node->getSubject());
    if (!place)
      return Val();
    Val at = B.load(*place, "subj.addr");
    if (ast::Type *t = node->getSubject()->getResolvedType();
        ast::isValueOptional(t)) {
      subjRaw = B.ptrLoad(at, toPIRType(t), "subj");
    } else {
      sharedSubj = B.ptrLoad(at, Type::Box, "subj.box");
      emitRetain(sharedSubj);
      subjRaw = emitSharedGet(sharedSubj, "subj.obj");
    }
  } else {
    subjRaw = emitExpr(node->getSubject());
    if (!subjRaw)
      return Val();
    if (exprAlreadyShared(node->getSubject())) {
      sharedSubj = takeSharedOwnership(node->getSubject(), subjRaw);
      subjRaw = emitSharedGet(sharedSubj, "subj.obj");
    } else if (isTrackedStringTemp(subjRaw)) {
      // A raw string temporary (a literal, a concatenation): box it so the
      // match owns it for its whole duration and arm bindings can share it.
      sharedSubj = emitSharedNew(subjRaw, "subj.box");
    }
  }
  if (sharedSubj)
    CurrentScope->PendingReleases.push_back(sharedSubj);

  if (auto *subjTy = node->getSubject()->getResolvedType())
    if (ast::isa<ast::EnumType>(subjTy))
      return emitEnumMatch(node, subjRaw);

  auto *optTy = ast::dyn_cast<ast::OptionalType>(
      node->getSubject()->getResolvedType()); // null-safe

  bool valueMode = false;
  if (!optTy)
    for (ast::MatchArm *arm : node->getArms())
      if (arm->isLiteral()) {
        valueMode = true;
        break;
      }
  if (valueMode)
    return emitValueMatch(node, subjRaw);

  ast::MatchArm *wildcard = findWildcardArm(node);
  auto emitDefault = [&]() {
    if (wildcard)
      emitMatchArmBody(wildcard);
  };

  // Optional subject: the `None` arm (if any).
  ast::MatchArm *noneArm = nullptr;
  if (optTy)
    for (ast::MatchArm *arm : node->getArms())
      if (arm->isLiteral()) {
        assert(ast::isa<ast::NoneLiteral>(arm->getLiteralPattern()) &&
               "Sema allows only a None literal arm on an optional subject");
        noneArm = arm;
      }

  // An optional primitive or enum (`int?`, `Suit?`, a value): its single
  // type arm names the primitive or the enum and always matches a present
  // value.
  ast::Type *primInner =
      optTy && ast::isValueOptional(optTy) ? optTy->getInnerType() : nullptr;

  struct TypeArm {
    ast::ClassType *CT;
    ast::Type *BindTy;
    bool MatchesAnySome;
    ast::MatchArm *Arm;
  };
  std::vector<TypeArm> typeArms;
  for (ast::MatchArm *arm : node->getArms()) {
    if (arm->isWildcard() || arm->isLiteral())
      continue;
    if (primInner) {
      typeArms.push_back(
          {nullptr, arm->getBindType(), /*MatchesAnySome=*/true, arm});
      continue;
    }
    // A class arm is an exact-type test; the arm naming an optional's
    // wrapped class matches every present value.
    ast::ClassType *armCt = ast::dyn_cast<ast::ClassType>(arm->getArmType());
    bool matchesAnySome = optTy && arm->getArmType() == optTy->getInnerType();
    assert((armCt || matchesAnySome) &&
           "Sema should have verified arm type exists");
    typeArms.push_back({armCt, arm->getBindType(), matchesAnySome, arm});
  }

  auto emitTypeArmBody = [&](size_t i) {
    const TypeArm &ta = typeArms[i];
    ScopeGuard armGuard(*this);
    if (ta.Arm->hasBinding()) {
      const std::string &name = ta.Arm->getBinding();
      pir::LocalId local;
      if (primInner) {
        // `n: int` on an `int?`: n is a plain value, the present payload
        // (the binding owns nothing); an arm binding a `T` that is the
        // `int?` (ast::MatchArm::getBindType) holds the optional itself.
        Val v = ast::isa<ast::OptionalType>(ta.BindTy)
                    ? subjRaw
                    : B.unwrap(subjRaw, name);
        local = B.addLocal(name, v.Ty);
        B.store(local, v);
      } else {
        // The binding is an ordinary owned variable holding its own +1
        // reference to the subject's box, released when the arm's scope
        // exits.  It can therefore be re-assigned, moved or stored like any
        // variable, and stays valid even if the arm re-assigns the subject
        // itself.  The subject is non-null here: a None subject never
        // reaches a type arm.
        Val box;
        if (sharedSubj) {
          emitRetain(sharedSubj);
          box = sharedSubj;
        } else {
          // PaykanShared_new acquires the object's existing unique box (+1).
          box = B.box(subjRaw, name + ".box");
        }
        local = B.addLocal(name, Type::Box);
        if (ta.Arm->getMode() == ast::ParamMode::View)
          B.markView(local);
        B.store(local, box);
      }
      CurrentScope->declare(name, local, ta.BindTy);
      // `n: inout T` writes it back to the subject after its last use
      // (bindInoutArm).
      if (ta.Arm->getMode() == ast::ParamMode::Inout && place)
        bindInoutArm(ta.Arm, local, *place,
                     node->getSubject()->getResolvedType(), ta.BindTy);
    }
    emitArmBody(ta.Arm);
  };

  auto emitTypeChain = [&]() {
    emitMatchChain(
        typeArms.size(),
        [&](size_t i) -> Val {
          if (typeArms[i].MatchesAnySome)
            return Val::boolean(true);
          return emitIsExactType(subjRaw, typeArms[i].CT);
        },
        emitTypeArmBody, emitDefault);
  };

  if (optTy) {
    // Route None to the `None` arm (else the wildcard); the vtable checks
    // below run only on the non-None path.
    Val isNone = primInner ? B.unary(pir::Opcode::Not,
                                     B.isSome(subjRaw, "opt.has"), "opt.isnone")
                           : B.cmp(CmpPred::Eq, subjRaw, Val::null(Type::Obj),
                                   "opt.isnone");
    pir::If *s = B.openIf(isNone, true);
    B.enter(*s->Then);
    if (noneArm)
      emitMatchArmBody(noneArm);
    else
      emitDefault();
    B.leave();
    B.enter(*s->Else);
    emitTypeChain();
    B.leave();
    return Val();
  }
  emitTypeChain();
  return Val();
}

// Value-mode match: a switch over the literal arms of an integer, char or
// bool subject, an equality chain over those of a float or Str one.
// `subjRaw` is a raw scalar for primitives or a raw PaykanString* for Str
// subjects.
Val ModuleLowering::emitValueMatch(ast::MatchStmt *node, const Val &subjRaw) {
  std::vector<ast::MatchArm *> litArms;
  for (ast::MatchArm *arm : node->getArms())
    if (arm->isLiteral())
      litArms.push_back(arm);
  ast::MatchArm *wildcard = findWildcardArm(node);

  if (subjRaw.Ty == Type::I64 || subjRaw.Ty == Type::Char ||
      subjRaw.Ty == Type::Bool) {
    // Their literals are constants of the subject's type.
    std::vector<std::pair<Val, ast::MatchArm *>> arms;
    arms.reserve(litArms.size());
    for (ast::MatchArm *arm : litArms)
      arms.emplace_back(emitExpr(arm->getLiteralPattern()), arm);
    emitMatchSwitch(subjRaw, arms, wildcard);
    return Val();
  }

  emitMatchChain(
      litArms.size(),
      [&](size_t i) -> Val {
        ast::Expr *lit = litArms[i]->getLiteralPattern();
        if (auto *sl = ast::dyn_cast<ast::StringLiteral>(lit)) {
          // PaykanString_equals consumes `other` as a box (the vtable-equals
          // ABI): box the literal and let the call release it.
          Val litStr = emitExpr(sl);
          Val litBox = emitSharedNew(litStr, "match.eq");
          Val eq =
              callRuntime(kPaykanStringEquals, {subjRaw, litBox}, "match.eq");
          return B.cmp(CmpPred::Ne, eq, Val::i64(0), "match.eq");
        }
        Val litVal = emitExpr(lit);
        if (litVal.Ty == Type::I64 && subjRaw.Ty == Type::F64)
          litVal = promoteIntToFloat(litVal, ASTCtx.getFloatTy());
        return B.cmp(CmpPred::Eq, subjRaw, litVal, "match.eq");
      },
      [&](size_t i) { emitMatchArmBody(litArms[i]); },
      [&]() {
        if (wildcard)
          emitMatchArmBody(wildcard);
      });
  return Val();
}

// Enum-mode match: a switch on the subject's i64, a case per variant's index.
Val ModuleLowering::emitEnumMatch(ast::MatchStmt *node, const Val &subjRaw) {
  auto *enumTy =
      ast::cast<ast::EnumType>(node->getSubject()->getResolvedType());
  std::vector<std::pair<Val, ast::MatchArm *>> arms;
  for (ast::MatchArm *arm : node->getArms()) {
    if (arm->isWildcard())
      continue;
    std::string variant;
    if (auto *stub = ast::dyn_cast<ast::ClassType>(arm->getArmType()))
      variant = stub->getName();
    else if (auto *et = ast::dyn_cast<ast::EnumType>(arm->getArmType()))
      variant = et->getName();
    int64_t val = enumTy->findVariant(variant);
    assert(val >= 0 && "Sema should have verified the variant exists");
    arms.emplace_back(Val::i64(val), arm);
  }
  emitMatchSwitch(subjRaw, arms, findWildcardArm(node));
  return Val();
}

} // namespace paykan::lowering
