// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// AST -> PIR lowering: `match` in its three modes (class / optional, value,
// enum) as a structured if-else chain.

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

Val ModuleLowering::visitMatchStmt(ast::MatchStmt *node) {
  // 1. Evaluate the subject once.  A boxed subject (call result, field read)
  //    is owned for the duration of the match and released on every exit.
  Val subjRaw = emitExpr(node->getSubject());
  if (!subjRaw)
    return Val();
  // A string-literal subject evaluates to its C string: build the Str (a
  // tracked temporary, boxed below like any other string temporary).
  if (auto *sl = ast::dyn_cast<ast::StringLiteral>(node->getSubject()))
    subjRaw = wrapStringLiteral(subjRaw, sl->getValue().size());
  Val sharedSubj; // non-null iff we must release at the end
  if (exprAlreadyShared(node->getSubject())) {
    sharedSubj = takeSharedOwnership(node->getSubject(), subjRaw);
    subjRaw = emitSharedGet(sharedSubj, "subj.obj");
  } else if (isTrackedStringTemp(subjRaw)) {
    // A raw string temporary (e.g. a concatenation): box it so the match owns
    // it for its whole duration and arm bindings can share it.
    sharedSubj = emitSharedNew(subjRaw, "subj.box");
  }

  // A scope spanning the whole match: the subject box is a pending release
  // of this scope, so fall-through, `return`, `break` and `continue` all
  // release it.
  ScopeGuard matchGuard(*this);
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

  // An optional primitive (`int?`): its single type arm names the primitive
  // and always matches a present value.
  ast::BuiltinType *primInner =
      optTy ? ast::dyn_cast<ast::BuiltinType>(optTy->getInnerType()) : nullptr;

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
      typeArms.push_back({nullptr, primInner, /*MatchesAnySome=*/true, arm});
      continue;
    }
    ast::ClassType *armCt = ast::dyn_cast<ast::ClassType>(arm->getArmType());
    ast::Type *bindTy = armCt;
    // Array-type arms resolve to the specialized array ClassType so vtable
    // identity comparison works like class arms.  The binding keeps the
    // array type itself: the specialized class is only a vtable key (it has
    // no PIR class or slots), and method calls, push/pop and subscripts on
    // the binding dispatch through the array type.
    if (!armCt) {
      if (auto *at = ast::dyn_cast<ast::ArrayType>(arm->getArmType())) {
        armCt = ASTCtx.getOrCreateSpecializedArrayType(at->getElementType());
        bindTy = at;
      }
    }
    assert(armCt && "Sema should have verified arm type exists");
    bool matchesAnySome = optTy && arm->getArmType() == optTy->getInnerType();
    typeArms.push_back({armCt, bindTy, matchesAnySome, arm});
  }

  auto emitTypeArmBody = [&](size_t i) {
    const TypeArm &ta = typeArms[i];
    ScopeGuard armGuard(*this);
    if (ta.Arm->hasBinding() && primInner) {
      // `n: int` on an `int?`: n is a plain value read out of the box (the
      // subject keeps the box; the binding owns nothing).
      Val v = emitPrimitiveUnbox(subjRaw, primInner, ta.Arm->getBinding());
      if (!v)
        return;
      pir::LocalId local = B.addLocal(ta.Arm->getBinding(), v.Ty);
      B.store(local, v);
      CurrentScope->declare(ta.Arm->getBinding(), local, primInner);
    } else if (ta.Arm->hasBinding()) {
      // The binding is an ordinary owned variable holding its own +1
      // reference to the subject's box, released when the arm's scope exits.
      // It can therefore be re-assigned, moved or stored like any variable,
      // and stays valid even if the arm re-assigns the subject itself.
      // The subject is non-null here: a None subject never reaches a type arm.
      Val box;
      if (sharedSubj) {
        emitRetain(sharedSubj);
        box = sharedSubj;
      } else {
        // PaykanShared_new acquires the object's existing unique box (+1).
        box = B.box(subjRaw, ta.Arm->getBinding() + ".box");
      }
      pir::LocalId local = B.addLocal(ta.Arm->getBinding(), Type::Box);
      B.store(local, box);
      CurrentScope->declare(ta.Arm->getBinding(), local, ta.BindTy);
    }
    emitBody(ta.Arm->getBody());
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
    Val isNone =
        B.cmp(CmpPred::Eq, subjRaw, Val::null(Type::Obj), "opt.isnone");
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

// Value-mode match: an equality chain over literal arms.  `subjRaw` is a
// raw scalar for primitives or a raw PaykanString* for Str subjects.
Val ModuleLowering::emitValueMatch(ast::MatchStmt *node, const Val &subjRaw) {
  std::vector<ast::MatchArm *> litArms;
  for (ast::MatchArm *arm : node->getArms())
    if (arm->isLiteral())
      litArms.push_back(arm);
  ast::MatchArm *wildcard = findWildcardArm(node);

  emitMatchChain(
      litArms.size(),
      [&](size_t i) -> Val {
        ast::Expr *lit = litArms[i]->getLiteralPattern();
        if (auto *sl = ast::dyn_cast<ast::StringLiteral>(lit)) {
          // PaykanString_equals consumes `other` as a box (the vtable-equals
          // ABI): box the literal and let the call release it.
          Val litStr = wrapStringLiteral(emitExpr(sl), sl->getValue().size());
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

// Enum-mode match: compare the subject's i64 against each variant's index.
Val ModuleLowering::emitEnumMatch(ast::MatchStmt *node, const Val &subjRaw) {
  auto *enumTy =
      ast::cast<ast::EnumType>(node->getSubject()->getResolvedType());
  struct VariantArm {
    ast::MatchArm *Arm;
    int64_t Value;
  };
  std::vector<VariantArm> variantArms;
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
    variantArms.push_back({arm, val});
  }
  ast::MatchArm *wildcard = findWildcardArm(node);

  emitMatchChain(
      variantArms.size(),
      [&](size_t i) -> Val {
        return B.cmp(CmpPred::Eq, subjRaw, Val::i64(variantArms[i].Value),
                     "match.eq");
      },
      [&](size_t i) { emitMatchArmBody(variantArms[i].Arm); },
      [&]() {
        if (wildcard)
          emitMatchArmBody(wildcard);
      });
  return Val();
}

} // namespace paykan::lowering
