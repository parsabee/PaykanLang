// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0

#include "Sema.h"
#include "ASTClone.h"
#include "Names.h"
#include "NativeABI.h"
#include "SemaInternal.h"
#include "paykan/modules/ModuleResolver.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace paykan {
namespace sema {

// Static module cache.
StringMap<Sema::ModuleInfo> Sema::ModuleCache;
StringMap<std::shared_ptr<SemaContext>> Sema::ModuleContextCache;

// -- Scope / ScopeGuard

Sema::Scope::Scope(Scope *parent) : Parent(parent) {}

ast::Type *Sema::Scope::lookup(std::string_view name) const {
  auto it = Locals.find(name);
  if (it != Locals.end())
    return it->second;
  return Parent ? Parent->lookup(name) : nullptr;
}

bool Sema::Scope::declare(std::string_view name, ast::Type *ty) {
  if (!Locals.try_emplace(std::string(name), ty).second)
    return false;
  return true;
}

void Sema::Scope::set(std::string_view name, ast::Type *ty) {
  Locals[std::string(name)] = ty;
}

bool Sema::Scope::contains(std::string_view name) const {
  return Locals.count(name);
}

Sema::Scope *Sema::Scope::findOwner(std::string_view name) {
  if (Locals.count(name))
    return this;
  return Parent ? Parent->findOwner(name) : nullptr;
}

Sema::ScopeGuard::ScopeGuard(Sema &s)
    : S(s), ScopeObj(std::make_unique<Scope>(s.CurrentScope)) {
  ScopeObj->Id = S.NextScopeId++;
  S.CurrentScope = ScopeObj.get();
}

Sema::ScopeGuard::~ScopeGuard() { S.CurrentScope = ScopeObj->Parent; }

// -- Helpers

Sema::Sema(ast::ASTContext &ctx, DiagEngine &diags,
           const std::string &projectRoot, const std::string &frontendName)
    : Diags(diags), Ctx(ctx), ProjectRoot(projectRoot),
      FrontendName(frontendName) {}

void Sema::declareFunction(std::string_view name, ast::Type *retTy,
                           std::vector<ast::Type *> paramTys, bool isBuiltin) {
  FunctionSig sig;
  sig.ReturnType = retTy;
  sig.ParamTypes = std::move(paramTys);
  sig.IsBuiltin = isBuiltin;
  FunctionTable[std::string(name)] = std::move(sig);
}

const Sema::FunctionSig *Sema::lookupFunction(std::string_view name) const {
  auto it = FunctionTable.find(name);
  return it != FunctionTable.end() ? &it->second : nullptr;
}

bool Sema::checkDeclNameAvailable(const std::string &name,
                                  ast::SourceLocation loc, DeclKind kind) {
  // A builtin type's name (`int`, `u8`, `void`, ...; the parser resolves
  // them before any declaration is seen) is nobody's to declare.
  if (ast::BuiltinType::Kind builtin; ast::builtinKindOfName(name, builtin)) {
    error(loc, "'" + name + "' is a builtin type and cannot be redeclared");
    return false;
  }
  // Type names are global across an import graph (docs/language/
  // 15-modules.md): a declaration named like a class, struct or enum an
  // import reached conflicts with it, whatever it declares.
  const bool imported = ImportedTypeOrigins.count(name) != 0;
  const auto importConflict = [&] {
    const char *what = "function";
    switch (kind) {
    case DeclKind::Function:
      break;
    case DeclKind::Class:
      what = "class";
      break;
    case DeclKind::Enum:
      what = "enum";
      break;
    case DeclKind::Trait:
      what = "trait";
      break;
    case DeclKind::Struct:
      what = "struct";
      break;
    }
    error(loc, std::string(what) + " '" + name +
                   "' conflicts with an imported type of the same name");
    // Its uses mean this declaration, and are follow-ons of this error.
    ErroneousNames.insert(name);
    return false;
  };
  // Compiler builtins are registered before any user declaration and must
  // never be replaced.  Which names are builtin is not spelled out here: a
  // class is builtin iff the ASTContext bootstrap flagged it
  // (ClassType::Builtin), and a function iff run() registered it so
  // (FunctionSig::IsBuiltin).  Classes are checked first so a name that is
  // both (`Str` is a class and a conversion function) reports as a class.
  if (auto *ct = Ctx.lookupClassType(name)) {
    if (ct->isBuiltin())
      error(loc, "'" + name + "' is a builtin class and cannot be redeclared");
    else if (imported)
      return importConflict();
    else
      error(loc, "'" + name + "' is already declared as a class");
    return false;
  }
  if (const auto *sig = lookupFunction(name); sig && sig->IsBuiltin) {
    error(loc, "'" + name + "' is a builtin function and cannot be redeclared");
    return false;
  }
  if (Ctx.lookupStructType(name)) {
    if (imported)
      return importConflict();
    if (kind == DeclKind::Class)
      error(loc,
            "class '" + name + "' conflicts with a struct of the same name");
    else
      error(loc, "'" + name + "' is already declared as a struct");
    return false;
  }
  if (Ctx.lookupEnumType(name)) {
    if (imported)
      return importConflict();
    if (kind == DeclKind::Enum)
      error(loc, "redefinition of enum '" + name + "'");
    else if (kind == DeclKind::Class)
      error(loc,
            "class '" + name + "' conflicts with an enum of the same name");
    else
      error(loc, "'" + name + "' is already declared as an enum");
    return false;
  }
  if (lookupFunction(name)) {
    if (kind == DeclKind::Function)
      error(loc, "redefinition of function '" + name + "'");
    else
      error(loc, "'" + name + "' is already declared as a function");
    return false;
  }
  if (auto t = Traits.find(name); t != Traits.end()) {
    error(loc,
          t->second.IsBuiltin
              ? "'" + name + "' is a builtin trait and cannot be redeclared"
              : "'" + name + "' is already declared as a trait");
    return false;
  }
  // Generic templates share the namespace: a class name is also its
  // constructor, and a generic function is called like any other.
  if (ClassTemplates.count(name)) {
    error(loc, "'" + name + "' is already declared as a generic class");
    return false;
  }
  if (StructTemplates.count(name)) {
    error(loc, "'" + name + "' is already declared as a generic struct");
    return false;
  }
  if (EnumTemplates.count(name)) {
    error(loc, "'" + name + "' is already declared as a generic enum");
    return false;
  }
  if (FuncTemplates.count(name)) {
    error(loc, "'" + name + "' is already declared as a generic function");
    return false;
  }
  return true;
}

bool Sema::isImportClashUse(std::string_view name) {
  const std::string_view type = name.substr(0, name.find(names::kQualSep));
  if (!ErroneousNames.count(type) || !ImportedTypeOrigins.count(type))
    return false;
  ++SuppressedFollowOns;
  return true;
}

void Sema::error(ast::SourceLocation loc, const std::string &msg) {
  // A conditional's branches are checked against one slot's location: the
  // same error from another branch is the one already reported.
  if (BranchErrors &&
      !BranchErrors
           ->insert(std::to_string(loc.getLineStart()) + ":" +
                    std::to_string(loc.getColumnStart()) + ":" + msg)
           .second)
    return;
  Diags.error(loc, msg);
  // An error inside an instantiation points at the template's source text
  // (clones keep their locations); say which instantiation was being checked
  // and where it was requested, innermost first, like a C++ compiler.  A
  // request in another module's template is shown in that module's file.
  if (InstantiationStack.empty())
    return;
  const std::string file = Diags.getSourceName();
  const std::vector<std::string> *lines = Diags.getSourceLines();
  for (auto it = InstantiationStack.rbegin(); it != InstantiationStack.rend();
       ++it) {
    Diags.setSourceInfo(it->File, it->Lines);
    Diags.note(it->RequestLoc, "in instantiation of '" + displayName(it->Name) +
                                   "' requested here");
  }
  Diags.setSourceInfo(file, lines);
}

void Sema::warning(ast::SourceLocation loc, const std::string &msg) {
  Diags.warning(loc, msg);
}

void Sema::note(ast::SourceLocation loc, const std::string &msg) {
  Diags.note(loc, msg);
}

// Type equality.  Every type Sema compares is canonical -- builtin singletons,
// registered ClassType/EnumType instances, and the types ASTContext interns
// per element (arrays, slices, optionals, tuples) -- so pointer identity is
// the whole test; the structural fallback never fires for resolved types.
bool Sema::typesEqual(ast::Type *a, ast::Type *b) {
  if (a == b)
    return true;
  if (!a || !b)
    return false;
  if (a->getKind() != b->getKind())
    return false;
  // A struct is nominal: equal to itself only (the identity test above).
  if (ast::isa<ast::StructType>(a))
    return false;
  if (auto *ao = ast::dyn_cast<ast::OptionalType>(a))
    return typesEqual(ao->getInnerType(),
                      ast::cast<ast::OptionalType>(b)->getInnerType());
  if (auto *ta = ast::dyn_cast<ast::TupleType>(a)) {
    auto *tb = ast::cast<ast::TupleType>(b);
    if (ta->getArity() != tb->getArity())
      return false;
    for (size_t i = 0; i < ta->getArity(); ++i)
      if (!typesEqual(ta->getElementType(i), tb->getElementType(i)) ||
          !(ta->getElemQual(i) == tb->getElemQual(i)))
        return false;
    return true;
  }
  return false;
}

ast::Type *Sema::promotedOperandType(const ast::Type *lhs,
                                     const ast::Type *rhs) const {
  const ast::Type *intTy = Ctx.getIntTy();
  ast::Type *floatTy = Ctx.getFloatTy();
  if ((lhs == intTy && rhs == floatTy) || (lhs == floatTy && rhs == intTy))
    return floatTy;
  return nullptr;
}

bool Sema::isAssignable(ast::Type *dst, ast::Type *src) const {
  if (dst == src)
    return true;

  // A slice takes a fixed-size array of exactly its element type, of any
  // length (and another slice, the same type).
  if (auto *dstST = ast::dyn_cast<ast::SliceType>(dst)) {
    auto *srcFT = ast::dyn_cast<ast::FixedArrayType>(src);
    // Its elements' form as they are, or a unique one into a `shared` one
    // (#368: a fresh array only, checkAssignable).
    const ast::Qualifier dq = dstST->getElemQual();
    const ast::Qualifier sq = srcFT ? srcFT->getElemQual() : ast::Qualifier{};
    if (!srcFT ||
        !(sq == dq || (sq.isUnique() && dq.Form == ast::QualForm::Shared)))
      return false;
    ast::Type *d = dstST->getElementType();
    ast::Type *s = srcFT->getElementType();
    return d == s ||
           (ast::isa<ast::TupleType>(d) && ast::isa<ast::TupleType>(s) &&
            !ast::isa<ast::StructType>(d) && isAssignable(d, s));
  }

  // int -> float promotion.
  if (dst == Ctx.getFloatTy() && src == Ctx.getIntTy())
    return true;

  // Optional destinations (issue #5):
  //   T  -> T?   implicit widening (a present value);
  //   S? -> T?   when S -> T (covariant in the wrapped type).
  // The `None` literal is handled by checkAssignable, which sees the
  // expression; its static type is `Obj`, which is NOT assignable to `T?`.
  //   An optional primitive `int?` holds its value inline, so a plain `int`
  //   (or, for `float?`, an `int` after promotion) widens to a present
  //   value, but an `S?` only converts to `T?` when the representation is
  //   the same: `int?` -> `float?` would have to convert and is rejected.
  if (auto *dstOT = ast::dyn_cast<ast::OptionalType>(dst)) {
    if (ast::isa<ast::BuiltinType>(dstOT->getInnerType()))
      if (auto *srcOT = ast::dyn_cast<ast::OptionalType>(src))
        return srcOT->getInnerType() == dstOT->getInnerType();
    return isAssignable(dstOT->getInnerType(), ast::stripOptional(src));
  }

  // An optional never flows into a non-optional slot: `T?` -> `T` requires a
  // `match`, and an optional is not an object, so not an `Obj` either (#96;
  // the print builtins take one, see visitCallExpr).
  if (ast::isa<ast::OptionalType>(src))
    return false;

  // A struct is nominal: only itself flows into it (the identity test
  // above), and it is never a tuple of the same shape.  (Its fields can be
  // assigned, so a covariant copy would let a `Point` holding a `Dog`
  // become one holding a `Cat` through an `Animal`-typed field.)
  if (ast::isa<ast::StructType>(dst) || ast::isa<ast::StructType>(src))
    return false;

  // Tuples: assignability is element-wise and *representation-preserving*:
  // a copy is the tuple's bytes, so value elements must match exactly (no int
  // -> float promotion), while reference-typed elements may be covariant
  // (`(int, Str)` is assignable to `(int, Obj)`) because tuples are
  // immutable and every reference slot holds a box regardless of its static
  // type.
  if (auto *srcTT = ast::dyn_cast<ast::TupleType>(src)) {
    auto *dstTT = ast::dyn_cast<ast::TupleType>(dst);
    if (!dstTT || dstTT->getArity() != srcTT->getArity())
      return false;
    for (size_t i = 0; i < dstTT->getArity(); ++i) {
      ast::Type *d = dstTT->getElementType(i);
      ast::Type *s = srcTT->getElementType(i);
      // An element's form (#368): a unique reference converts into a
      // `shared` one, as a whole one does (a place's moves, under the copy
      // rule); nothing converts back.
      const ast::Qualifier dq = dstTT->getElemQual(i);
      const ast::Qualifier sq = srcTT->getElemQual(i);
      if (!(dq == sq) && !(sq.isUnique() && dq.Form == ast::QualForm::Shared))
        return false;
      if (d == s)
        continue;
      const bool sameShape =
          (ast::isRefType(d) && ast::isRefType(s)) ||
          (ast::isa<ast::TupleType>(d) && ast::isa<ast::TupleType>(s));
      if (!sameShape || !isAssignable(d, s))
        return false;
    }
    return true;
  }

  // A fixed-size array's elements convert as a tuple's do (#368): a unique
  // reference into a `shared` one, of the same type, nothing back.
  if (auto *srcFT = ast::dyn_cast<ast::FixedArrayType>(src))
    if (auto *dstFT = ast::dyn_cast<ast::FixedArrayType>(dst);
        dstFT && dstFT->getLength() == srcFT->getLength()) {
      const ast::Qualifier dq = dstFT->getElemQual();
      const ast::Qualifier sq = srcFT->getElemQual();
      if (!(dq == sq) && !(sq.isUnique() && dq.Form == ast::QualForm::Shared))
        return false;
      ast::Type *d = dstFT->getElementType();
      ast::Type *s = srcFT->getElementType();
      return d == s ||
             (((ast::isa<ast::FixedArrayType>(d) &&
                ast::isa<ast::FixedArrayType>(s)) ||
               (ast::isa<ast::TupleType>(d) && ast::isa<ast::TupleType>(s))) &&
              isAssignable(d, s));
    }

  // ClassType subtyping: src <: dst.
  if (auto *dstCT = ast::dyn_cast<ast::ClassType>(dst))
    if (auto *srcCT = ast::dyn_cast<ast::ClassType>(src))
      return srcCT->isSubtypeOf(dstCT);

  return false;
}

bool Sema::checkAssignable(ast::Type *dst, ast::Type *srcTy, ast::Expr *src) {
  // A value into a slot of an `atomic` type (the prototype): the slot takes
  // a new cell holding it (an `atomic` place's handle is passed as it is).
  // A cell keeps the class it was made with: through a handle of a wider
  // one, an `inout` block could replace its contents with another class.
  if (auto *te = src ? ast::dyn_cast<ast::TernaryExpr>(src) : nullptr)
    for (ast::Expr *b : conditionalBranches(te))
      if (ast::Type *bt = b->getResolvedType();
          dst && bt && ast::isRefType(ast::stripOptional(bt)) &&
          ast::stripOptional(dst) != ast::stripOptional(bt) &&
          qualifierOf(b).Form == ast::QualForm::Locked) {
        error(b->getLocation(),
              "a 'locked' cell keeps the class it was made with: cannot store "
              "'" +
                  typeName(bt) + "' in '" + typeName(dst) + "'");
        return false;
      }
  if (dst && srcTy && src &&
      ast::stripOptional(dst) != ast::stripOptional(srcTy) &&
      ast::isRefType(ast::stripOptional(srcTy)) &&
      qualifierOf(src).Form == ast::QualForm::Locked) {
    error(src->getLocation(),
          "a 'locked' cell keeps the class it was made with: cannot store '" +
              typeName(srcTy) + "' in '" + typeName(dst) + "'");
    return false;
  }
  if (auto *at = ast::dyn_cast<ast::AtomicType>(dst);
      at && srcTy && !ast::isa<ast::AtomicType>(srcTy) &&
      isAssignable(at->getInnerType(), srcTy)) {
    if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(src);
        se && se->isAtomicElement()) {
      error(
          src->getLocation(),
          "'" + placeText(src) + "' is an element of an '" +
              typeName(
                  const_cast<ast::Expr *>(se->getArray())->getResolvedType()) +
              "': it has no handle to share");
      return false;
    }
    src->setNewAtomicCell(at);
    return true;
  }
  // `None` into an optional slot: statically the literal is `Obj` (a design
  // decision — see docs/language/12-optionals.md), but in this position it
  // denotes the absent `T?` value.  Record that contextual type on the
  // literal so the lowering emits a null box rather than boxing the `None`
  // singleton, which is how a present `Obj` value spells None.
  if (ast::isa<ast::NoneLiteral>(src) && ast::isa<ast::OptionalType>(dst)) {
    src->setResolvedType(dst);
    return true;
  }
  // A literal whose elements take the destination's element types (`[1, 2]`
  // into `int?[]`, `(None, "a")` into `(int?, Str)`, nested literals): retype
  // the literal and mark each element that is wrapped.
  srcTy = adoptLiteralElements(dst, srcTy, src);
  if (!isAssignable(dst, srcTy)) {
    if (!lostSuperclassMismatch(dst, srcTy))
      return false;
    ++SuppressedFollowOns;
  }
  // A tuple's or an array's unique element converts into a `shared` one
  // (#368) only out of a fresh value (a literal, a result by value, `move
  // x`): a place's value, borrowed by a `view` or reached through a
  // conditional, would still own the object the `shared` copy names.
  if (src && !isFreshValue(src) && !sameElementForms(dst, srcTy))
    return false;
  // `int` -> `int?` (and `int` -> `float?`): the lowering makes the value
  // the optional's present value.  A fixed-size array into a slice is
  // passed as its address and length.
  if (ast::wrapsInOptional(dst, srcTy) || ast::dyn_cast<ast::SliceType>(dst))
    src->setCoercedType(dst);
  return true;
}

bool Sema::sameShapeOfParts(const ast::Type *a, const ast::Type *b) {
  if (a == b)
    return true;
  if (!a || !b || a->getKind() != b->getKind() || ast::isa<ast::StructType>(a))
    return false;
  if (const auto *oa = ast::dyn_cast<ast::OptionalType>(a))
    return sameShapeOfParts(oa->getInnerType(),
                            ast::cast<ast::OptionalType>(b)->getInnerType());
  if (const auto *fa = ast::dyn_cast<ast::FixedArrayType>(a)) {
    const auto *fb = ast::cast<ast::FixedArrayType>(b);
    return fa->getLength() == fb->getLength() &&
           sameShapeOfParts(fa->getElementType(), fb->getElementType());
  }
  if (const auto *ta = ast::dyn_cast<ast::TupleType>(a)) {
    const auto *tb = ast::cast<ast::TupleType>(b);
    if (ta->getArity() != tb->getArity())
      return false;
    for (size_t i = 0; i < ta->getArity(); ++i)
      if (!sameShapeOfParts(ta->getElementType(i), tb->getElementType(i)))
        return false;
    return true;
  }
  return false;
}

bool Sema::sameElementForms(const ast::Type *dst, const ast::Type *src) {
  // Through an optional on either side (`T` flows into `T?`).
  if (const auto *d = ast::dyn_cast<ast::OptionalType>(dst))
    return sameElementForms(d->getInnerType(),
                            ast::stripOptional(const_cast<ast::Type *>(src)));
  if (const auto *o = ast::dyn_cast<ast::OptionalType>(src))
    return sameElementForms(dst, o->getInnerType());
  if (const auto *da = ast::dyn_cast<ast::FixedArrayType>(dst))
    if (const auto *sa = ast::dyn_cast<ast::FixedArrayType>(src))
      return da->getElemQual() == sa->getElemQual() &&
             sameElementForms(da->getElementType(), sa->getElementType());
  if (const auto *ds = ast::dyn_cast<ast::SliceType>(dst))
    if (const auto *sa = ast::dyn_cast<ast::FixedArrayType>(src))
      return ds->getElemQual() == sa->getElemQual() &&
             sameElementForms(ds->getElementType(), sa->getElementType());
  const auto *d = ast::dyn_cast<ast::TupleType>(dst);
  const auto *t = ast::dyn_cast<ast::TupleType>(src);
  if (!d || !t || ast::isa<ast::StructType>(d) ||
      d->getArity() != t->getArity())
    return true;
  for (size_t i = 0; i < d->getArity(); ++i)
    if (!(d->getElemQual(i) == t->getElemQual(i)) ||
        !sameElementForms(d->getElementType(i), t->getElementType(i)))
      return false;
  return true;
}

bool Sema::lostSuperclassMismatch(ast::Type *dst, ast::Type *srcTy) const {
  auto *dstCT = ast::dyn_cast<ast::ClassType>(ast::stripOptional(dst));
  auto *srcCT = ast::dyn_cast<ast::ClassType>(ast::stripOptional(srcTy));
  // Whether a class is a subclass of the destination never depends on the
  // destination's own superclass, and a final one (`Str`) has none.  An
  // optional into a plain class is no question of subclasses.
  if (!dstCT || !srcCT || dstCT->isFinal() ||
      (ast::isa<ast::OptionalType>(srcTy) && !ast::isa<ast::OptionalType>(dst)))
    return false;
  for (const ast::ClassType *c = srcCT; c; c = c->getSuperClass())
    if (SuperclassLost.count(c->getName()))
      return true;
  return false;
}

ast::Type *Sema::adoptLiteralElements(ast::Type *dst, ast::Type *srcTy,
                                      ast::Expr *src) {
  // Only an element that needs it is touched; the literal's other elements
  // keep their own types (the usual representation-preserving rules then
  // decide assignability).
  auto boxes = [&](ast::Type *d, ast::Type *s) {
    return ast::wrapsInOptional(d, s) && isAssignable(ast::stripOptional(d), s);
  };
  // One element flowing into slot type @p d: a nested literal is adopted
  // recursively, `None` takes an optional slot's type (a null box, as in
  // checkAssignable), and a primitive is wrapped for an optional primitive
  // slot.  Returns the element's (possibly new) type.
  auto adoptOperand = [&](ast::Type *d, ast::Expr *e) -> ast::Type * {
    ast::Type *t = e->getResolvedType();
    if (!t)
      return t;
    if (ast::isa<ast::NoneLiteral>(e)) {
      if (!ast::isa<ast::OptionalType>(d))
        return t;
      e->setResolvedType(d);
      return d;
    }
    if (ast::isa<ast::ArrayLiteralExpr>(e) ||
        ast::isa<ast::TupleLiteralExpr>(e)) {
      ast::Type *adopted = adoptLiteralElements(d, t, e);
      // The adopted literal is the present value of an optional slot
      // (`(1, ("x", "y"))` into `(int, (Str, Str)?)`): wrapped as a call's
      // result would be.
      if (adopted && boxes(d, adopted)) {
        e->setCoercedType(d);
        return d;
      }
      return adopted;
    }
    if (!boxes(d, t))
      return t;
    e->setCoercedType(d);
    return d;
  };

  if (auto *lit = ast::dyn_cast<ast::TupleLiteralExpr>(src)) {
    auto *dstTT = ast::dyn_cast<ast::TupleType>(ast::stripOptional(dst));
    auto *litTT = ast::dyn_cast<ast::TupleType>(srcTy);
    // A tuple literal never flows into a struct (nominal typing).
    if (!dstTT || ast::isa<ast::StructType>(dstTT) || !litTT ||
        dstTT->getArity() != litTT->getArity() ||
        litTT->getArity() != lit->getNumElements())
      return srcTy;
    std::vector<ast::Type *> elems = litTT->getElementTypes();
    bool changed = false;
    for (size_t i = 0; i < elems.size(); ++i) {
      ast::Type *t =
          adoptOperand(dstTT->getElementType(i), lit->getElements()[i]);
      if (!t || t == elems[i])
        continue;
      elems[i] = t;
      changed = true;
    }
    if (!changed)
      return srcTy;
    auto *retyped = Ctx.getTupleType(std::move(elems), litTT->getElemQuals());
    lit->setResolvedType(retyped);
    return retyped;
  }
  return srcTy;
}

bool Sema::diagnoseOptionalNarrowing(ast::SourceLocation loc, ast::Type *dst,
                                     ast::Type *srcTy) {
  auto *srcOT = ast::dyn_cast<ast::OptionalType>(srcTy);
  if (!srcOT || ast::isa<ast::OptionalType>(dst))
    return false;
  // An optional is not an object (#96): it does not become an `Obj`.
  if (dst == Ctx.getObjTy()) {
    bool unwrapHelps = isAssignable(dst, srcOT->getInnerType());
    error(loc, "cannot use optional '" + typeName(srcOT) +
                   "' as 'Obj': an optional is not an object" +
                   (unwrapHelps ? "; unwrap it with match" : ""));
    return true;
  }
  // Only report the unwrap hint when unwrapping would actually help, i.e. the
  // wrapped type itself fits the destination; otherwise it is an ordinary
  // type mismatch and the generic diagnostic is more accurate.
  if (!isAssignable(dst, srcOT->getInnerType()))
    return false;
  errorOptionalUnwrap(loc, srcOT);
  return true;
}

void Sema::errorOptionalUnwrap(ast::SourceLocation loc,
                               ast::OptionalType *optTy) {
  error(loc, "cannot use optional '" + typeName(optTy) + "' as '" +
                 typeName(optTy->getInnerType()) +
                 "' without unwrapping (use match)");
}

void Sema::errorNoToString(ast::SourceLocation loc, ast::Type *ty,
                           const std::string &why) {
  error(loc, "no method '" + std::string(names::kMethodToString) +
                 "' on type '" + typeName(ty) +
                 "', which does not implement trait '" + names::kTraitShowable +
                 "'" + (why.empty() ? "" : ": " + why));
}

/// The alignment of a value of @p ty held in memory: a scalar's or a box's
/// is its size, an optional value's its payload's, an array's its element's,
/// a tuple's its most aligned element's.
static int64_t elementAlign(const ast::Type *ty) {
  if (const auto *tt = ast::dyn_cast<ast::TupleType>(ty)) {
    int64_t align = 1;
    for (const ast::Type *e : tt->getElementTypes())
      align = std::max(align, elementAlign(e));
    return align;
  }
  if (const auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty))
    return elementAlign(ft->getElementType());
  if (ast::isValueOptional(ty) || ast::isMemOptional(ty))
    return elementAlign(ast::cast<ast::OptionalType>(ty)->getInnerType());
  if (const auto *bt = ast::dyn_cast<ast::BuiltinType>(ty))
    return bt->getTypeKind() == ast::BuiltinType::Bool ||
                   bt->getTypeKind() == ast::BuiltinType::Char
               ? 1
               : 8;
  return 8; // an enum value, or a box
}

int64_t Sema::fixedElementBytes(const ast::Type *ty) {
  if (ast::isPayloadEnum(ty))
    return valueSize(ty); // its tag, then its variants' fields in place
  if (ast::isa<ast::EnumType>(ty))
    return 8;
  if (const auto *bt = ast::dyn_cast<ast::BuiltinType>(ty)) {
    switch (bt->getTypeKind()) {
    case ast::BuiltinType::Int:
    case ast::BuiltinType::Float:
    case ast::BuiltinType::I8:
    case ast::BuiltinType::I16:
    case ast::BuiltinType::I32:
    case ast::BuiltinType::U8:
    case ast::BuiltinType::U16:
    case ast::BuiltinType::U32:
    case ast::BuiltinType::U64:
    case ast::BuiltinType::F16:
    case ast::BuiltinType::F32:
      return 8; // every numeric type takes an 8-byte slot
    case ast::BuiltinType::Bool:
    case ast::BuiltinType::Char:
      return 1;
    case ast::BuiltinType::Void:
      return -1;
    }
  }
  // A reference is held as its box; a function value as its code and its
  // closure's box (an optional one the same: None has no code).
  if (ast::isRefType(ty))
    return 8;
  if (ast::isFunctionValue(ty))
    return 16;
  // An optional tuple or struct: a presence flag, padded to the value's
  // alignment, then the value (Lowering's `(bool, T)`).
  if (ast::isMemOptional(ty)) {
    const ast::Type *inner = ast::cast<ast::OptionalType>(ty)->getInnerType();
    int64_t bytes = fixedElementBytes(inner);
    if (bytes < 0)
      return -1;
    int64_t align = elementAlign(inner);
    return ((align + bytes) + align - 1) / align * align;
  }
  // An optional value is a presence flag padded to its value's alignment,
  // then the value (`int?` takes 16 bytes, `bool?` 2).
  if (ast::isValueOptional(ty))
    return 2 *
           fixedElementBytes(ast::cast<ast::OptionalType>(ty)->getInnerType());
  if (const auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty)) {
    // Its elements, counted across its rows.
    int64_t count = 1;
    for (const auto *f = ft; f;
         f = ast::dyn_cast<ast::FixedArrayType>(f->getElementType())) {
      if (f->getLength() > kMaxFixedArrayElements / count)
        return -1;
      count *= f->getLength();
    }
    int64_t elem = fixedElementBytes(ft->getElementType());
    return elem < 0 ? -1 : elem * ft->getLength();
  }
  // A tuple: its elements at their alignments, like a C struct's fields.
  if (const auto *tt = ast::dyn_cast<ast::TupleType>(ty)) {
    auto alignUp = [](int64_t n, int64_t a) { return (n + a - 1) / a * a; };
    int64_t size = 0;
    for (const ast::Type *e : tt->getElementTypes()) {
      int64_t bytes = fixedElementBytes(e);
      if (bytes < 0)
        return -1;
      size = alignUp(size, elementAlign(e)) + bytes;
    }
    return alignUp(size, elementAlign(tt));
  }
  return -1;
}

bool Sema::holdsFixedArray(const ast::Type *ty) {
  if (!ty)
    return false;
  if (ast::isa<ast::FixedArrayType>(ty) || ast::isa<ast::SliceType>(ty))
    return true;
  if (const auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    return holdsFixedArray(ot->getInnerType());
  if (const auto *tt = ast::dyn_cast<ast::TupleType>(ty))
    for (const ast::Type *e : tt->getElementTypes())
      if (holdsFixedArray(e))
        return true;
  return false;
}

std::string Sema::sliceArgNote(const ast::Type *param, const ast::Type *arg) {
  if (!ast::dyn_cast<ast::SliceType>(param))
    return "";
  if (ast::dyn_cast<ast::FixedArrayType>(arg))
    return " (a slice takes a fixed-size array of exactly its element type)";
  return "";
}

bool Sema::hasInferredLength(const ast::Type *ty) {
  for (const auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty); ft;
       ft = ast::dyn_cast<ast::FixedArrayType>(ft->getElementType()))
    if (ft->isInferred())
      return true;
  return false;
}

ast::Type *Sema::fillLengths(ast::Type *ty, ast::Type *from) {
  auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty);
  if (!ft || !hasInferredLength(ft))
    return ty;
  auto *fromFT = ast::dyn_cast<ast::FixedArrayType>(from);
  if (!fromFT)
    return nullptr;
  ast::Type *elem = fillLengths(ft->getElementType(), fromFT->getElementType());
  if (!elem)
    return nullptr;
  return Ctx.getFixedArrayType(
      elem, ft->isInferred() ? fromFT->getLength() : ft->getLength(),
      ft->getElemQual());
}

void Sema::errorNoLengths(ast::SourceLocation loc, ast::Type *ty,
                          ast::Type *from) {
  error(loc, "'" + typeName(ty) +
                 "' takes its lengths from a fixed-size array of its shape, "
                 "not from '" +
                 typeName(from) + "'");
}

bool Sema::errorFixedArrayHere(ast::SourceLocation loc, ast::Type *ty,
                               const std::string &where) {
  if (ast::isa<ast::SliceType>(ty)) {
    error(loc, where + ": a slice ('" + typeName(ty) +
                   "') can only be indexed, measured with 'len()' or passed "
                   "to a 'view' or 'inout' array parameter");
    return false;
  }
  error(loc, where + ": a fixed-size array ('" + typeName(ty) +
                 "') can only be a local variable, a parameter, a result or "
                 "a class field for now");
  return false;
}

bool Sema::checkTypeArgs(const std::string &name,
                         const std::vector<ast::Type *> &args,
                         ast::SourceLocation loc) {
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string what =
        "type argument " + std::to_string(i + 1) + " of '" + name + "'";
    if (holdsFixedArray(args[i]))
      return errorFixedArrayHere(loc, args[i], what);
    if (args[i] == Ctx.getVoidTy()) {
      error(loc, what + " cannot be 'void': it has no value (it is a "
                        "function's result type only)");
      return false;
    }
    if (ast::isa<ast::TaskType>(args[i])) {
      error(loc, what + " cannot be a task yet: a task is held by a variable");
      return false;
    }
  }
  return true;
}

bool Sema::checkFixedResult(ast::Type *retTy, ast::ParamMode mode,
                            ast::SourceLocation loc) {
  if (mode == ast::ParamMode::Value || !ast::isa<ast::FixedArrayType>(retTy))
    return true;
  error(loc, std::string("a '") +
                 (mode == ast::ParamMode::View ? "view" : "inout") +
                 "' result of a fixed-size array ('" + typeName(retTy) +
                 "') is not supported yet; return a copy ('-> " +
                 typeName(retTy) + "')");
  return false;
}

const ast::TupleIndexExpr *Sema::tupleElementHolding(const ast::Expr *e) {
  for (;;) {
    if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(e))
      return ti;
    const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e);
    if (!ma || !ast::isa<ast::StructType>(ma->getReceiver()->getResolvedType()))
      return nullptr;
    e = ma->getReceiver();
  }
}

std::string Sema::letValueRoot(const ast::Expr *e) {
  for (;;) {
    if (const auto *id = ast::dyn_cast<ast::Identifier>(e))
      return varKind(id->getName()) == VarKind::Let ? id->getName() : "";
    // An element of a fixed-size array, a field of a struct or an element
    // of a tuple is part of the value that holds it.
    if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(e)) {
      const ast::Type *base = se->getArray()->getResolvedType();
      if (!base || !ast::isa<ast::FixedArrayType>(base))
        return "";
      e = se->getArray();
    } else if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e)) {
      const ast::Type *base = ma->getReceiver()->getResolvedType();
      if (!base || !ast::isa<ast::StructType>(base))
        return "";
      e = ma->getReceiver();
    } else if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(e)) {
      e = ti->getTuple();
    } else {
      return "";
    }
  }
}

Sema::TypeContext::TypeContext(const TypeContext *parent,
                               std::initializer_list<std::string_view> pieces)
    : Parent(parent), NumPieces(pieces.size()) {
  assert(NumPieces <= kMaxPieces && "a TypeContext holds kMaxPieces pieces");
  std::copy(pieces.begin(), pieces.end(), Pieces.begin());
}

std::string Sema::TypeContext::str() const {
  std::string s = Parent ? Parent->str() : Text;
  for (size_t i = 0; i < NumPieces; ++i)
    s += Pieces[i];
  return s;
}

ast::Type *Sema::resolveType(ast::Type *ty, ast::SourceLocation loc,
                             const TypeContext &context) {
  // Each kind of annotation in a function of its own, so that a type
  // nested to the limit (docs/grammar.md section 9) recurses through small
  // frames.
  const TypeFlags flags{std::exchange(FixedArrayAllowed, false),
                        std::exchange(InferredLengthAllowed, false),
                        std::exchange(SliceAllowed, false),
                        std::exchange(NoArrayHolder, nullptr),
                        DeferredOptionalChecks.size()};
  // Where this annotation's qualifier goes: a whole place's to its
  // declaration (takeQualifier), a part's to the composite holding it
  // (resolvePart).  Cleared first, so a qualifier no declaration took (an
  // error's, a cast's) is gone by the next annotation.
  typeSlot() = {};
  // Made here before, and checked then (ResolvedTypes).
  if (ResolvedTypes.count(ty))
    return ty;
  // A qualified type: the inner type, its qualifier left in the slot.
  if (auto *qt = ast::dyn_cast<ast::QualifiedType>(ty))
    return resolveQualifiedAnnotation(qt, loc, context, flags);
  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(ty))
    return Ctx.getBuiltinType(bt->getTypeKind());
  // `atomic T`: T is a number, a `bool`, a `char` or a fixed array of them.
  if (auto *at = ast::dyn_cast<ast::AtomicType>(ty))
    return resolveAtomicAnnotation(at, loc, context, flags);
  if (ast::isa<ast::TaskType>(ty))
    return ty; // canonical already: never written by a program
  // A parameter's slice, resolved already.
  if (ast::isa<ast::SliceType>(ty))
    return ty;

  if (auto *et = ast::dyn_cast<ast::EnumType>(ty))
    return Ctx.lookupEnumType(et->getName());

  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty))
    return resolveClassNameAnnotation(ct, loc, context, flags);

  // `T[]` is a slice, the type of a `view` or `inout` parameter only.
  if (auto *at = ast::dyn_cast<ast::ArrayType>(ty))
    return resolveSliceAnnotation(at, loc, context, flags);

  if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty))
    return resolveFixedArrayAnnotation(ft, loc, context, flags);

  if (auto *gt = ast::dyn_cast<ast::GenericType>(ty))
    return resolveGenericAnnotation(gt, loc, context, flags);

  if (auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    return resolveOptionalAnnotation(ot, loc, context, flags);

  // A struct type is canonical already (a resolved annotation, or a type
  // argument put into a template); before the tuple case, which it is too.
  if (ast::isa<ast::StructType>(ty))
    return ty;

  if (auto *fnt = ast::dyn_cast<ast::FunctionType>(ty))
    return resolveFunctionAnnotation(fnt, loc, context, flags);

  if (auto *tt = ast::dyn_cast<ast::TupleType>(ty))
    return resolveTupleAnnotation(tt, loc, context, flags);

  error(loc, context.str() + " has unknown type");
  return nullptr;
}

ast::Type *Sema::checkedType(const TypeFlags &flags, ast::Type *canonical) {
  // The canonical tuple, optional or function type made of what an
  // annotation resolved, remembered as checked unless a check of a part
  // waits for the structs (it is made again then).
  if (DeferredOptionalChecks.size() == flags.DeferredChecks)
    ResolvedTypes.insert(canonical);
  return canonical;
}

// A qualified type: the inner type, its qualifier left in the slot.
ast::Type *Sema::resolveQualifiedAnnotation(ast::QualifiedType *qt,
                                            ast::SourceLocation loc,
                                            const TypeContext &context,
                                            const TypeFlags &flags) {
  ast::Qualifier &slot = typeSlot();
  const ast::Qualifier written = qt->getQualifier();
  if (!qt->getInnerType()) {
    error(loc, std::string("'") + written.spelling() +
                   "' needs a type here ('" + written.spelling() +
                   " T'); only a variable declaration infers it");
    return nullptr;
  }
  FixedArrayAllowed = flags.Fixed;
  InferredLengthAllowed = flags.Inferred;
  SliceAllowed = flags.Slice;
  NoArrayHolder = flags.NoArray;
  ast::Type *inner = resolveType(qt->getInnerType(), loc, context);
  if (!inner)
    return nullptr;
  // `shared Node[4]` is four shared references: `shared` and `shared mut`
  // bind to the primary type (docs/design/concurrency.md, "Syntax"), so
  // on an array of references they qualify its elements.
  if (written.Form == ast::QualForm::Shared) {
    bool failed = false;
    if (ast::Type *each = qualifyElements(written, inner, loc, failed))
      return each;
    if (failed)
      return nullptr;
  }
  // What the inner annotation held: a template's type argument (`mut T`
  // with T = `shared mut Node`, `mut T?` with T = `shared Node`).  A
  // qualifier written on T joins it (#368).
  const ast::Qualifier held = std::exchange(slot, ast::Qualifier{});
  // Marked as a type argument's when the argument gave the form: `mut T`
  // with T = `shared mut Node` is, `shared mut T` with T = `mut Node` is the
  // template's own.
  bool substituted = qt->isCanonical() || held.FromArgument;
  ast::Qualifier q = written;
  if (!joinQualifiers(written, held, q)) {
    // `weak T` with T = `shared mut Node`: the declaration's form.
    if (ResolvingWeakDecl) {
      error(loc, std::string("a '") + (written.Mut ? "weak mut" : "weak") +
                     "' reference names a '" + written.spelling() +
                     "' object, and the type argument is '" +
                     qualifiedName(held, inner) + "'" +
                     (held == ast::Qualifier{ast::QualForm::Shared, true}
                          ? ": write 'weak mut'"
                          : ""));
      return nullptr;
    }
    const bool unwritable =
        written == ast::Qualifier{ast::QualForm::Unique, true};
    error(loc,
          std::string("'") + written.spelling() +
              "' cannot qualify the type argument '" +
              qualifiedName(held, inner) + "': " +
              (unwritable ? std::string(held.Form == ast::QualForm::AtomicShared
                                            ? "an '"
                                            : "a '") +
                                held.spelling() +
                                "' reference is never written through"
                          : std::string("a type takes one qualifier, "
                                        "and '") +
                                written.spelling() + "' and '" +
                                held.spelling() + "' are two"));
    return nullptr;
  }
  if (!qt->isCanonical() && !(q == held))
    substituted = false; // the written qualifier gave the form
  if (!checkQualifierApplies(q, inner, loc))
    return nullptr;
  if (inTypePart()) {
    // A template's `mut` on an element (`(T, int)` with T = `mut Node`)
    // is the element's own: an element is written as its variable is.
    if (substituted && PartFrames.back().Kind == PartKind::Element &&
        q == ast::Qualifier{ast::QualForm::Unique, true})
      q = {};
    if (!checkPartQualifier(q, PartFrames.back().Kind, loc))
      return nullptr;
  }
  q.FromArgument = substituted;
  slot = q;
  return inner;
}

// `atomic T`: T is a number, a `bool`, a `char` or a fixed array of them.
ast::Type *Sema::resolveAtomicAnnotation(ast::AtomicType *at,
                                         ast::SourceLocation loc,
                                         const TypeContext &context,
                                         const TypeFlags & /*flags*/) {
  // An array under `atomic` is the cell's contents, held in the object.
  FixedArrayAllowed = ast::isa<ast::FixedArrayType>(at->getInnerType());
  ast::Type *inner = resolveType(at->getInnerType(), loc, context);
  if (!inner)
    return nullptr;
  ast::Type *element = inner;
  if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(inner))
    element = ft->getElementType();
  auto *bt = ast::dyn_cast<ast::BuiltinType>(element);
  if (!bt || bt->getTypeKind() == ast::BuiltinType::Void) {
    std::string msg = "'atomic' holds a number, a 'bool', a 'char' or a "
                      "fixed array of them, not '";
    msg += typeName(inner);
    msg += "'";
    if (ast::isRefType(inner)) {
      // An object crosses threads frozen or behind a lock.
      msg += ": an object crosses threads as 'atomic shared ";
      msg += typeName(inner);
      msg += "' (frozen) or 'locked mut ";
      msg += typeName(inner);
      msg += "'";
    }
    error(loc, msg);
    return nullptr;
  }
  return Ctx.getAtomicType(inner);
}

ast::Type *Sema::resolveClassNameAnnotation(ast::ClassType *ct,
                                            ast::SourceLocation loc,
                                            const TypeContext &context,
                                            const TypeFlags & /*flags*/) {
  // `Self` in a method is its class or struct (in a trait, the
  // implementing type).
  if (ct->getName() == names::kSelfType) {
    ast::Type *self = SelfType;
    if (!self && CurrentClassCtx)
      self = CurrentClassCtx->ClassType
                 ? static_cast<ast::Type *>(CurrentClassCtx->ClassType)
                 : CurrentClassCtx->Struct;
    if (self)
      return self;
  }
  if (isImportClashUse(ct->getName()))
    return nullptr;
  // A private type of an imported module, spelled bare: it is that
  // module's own (a qualified spelling is reported below).
  if (names::isModulePrivateName(ct->getName()) &&
      ct->getName().find(names::kQualSep) == std::string::npos &&
      ct->getName().find('<') == std::string::npos) // not an instantiation
    if (auto o = ImportedTypeOrigins.find(ct->getName());
        o != ImportedTypeOrigins.end()) {
      error(loc,
            "'" + ct->getName() + "' is private to module '" + o->second + "'");
      return nullptr;
    }
  // The parser emits a ClassType stub for any unknown type name; it may in
  // fact be an enum or a struct, which are resolved here before falling
  // back to classes.
  if (auto *enumTy = Ctx.lookupEnumType(ct->getName()))
    return enumTy;
  if (auto *structTy = Ctx.lookupStructType(ct->getName()))
    return structTy;
  if (auto *canonical = Ctx.lookupClassType(ct->getName()))
    return canonical;
  if (ClassTemplates.count(ct->getName()) ||
      StructTemplates.count(ct->getName()) ||
      EnumTemplates.count(ct->getName())) {
    error(loc, context.str() + " names generic " +
                   (ClassTemplates.count(ct->getName())    ? "class '"
                    : StructTemplates.count(ct->getName()) ? "struct '"
                                                           : "enum '") +
                   ct->getName() + "' without type arguments (write '" +
                   ct->getName() + "<...>')");
    return nullptr;
  }
  if (Traits.count(ct->getName())) {
    error(loc, context.str() + " names trait '" + ct->getName() +
                   "', which is not a type (use a type parameter bounded "
                   "by it: '<T: " +
                   ct->getName() + ">')");
    return nullptr;
  }
  if (reportPrivateImport(ct->getName(), loc))
    return nullptr;
  if (!isFailedImportUse(ct->getName()))
    error(loc,
          context.str() + " has unknown class type '" + ct->getName() + "'");
  return nullptr;
}

// `T[]` is a slice, the type of a `view` or `inout` parameter only.
ast::Type *Sema::resolveSliceAnnotation(ast::ArrayType *at,
                                        ast::SourceLocation loc,
                                        const TypeContext &context,
                                        const TypeFlags &flags) {
  if (!flags.Slice) {
    std::string msg = context.str() + " has type '" + typeName(at) +
                      "': a slice can only be a 'view' or 'inout' parameter";
    if (flags.NoArray) {
      msg += ", and ";
      msg += flags.NoArray;
      msg += " cannot be an array yet";
    } else if (flags.Fixed) {
      // Here a fixed-size array would do.
      const std::string elem = typeName(at->getElementType());
      msg +=
          ", and an array has a length ('" +
          (ast::isa<ast::FunctionType>(at->getElementType()) ? "(" + elem + ")"
                                                             : elem) +
          "[3]')";
    }
    error(loc, msg);
    return nullptr;
  }
  FixedArrayAllowed = true; // a slice's elements may be fixed-size arrays
  const TypeContext element(context, {" element"});
  ast::Qualifier elemQual;
  auto *elemTy = resolvePart(at->getElementType(), loc, element,
                             PartKind::Element, elemQual);
  if (!elemTy)
    return nullptr;
  if (elemTy == Ctx.getVoidTy()) { // it has no value (resolveValueType)
    error(loc, context.str() + " has type 'void[]': an array element "
                               "cannot have type 'void'");
    return nullptr;
  }
  if (fixedElementBytes(elemTy) < 0) {
    error(loc, context.str() + " has type '" + typeName(elemTy) +
                   "[]': a slice of '" + typeName(elemTy) +
                   "' is not supported yet");
    return nullptr;
  }
  return Ctx.getSliceType(elemTy, elemQual);
}

ast::Type *Sema::resolveFixedArrayAnnotation(ast::FixedArrayType *ft,
                                             ast::SourceLocation loc,
                                             const TypeContext &context,
                                             const TypeFlags &flags) {
  // An array of arrays is one type, checked as a whole; the innermost
  // array holds its elements' qualifier (a resolved one's is kept).
  std::vector<int64_t> lengths;
  ast::Type *elem = ft;
  ast::Qualifier held;
  while (auto *f = ast::dyn_cast<ast::FixedArrayType>(elem)) {
    lengths.push_back(f->getLength());
    held = f->getElemQual();
    elem = f->getElementType();
  }
  elem = Ctx.getQualifiedType(held, elem);
  const TypeContext element(context, {" element"});
  ast::Qualifier elemQual;
  auto *base = resolvePart(elem, loc, element, PartKind::Element, elemQual);
  if (!base)
    return nullptr;
  ast::Type *fixed = base;
  for (auto it = lengths.rbegin(); it != lengths.rend(); ++it)
    fixed = Ctx.getFixedArrayType(
        fixed, *it, it == lengths.rbegin() ? elemQual : ast::Qualifier{});
  if (base == Ctx.getVoidTy()) { // it has no value (resolveValueType)
    error(loc, context.str() + " has type '" + typeName(fixed) +
                   "': an array element cannot have type 'void'");
    return nullptr;
  }
  if (!flags.Fixed) {
    errorFixedArrayHere(loc, fixed, context.str());
    return nullptr;
  }
  if (fixedElementBytes(base) < 0) {
    error(loc, context.str() + " has type '" + typeName(fixed) +
                   "': a fixed-size array of '" + typeName(base) +
                   "' is not supported yet");
    return nullptr;
  }
  // Its size is checked once its lengths are inferred (visitVarDecl).
  if (hasInferredLength(fixed)) {
    if (flags.Inferred)
      return fixed;
    error(loc, context.str() + " has type '" + typeName(fixed) +
                   "': a length can be '_' only in a variable declaration "
                   "with an initializer, which gives it");
    return nullptr;
  }
  if (fixedElementBytes(fixed) < 0) {
    error(loc, context.str() + " has type '" + typeName(fixed) +
                   "', which is too big: a fixed-size array holds at most " +
                   std::to_string(kMaxFixedArrayElements) + " elements");
    return nullptr;
  }
  return fixed;
}

ast::Type *Sema::resolveGenericAnnotation(ast::GenericType *gt,
                                          ast::SourceLocation loc,
                                          const TypeContext &context,
                                          const TypeFlags & /*flags*/) {
  // Generic type application `Box<int>`: resolve the arguments to canonical
  // types, then instantiate (or fetch the cached instantiation of) the class
  // template.  The result is an ordinary ClassType.
  std::vector<ast::Type *> args;
  for (size_t i = 0; i < gt->getNumArgs(); ++i) {
    const std::string n = std::to_string(i + 1);
    const TypeContext argument(
        context, {" type argument ", n, " of '", gt->getName(), "'"});
    // A qualified argument is part of the instance (#368):
    // `List<shared Node>` is not `List<Node>`.
    auto *arg = resolveTypeArgument(gt->getArgs()[i], loc, argument);
    if (!arg)
      return nullptr;
    args.push_back(arg);
  }
  // The template's own declarations are no part of this type: their
  // qualifiers stand (`fn put(o: mut Obj)` of a `Tag<int>` in a tuple),
  // and are theirs, not this annotation's.
  const size_t savedBase = std::exchange(PartBase, PartFrames.size());
  const ast::Qualifier savedQual = LastQualifier;
  ast::Type *inst = nullptr;
  if (StructTemplates.count(gt->getName()))
    inst = instantiateStruct(gt->getName(), args, loc, gt);
  else if (EnumTemplates.count(gt->getName()))
    inst = instantiateEnum(gt->getName(), args, loc, gt);
  else
    inst = instantiateClass(gt->getName(), args, loc, gt);
  PartBase = savedBase;
  LastQualifier = savedQual;
  return inst;
}

ast::Type *Sema::resolveOptionalAnnotation(ast::OptionalType *ot,
                                           ast::SourceLocation loc,
                                           const TypeContext &context,
                                           const TypeFlags &flags) {
  auto *inner = resolveType(ot->getInnerType(), loc, context);
  if (!inner)
    return nullptr;
  // A `T?` of a reference type is T's own PaykanShared* box with NULL
  // meaning None; a primitive's (`int?`, `float?`, `bool?`, `char?`) or an
  // enum's is a value, the payload and a presence flag (#96); a tuple's or
  // a struct's is held in place, a presence flag and the value.  `void` has
  // no value (the parser rejects the spelled `void?`; a type argument can
  // still produce it).  Nested optionals are rejected because a `T??` would
  // need a second None to distinguish `None` from `Some(None)` (a
  // template's `T?` of an optional T is that optional: ASTCloner).
  if (inner == Ctx.getVoidTy()) {
    error(loc, context.str() + " has type 'void?': optional type 'void?' "
                               "is not supported");
    return nullptr;
  }
  if (ast::isa<ast::OptionalType>(inner)) {
    error(loc, context.str() + " has nested optional type '" + typeName(inner) +
                   "?', which is not supported");
    return nullptr;
  }
  // An optional of a tuple or a struct is held in place, the presence flag
  // and the value: a value of more than kOptionalValueLimit bytes goes in
  // a `memory::Box`, and that is made optional.  A struct's size is known
  // once populateStructs ran: a class's fields and signatures, resolved
  // before, are checked then.
  if (ast::isMemType(inner) && !ast::isFunctionValue(inner)) {
    if (!StructsPopulated)
      DeferredOptionalChecks.push_back(
          {inner, loc, context.str(), CurrentOrigin, InstantiationStack});
    else if (!checkOptionalValueSize(inner, loc, context))
      return nullptr;
  }
  // Resolve to the context's canonical (interned) instance, as for arrays.
  return checkedType(flags, Ctx.getOptionalType(inner));
}

ast::Type *Sema::resolveFunctionAnnotation(ast::FunctionType *fnt,
                                           ast::SourceLocation loc,
                                           const TypeContext &context,
                                           const TypeFlags &flags) {
  // `(int, view Str) -> bool`: the parameters as a function's (a slice
  // for a `view` or `inout` one, a fixed-size array for any), the result
  // any type but a fixed-size array; then the canonical instance.
  // Each part's qualifier is part of the type (#368): a resolved one's
  // are kept.
  std::vector<ast::Type *> params;
  std::vector<ast::ParamMode> modes;
  std::vector<ast::Qualifier> paramQuals;
  for (size_t i = 0; i < fnt->getNumParams(); ++i) {
    const std::string n = std::to_string(i + 1);
    const TypeContext what(context, {" parameter ", n});
    FixedArrayAllowed = true;
    SliceAllowed = fnt->getMode(i) != ast::ParamMode::Value;
    ast::Qualifier q;
    auto *pty = resolvePart(
        Ctx.getQualifiedType(fnt->getParamQual(i), fnt->getParamType(i)), loc,
        what, PartKind::FunctionPart, q);
    if (!pty)
      return nullptr;
    if (pty == Ctx.getVoidTy()) {
      error(loc, what.str() + " cannot have type 'void'");
      return nullptr;
    }
    // A borrow is written with no qualifier, as a function's parameter
    // is; a template's `view T` keeps T's.
    if (!checkBorrowQualifier(q, fnt->getMode(i), what.str(), loc))
      return nullptr;
    params.push_back(pty);
    modes.push_back(fnt->getMode(i));
    paramQuals.push_back(q);
  }
  FixedArrayAllowed = true; // a result may be a fixed-size array
  const TypeContext result(context, {" result"});
  ast::Qualifier resultQual;
  auto *ret = resolvePart(
      Ctx.getQualifiedType(fnt->getResultQual(), fnt->getReturnType()), loc,
      result, PartKind::FunctionPart, resultQual);
  if (!ret)
    return nullptr;
  return checkedType(
      flags, Ctx.getFunctionType(std::move(params), std::move(modes), ret,
                                 std::move(paramQuals), resultQual));
}

ast::Type *Sema::resolveTupleAnnotation(ast::TupleType *tt,
                                        ast::SourceLocation loc,
                                        const TypeContext &context,
                                        const TypeFlags &flags) {
  std::vector<ast::Type *> elems;
  std::vector<ast::Qualifier> quals;
  elems.reserve(tt->getArity());
  quals.reserve(tt->getArity());
  for (size_t i = 0; i < tt->getArity(); ++i) {
    NoArrayHolder = "a tuple element";
    const std::string n = std::to_string(i);
    const TypeContext element(context, {" element ", n});
    ast::Qualifier q;
    auto *elemTy = resolvePart(
        Ctx.getQualifiedType(tt->getElemQual(i), tt->getElementType(i)), loc,
        element, PartKind::Element, q);
    if (!elemTy)
      return nullptr;
    if (elemTy == Ctx.getVoidTy()) {
      error(loc, element.str() + " cannot have type 'void'");
      return nullptr;
    }
    elems.push_back(elemTy);
    quals.push_back(q);
  }
  // Same canonicalisation as arrays: the interned per-element-list node,
  // its elements' qualifiers part of it (#368).
  return checkedType(flags,
                     Ctx.getTupleType(std::move(elems), std::move(quals)));
}

bool Sema::checkNotVoidValue(ast::Type *ty, const std::string &name,
                             ast::SourceLocation loc) {
  if (ty != Ctx.getVoidTy())
    return true;
  error(loc, "variable '" + name +
                 "' cannot have type 'void' (the expression produces no "
                 "value)");
  return false;
}

ast::Type *Sema::resolveValueType(ast::Type *ty, ast::SourceLocation loc,
                                  const TypeContext &context) {
  ast::Type *resolved = resolveType(ty, loc, context);
  if (resolved != Ctx.getVoidTy())
    return resolved;
  error(loc, context.str() + " cannot have type 'void'");
  return nullptr;
}

// Check that a variable is declared.
ast::Type *Sema::checkIdentLive(std::string_view name,
                                ast::SourceLocation loc) {
  auto *ty = CurrentScope->lookup(name);
  if (!ty) {
    error(loc, "use of undeclared variable '" + std::string(name) + "'");
    return nullptr;
  }
  // A poisoned variable: its declaration failed and was reported there.  The
  // use is absorbed silently, and so is every expression built on it (each
  // check already stays quiet for an operand that has no type).
  if (ast::isa<ast::PoisonType>(ty)) {
    ++SuppressedFollowOns;
    return nullptr;
  }
  return ty;
}

void Sema::declarePoisoned(Scope *scope, std::string_view name) {
  scope->set(name, Ctx.getPoisonTy());
  ++PoisonedBindings;
}

bool Sema::checkBinderName(const std::string &name, ast::SourceLocation loc,
                           bool binds, std::string_view what) {
  if (isTypeNameForVariable(name)) {
    error(loc, "'" + name + "' is a type name and cannot be used as " +
                   std::string(what));
    return false;
  }
  if (binds && isFunctionNameForVariable(name)) {
    error(loc, "'" + name +
                   "' is a function of this module and cannot be used as " +
                   std::string(what));
    return false;
  }
  return true;
}

bool Sema::checkNotSelf(const std::string &name, ast::SourceLocation loc) {
  if (name != names::kSelf || !CurrentClassCtx ||
      CurrentClassCtx->MethodName.empty())
    return true;
  ast::Type *owner = CurrentClassCtx->ClassType;
  if (!owner)
    owner = CurrentClassCtx->Struct;
  error(loc, "'" + std::string(names::kSelf) +
                 "' cannot be used as a variable name in method '" +
                 CurrentClassCtx->MethodName + "' of " + typeKindWord(owner) +
                 " '" + typeName(owner) + "'; it is the implicit receiver");
  return false;
}

bool Sema::checkDistinctParams(const ast::FuncDecl *fn,
                               const std::string &what) {
  StringSet seen;
  for (const ast::Param &p : fn->getParams())
    if (!seen.insert(p.getName()).second) {
      error(p.Loc, "duplicate parameter '" + p.getName() + "' in " + what);
      return false;
    }
  return true;
}

/// Whether @p e, the value of a `[v; n]` literal, holds no reference that n
/// copies would share: None, or an enum's variant without fields.
static bool holdsNothingToCopy(const ast::Expr *e) {
  if (ast::isa<ast::NoneLiteral>(e))
    return true;
  const auto *ev = ast::dyn_cast<ast::EnumValueExpr>(e);
  ast::Type *ty =
      ev ? const_cast<ast::EnumValueExpr *>(ev)->getResolvedType() : nullptr;
  const auto *et = ty ? ast::dyn_cast<ast::EnumType>(ty) : nullptr;
  if (!et)
    return false;
  const int64_t i = et->findVariant(ev->getVariantName());
  return i >= 0 && !et->getPayload(static_cast<size_t>(i));
}

bool Sema::repeatsAPlace(const ast::ArrayLiteralExpr *node, ast::Type *elemTy) {
  if (!node->isRepeat() || node->getLength() <= 1)
    return false;
  // A place in a form that copies (a `shared` reference) is the copy
  // rule's: it may not be an element (checkQualifiedFlow).
  const ast::Expr *v = node->getElements().front();
  return isCopyPlace(v) && !holdsNothingToCopy(v) && !isCopyable(elemTy) &&
         !isCopyablePart(elemTy, qualifierOf(v));
}

bool Sema::checkRepeatValue(ast::ArrayLiteralExpr *node, ast::Type *elemTy,
                            const MovedMap &before) {
  if (!node->isRepeat() || node->getLength() <= 1)
    return true;
  ast::Expr *v = node->getElements().front();
  if (holdsNothingToCopy(v) || isCopyable(elemTy) ||
      (isCopyPlace(v) && isCopyablePart(elemTy, qualifierOf(v))))
    return true; // evaluated once, and copied (or not an element's form)
  node->setEachElement(true);
  const std::string n = std::to_string(node->getLength());
  const std::string each =
      "'[v; n]' evaluates its value for each of its " + n + " elements";
  if (repeatsAPlace(node, elemTy)) {
    noteArchetypeCopy(v->getLocation(), elemTy, "to repeat it");
    std::string msg = each + ", and '" + placeText(v) + "' (a '" +
                      typeName(elemTy) +
                      "') is not Copyable: write a value made for each";
    if (isCloneable(elemTy))
      msg += ", such as '[" + placeText(v) + ".clone(); " + n + "]'";
    error(v->getLocation(), msg);
    return false;
  }
  // A `move` in it would move its variable once per element.
  const ast::Expr *moved = nullptr;
  if (const auto *u = ast::dyn_cast<ast::UnaryExpr>(v);
      u && u->getOpcode() == ast::UnaryOpcode::Move)
    moved = u->getOperand();
  std::string name = moved ? placeText(moved) : std::string();
  ast::SourceLocation at = v->getLocation();
  for (const auto &[key, state] : MovedVars) {
    if (!name.empty())
      break;
    auto it = before.find(key);
    if (it == before.end() || it->second.At != state.At) {
      name = std::string(keyName(key));
      at = state.At;
    }
  }
  if (!name.empty()) {
    noteArchetypeCopy(at, elemTy, "to repeat it");
    error(at, each + ", so it cannot move '" + name + "' (that would move it " +
                  n + " times)");
    return false;
  }
  return true;
}

/// Whether @p e names storage: a variable, a field, an element or a tuple's
/// element (not a temporary, and not `move x`).
static bool isPlaceExpr(const ast::Expr *e) {
  if (const auto *id = ast::dyn_cast<ast::Identifier>(e))
    return !id->isFunctionRef();
  return ast::isa<ast::MemberAccessExpr>(e) ||
         ast::isa<ast::SubscriptExpr>(e) || ast::isa<ast::TupleIndexExpr>(e);
}

/// Whether a `match` on @p e reads storage (#368): a place, a call's
/// `inout` or `view` result, or a conditional with a branch that is one.
/// A binding of a move-only part of it is a borrow of that storage, never
/// a copy, and none of it is a new value.
bool Sema::readsStorage(const ast::Expr *e) const {
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e))
    return readsStorage(te->getTrueExpr()) || readsStorage(te->getFalseExpr());
  return isPlaceExpr(e) || e->isInoutResult() || borrowResultOf(e);
}

bool Sema::bindArmName(ast::MatchArm *arm, const ast::Expr *subject,
                       ast::Type *bindTy, ast::Qualifier written) {
  const std::string &name = arm->getBinding();
  std::string msg;
  arm->setBindsUncounted(false);
  // The binding holds its subject's form (#368: on a `shared Obj`, `p:
  // Node` binds a `shared Node`), which a qualifier written on the arm must
  // be; a borrow's arm takes none written, but a type argument's.
  const ast::Qualifier form = qualifierOf(subject);
  if (!written.isUnique() && arm->getMode() != ast::ParamMode::Value &&
      !written.FromArgument) {
    error(arm->getLocation(),
          std::string("'") + ast::paramModeName(arm->getMode()) + "' arm '" +
              name + "' takes no qualifier ('" + written.spelling() +
              "'): a borrow binds the subject whatever its form");
    declarePoisoned(CurrentScope, name);
    return false;
  }
  if (!checkArmForm(subject, bindTy, written, "'" + name + "' binds",
                    arm->getLocation())) {
    declarePoisoned(CurrentScope, name);
    return false;
  }
  // A conditional is held in its true branch's form (qualifierOf), which
  // each branch must have, a `None` aside: a frozen object is never bound
  // `shared mut`, a `shared mut` one never `atomic shared`.  A new object
  // may become `shared` (only a whole conditional is made a cell or
  // frozen); storage of a unique one never does.
  if (ast::isRefType(bindTy) && ast::isa<ast::TernaryExpr>(subject)) {
    std::string other;
    std::function<void(const ast::Expr *)> branches = [&](const ast::Expr *b) {
      if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(b)) {
        branches(te->getTrueExpr());
        branches(te->getFalseExpr());
        return;
      }
      if (ast::isa<ast::NoneLiteral>(b) || !other.empty())
        return;
      const ast::Qualifier q = qualifierOf(b);
      const bool same = q.Form == form.Form &&
                        (q.Form == ast::QualForm::Unique || q.Mut == form.Mut);
      const bool fresh = q.Form == ast::QualForm::Unique &&
                         form.Form == ast::QualForm::Shared && !readsStorage(b);
      if (!same && !fresh)
        other = qualifiedName(q, const_cast<ast::Expr *>(b)->getResolvedType());
    };
    branches(subject);
    if (!other.empty()) {
      error(arm->getLocation(),
            "'" + name +
                "' binds part of a conditional whose branches have two "
                "forms, '" +
                qualifiedName(
                    form, const_cast<ast::Expr *>(subject)->getResolvedType()) +
                "' and '" + other + "': a binding holds one");
      declarePoisoned(CurrentScope, name);
      return false;
    }
  }
  if (!checkBinderName(name, arm->getLocation()) ||
      !checkNotSelf(name, arm->getLocation())) {
    // Reported.
  } else if (arm->getMode() == ast::ParamMode::Inout) {
    // `n: inout T` is another name for the subject's storage, as an `inout`
    // local is for what it names, a call's `inout` result too; a T written
    // through it fits there.
    const std::string what = "'inout' arm '" + name + "'";
    if (!ast::isa<ast::Identifier>(subject) &&
        !ast::isa<ast::MemberAccessExpr>(subject) &&
        !ast::isa<ast::SubscriptExpr>(subject) && !isInoutResult(subject))
      msg = what + " needs a variable or a field to match on";
    else if (ast::isa<ast::BuiltinType>(bindTy) ||
             ast::isa<ast::EnumType>(bindTy))
      msg = what + " cannot change the '" + typeName(bindTy) +
            "' inside an optional yet";
    else
      msg = inoutPlaceError(subject, "bound by " + what);
    if (msg.empty() && !checkParallelWrite(subject, nullptr, "bound 'inout'",
                                           arm->getLocation())) {
      // Reported.
    } else if (msg.empty()) {
      // The storage's writes; a `shared mut` subject's binding is that
      // form (#368), so `mem::replace` through it trades shared references.
      CurrentScope->Kinds[name] = VarKind::Inout;
      CurrentScope->Quals[name] =
          form.Form == ast::QualForm::Unique
              ? ast::Qualifier{ast::QualForm::Unique, true}
              : form;
      return true;
    }
    if (!msg.empty())
      error(subject->getLocation(), msg);
  } else {
    // `n: view T` is a `view` local of the subject, whatever the subject is,
    // reading it in its form (#368).
    bool view = true;
    if (arm->getMode() == ast::ParamMode::View) {
      CurrentScope->Kinds[name] = VarKind::ViewLocal;
    } else if (MatchSubjectIsView ||
               (readsStorage(subject) && !isCopyablePart(bindTy, form))) {
      // A binding of a `view` subject is a `view`; so is a plain binding of
      // a move-only part of a place (the copy rule): the place keeps its
      // value, and `match move x` binds owned parts instead.
      CurrentScope->Kinds[name] = VarKind::ViewBinding;
      CurrentScope->ViewSubjects[name] = subject;
    } else {
      view = false;
    }
    // A `view` of a subject that is a borrow itself counts nothing (#368):
    // the subject's holder keeps the object for the whole arm.  A binding
    // that is a copy (a `shared` part, of a `shared fn`'s `self` too) may
    // be assigned, moved or passed `inout`, and is counted.
    if (view)
      if (const auto *id = ast::dyn_cast<ast::Identifier>(subject)) {
        const VarKind k = varKind(id->getName());
        arm->setBindsUncounted((id->getName() == names::kSelf &&
                                CurrentClassCtx && !CurrentClassCtx->Struct) ||
                               k == VarKind::View || k == VarKind::ViewLocal ||
                               k == VarKind::ViewBinding);
      }
    // A plain binding is a copy of the subject's reference and keeps its
    // form (the prototype): a `mut` subject's object stays writable.  A
    // borrow's keeps it too, for what it reads (a type argument's form, a
    // clone of which is its copy in a template's code).
    if (!form.isUnique())
      CurrentScope->Quals[name] = form;
    return true;
  }
  // Its uses in the arm's body are follow-ons of the reported error.
  declarePoisoned(CurrentScope, arm->getBinding());
  return false;
}

bool Sema::checkArmForm(const ast::Expr *subject, ast::Type *bindTy,
                        ast::Qualifier written, const std::string &who,
                        ast::SourceLocation loc) {
  if (written.isUnique() || !ast::isRefType(bindTy))
    return true;
  // An `inout` result names writable storage, as a `mut` place is; so
  // does a conditional of such branches.  A `None` branch is no storage,
  // and the binding is then immutable (qualifierOf), as a `mut` would not be.
  ast::Qualifier form = qualifierOf(subject);
  std::function<bool(const ast::Expr *)> writable = [&](const ast::Expr *b) {
    if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(b))
      return writable(te->getTrueExpr()) && writable(te->getFalseExpr());
    return b->isInoutResult() || qualifierOf(b).Mut;
  };
  if (form.Form == ast::QualForm::Unique && writable(subject))
    form.Mut = true;
  if (written.Form == form.Form && written.Mut == form.Mut)
    return true;
  error(loc,
        who + " part of a '" +
            qualifiedName(form,
                          const_cast<ast::Expr *>(subject)->getResolvedType()) +
            "': its type is '" + qualifiedName(form.form(), bindTy) +
            "', not '" + qualifiedName(written.form(), bindTy) + "'");
  return false;
}

bool Sema::isTypeNameForVariable(const std::string &name) const {
  ast::BuiltinType::Kind kind;
  return Ctx.lookupClassType(name) || Ctx.lookupEnumType(name) ||
         Ctx.lookupStructType(name) || ClassTemplates.count(name) ||
         StructTemplates.count(name) || EnumTemplates.count(name) ||
         (CurrentTypeParams && CurrentTypeParams->count(name)) ||
         name == names::kObj || name == names::kString ||
         (ast::builtinKindOfName(name, kind) && kind != ast::BuiltinType::Void);
}

bool Sema::rebindsPoisonedFunctionName(const std::string &name) {
  if (!isFunctionNameForVariable(name))
    return false;
  Scope *owner = CurrentScope->findOwner(name);
  if (!owner || !ast::isa<ast::PoisonType>(owner->lookup(name)))
    return false;
  ++SuppressedFollowOns;
  return true;
}

bool Sema::isFunctionNameForVariable(const std::string &name) const {
  // Another module's template names its own module's functions under that
  // module's hidden qualifier; this module's are unqualified.
  if (!CurrentOrigin)
    return ModuleFunctionNames.count(name) != 0;
  const std::string key =
      hiddenQualifier(CurrentOrigin->Module) + names::kQualSep + name;
  return FunctionTable.count(key) || FuncTemplates.count(key);
}

// Visit an expression and return its resolved type (nullptr on error).
ast::Type *Sema::resolveExprType(ast::Expr *expr) { return EC.visit(expr); }
ast::Type *Sema::resolveExprType(ast::Expr *expr, ast::Type *expected) {
  return EC.visitExpecting(expr, expected);
}

// -- ExprVisitor

bool Sema::ExprChecker::isNumericLiteral(const ast::Expr *e) {
  // `~5` is one, as `-5` is, but not `-~5` or `~~5`
  // (docs/design/bitwise.md).
  if (const auto *u = ast::dyn_cast<ast::UnaryExpr>(e);
      u && u->getOpcode() == ast::UnaryOpcode::BitNot)
    return ast::isa<ast::IntegerLiteral>(u->getOperand());
  for (const auto *u = ast::dyn_cast<ast::UnaryExpr>(e); u;
       u = ast::dyn_cast<ast::UnaryExpr>(e)) {
    if (u->getOpcode() != ast::UnaryOpcode::Neg)
      return false;
    e = u->getOperand();
  }
  return ast::isa<ast::IntegerLiteral>(e) || ast::isa<ast::FloatLiteral>(e);
}

ast::Type *Sema::ExprChecker::sizedLiteralTarget(const ast::Expr *e,
                                                 ast::Type *expected) {
  if (!expected || !isNumericLiteral(e))
    return nullptr;
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(expected))
    expected = ot->getInnerType();
  auto *bt = ast::dyn_cast<ast::BuiltinType>(expected);
  if (!bt || !bt->isNumeric() || bt->getTypeKind() == ast::BuiltinType::Int)
    return nullptr;
  const ast::Expr *lit = e;
  if (const auto *u = ast::dyn_cast<ast::UnaryExpr>(lit)) {
    // Only an unsigned slot gives `~5` a type: elsewhere it is `~` of an
    // `int`, an error (docs/design/bitwise.md).
    if (u->getOpcode() == ast::UnaryOpcode::BitNot)
      return bt->isUnsigned() ? bt : nullptr;
    lit = u->getOperand();
  }
  // A `float` slot leaves a literal its own type, which `int` promotes to,
  // but for one above INT64_MAX, which is no `int`: that is the `float`
  // (docs/design/bitwise.md).
  if (bt->getTypeKind() == ast::BuiltinType::Float) {
    const auto *il = ast::dyn_cast<ast::IntegerLiteral>(lit);
    return il && il->isAboveInt64Max() ? bt : nullptr;
  }
  // A float literal is never an integer.
  if (bt->isInteger() && ast::isa<ast::FloatLiteral>(lit))
    return nullptr;
  return bt;
}

namespace {

/// The range of the integer kind @p k as magnitudes: -@p loMagnitude to
/// @p hi (@p loMagnitude is 0 for an unsigned kind).
void integerRange(ast::BuiltinType::Kind k, uint64_t &loMagnitude,
                  uint64_t &hi) {
  const unsigned bits = ast::BuiltinType::integerBits(k);
  if (ast::BuiltinType::isUnsignedKind(k)) {
    loMagnitude = 0;
    hi = bits == 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
  } else {
    loMagnitude = uint64_t{1} << (bits - 1);
    hi = loMagnitude - 1;
  }
}

/// The text of the range of the integer kind @p k for a diagnostic:
/// `0 to 255`, `-128 to 127`, `0 to 18446744073709551615`.
std::string integerRangeText(ast::BuiltinType::Kind k) {
  uint64_t lo, hi;
  integerRange(k, lo, hi);
  return (lo ? "-" : "") + std::to_string(lo) + " to " + std::to_string(hi);
}

/// The value of the integer literal @p lit as a sign and a magnitude: a
/// literal above INT64_MAX is its unsigned value, a negative one (a match
/// pattern's, or the AST interchange format's `(int -5)`) its magnitude.
std::pair<bool, uint64_t> literalMagnitude(const ast::IntegerLiteral *lit) {
  if (lit->isAboveInt64Max())
    return {false, lit->getUnsignedValue()};
  if (lit->getValue() < 0)
    return {true, 0 - lit->getUnsignedValue()};
  return {false, lit->getUnsignedValue()};
}

/// The error of the literal @p magnitude, above INT64_MAX, where an `int` is
/// expected: `the literal N does not fit in 'int' (... ; hint)`, with the
/// @p hint after the range when there is one.
std::string wideLiteralText(uint64_t magnitude, const std::string &hint) {
  return "the literal " + std::to_string(magnitude) +
         " does not fit in 'int' (" + integerRangeText(ast::BuiltinType::Int) +
         (hint.empty() ? "" : "; " + hint) + ")";
}

/// The text of a float literal for a diagnostic.
std::string floatText(double value) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%g", value);
  return buf;
}

/// The value of @p e, an integer literal or a negation of one, in decimal
/// for a hint (`1`, `-1`); empty for any other expression.
std::string intLiteralText(const ast::Expr *e) {
  bool negated = false;
  for (const auto *u = ast::dyn_cast<ast::UnaryExpr>(e);
       u && u->getOpcode() == ast::UnaryOpcode::Neg;
       u = ast::dyn_cast<ast::UnaryExpr>(e)) {
    negated = !negated;
    e = u->getOperand();
  }
  const auto *il = ast::dyn_cast<ast::IntegerLiteral>(e);
  if (!il)
    return {};
  const auto [negative, magnitude] = literalMagnitude(il);
  return (negative != negated && magnitude ? "-" : "") +
         std::to_string(magnitude);
}

/// What an operand of a bitwise operator is, for its error's hint.
enum class BitOperand { Unsigned, SignedOrChar, Float, Bool, Other };

BitOperand bitOperand(const ast::Type *ty) {
  const auto *bt = ast::dyn_cast<ast::BuiltinType>(ty);
  if (!bt)
    return BitOperand::Other;
  if (bt->isUnsigned())
    return BitOperand::Unsigned;
  if (bt->isInteger() || bt->getTypeKind() == ast::BuiltinType::Char)
    return BitOperand::SignedOrChar;
  if (bt->isFloat())
    return BitOperand::Float;
  if (bt->getTypeKind() == ast::BuiltinType::Bool)
    return BitOperand::Bool;
  return BitOperand::Other;
}

/// The unsigned type as wide as the signed integer or `char` type @p ty,
/// which a bitwise operator's hint converts it with: `int` gives
/// `unsigned`, `i32` `u32`, `i16` `u16`, `i8` and `char` `u8`.
std::string unsignedOfWidth(const ast::Type *ty) {
  switch (ast::BuiltinType::integerBits(
      ast::cast<ast::BuiltinType>(ty)->getTypeKind())) {
  case 64:
    return ast::builtinKindName(ast::BuiltinType::U64);
  case 32:
    return ast::builtinKindName(ast::BuiltinType::U32);
  case 16:
    return ast::builtinKindName(ast::BuiltinType::U16);
  default: // `i8`, and `char`, which has no integer width
    return ast::builtinKindName(ast::BuiltinType::U8);
  }
}

/// The hint of `unary '~' is not defined for type 'A'`, for an operand of
/// type @p ty (docs/design/bitwise.md, "Errors").
std::string complementHint(const ast::Type *ty) {
  switch (bitOperand(ty)) {
  case BitOperand::Bool:
    return " (for a logical not, write '!')";
  case BitOperand::SignedOrChar:
    return " (the bitwise operators take unsigned operands: convert with '" +
           unsignedOfWidth(ty) + "(...)')";
  case BitOperand::Float:
    return " (the bitwise operators take unsigned operands, not floats)";
  case BitOperand::Unsigned:
  case BitOperand::Other:
    break;
  }
  return {};
}

/// The hint of `operator 'OP' is not defined for types 'A' and 'B'` for
/// @p node, a bitwise operator whose operands have types @p lhsTy and
/// @p rhsTy: the first of docs/design/bitwise.md, "Errors", that applies;
/// empty when none does.
std::string bitwiseHint(const ast::BinaryExpr *node, ast::Type *lhsTy,
                        ast::Type *rhsTy) {
  const ast::BinaryOpcode op = node->getOpcode();
  const BitOperand l = bitOperand(lhsTy);
  const BitOperand r = bitOperand(rhsTy);
  const bool shift =
      op == ast::BinaryOpcode::Shl || op == ast::BinaryOpcode::Shr;
  // `^` of two numbers neither of which is unsigned may be meant as a power.
  auto numericNotUnsigned = [](const ast::Type *ty) {
    const auto *bt = ast::dyn_cast<ast::BuiltinType>(ty);
    return bt && bt->isNumeric() && !bt->isUnsigned();
  };
  const std::string power = op == ast::BinaryOpcode::BitXor &&
                                    numericNotUnsigned(lhsTy) &&
                                    numericNotUnsigned(rhsTy)
                                ? "; '^' is exclusive or, not a power"
                                : "";
  const std::string unsignedOnly =
      " (the bitwise operators take unsigned operands: convert ";
  if (!shift && (l == BitOperand::Bool || r == BitOperand::Bool)) {
    switch (op) {
    case ast::BinaryOpcode::BitAnd:
      return " (for a logical and, write '&&')";
    case ast::BinaryOpcode::BitOr:
      return " (for a logical or, write '||')";
    default:
      return " (for a logical exclusive or, write '!=')";
    }
  }
  if (shift && l == BitOperand::Unsigned && r == BitOperand::SignedOrChar)
    return " (a shift count is unsigned: convert it with 'unsigned(...)')";
  const std::string lhsLiteral = intLiteralText(node->getLHS());
  const std::string rhsLiteral = intLiteralText(node->getRHS());
  if (shift && !lhsLiteral.empty() && r == BitOperand::Unsigned)
    return " (the value shifted takes no type from the count: write its "
           "type, as in 'unsigned(" +
           lhsLiteral + ")')";
  if (!lhsLiteral.empty() && !rhsLiteral.empty())
    return " (two literals are 'int': give one a type, as in 'unsigned(" +
           lhsLiteral + ") " + node->getOpcodeStr() + " " + rhsLiteral + "'" +
           power + ")";
  // A shift's count never decides its result's type, so a signed value
  // takes the unsigned type of its own width (below), never the count's.
  if (!shift && l == BitOperand::Unsigned && r == BitOperand::SignedOrChar)
    return unsignedOnly + "the '" + typeName(rhsTy) + "' with '" +
           typeName(lhsTy) + "(...)')";
  if (!shift && r == BitOperand::Unsigned && l == BitOperand::SignedOrChar)
    return unsignedOnly + "the '" + typeName(lhsTy) + "' with '" +
           typeName(rhsTy) + "(...)')";
  if (l == BitOperand::SignedOrChar || r == BitOperand::SignedOrChar)
    return unsignedOnly + "with '" +
           unsignedOfWidth(l == BitOperand::SignedOrChar ? lhsTy : rhsTy) +
           "(...)'" + power + ")";
  if (l == BitOperand::Float || r == BitOperand::Float)
    return op == ast::BinaryOpcode::BitXor
               ? " ('^' is exclusive or, not a power)"
               : " (the bitwise operators take unsigned operands, not "
                 "floats)";
  return {};
}

/// The hint for a parse method of `Str` spelled with another name of its
/// type (`toI64` for `toInt`): there is one method per type, under its
/// canonical name.  Empty for any other method.
std::string parseAliasHint(const std::string &method) {
  struct Alias {
    const char *Method;
    const char *Hint;
  };
  static constexpr Alias kAliases[] = {
      {"toI64", " (an 'i64' is an 'int': parse it with 'toInt()')"},
      {"toU64", " (a 'u64' is an 'unsigned': parse it with 'toUnsigned()')"},
      {"toF64", " (an 'f64' is a 'float': parse it with 'toFloat()')"},
  };
  for (const Alias &a : kAliases)
    if (method == a.Method)
      return a.Hint;
  return {};
}

/// Whether the finite @p value, rounded to the float kind @p k, stays finite.
bool floatFits(ast::BuiltinType::Kind k, double value) {
  if (std::isinf(value) || std::isnan(value))
    return true; // `inf` and `nan` are not literals; nothing to check
  switch (k) {
  case ast::BuiltinType::F32:
    // Below 2^128 - 2^103, the f32 maximum plus half an ulp: that midpoint
    // itself is a tie, which rounds to even, that is to infinity.
    return std::fabs(value) < 0x1.ffffffp+127;
  case ast::BuiltinType::F16:
    return std::fabs(value) < 65520.0; // rounds to the f16 maximum, 65504
  default:
    return true;
  }
}

} // namespace

ast::Type *Sema::ExprChecker::checkSizedLiteral(ast::Expr *node,
                                                ast::Type *target,
                                                bool isInteger, bool negative,
                                                uint64_t magnitude,
                                                double floatValue) {
  auto *bt = ast::cast<ast::BuiltinType>(target);
  const ast::BuiltinType::Kind k = bt->getTypeKind();
  if (bt->isInteger()) {
    uint64_t lo, hi;
    integerRange(k, lo, hi);
    if (negative ? magnitude > lo : magnitude > hi) {
      S.error(node->getLocation(),
              "the literal " + std::string(negative ? "-" : "") +
                  std::to_string(magnitude) + " does not fit in '" +
                  typeName(bt) + "' (" + integerRangeText(k) + ")");
      return nullptr;
    }
    return bt;
  }
  const double value = !isInteger ? floatValue
                       : negative ? -static_cast<double>(magnitude)
                                  : static_cast<double>(magnitude);
  if (!floatFits(k, value)) {
    S.error(node->getLocation(), "the literal " + floatText(value) +
                                     " does not fit in '" + typeName(bt) + "'");
    return nullptr;
  }
  return bt;
}

ast::Type *Sema::ExprChecker::visitIntegerLiteral(ast::IntegerLiteral *node) {
  if (ast::Type *target = std::exchange(S.LiteralExpectation, nullptr)) {
    const auto [negative, magnitude] = literalMagnitude(node);
    return checkSizedLiteral(node, target, true, negative, magnitude, 0.0);
  }
  // Without a sized context a literal is an `int`, which one above INT64_MAX
  // is not (docs/design/bitwise.md).
  if (node->isAboveInt64Max()) {
    const std::string value = std::to_string(node->getUnsignedValue());
    S.error(node->getLocation(),
            wideLiteralText(node->getUnsignedValue(),
                            "a literal above 9223372036854775807 needs an "
                            "unsigned context, as in 'u: unsigned = " +
                                value + "'"));
    return nullptr;
  }
  return S.Ctx.getIntTy();
}

ast::Type *Sema::ExprChecker::visitFloatLiteral(ast::FloatLiteral *node) {
  if (ast::Type *target = std::exchange(S.LiteralExpectation, nullptr))
    return checkSizedLiteral(node, target, false, false, 0, node->getValue());
  return S.Ctx.getFloatTy();
}

ast::Type *Sema::ExprChecker::visitBoolLiteral(ast::BoolLiteral *) {
  return S.Ctx.getBoolTy();
}

ast::Type *Sema::ExprChecker::visitCharLiteral(ast::CharLiteral *) {
  return S.Ctx.getCharTy();
}

ast::Type *Sema::ExprChecker::visitNoneLiteral(ast::NoneLiteral *) {
  return S.Ctx.getObjTy();
}

ast::Type *Sema::ExprChecker::visitStringLiteral(ast::StringLiteral *) {
  return S.Ctx.getStrTy();
}

ast::Type *Sema::ExprChecker::visitIdentifier(ast::Identifier *node) {
  // Inside an instantiation body, a bare type parameter name is a type, not a
  // value (a local variable of the same name shadows it, as usual).
  if (S.CurrentTypeParams && S.CurrentTypeParams->count(node->getName()) &&
      !(S.CurrentScope && S.CurrentScope->lookup(node->getName()))) {
    S.error(node->getLocation(), "type parameter '" + node->getName() +
                                     "' cannot be used as a value");
    return nullptr;
  }
  // A function used as a value (`f = square;`): its function type.  Inside
  // another module's template, a bare name is that module's function first,
  // as a call's is (ModuleTemplates' Names), not one the importer declares.
  if (!S.CurrentScope->lookup(node->getName())) {
    std::string fnName = node->getName();
    if (S.CurrentOrigin) {
      std::string home = Sema::hiddenQualifier(S.CurrentOrigin->Module) +
                         names::kQualSep + fnName;
      if (S.lookupFunction(home))
        fnName = std::move(home);
    }
    if (const FunctionSig *sig = S.lookupFunction(fnName)) {
      if (!S.checkFunctionValue(fnName, *sig, node->getLocation()) ||
          !S.inferResultOf(fnName, node->getLocation()))
        return nullptr;
      node->setFunctionRef(S.Ctx.intern(fnName));
      auto *fnt = S.functionTypeOf(*sig);
      node->setResolvedType(fnt);
      return fnt;
    }
    if (S.FuncTemplates.count(node->getName())) {
      S.errorGenericFunctionValue(node->getName(), node->getLocation());
      return nullptr;
    }
  }
  S.checkNotBorrowed(node->getName(), node->getLocation());
  ast::Type *ty = S.checkIdentLive(node->getName(), node->getLocation());
  // The concurrency prototype: not after a `move`, only values and handles
  // in a `parallel for` body, and an `atomic` variable's read is its
  // value unless its handle is wanted.
  if (ty && !S.checkNotMoved(node->getName(), node->getLocation()))
    return nullptr;
  if (ty && !S.ParallelStack.empty() &&
      !S.checkParallelRead(node, node->getName(), ty, node->getLocation()))
    return nullptr;
  // Inside a lambda, a variable of the function it is written in is
  // captured (by copy) when it is first used; an `atomic` one too (before
  // its handle or value is taken below).
  if (ty && !S.LambdaStack.empty() &&
      !S.captureVariable(node->getName(), ty,
                         S.CurrentScope->findOwner(node->getName()),
                         node->getLocation()))
    return nullptr;
  if (auto *at = ast::dyn_cast<ast::AtomicType>(ty)) {
    if (std::exchange(S.WantAtomicHandle, false)) {
      node->setAtomicHandle(true);
      return at;
    }
    if (!S.checkAtomicReadable(at, node->getName(), node->getLocation()))
      return nullptr;
    return at->getInnerType();
  }
  return ty;
}

ast::Type *Sema::ExprChecker::visitLambdaExpr(ast::LambdaExpr *node) {
  return S.checkLambda(node);
}

ast::Type *Sema::ExprChecker::visitCallValueExpr(ast::CallValueExpr *node) {
  // `xs[0](1)`: the callee is any expression of function type.
  ast::Type *calleeTy = visit(node->getCallee());
  if (!calleeTy)
    return nullptr;
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(calleeTy)) {
    S.errorOptionalUnwrap(node->getLocation(), ot);
    return nullptr;
  }
  auto *fnt = ast::dyn_cast<ast::FunctionType>(calleeTy);
  if (!fnt) {
    S.error(node->getLocation(), "a value of type '" + typeName(calleeTy) +
                                     "' is not a function value; it cannot "
                                     "be called");
    return nullptr;
  }
  // Named by its type: the callee is an expression.
  ast::Type *ret =
      S.checkFunctionValueCall(fnt, node->getArguments(), typeName(fnt), node);
  if (!ret)
    return nullptr;
  node->setResolvedType(ret);
  return ret;
}

ast::Type *Sema::ExprChecker::visitEnumValueExpr(ast::EnumValueExpr *node) {
  ast::Type *expected = std::exchange(S.VariantExpectation, nullptr);
  if (S.isImportClashUse(node->getEnumName()))
    return nullptr;
  ast::EnumType *enumTy = nullptr;
  if (S.EnumTemplates.count(node->getEnumName())) {
    // `Maybe<int>::Nothing`, or `Maybe::Nothing` flowing into a
    // `Maybe<int>`.
    enumTy =
        genericVariantEnum(node, node->getEnumName(), node->getVariantName(),
                           node->getTypeArgs(), expected, nullptr);
    if (!enumTy)
      return nullptr;
  } else if (node->hasTypeArgs()) {
    // `Box<int>::of` without a call: only a variant is a value with type
    // arguments.
    const std::string &name = node->getEnumName();
    if (S.reportPrivateImport(name, node->getLocation()) ||
        S.isFailedImportUse(name))
      return nullptr;
    std::string msg = "'";
    msg += name;
    if (S.Ctx.lookupEnumType(name)) {
      msg += "' is not a generic enum and takes no type arguments";
    } else {
      msg += "' is not a generic enum: only a generic enum's variant takes "
             "type arguments without a call ('";
      msg += name;
      msg += "<...>";
      msg += names::kQualSep;
      msg += node->getVariantName();
      msg += "(...)' calls a function)";
    }
    S.error(node->getLocation(), msg);
    return nullptr;
  } else {
    enumTy = S.Ctx.lookupEnumType(node->getEnumName());
  }
  if (!enumTy) {
    // `m::f` or `S::f`, an imported function or a `static fn` used as a
    // value: its function type.
    const std::string qualified =
        node->getEnumName() + names::kQualSep + node->getVariantName();
    if (!S.checkStaticMemberUse(qualified, node->getLocation()))
      return nullptr;
    if (const FunctionSig *sig = S.lookupFunction(qualified)) {
      if (!S.checkFunctionValue(qualified, *sig, node->getLocation()) ||
          !S.inferResultOf(qualified, node->getLocation()))
        return nullptr;
      node->setFunctionRef(S.Ctx.intern(qualified));
      auto *fnt = S.functionTypeOf(*sig);
      node->setResolvedType(fnt);
      return fnt;
    }
    if (S.FuncTemplates.count(qualified) || S.isGenericStatic(qualified)) {
      S.errorGenericFunctionValue(qualified, node->getLocation());
      return nullptr;
    }
    if (S.reportPrivateImport(qualified, node->getLocation()) ||
        S.reportPrivateImport(node->getEnumName(), node->getLocation()) ||
        S.isFailedImportUse(node->getEnumName()))
      return nullptr;
    // Under an import's qualifier, a value is one of the module's
    // functions.
    if (auto q = S.ImportQualifiers.find(node->getEnumName());
        q != S.ImportQualifiers.end())
      S.error(node->getLocation(), "'" + qualified +
                                       "' is not a function of module '" +
                                       q->second.Module + "'");
    else
      S.error(node->getLocation(),
              "unknown enum type '" + node->getEnumName() + "'");
    return nullptr;
  }
  int64_t idx = enumTy->findVariant(node->getVariantName());
  if (idx < 0) {
    S.error(node->getLocation(), "enum '" + node->getEnumName() +
                                     "' has no variant '" +
                                     node->getVariantName() + "'");
    return nullptr;
  }
  // A variant with fields is made with them: `E::V(1, "a")`.
  if (const ast::StructType *p = enumTy->getPayload(static_cast<size_t>(idx))) {
    std::string fields;
    for (size_t i = 0; i < p->getNumFields(); ++i)
      fields += (i ? ", " : "") + p->getFieldName(i);
    S.error(node->getLocation(),
            "variant '" + node->getVariantName() + "' of enum '" +
                enumTy->getName() + "' has fields: write '" +
                node->getEnumName() + names::kQualSep + node->getVariantName() +
                "(" + fields + ")'");
    return nullptr;
  }
  node->setResolvedEnum(enumTy);
  node->setValue(idx);
  node->setResolvedType(enumTy);
  return enumTy;
}

ast::EnumType *Sema::ExprChecker::genericVariantEnum(
    ast::Expr *node, const std::string &tmplName, const std::string &variant,
    const std::vector<ast::Type *> &typeArgAnns, ast::Type *expected,
    std::optional<std::vector<ast::Type *>> *argTypes) {
  const ast::SourceLocation loc = node->getLocation();
  const ast::EnumDecl *tmpl = S.EnumTemplates.find(tmplName)->second;
  // The messages name it as the program spells it, not by another module's
  // hidden qualifier (a variant made in that module's template).
  const std::string shown = S.displayName(tmplName);
  const std::string spelled = shown + names::kQualSep + variant;
  // Written: `Maybe<int>::Just(1)`.
  if (!typeArgAnns.empty()) {
    std::vector<ast::Type *> typeArgs;
    for (size_t i = 0; i < typeArgAnns.size(); ++i) {
      auto *t = S.resolveTypeArgument(typeArgAnns[i], loc,
                                      "type argument " + std::to_string(i + 1) +
                                          " of '" + spelled + "'");
      if (!t)
        return nullptr;
      typeArgs.push_back(t);
    }
    return S.instantiateEnum(tmplName, typeArgs, loc, node);
  }
  // The slot's: `m: Maybe<int> = Maybe::Nothing` (or a `Maybe<int>?`).
  if (auto *ot =
          expected ? ast::dyn_cast<ast::OptionalType>(expected) : nullptr)
    expected = ot->getInnerType();
  if (auto *et = expected ? ast::dyn_cast<ast::EnumType>(expected) : nullptr)
    if (const EnumInstantiationInfo *info = S.enumInstantiationInfo(et);
        info && info->Template == tmpl)
      return et;
  // Inferred from the fields: `Maybe::Just(1)` unifies the variant's field
  // types with the arguments' types, as a generic struct's constructor
  // does.
  const auto &variants = tmpl->getVariants();
  size_t idx = 0;
  while (idx < variants.size() && *variants[idx] != variant)
    ++idx;
  if (idx == variants.size()) {
    S.error(loc, "enum '" + shown + "' has no variant '" + variant + "'");
    return nullptr;
  }
  const auto &fields = tmpl->getPayload(idx);
  if (fields.empty()) {
    if (argTypes)
      S.error(loc, "variant '" + variant + "' of enum '" + shown +
                       "' has no fields: write '" + spelled +
                       "' without arguments");
    else
      S.error(loc, "cannot infer the type arguments of generic enum '" + shown +
                       "' for '" + spelled + "': write them ('" + shown +
                       "<...>" + names::kQualSep + variant +
                       "'), or give the value a type ('x: " + shown +
                       "<...> = " + spelled + "')");
    return nullptr;
  }
  auto *call = ast::cast<ast::CallExpr>(node);
  const auto &args = call->getArguments();
  std::vector<ast::Type *> &tys = argTypes->emplace();
  bool ok = true;
  for (auto *a : args) {
    tys.push_back(visit(a));
    if (!tys.back())
      ok = false;
  }
  if (!ok)
    return nullptr;
  if (args.size() != fields.size()) {
    std::string list;
    for (size_t i = 0; i < fields.size(); ++i)
      list += (i ? ", " : "") + fields[i]->getName() + ": " +
              typeName(fields[i]->getType());
    S.error(loc, "variant '" + variant + "' of enum '" + shown + "' has " +
                     std::to_string(fields.size()) + " field(s) (" + list +
                     "), got " + std::to_string(args.size()) + " argument(s)");
    return nullptr;
  }
  // A field type of another module's template, as this context spells it.
  std::vector<ast::Type *> paramTypes;
  paramTypes.reserve(fields.size());
  for (auto *f : fields)
    paramTypes.push_back(
        S.foreignOrigin(tmpl)
            ? ast::ASTCloner(S.Ctx, {}, nullptr, /*foreign=*/true)
                  .cloneType(f->getType())
            : f->getType());
  std::vector<ast::Type *> typeArgs;
  if (!S.inferTypeArgs(
          tmplName, tmpl->getTypeParams(), paramTypes, tys, loc, typeArgs,
          shown + "<...>" + names::kQualSep + variant + "(...)", nullptr,
          nullptr,
          S.inferenceForms(args, [](size_t) { return ast::ParamMode::Value; })))
    return nullptr;
  return S.instantiateEnum(tmplName, typeArgs, loc);
}

ast::Type *
Sema::ExprChecker::checkVariantCall(ast::CallExpr *node, ast::EnumType *enumTy,
                                    const std::string &variant,
                                    const std::vector<ast::Type *> *argTypes) {
  // For the messages only: as the program spells it, not by another
  // module's hidden qualifier (a call in that module's template).
  const std::string callee = S.displayName(node->getCalleeName());
  int64_t idx = enumTy->findVariant(variant);
  if (idx < 0) {
    S.error(node->getLocation(), "enum '" + enumTy->getName() +
                                     "' has no variant '" + variant + "'");
    return nullptr;
  }
  ast::StructType *payload = enumTy->getPayload(static_cast<size_t>(idx));
  if (!payload) {
    S.error(node->getLocation(),
            "variant '" + variant + "' of enum '" + enumTy->getName() +
                "' has no fields: write '" + callee + "' without arguments");
    return nullptr;
  }
  if (S.ErroneousClasses.count(payload->getName())) {
    ++S.SuppressedFollowOns; // its fields were reported
    return nullptr;
  }
  // The arguments are its fields, in order, as a struct's constructor
  // takes them (visited already when they gave its type arguments).
  const std::vector<ast::Type *> &fieldTys = payload->getElementTypes();
  const auto &args = node->getArguments();
  std::vector<ast::Type *> visited;
  if (!argTypes) {
    visited.reserve(args.size());
    for (size_t i = 0; i < args.size(); ++i)
      visited.push_back(
          visitExpecting(args[i], i < fieldTys.size() ? fieldTys[i] : nullptr));
    argTypes = &visited;
  }
  if (args.size() != fieldTys.size()) {
    std::string fields;
    for (size_t i = 0; i < payload->getNumFields(); ++i)
      fields += (i ? ", " : "") + payload->getFieldName(i) + ": " +
                typeName(fieldTys[i]);
    S.error(node->getLocation(),
            "variant '" + variant + "' of enum '" + enumTy->getName() +
                "' has " + std::to_string(fieldTys.size()) + " field(s) (" +
                fields + "), got " + std::to_string(args.size()) +
                " argument(s)");
    return nullptr;
  }
  bool ok = true;
  for (size_t i = 0; i < args.size(); ++i) {
    if (!(*argTypes)[i]) {
      ok = false;
      continue;
    }
    if (!S.checkAssignable(fieldTys[i], (*argTypes)[i], args[i]) &&
        !S.diagnoseOptionalNarrowing(args[i]->getLocation(), fieldTys[i],
                                     (*argTypes)[i])) {
      S.error(args[i]->getLocation(), "field '" + payload->getFieldName(i) +
                                          "' of '" + callee + "' has type '" +
                                          typeName(fieldTys[i]) + "', got '" +
                                          typeName((*argTypes)[i]) + "'");
      ok = false;
    }
  }
  // A `view` is never stored: the fields are values.
  if (!ok || !S.checkViewArgs({}, fieldTys, args, callee))
    return nullptr;
  // Each field owns what it is given (#369): its qualifier, and the copy
  // rule.
  for (size_t i = 0; i < args.size(); ++i)
    if (!S.checkQualifiedFlow(
            payload->findFieldQualifier(payload->getFieldName(i)), args[i],
            "field '" + payload->getFieldName(i) + "' of '" + callee + "'",
            args[i]->getLocation()))
      ok = false;
  if (!ok)
    return nullptr;
  node->setVariant(enumTy, static_cast<size_t>(idx));
  node->setResolvedType(enumTy);
  return enumTy;
}

ast::Type *Sema::ExprChecker::visitUnaryExpr(ast::UnaryExpr *node) {
  ast::Expr *operand = node->getOperand();
  // The concurrency prototype: `await t` is the task's result, `move x`
  // x's value, which x no longer holds.
  if (node->getOpcode() == ast::UnaryOpcode::Await) {
    S.LiteralExpectation = nullptr;
    S.VariantExpectation = nullptr;
    if (!S.ParallelStack.empty()) {
      S.error(node->getLocation(),
              "'await' cannot be used in a 'parallel for' body");
      return nullptr;
    }
    if (S.LockDepth > 0) {
      S.error(node->getLocation(), "'await' cannot be used in a 'lock' "
                                   "block: a wait holding a lock");
      return nullptr;
    }
    if (S.CurrentClassCtx &&
        S.CurrentClassCtx->MethodName == names::kMethodDeinit) {
      S.error(node->getLocation(),
              std::string("'await' cannot be used in '__deinit__': ") +
                  (S.CurrentClassCtx->Struct
                       ? "a value is destroyed where its storage ends"
                       : "an object is destroyed where its last reference "
                         "goes") +
                  ", which waits for no task");
      return nullptr;
    }
    // A task variable awaited in an operand that runs on some paths only
    // (`await t`, `await move t`) is left running on the others.
    const ast::Expr *named = operand;
    if (const auto *mv = ast::dyn_cast<ast::UnaryExpr>(operand);
        mv && mv->getOpcode() == ast::UnaryOpcode::Move)
      named = mv->getOperand();
    if (const auto *task = ast::dyn_cast<ast::Identifier>(named);
        task && S.ConditionalOperand > 0 && !task->isFunctionRef()) {
      ast::Type *held = S.CurrentScope->lookup(task->getName());
      if (held && ast::isa<ast::TaskType>(held)) {
        S.error(node->getLocation(),
                "task '" + task->getName() +
                    "' would be awaited on some paths only: 'await' it "
                    "before the condition");
        // Reported: not again as a task its scope leaves unawaited.
        if (S.CurrentScope->findOwner(task->getName()))
          S.MovedVars[S.varKey(task->getName())] =
              Sema::MovedState{node->getLocation(), false, "await"};
        return nullptr;
      }
    }
    const ast::Expr *savedOperand = std::exchange(S.AwaitOperand, operand);
    ast::Type *t = visit(operand);
    S.AwaitOperand = savedOperand;
    if (!t)
      return nullptr;
    auto *tt = ast::dyn_cast<ast::TaskType>(t);
    if (!tt) {
      S.error(node->getLocation(),
              "'await' takes a task (the result of calling an 'async fn'), "
              "not a '" +
                  typeName(t) + "'");
      return nullptr;
    }
    // Other tasks run while this one waits: what it borrowed `inout` from
    // storage it does not own may change under it.
    S.checkBorrowsAcrossWait(node);
    // `await t` spends the variable's handle (the prototype's typestate):
    // a second await, or any use after, is an error, as after a move.
    if (const auto *id = ast::dyn_cast<ast::Identifier>(operand);
        id && !id->isFunctionRef() && id->getName() != names::kSelf &&
        S.CurrentScope->findOwner(id->getName())) {
      if (!S.checkMovable(id->getName(), node->getLocation(), "await"))
        return nullptr;
      if (std::string why = S.viewedBy(id->getName()); !why.empty()) {
        S.error(node->getLocation(), why + "; it cannot be awaited");
        return nullptr;
      }
      S.MovedVars[S.varKey(id->getName())] =
          Sema::MovedState{node->getLocation(), false, "await"};
    }
    if (auto *qt = ast::dyn_cast<ast::QualifiedType>(tt->getInnerType()))
      return qt->getInnerType();
    return tt->getInnerType();
  }
  if (node->getOpcode() == ast::UnaryOpcode::Move) {
    S.LiteralExpectation = nullptr;
    S.VariantExpectation = nullptr;
    return S.checkMove(node);
  }
  const bool negation = node->getOpcode() == ast::UnaryOpcode::Neg;
  // `-5` flowing into a sized numeric slot: the negated literal takes the
  // slot's type when it fits (`-128` fits an `i8`, `-1` no `u8`).
  if (ast::Type *target = std::exchange(S.LiteralExpectation, nullptr);
      target && negation) {
    if (auto *il = ast::dyn_cast<ast::IntegerLiteral>(operand)) {
      const auto [negative, magnitude] = literalMagnitude(il);
      ast::Type *ty =
          checkSizedLiteral(node, target, true, !negative, magnitude, 0.0);
      if (!ty)
        return nullptr;
      operand->setResolvedType(ty);
      return ty;
    }
    if (auto *fl = ast::dyn_cast<ast::FloatLiteral>(operand)) {
      ast::Type *ty =
          checkSizedLiteral(node, target, false, false, 0, -fl->getValue());
      if (!ty)
        return nullptr;
      operand->setResolvedType(ty);
      return ty;
    }
    // `-(-5)`: the inner negation takes the slot's type.
    S.LiteralExpectation = target;
  } else if (target && node->getOpcode() == ast::UnaryOpcode::BitNot) {
    // `~0x0F` in an unsigned slot (sizedLiteralTarget): the literal is
    // checked against the slot's type, and its complement is within that
    // type's width (docs/design/bitwise.md).
    S.LiteralExpectation = target;
  } else if (auto *il = ast::dyn_cast<ast::IntegerLiteral>(operand);
             il && negation && il->isAboveInt64Max()) {
    // No sized context: a literal above INT64_MAX is no `int`, INT64_MIN's
    // magnitude neither, which is written `-9223372036854775807 - 1`
    // (docs/design/bitwise.md).
    const bool int64Min =
        il->getUnsignedValue() == static_cast<uint64_t>(INT64_MAX) + 1;
    S.error(il->getLocation(),
            wideLiteralText(il->getUnsignedValue(),
                            int64Min ? "write the most negative 'int' as "
                                       "'-9223372036854775807 - 1'"
                                     : ""));
    return nullptr;
  }
  auto *operandTy = visit(node->getOperand());
  if (!operandTy)
    return nullptr;

  // No unary operator is defined on an optional value; point at `match`.
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(operandTy)) {
    S.errorOptionalUnwrap(node->getLocation(), ot);
    return nullptr;
  }

  if (!operandTy->hasUnaryOp(node->getOpcode())) {
    S.error(node->getLocation(),
            "unary '" + std::string(node->getOpcodeStr()) +
                "' is not defined for type '" + typeName(operandTy) + "'" +
                (node->getOpcode() == ast::UnaryOpcode::BitNot
                     ? complementHint(operandTy)
                     : ""));
    return nullptr;
  }

  switch (node->getOpcode()) {
  case ast::UnaryOpcode::Neg:
  case ast::UnaryOpcode::BitNot:
    return operandTy;
  case ast::UnaryOpcode::Not:
    return S.Ctx.getBoolTy();
  case ast::UnaryOpcode::Move:  // handled above
  case ast::UnaryOpcode::Await: // handled above
  case ast::UnaryOpcode::Count:
    break;
  }
  PAYKAN_UNREACHABLE("unknown UnaryOpcode");
}

void Sema::ExprChecker::typeChainBelow(ast::Expr *e) {
  ast::Expr *operand = ast::chainOperand(e);
  if (!operand || !ast::chainOperand(operand) || ChainTypes.count(operand))
    return;
  std::vector<ast::Expr *> links;
  for (ast::Expr *x = operand; ast::chainOperand(x); x = ast::chainOperand(x))
    links.push_back(x);
  // What the caller wants is of the chain's top, typed after these.
  const bool wanted = S.WantAtomicHandle;
  for (auto it = links.rbegin(); it != links.rend(); ++it) {
    // The link that takes this one: a method call on an `atomic` place
    // takes its handle (the prototype).
    ast::Expr *outer = std::next(it) == links.rend() ? e : *std::next(it);
    S.WantAtomicHandle =
        ((ast::isa<ast::MethodCallExpr>(outer) ||
          ast::isa<ast::SubscriptExpr>(outer)) &&
         Sema::isPlaceChain(*it)) ||
        (ast::isa<ast::MethodCallExpr>(outer) && Sema::isElementOfPlace(*it));
    ChainTypes[*it] = visit(*it);
    S.WantAtomicHandle = false;
  }
  S.WantAtomicHandle = wanted;
}

namespace {

// Adds to @p out the names in @p t where inference can bind a type
// parameter (Sema::unifyTypes): a named type, and the parts of a slice,
// tuple, optional, function or generic type.
void inferableNames(ast::Type *t, StringSet &out) {
  if (!t)
    return;
  if (auto *ct = ast::dyn_cast<ast::ClassType>(t)) {
    out.insert(ct->getName());
  } else if (auto *at = ast::dyn_cast<ast::ArrayType>(t)) {
    inferableNames(at->getElementType(), out);
  } else if (auto *tt = ast::dyn_cast<ast::TupleType>(t)) {
    for (ast::Type *el : tt->getElementTypes())
      inferableNames(el, out);
  } else if (auto *ot = ast::dyn_cast<ast::OptionalType>(t)) {
    inferableNames(ot->getInnerType(), out);
  } else if (auto *ft = ast::dyn_cast<ast::FunctionType>(t)) {
    for (ast::Type *p : ft->getParamTypes())
      inferableNames(p, out);
    inferableNames(ft->getReturnType(), out);
  } else if (auto *gt = ast::dyn_cast<ast::GenericType>(t)) {
    for (ast::Type *arg : gt->getArgs())
      inferableNames(arg, out);
  }
}

// Whether @p types name every one of @p params where inference can bind it.
bool inferableFrom(const std::vector<const std::string *> &params,
                   const std::vector<ast::Type *> &types) {
  StringSet named;
  for (ast::Type *t : types)
    inferableNames(t, named);
  return std::all_of(params.begin(), params.end(),
                     [&](const std::string *p) { return named.count(*p); });
}

} // namespace

Sema::ExprChecker::OtherSide
Sema::ExprChecker::needsOtherSide(const ast::Expr *e) const {
  if (const auto *t = ast::dyn_cast<ast::TernaryExpr>(e))
    return std::min(needsOtherSide(t->getTrueExpr()),
                    needsOtherSide(t->getFalseExpr()));
  // A literal takes the other side's element types and forms (#368: `if c
  // then (1, Node(3)) else (2, s)` holds a `shared` element).
  if (ast::isa<ast::TupleLiteralExpr>(e) || ast::isa<ast::ArrayLiteralExpr>(e))
    return OtherSide::Helps;
  if (const auto *ev = ast::dyn_cast<ast::EnumValueExpr>(e))
    return S.EnumTemplates.count(ev->getEnumName()) && !ev->hasTypeArgs()
               ? OtherSide::Needed
               : OtherSide::NotNeeded;
  const auto *call = ast::dyn_cast<ast::CallExpr>(e);
  if (!call || call->hasTypeArgs())
    return OtherSide::NotNeeded;
  const std::string &callee = call->getCalleeName();
  // `Point::none()`, `Box<int>::none()`: a generic `static fn` (#353),
  // whose own type parameters its parameters may leave open.
  if (const ast::FuncDecl *fn = S.genericStaticNamed(callee)) {
    std::vector<ast::Type *> paramTypes;
    for (const ast::Param &p : fn->getParams())
      paramTypes.push_back(p.ParamType);
    return inferableFrom(fn->getTypeParams(), paramTypes) ? OtherSide::NotNeeded
                                                          : OtherSide::Needed;
  }
  if (call->hasOwnerTypeArgs())
    return OtherSide::NotNeeded;
  // `none()`, `res::ok(1)`: a generic function's type parameters that its
  // parameters leave open come from the type its result flows into.
  if (auto fn = S.FuncTemplates.find(callee); fn != S.FuncTemplates.end()) {
    std::vector<ast::Type *> paramTypes;
    for (const ast::Param &p : fn->second->getParams())
      paramTypes.push_back(p.ParamType);
    return inferableFrom(fn->second->getTypeParams(), paramTypes)
               ? OtherSide::NotNeeded
               : OtherSide::Needed;
  }
  // `Maybe::Just(n)`: a generic enum's variant made with fields, which its
  // arguments are typed for without the other side's type.  It needs that
  // type when the fields leave a type parameter open (`Pair::Left(1)` of a
  // `Pair<A, B>`, `Result::Ok(1)`) or an argument needs it
  // (`Maybe::Just(Maybe::Nothing)`); otherwise it may infer them.
  const size_t sep = callee.rfind(names::kQualSep);
  if (sep == std::string::npos)
    return OtherSide::NotNeeded;
  auto tmpl = S.EnumTemplates.find(callee.substr(0, sep));
  if (tmpl == S.EnumTemplates.end())
    return OtherSide::NotNeeded;
  const ast::EnumDecl *ed = tmpl->second;
  const std::string_view variant = std::string_view(callee).substr(sep + 2);
  for (size_t v = 0; v < ed->getNumVariants(); ++v) {
    if (*ed->getVariants()[v] != variant)
      continue;
    std::vector<ast::Type *> fieldTypes;
    for (const ast::VarDecl *f : ed->getPayload(v))
      fieldTypes.push_back(f->getType());
    if (!inferableFrom(ed->getTypeParams(), fieldTypes))
      return OtherSide::Needed;
  }
  for (const ast::Expr *arg : call->getArguments())
    if (needsOtherSide(arg) == OtherSide::Needed)
      return OtherSide::Needed;
  return OtherSide::Helps;
}

ast::Type *Sema::ExprChecker::visitBinaryExpr(ast::BinaryExpr *node) {
  if (node->getOpcode() == ast::BinaryOpcode::Shl ||
      node->getOpcode() == ast::BinaryOpcode::Shr)
    return visitShift(node);
  // A numeric literal operand takes the other operand's sized type (`x + 1`
  // with `x: u8` adds two `u8`), so the non-literal operand is typed first,
  // and so is the operand of `==` or `!=` a generic enum's variant or a
  // generic function's call without type arguments takes them from
  // (`Maybe::Nothing == m`, `res::ok(1) == r`; needsOtherSide).
  ast::Type *lhsTy = nullptr;
  ast::Type *rhsTy = nullptr;
  const bool equality = node->getOpcode() == ast::BinaryOpcode::Eq ||
                        node->getOpcode() == ast::BinaryOpcode::Ne;
  if ((isNumericLiteral(node->getLHS()) && !isNumericLiteral(node->getRHS())) ||
      (equality &&
       needsOtherSide(node->getLHS()) > needsOtherSide(node->getRHS()))) {
    rhsTy = visit(node->getRHS());
    lhsTy = visitOperand(node->getLHS(), rhsTy);
  } else {
    lhsTy = visit(node->getLHS());
    const bool someOnly = node->getOpcode() == ast::BinaryOpcode::And ||
                          node->getOpcode() == ast::BinaryOpcode::Or;
    S.ConditionalOperand += someOnly;
    rhsTy = visitOperand(node->getRHS(), lhsTy);
    S.ConditionalOperand -= someOnly;
  }
  // A `locked Str`'s text is its contents, reached in a `lock` block.
  if (lhsTy == S.Ctx.getStrTy() &&
      !S.checkNotLocked(node->getLHS(), node->getLocation()))
    return nullptr;
  if (rhsTy == S.Ctx.getStrTy() &&
      !S.checkNotLocked(node->getRHS(), node->getLocation()))
    return nullptr;
  if (!lhsTy || !rhsTy)
    return nullptr;

  // Record operand types so the lowering can distinguish reference-typed
  // equality (lowered to a virtual `equals` call) from primitive/enum equality
  // without re-deriving the type.
  node->getLHS()->setResolvedType(lhsTy);
  node->getRHS()->setResolvedType(rhsTy);

  // Optional operands (issue #5).  The only operators defined on a
  // `T?` are == and !=, in exactly two forms:
  //   x == None / None != x   — a null check on the box (the lowering never
  //                             dispatches `equals` for this form);
  //   a == b, both optional   — both None: equal; one None: not equal;
  //                             otherwise the usual `equals` dispatch.
  // Anything else needs the value unwrapped with `match`.
  auto *lhsOpt = ast::dyn_cast<ast::OptionalType>(lhsTy);
  auto *rhsOpt = ast::dyn_cast<ast::OptionalType>(rhsTy);
  if (lhsOpt || rhsOpt) {
    bool isEquality = node->getOpcode() == ast::BinaryOpcode::Eq ||
                      node->getOpcode() == ast::BinaryOpcode::Ne;
    if (!isEquality) {
      S.errorOptionalUnwrap(node->getLocation(), lhsOpt ? lhsOpt : rhsOpt);
      return nullptr;
    }
    if (ast::isa<ast::NoneLiteral>(node->getLHS()) ||
        ast::isa<ast::NoneLiteral>(node->getRHS()))
      return S.Ctx.getBoolTy();
    if (lhsOpt && rhsOpt) {
      ast::Type *li = lhsOpt->getInnerType();
      ast::Type *ri = rhsOpt->getInnerType();
      bool compatible = typesEqual(li, ri) || sameShapeOfParts(li, ri);
      if (!compatible) {
        auto *lc = ast::dyn_cast<ast::ClassType>(li);
        auto *rc = ast::dyn_cast<ast::ClassType>(ri);
        compatible = lc && rc && (lc->isSubtypeOf(rc) || rc->isSubtypeOf(lc));
      }
      if (!compatible) {
        S.error(node->getLocation(),
                "operands of '" + std::string(node->getOpcodeStr()) +
                    "' have mismatched types '" + typeName(lhsTy) + "' and '" +
                    typeName(rhsTy) + "'");
        return nullptr;
      }
      return S.Ctx.getBoolTy();
    }
    // Exactly one side is optional and the other is a present value.
    S.error(node->getLocation(),
            "cannot compare optional '" + typeName(lhsOpt ? lhsTy : rhsTy) +
                "' with non-optional '" + typeName(lhsOpt ? rhsTy : lhsTy) +
                "'; compare against None or unwrap it with match");
    return nullptr;
  }

  // `+` joins two strings: of a `Str` and another value with a text, the
  // error says how to make the other one a `Str`.
  auto concatHint = [&]() -> std::string {
    if (node->getOpcode() != ast::BinaryOpcode::Add ||
        (lhsTy == S.Ctx.getStrTy()) == (rhsTy == S.Ctx.getStrTy()))
      return {};
    ast::Type *other = lhsTy == S.Ctx.getStrTy() ? rhsTy : lhsTy;
    if (!S.hasText(other))
      return {};
    return " (convert the '" + typeName(other) + "' with 'Str(...)')";
  };
  if (!lhsTy->hasBinaryOp(node->getOpcode(), rhsTy)) {
    const bool bitwise = node->getOpcode() == ast::BinaryOpcode::BitAnd ||
                         node->getOpcode() == ast::BinaryOpcode::BitOr ||
                         node->getOpcode() == ast::BinaryOpcode::BitXor;
    S.error(node->getLocation(),
            "operator '" + std::string(node->getOpcodeStr()) +
                "' is not defined for types '" + typeName(lhsTy) + "' and '" +
                typeName(rhsTy) + "'" +
                (bitwise ? bitwiseHint(node, lhsTy, rhsTy) : concatHint()));
    return nullptr;
  }

  // Two numeric operands have one type; the one promotion is `int` with
  // `float` (docs/design/sized-types.md).
  auto *lhsBT = ast::dyn_cast<ast::BuiltinType>(lhsTy);
  auto *rhsBT = ast::dyn_cast<ast::BuiltinType>(rhsTy);
  const bool numericPair =
      lhsBT && rhsBT && lhsBT->isNumeric() && rhsBT->isNumeric();
  ast::Type *promoted = S.promotedOperandType(lhsTy, rhsTy);
  if (numericPair && lhsTy != rhsTy && !promoted) {
    S.error(node->getLocation(),
            "operator '" + std::string(node->getOpcodeStr()) +
                "' is not defined for types '" + typeName(lhsTy) + "' and '" +
                typeName(rhsTy) +
                "' (the operands must have one type: convert one with '" +
                typeName(lhsTy) + "(...)' or '" + typeName(rhsTy) + "(...)')");
    return nullptr;
  }

  // A comparison takes two operands of one type (types are canonical, so
  // `(int, Str) == (int, Str)` unifies while `(int, Str) == (Str, int)`
  // stays an error), or `int` with `float`, compared as floats (`2 ==
  // 2.0`), and equality two classes one of which is the other's subclass.
  // Not two tuples whose elements differ so (`(1, 2) == (1, 2.0)`), nor a
  // char with a number, for `<` as for `==` (`c < 100`).
  const ast::BinaryOpcode op = node->getOpcode();
  const bool comparison =
      op == ast::BinaryOpcode::Lt || op == ast::BinaryOpcode::Gt ||
      op == ast::BinaryOpcode::Le || op == ast::BinaryOpcode::Ge || equality;
  // Values whose parts differ only in their forms compare as their parts
  // do (#368: `(1, s) == (1, Node(1))`): a comparison only reads them.
  if (comparison && !typesEqual(lhsTy, rhsTy) && !promoted &&
      !sameShapeOfParts(lhsTy, rhsTy)) {
    auto *lhsCT = ast::dyn_cast<ast::ClassType>(lhsTy);
    auto *rhsCT = ast::dyn_cast<ast::ClassType>(rhsTy);
    if (!equality || !lhsCT || !rhsCT ||
        (!lhsCT->isSubtypeOf(rhsCT) && !rhsCT->isSubtypeOf(lhsCT))) {
      S.error(node->getLocation(),
              "operands of '" + std::string(node->getOpcodeStr()) +
                  "' have mismatched types '" + typeName(lhsTy) + "' and '" +
                  typeName(rhsTy) + "'");
      return nullptr;
    }
  }

  switch (node->getOpcode()) {
  // Arithmetic: result is float if either operand is float, else int.
  case ast::BinaryOpcode::Add:
    // String concatenation: String + String -> String.
    if (lhsTy == S.Ctx.getStrTy() && rhsTy == S.Ctx.getStrTy())
      return S.Ctx.getStrTy();
    // `+` is both Str's and the numbers', but never between them.
    if (lhsTy == S.Ctx.getStrTy() || rhsTy == S.Ctx.getStrTy()) {
      S.error(node->getLocation(), "operator '+' is not defined for types '" +
                                       typeName(lhsTy) + "' and '" +
                                       typeName(rhsTy) + "'" + concatHint());
      return nullptr;
    }
    [[fallthrough]];
  case ast::BinaryOpcode::Sub:
  case ast::BinaryOpcode::Mul:
  case ast::BinaryOpcode::Div:
  case ast::BinaryOpcode::Mod:
    // One numeric type gives that type; `int` with `float` gives `float`.
    return promoted ? promoted : lhsTy;

  // Relational / Equality: result is bool.
  case ast::BinaryOpcode::Lt:
  case ast::BinaryOpcode::Gt:
  case ast::BinaryOpcode::Le:
  case ast::BinaryOpcode::Ge:
  case ast::BinaryOpcode::Eq:
  case ast::BinaryOpcode::Ne:
    return S.Ctx.getBoolTy();

  // Logical: both operands must be bool, result is bool.
  case ast::BinaryOpcode::And:
  case ast::BinaryOpcode::Or:
    if (lhsTy != S.Ctx.getBoolTy()) {
      S.error(node->getLHS()->getLocation(),
              "left operand of '" + std::string(node->getOpcodeStr()) +
                  "' must be 'bool', got '" + typeName(lhsTy) + "'");
      return nullptr;
    }
    if (rhsTy != S.Ctx.getBoolTy()) {
      S.error(node->getRHS()->getLocation(),
              "right operand of '" + std::string(node->getOpcodeStr()) +
                  "' must be 'bool', got '" + typeName(rhsTy) + "'");
      return nullptr;
    }
    return S.Ctx.getBoolTy();

  // Bitwise: the operands' one type (docs/design/bitwise.md).
  case ast::BinaryOpcode::BitAnd:
  case ast::BinaryOpcode::BitOr:
  case ast::BinaryOpcode::BitXor:
    return lhsTy;
  case ast::BinaryOpcode::Shl: // visitShift's
  case ast::BinaryOpcode::Shr:
  case ast::BinaryOpcode::Count:
    break;
  }
  PAYKAN_UNREACHABLE("unknown BinaryOpcode");
}

ast::Type *Sema::ExprChecker::visitShift(ast::BinaryExpr *node) {
  // The value is typed first and takes no type from the count, so a
  // literal value is an `int`; a literal count is an `unsigned` when the
  // value is unsigned (`x << 3`, `x << ~0`).  The count may have any
  // unsigned type, of any width: the one-type rule does not apply, and the
  // result has the value's type (docs/design/bitwise.md, "Shifts").
  ast::Expr *value = node->getLHS();
  ast::Expr *count = node->getRHS();
  ast::Type *lhsTy = visit(value);
  if (!lhsTy)
    return nullptr;
  const auto *valueBT = ast::dyn_cast<ast::BuiltinType>(lhsTy);
  ast::Type *rhsTy =
      valueBT && valueBT->isUnsigned() && isNumericLiteral(count)
          ? visitExpecting(count, S.Ctx.getBuiltinType(ast::BuiltinType::U64))
          : visit(count);
  if (!rhsTy)
    return nullptr;
  value->setResolvedType(lhsTy);
  count->setResolvedType(rhsTy);

  // As for any operator but `==` and `!=`: an optional is unwrapped first.
  auto *lhsOpt = ast::dyn_cast<ast::OptionalType>(lhsTy);
  if (auto *opt = lhsOpt ? lhsOpt : ast::dyn_cast<ast::OptionalType>(rhsTy)) {
    S.errorOptionalUnwrap(node->getLocation(), opt);
    return nullptr;
  }
  if (!lhsTy->hasBinaryOp(node->getOpcode(), rhsTy)) {
    S.error(node->getLocation(),
            "operator '" + std::string(node->getOpcodeStr()) +
                "' is not defined for types '" + typeName(lhsTy) + "' and '" +
                typeName(rhsTy) + "'" + bitwiseHint(node, lhsTy, rhsTy));
    return nullptr;
  }

  // A literal count at or above the width is valid and always gives 0:
  // a warning, once per place though several instantiations of a generic
  // body reach it.
  if (const auto *lit = ast::dyn_cast<ast::IntegerLiteral>(count)) {
    const uint64_t n = literalMagnitude(lit).second;
    const unsigned width = ast::BuiltinType::integerBits(
        ast::cast<ast::BuiltinType>(lhsTy)->getTypeKind());
    const ast::SourceLocation loc = count->getLocation();
    if (n >= width && S.WarnedShiftCounts
                          .insert({S.Diags.getSourceName(), loc.getLineStart(),
                                   loc.getColumnStart()})
                          .second)
      S.warning(loc, "the shift count " + std::to_string(n) +
                         " is not less than the width of '" + typeName(lhsTy) +
                         "' (" + std::to_string(width) +
                         " bits): the result is always 0");
  }
  return lhsTy;
}

ast::Type *Sema::ExprChecker::visitCallExpr(ast::CallExpr *node) {
  ast::Type *expected = std::exchange(S.VariantExpectation, nullptr);
  // `E::V(a, b)`: an enum's variant, made with its fields; `G<int>::V(a)`
  // or `G::V(a)` a generic enum's.
  if (const std::string &callee = node->getCalleeName();
      callee.find(names::kQualSep) != std::string::npos) {
    const size_t sep = callee.rfind(names::kQualSep);
    const std::string owner = callee.substr(0, sep);
    const std::string variant =
        callee.substr(sep + std::string_view(names::kQualSep).size());
    if (auto *et = S.Ctx.lookupEnumType(owner)) {
      if (!node->hasTypeArgs() && !node->hasOwnerTypeArgs())
        return checkVariantCall(node, et, variant);
      S.error(node->getLocation(), "'" + owner +
                                       "' is not a generic enum and takes no "
                                       "type arguments");
      return nullptr;
    }
    if (S.EnumTemplates.count(owner)) {
      // The enum's arguments go after the enum (`Maybe<int>::Just(1)`), as
      // an owner's do (#353): a variant has none of its own.
      if (node->hasTypeArgs()) {
        S.error(node->getLocation(),
                "'" + S.displayName(callee) +
                    "' is a variant of the generic enum '" +
                    S.displayName(owner) + "': write '" + S.displayName(owner) +
                    "<...>::" + variant + "(...)' with its type arguments");
        return nullptr;
      }
      std::optional<std::vector<ast::Type *>> argTypes;
      const ast::ASTNode *outer = std::exchange(S.OwnerListOf, node);
      ast::EnumType *et = genericVariantEnum(
          node, owner, variant, node->getOwnerTypeArgs(), expected, &argTypes);
      S.OwnerListOf = outer;
      if (!et)
        return nullptr;
      return checkVariantCall(node, et, variant,
                              argTypes ? &*argTypes : nullptr);
    }
  }
  // `C::__m(...)`: a private static method is its type's own.
  if (!S.checkStaticMemberUse(node->getCalleeName(), node->getLocation()))
    return nullptr;
  // A call through a function value (`f(1)` with `f: (int) -> int`): the
  // variable, which shadows a function of the same name, is read as any
  // other (its liveness, borrows and capture checked), then called.
  if (ast::Type *varTy = S.CurrentScope->lookup(node->getCalleeName());
      varTy && !node->hasTypeArgs()) {
    const std::string &name = node->getCalleeName();
    S.checkNotBorrowed(name, node->getLocation());
    varTy = S.checkIdentLive(name, node->getLocation());
    if (!varTy)
      return nullptr;
    if (!S.LambdaStack.empty() &&
        !S.captureVariable(name, varTy, S.CurrentScope->findOwner(name),
                           node->getLocation()))
      return nullptr;
    if (auto *ot = ast::dyn_cast<ast::OptionalType>(varTy)) {
      S.errorOptionalUnwrap(node->getLocation(), ot);
      return nullptr;
    }
    auto *fnt = ast::dyn_cast<ast::FunctionType>(varTy);
    if (!fnt) {
      S.error(node->getLocation(), "'" + name + "' has type '" +
                                       typeName(varTy) +
                                       "', which is not a function type; it "
                                       "cannot be called");
      return nullptr;
    }
    ast::Type *ret =
        S.checkFunctionValueCall(fnt, node->getArguments(), name, node);
    if (!ret)
      return nullptr;
    node->setCalleeIsValue(true);
    node->setResolvedType(ret);
    return ret;
  }

  // Type-check all arguments first.  A plain call to a known function gives
  // each argument its parameter's type as the expected type (a generic call
  // infers from the arguments, so they come first there).
  const FunctionSig *known = nullptr;
  // Explicit type arguments (`Box<int>(...)`) need no argument types: the
  // instance is known first, and so are its parameters.
  // A generic `static fn` (#353) is one once its own are written; the
  // owner's alone leave them to the arguments.
  const bool genericStatic = S.isGenericStatic(node->getCalleeName());
  const bool explicitInstance =
      !S.isConversionTarget(node->getCalleeName()) &&
      ((node->hasTypeArgs() && S.isTemplate(node->getCalleeName())) ||
       (genericStatic ? node->hasTypeArgs()
                      : (node->hasTypeArgs() || node->hasOwnerTypeArgs()) &&
                            S.isTemplateStatic(node->getCalleeName())));
  if (explicitInstance && !S.resolveGenericCall(node, {}))
    return nullptr;
  if (explicitInstance ||
      (!node->hasTypeArgs() && !S.isTemplate(node->getCalleeName())))
    if (const auto *sig = S.lookupFunction(node->getCalleeName());
        sig && sig->ParamTypes.size() == node->getNumArguments())
      known = sig;
  std::vector<ast::Type *> expectedArgs;
  // `unsigned(0xFFFF_FFFF_FFFF_FFFF)`, `Str(0x8000_0000_0000_0000)`: an
  // inferred conversion of a literal above INT64_MAX converts it as the
  // `unsigned` it is (docs/design/bitwise.md).
  const auto *wide =
      !node->hasTypeArgs() && node->getNumArguments() == 1 &&
              S.isConversionTarget(node->getCalleeName())
          ? ast::dyn_cast<ast::IntegerLiteral>(node->getArguments()[0])
          : nullptr;
  if (wide && wide->isAboveInt64Max())
    expectedArgs = {S.Ctx.getBuiltinType(ast::BuiltinType::U64)};
  else if (known)
    expectedArgs = known->ParamTypes;
  // `Str<u16>(3)`: the argument of an explicit conversion flows into the
  // written source type, so a literal takes that sized type.
  else if (node->hasTypeArgs() && node->getTypeArgs().size() == 1 &&
           S.isConversionTarget(node->getCalleeName()) &&
           ast::isa<ast::BuiltinType>(node->getTypeArgs()[0]))
    expectedArgs = {node->getTypeArgs()[0]};
  // `__super__(...)`: the superclass `__init__`'s parameters (checked below).
  else if (node->getCalleeName() == names::kMethodSuper && S.CurrentClassCtx &&
           S.CurrentClassCtx->ClassType)
    if (auto *sc = S.CurrentClassCtx->ClassType->getSuperClass();
        sc && !S.isErroneousClass(sc))
      if (auto *init = sc->findMethod(names::kMethodInit);
          init && init->getParamTypes().size() == node->getNumArguments())
        expectedArgs = init->getParamTypes();
  std::vector<ast::Type *> argTypes;
  for (size_t i = 0; i < node->getNumArguments(); ++i) {
    // An `atomic` parameter takes an `atomic` place's handle (the
    // prototype); any other value makes a new cell (checkAssignable).
    S.WantAtomicHandle = i < expectedArgs.size() &&
                         ast::isa<ast::AtomicType>(expectedArgs[i]) &&
                         Sema::isPlaceChain(node->getArguments()[i]);
    auto *ty =
        visitExpecting(node->getArguments()[i],
                       i < expectedArgs.size() ? expectedArgs[i] : nullptr);
    S.WantAtomicHandle = false;
    // `Str(s)` of a `locked Str` is its contents.
    if (ty == S.Ctx.getStrTy() &&
        node->getCalleeName() == S.Ctx.getStrTy()->getName() &&
        !S.checkNotLocked(node->getArguments()[i], node->getLocation()))
      return nullptr;
    argTypes.push_back(ty);
  }

  // -- __super__(args): superclass initializer call
  if (node->getCalleeName() == names::kMethodSuper) {
    if (!S.CurrentClassCtx ||
        S.CurrentClassCtx->MethodName != names::kMethodInit) {
      S.error(node->getLocation(), std::string("'") + names::kMethodSuper +
                                       "' can only be called inside '" +
                                       names::kMethodInit + "'");
      return nullptr;
    }
    if (!S.CurrentClassCtx->ClassType) {
      S.error(node->getLocation(), std::string("'") + names::kMethodSuper +
                                       "' called in struct '" +
                                       S.CurrentClassCtx->Struct->getName() +
                                       "', which has no superclass (a struct "
                                       "does not inherit)");
      return nullptr;
    }
    auto *superClass = S.CurrentClassCtx->ClassType->getSuperClass();
    if (superClass && S.isErroneousClass(superClass)) {
      ++S.SuppressedFollowOns; // its initializer may be missing
      return nullptr;
    }
    if (!superClass || superClass == S.Ctx.getObjTy()) {
      S.error(node->getLocation(), std::string("'") + names::kMethodSuper +
                                       "' called in class '" +
                                       S.CurrentClassCtx->ClassType->getName() +
                                       "' which has no explicit superclass");
      return nullptr;
    }
    // It assigns the superclass's fields, which are unassigned only before
    // anything else has run: a second call would store over the first's.
    if (node != S.CurrentClassCtx->FrontSuperCall) {
      S.error(node->getLocation(), std::string("'") + names::kMethodSuper +
                                       "' may be called only once, as the "
                                       "first statement of '" +
                                       names::kMethodInit + "'");
      return nullptr;
    }
    auto *superInit = superClass->findMethod(names::kMethodInit);
    // A superclass whose initializer is `___init__` is extended by its
    // module only.
    if (!S.checkConstructible(superClass->getName(), superClass->getName(),
                              superInit, "extended", node->getLocation()))
      return nullptr;
    std::vector<ast::Type *> expectedParams;
    if (superInit)
      expectedParams = superInit->getParamTypes();
    if (argTypes.size() != expectedParams.size()) {
      S.error(node->getLocation(),
              std::string("'") + names::kMethodSuper + "' expects " +
                  std::to_string(expectedParams.size()) + " argument(s), got " +
                  std::to_string(argTypes.size()));
    } else {
      for (size_t i = 0; i < argTypes.size(); ++i) {
        if (!argTypes[i])
          continue;
        auto *argExpr = node->getArguments()[i];
        if (!S.checkAssignable(expectedParams[i], argTypes[i], argExpr) &&
            !S.diagnoseOptionalNarrowing(argExpr->getLocation(),
                                         expectedParams[i], argTypes[i]))
          S.error(argExpr->getLocation(),
                  "argument " + std::to_string(i + 1) + " of '" +
                      names::kMethodSuper + "' has type '" +
                      typeName(argTypes[i]) + "', expected '" +
                      typeName(expectedParams[i]) + "'" +
                      sliceArgNote(expectedParams[i], argTypes[i]));
      }
    }
    if (superInit && argTypes.size() == expectedParams.size()) {
      // A by-value parameter owns what it is given: the copy rule (#369).
      for (size_t i = 0; i < argTypes.size(); ++i)
        if (argTypes[i] &&
            superInit->getParamModes().mode(i) == ast::ParamMode::Value)
          S.checkQualifiedFlow(superInit->getParamQual(i),
                               node->getArguments()[i],
                               "parameter " + std::to_string(i + 1) + " of '" +
                                   names::kMethodSuper + "'",
                               node->getArguments()[i]->getLocation());
      S.checkInoutArgs(node, superInit->getParamModes(), expectedParams,
                       node->getArguments(), names::kMethodSuper);
      S.checkViewArgs(superInit->getParamModes(), expectedParams,
                      node->getArguments(), names::kMethodSuper);
    }
    return S.Ctx.getVoidTy();
  }

  // -- Conversion constructor: Target<Source>(value) (#64)
  if (node->hasTypeArgs() && S.isConversionTarget(node->getCalleeName()))
    return S.checkConversion(node, argTypes);

  // -- Inferred conversion: Target(value) picks Target<typeof value> (#88)
  // `Str(s)` with a `Str` is the `Str` constructor, not a conversion.
  if (!node->hasTypeArgs() && S.isConversionTarget(node->getCalleeName()) &&
      !S.isStrConstruction(node, argTypes))
    return S.inferConversion(node, argTypes);

  // -- Generic call: instantiate the template and rebind the callee
  if (!explicitInstance &&
      (node->hasTypeArgs() || node->hasOwnerTypeArgs() || genericStatic ||
       S.isTemplate(node->getCalleeName()) ||
       S.isTemplateStatic(node->getCalleeName()))) {
    for (auto *ty : argTypes)
      if (!ty)
        return nullptr; // argument errors already reported; cannot infer
    if (!S.resolveGenericCall(node, argTypes, expected))
      return nullptr;
    // An `atomic` parameter of the instance takes a place's handle, which
    // the arguments typed for the inference did not know to give.
    if (const FunctionSig *made = S.lookupFunction(node->getCalleeName()))
      for (size_t i = 0; i < argTypes.size() && i < made->ParamTypes.size();
           ++i)
        if (ast::isa<ast::AtomicType>(made->ParamTypes[i]) &&
            Sema::isPlaceChain(node->getArguments()[i]) &&
            !Sema::movesPlace(node->getArguments()[i])) {
          S.WantAtomicHandle = true;
          argTypes[i] = visit(node->getArguments()[i]);
          S.WantAtomicHandle = false;
        }
  } else if (S.CurrentTypeParams &&
             S.CurrentTypeParams->count(node->getCalleeName())) {
    S.error(node->getLocation(),
            "type parameter '" + node->getCalleeName() +
                "' cannot be used as a value (a type parameter cannot be "
                "constructed)");
    return nullptr;
  }

  const auto *sig = S.lookupFunction(node->getCalleeName());
  if (sig && !S.inferResultOf(node->getCalleeName(), node->getLocation()))
    return nullptr;
  // A call by the name of a rejected declaration, or to the constructor of
  // an erroneous class, was already reported at the declaration.  (A builtin
  // keeps its name, and its calls are checked as usual.)
  if (S.ErroneousNames.count(node->getCalleeName()) &&
      !(sig && sig->IsBuiltin)) {
    ++S.SuppressedFollowOns;
    return sig ? sig->ReturnType : nullptr;
  }
  if (sig && !sig->IsBuiltin)
    if (auto *ct = S.Ctx.lookupClassType(node->getCalleeName());
        ct && S.isErroneousClass(ct)) {
      // Its signature may be incomplete: check no arguments, but keep the
      // class type flowing so its uses are not reported either.
      node->setResolvedType(sig->ReturnType);
      ++S.SuppressedFollowOns;
      return sig->ReturnType;
    }
  if (!sig) {
    // A struct whose declaration was rejected has no constructor: the
    // declaration's error is the one to report, and the struct's type
    // keeps flowing (its field accesses are follow-ons too).
    if (auto *st = S.Ctx.lookupStructType(node->getCalleeName());
        st && S.ErroneousClasses.count(st->getName())) {
      ++S.SuppressedFollowOns;
      node->setResolvedType(st);
      return st;
    }
    if (S.reportPrivateImport(node->getCalleeName(), node->getLocation()))
      return nullptr;
    // `m::E::V(x)` names a variant of an enum of the failed module `m`.
    const std::string &callee = node->getCalleeName();
    const auto sep = callee.rfind(names::kQualSep);
    if (!S.isFailedImportUse(callee) && !S.isImportClashUse(callee) &&
        (sep == std::string::npos ||
         !S.isFailedImportUse(callee.substr(0, sep))))
      S.error(node->getLocation(),
              "call to undeclared function '" + displayName(callee) + "'");
    return nullptr;
  }

  // Record "class A constructs class B" edges for instantiation ordering.
  if (S.CurrentClassCtx && S.CurrentClassCtx->ClassType &&
      S.Ctx.lookupClassType(node->getCalleeName()))
    S.ConstructsEdges[S.CurrentClassCtx->ClassType->getName()].insert(
        node->getCalleeName());

  // A type whose initializer is `___init__` is constructed by its module.
  // An abstract class is never constructed (every construction is a call by
  // its name: a constructor is not a function value); the arguments are
  // still checked.
  if (auto *ct = S.Ctx.lookupClassType(node->getCalleeName())) {
    if (!S.checkConstructible(ct->getName(), node->getCalleeName(),
                              ct->findMethod(names::kMethodInit), "constructed",
                              node->getLocation()))
      return nullptr;
    if (ct->isAbstract())
      S.reportAbstractConstruction(ct, node->getLocation());
  } else if (auto *st = S.Ctx.lookupStructType(node->getCalleeName())) {
    if (!S.checkConstructible(st->getName(), node->getCalleeName(),
                              st->findMethod(names::kMethodInit), "constructed",
                              node->getLocation()))
      return nullptr;
  }

  if (argTypes.size() != sig->ParamTypes.size()) {
    std::string msg = "function '" + displayName(node->getCalleeName()) +
                      "' expects " + std::to_string(sig->ParamTypes.size()) +
                      " argument(s), got " + std::to_string(argTypes.size());
    if (auto d = S.DefaultInits.find(node->getCalleeName());
        d != S.DefaultInits.end()) {
      msg += " (it has no '";
      msg += names::kMethodInit;
      msg += "', so it takes ";
      msg += d->second->getParams().empty() ? "nothing"
                                            : Sema::paramList(d->second);
      msg += ")";
    }
    S.error(node->getLocation(), msg);
    return sig->ReturnType;
  }

  // Check argument types.
  for (size_t i = 0; i < argTypes.size(); ++i) {
    if (!argTypes[i])
      continue; // already reported
    auto *argExpr = node->getArguments()[i];
    if (!S.checkAssignable(sig->ParamTypes[i], argTypes[i], argExpr) &&
        !S.diagnoseOptionalNarrowing(argExpr->getLocation(), sig->ParamTypes[i],
                                     argTypes[i])) {
      S.error(argExpr->getLocation(),
              "argument " + std::to_string(i + 1) + " of '" +
                  displayName(node->getCalleeName()) + "' has type '" +
                  typeName(argTypes[i]) + "', expected '" +
                  typeName(sig->ParamTypes[i]) + "'" +
                  sliceArgNote(sig->ParamTypes[i], argTypes[i]));
    }
  }
  // An async call starts a task: what it takes across is moved or copied
  // (the concurrency prototype's send check).
  if (sig->Async && !S.checkSendArgs(node, *sig))
    return nullptr;
  const std::string callee = displayName(node->getCalleeName());
  // A by-value parameter is an owning slot (the prototype): what flows
  // into it converts and, under the copy rule, moves.  A builtin borrows;
  // an imported function of Paykan code does not.
  const bool owns = !sig->IsBuiltin || (sig->Imported && !sig->Native);
  if (owns)
    for (size_t i = 0; i < argTypes.size() && i < sig->ParamTypes.size(); ++i)
      if (argTypes[i] && sig->Modes.mode(i) == ast::ParamMode::Value &&
          !S.checkQualifiedFlow(i < sig->ParamQuals.size() ? sig->ParamQuals[i]
                                                           : ast::Qualifier{},
                                node->getArguments()[i],
                                "parameter " + std::to_string(i + 1) + " of '" +
                                    callee + "'",
                                node->getArguments()[i]->getLocation()))
        return nullptr;
  // A borrow takes the qualifier its parameter says (the prototype).
  if (owns && !S.checkBorrowQualifiers(sig->Modes, sig->ParamQuals,
                                       node->getArguments(), callee))
    return nullptr;
  S.checkInoutArgs(node, sig->Modes, sig->ParamTypes, node->getArguments(),
                   callee);
  S.checkViewArgs(sig->Modes, sig->ParamTypes, node->getArguments(), callee);
  S.noteBorrowResult(node, callee, sig->Modes, sig->ParamTypes, sig->ReturnType,
                     node->getArguments(), nullptr);

  node->setResolvedType(sig->ReturnType);
  return sig->ReturnType;
}

ast::Type *Sema::ExprChecker::visitMethodCallExpr(ast::MethodCallExpr *node) {
  // Type arguments are a generic method's (#353), whose call is rebound to
  // its instantiation: any other method takes none, a builtin's too
  // (`n.toString<int>()`).
  const bool written = node->hasTypeArgs();
  const std::string name = node->getMethodName();
  ast::Type *ty = checkMethodCall(node);
  if (ty && written && node->getMethodName() == name) {
    ast::Type *recv = node->getReceiver()->getResolvedType();
    S.error(node->getLocation(),
            "method '" + name + "' of '" + (recv ? typeName(recv) : "?") +
                "' is not generic and takes no type arguments");
    return nullptr;
  }
  return ty;
}

ast::Type *Sema::ExprChecker::checkMethodCall(ast::MethodCallExpr *node) {
  // The type the call flows into, if known: a generic method's own type
  // parameters may come from it (visitExpecting).
  ast::Type *expected = nullptr;
  if (auto it = S.MethodExpectations.find(node);
      it != S.MethodExpectations.end()) {
    expected = it->second;
    S.MethodExpectations.erase(it);
  }
  // Resolve receiver type.  A method call on an `atomic` place (the
  // prototype) takes its handle, not its value.
  S.WantAtomicHandle = Sema::isPlaceChain(node->getReceiver()) ||
                       Sema::isElementOfPlace(node->getReceiver());
  auto *recvTy = visit(node->getReceiver());
  S.WantAtomicHandle = false;
  if (!recvTy)
    return nullptr;
  if (auto *at = ast::dyn_cast<ast::AtomicType>(recvTy))
    return S.checkAtomicMethodCall(node, at);
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(recvTy);
      ot && ast::isa<ast::AtomicType>(ot->getInnerType()) &&
      node->getMethodName() == names::kMethodClone) {
    S.error(node->getLocation(),
            "'clone' of an optional 'atomic' handle is not supported yet");
    return nullptr;
  }
  if (!S.checkNotLocked(node->getReceiver(), node->getLocation()))
    return nullptr;
  // A `mem::Buffer<T>` of the enclosing function in a `parallel for` body
  // (the prototype): reads, and `set` at the loop's variable.
  if (!S.ParallelStack.empty() && S.bufferElementType(recvTy))
    if (const auto *rid = ast::dyn_cast<ast::Identifier>(node->getReceiver());
        rid && S.isOuterScope(S.CurrentScope->findOwner(rid->getName())) &&
        !S.checkParallelBufferCall(node, rid->getName()))
      return nullptr;

  // `x.clone()` in a template's code of a reference whose form a type
  // argument gave, `shared`, `atomic shared` or `locked` (#368: a `T` with
  // T = `shared Node`, an element of a `T[]`): T is Copyable, and a
  // Copyable type's clone is its copy, the same object.  A form the
  // template or the program writes (`s: shared Node`) is cloned by its
  // class, into a new unique object.
  // A conditional's is each branch's: qualifierOf joins a unique one into
  // the other's form.
  const ast::Qualifier joined = S.qualifierOf(node->getReceiver());
  std::function<bool(const ast::Expr *)> eachFromArgument =
      [&](const ast::Expr *e) -> bool {
    if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e))
      return eachFromArgument(te->getTrueExpr()) &&
             eachFromArgument(te->getFalseExpr());
    if (ast::isa<ast::NoneLiteral>(e))
      return true;
    const ast::Qualifier q = S.qualifierOf(e);
    return q.FromArgument && q.Form == joined.Form && q.Mut == joined.Mut;
  };
  if (const ast::Qualifier q = S.qualifierOf(node->getReceiver());
      node->getMethodName() == names::kMethodClone &&
      node->getNumArguments() == 0 && S.inInstanceCode() && q.FromArgument &&
      eachFromArgument(node->getReceiver()) && ast::isRefType(recvTy) &&
      S.isCopyablePart(recvTy, q)) {
    node->setCloneIsCopy(true);
    node->setResolvedType(recvTy);
    return recvTy;
  }

  // A method cannot be called on an optional receiver (it may be None),
  // but for the builtin `Showable`'s `toString()` of an optional whose value
  // has a text: "None", or the value's text.
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(recvTy)) {
    // `o.clone()`: None, or the value's clone (the builtin `Cloneable`).
    if (node->getMethodName() == names::kMethodClone &&
        node->getNumArguments() == 0) {
      std::string why;
      if (!S.isCloneable(ot, &why)) {
        S.error(node->getLocation(),
                "'" + typeName(ot) + "' is not Cloneable: " + why);
        return nullptr;
      }
      node->setResolvedType(ot);
      return ot;
    }
    if (node->getMethodName() == names::kMethodToString &&
        node->getNumArguments() == 0) {
      if (S.hasText(ot)) {
        node->setResolvedType(S.Ctx.getStrTy());
        return S.Ctx.getStrTy();
      }
      if (std::string why = S.noTextReason(ot); !why.empty()) {
        S.errorNoToString(node->getLocation(), ot, why);
        return nullptr;
      }
    }
    S.errorOptionalUnwrap(node->getLocation(), ot);
    return nullptr;
  }

  // A fixed-size array, and a slice of one, has one method, `len()`, its
  // length.
  if (ast::isa<ast::FixedArrayType>(recvTy) &&
      node->getMethodName() == names::kMethodClone &&
      node->getNumArguments() == 0) {
    std::string why;
    if (!S.isCloneable(recvTy, &why)) {
      S.error(node->getLocation(),
              "'" + typeName(recvTy) + "' is not Cloneable: " + why);
      return nullptr;
    }
    node->setResolvedType(recvTy);
    return recvTy;
  }
  if (ast::dyn_cast<ast::FixedArrayType>(recvTy) ||
      ast::dyn_cast<ast::SliceType>(recvTy)) {
    if (node->getMethodName() != names::kLen || node->getNumArguments() != 0) {
      S.error(node->getLocation(),
              "'" + typeName(recvTy) + "' has no method '" +
                  node->getMethodName() + "' with these arguments (a " +
                  (ast::isa<ast::SliceType>(recvTy) ? "slice"
                                                    : "fixed-size array") +
                  " has 'len()')");
      return nullptr;
    }
    if (!S.ParallelStack.empty())
      S.noteParallelNoElementRead(node->getReceiver());
    node->setResolvedType(S.Ctx.getIntTy());
    return S.Ctx.getIntTy();
  }

  // Resolve the ClassType, or the StructType, to look up the method on.
  ast::ClassType *ct = ast::dyn_cast<ast::ClassType>(recvTy);
  ast::StructType *st = ast::dyn_cast<ast::StructType>(recvTy);

  // `n.toString()` on an int, float, bool or char, on a tuple or a struct,
  // or on an enum (its variant, and its fields), each part of which has a
  // text: the builtin `Showable`; `n.hash()` on an int, bool or char, and
  // `s.hash()` on a Str: the builtin `Hashable`; `s.toInt()` & co. on a Str:
  // its parses, each the optional of its type; `u.toInt()` on an `unsigned`:
  // its value as an `int?`, None above the `int` range.
  const std::string &mname = node->getMethodName();
  ast::Type *builtinResult = nullptr;
  ast::BuiltinType::Kind parsed = ast::BuiltinType::Void;
  if (!ct &&
      ((ast::isa<ast::BuiltinType>(recvTy) && recvTy != S.Ctx.getVoidTy()) ||
       ast::isa<ast::TupleType>(recvTy) || ast::isa<ast::EnumType>(recvTy)) &&
      mname == names::kMethodToString) {
    if (!S.hasText(recvTy)) {
      S.errorNoToString(node->getLocation(), recvTy, S.noTextReason(recvTy));
      return nullptr;
    }
    builtinResult = S.Ctx.getStrTy();
  } else if (mname == names::kMethodHash &&
             ((ast::isa<ast::BuiltinType>(recvTy) &&
               (ast::cast<ast::BuiltinType>(recvTy)->isInteger() ||
                recvTy == S.Ctx.getBoolTy() || recvTy == S.Ctx.getCharTy())) ||
              ct == S.Ctx.getStrTy())) {
    builtinResult = S.Ctx.getIntTy();
  } else if (Sema::FoundMethodTemplate own;
             mname == names::kMethodClone && (!ct || ct == S.Ctx.getStrTy()) &&
             !(st && (st->findMethod(mname) ||
                      S.findMethodTemplate(nullptr, st, mname, own)))) {
    // `x.clone()` (the builtin `Cloneable`, #369): a copy of a Copyable
    // value, a new `Str`, and part by part for a struct or a tuple.
    std::string why;
    if (!S.isCloneable(recvTy, &why)) {
      S.error(node->getLocation(),
              "'" + typeName(recvTy) + "' is not Cloneable: " + why);
      return nullptr;
    }
    builtinResult = recvTy;
  } else if (ct == S.Ctx.getStrTy() && ast::strParseKind(mname, parsed)) {
    builtinResult = S.Ctx.getOptionalType(S.Ctx.getBuiltinType(parsed));
  } else if (recvTy == S.Ctx.getBuiltinType(ast::BuiltinType::U64) &&
             mname == names::kMethodToInt) {
    builtinResult = S.Ctx.getOptionalType(S.Ctx.getIntTy());
  }
  if (builtinResult) {
    if (node->getNumArguments() != 0) {
      S.error(node->getLocation(), "method '" + mname +
                                       "' expects 0 argument(s), got " +
                                       std::to_string(node->getNumArguments()));
      return nullptr;
    }
    node->setResolvedType(builtinResult);
    return builtinResult;
  }
  if (!ct && !st) {
    S.error(node->getLocation(),
            "method call on non-class type '" + typeName(recvTy) + "'");
    return nullptr;
  }

  // `__deinit__` runs once, when the object is destroyed: run by name it
  // would run again then, on an object that is in use until it does.
  if (node->getMethodName() == names::kMethodDeinit) {
    S.error(node->getLocation(),
            std::string("'__deinit__' cannot be called directly; it runs "
                        "when the ") +
                (st ? "value" : "object") + " is destroyed");
    return nullptr;
  }
  // The name as the call wrote it: a generic method's call is rebound to
  // its instantiation's below.
  const std::string writtenName = node->getMethodName();
  // A struct's methods are those of its `impl` blocks, called directly.
  ast::MethodDecl *method = ct ? ct->findMethod(node->getMethodName())
                               : st->findMethod(node->getMethodName());
  std::string ownerName = ct ? ct->getName() : st->getName();
  // A generic method of an `impl` block (#353): the call names one of its
  // instantiations, by the type arguments written or inferred from the
  // arguments, which are typed first for that.
  // Written type arguments name the instance before the arguments are
  // checked, which then flow into its parameters as into any method's.
  std::vector<ast::Type *> inferredFrom;
  bool genericCall = false;
  if (!method) {
    Sema::FoundMethodTemplate probe;
    if (S.findMethodTemplate(ct, st, node->getMethodName(), probe)) {
      if (!node->hasTypeArgs()) {
        bool argsOk = true;
        for (ast::Expr *arg : node->getArguments()) {
          inferredFrom.push_back(visit(arg));
          argsOk = argsOk && inferredFrom.back();
        }
        if (!argsOk)
          return nullptr; // reported
        genericCall = true;
      }
      bool failed = false;
      method = S.resolveGenericMethodCall(node, ct, st, inferredFrom, expected,
                                          failed);
      if (!method)
        return nullptr; // reported (the template was found)
    }
  } else if (node->hasTypeArgs()) {
    S.error(node->getLocation(),
            "method '" + node->getMethodName() + "' of '" + ownerName +
                "' is not generic and takes no type arguments");
    return nullptr;
  }

  if (!method && ct && node->getMethodName() == names::kMethodClone &&
      node->getNumArguments() == 0 && !S.isErroneousClass(ct)) {
    S.error(node->getLocation(),
            "class '" + ct->getName() +
                "' is not Cloneable: write 'impl Cloneable for " +
                ct->getName() + " {}' to derive its 'clone', or write one");
    return nullptr;
  }
  if (!method) {
    if (ct ? S.isErroneousClass(ct)
           : S.ErroneousClasses.count(st->getName()) != 0) {
      ++S.SuppressedFollowOns; // the type's declaration was already reported
      return nullptr;
    }
    // `obj.cb(args)` with `cb` a field of function type: a call through
    // the field's value (the receiver is read, not changed).
    ast::Type *fieldTy = ct ? ct->findField(node->getMethodName())
                            : st->findField(node->getMethodName());
    auto *fnt = fieldTy ? ast::dyn_cast<ast::FunctionType>(fieldTy) : nullptr;
    if (fnt && node->hasTypeArgs()) {
      S.error(node->getLocation(),
              "'" + node->getMethodName() +
                  "' is a field of function type; a call through it takes no "
                  "type arguments");
      return nullptr;
    }
    if (fnt) {
      if (!S.checkMemberAccess(ct ? static_cast<ast::Type *>(
                                        S.fieldOwner(ct, node->getMethodName()))
                                  : static_cast<ast::Type *>(st),
                               node->getMethodName(), node->getLocation()))
        return nullptr;
      ast::Type *ret = S.checkFunctionValueCall(fnt, node->getArguments(),
                                                node->getMethodName(), node);
      if (!ret)
        return nullptr;
      node->setFieldCall(true);
      node->setResolvedType(ret);
      return ret;
    }
    if (fieldTy) {
      S.error(node->getLocation(),
              "no method '" + node->getMethodName() + "' on type '" +
                  ownerName + "' (its field '" + node->getMethodName() +
                  "' has type '" + typeName(fieldTy) +
                  "', which is not a function type)");
      return nullptr;
    }
    S.error(node->getLocation(),
            "no method '" + node->getMethodName() + "' on type '" + ownerName +
                "'" +
                (ct == S.Ctx.getStrTy() ? parseAliasHint(node->getMethodName())
                                        : std::string()));
    return nullptr;
  }

  // A private method is its type's own (a generic one by its template's
  // name, which starts its instantiation's).
  if (!S.checkMemberAccess(ct ? static_cast<ast::Type *>(
                                    S.declaringClass(ct, node->getMethodName()))
                              : static_cast<ast::Type *>(st),
                           writtenName, node->getLocation()))
    return nullptr;

  // `destroy` is the compiler-generated ARC destructor; it runs automatically
  // when the last reference is released. Calling it directly would free a
  // still-referenced object and lead to a use-after-free, so reject it.
  if (node->getMethodName() == names::kMethodDestroy) {
    S.error(node->getLocation(),
            "'destroy' cannot be called directly; an object is destroyed "
            "automatically when its last reference is released");
    return method->getReturnType();
  }
  // `__init__` runs once, on construction (`P(...)`); run again by name it
  // would re-initialise a value that is in use.
  if (node->getMethodName() == names::kMethodInit) {
    S.error(node->getLocation(),
            "'__init__' cannot be called directly; it runs when a value is "
            "constructed ('" +
                ownerName + "(...)')");
    return nullptr;
  }

  // A `shared fn` takes a shared receiver; only a `view fn` may be called
  // on a `view`.
  if (S.checkSharedReceiver(node, method))
    S.checkMethodReceiver(node, method);
  // Another module's instantiation: what the method does is read from its
  // template.
  S.copyImportedInstance(recvTy, node->getLocation());

  // Type-check arguments (receiver is implicit — not in getArguments()).
  const auto &paramTys = method->getParamTypes();
  if (node->getNumArguments() != paramTys.size()) {
    S.error(node->getLocation(),
            "method '" + node->getMethodName() + "' expects " +
                std::to_string(paramTys.size()) + " argument(s), got " +
                std::to_string(node->getNumArguments()));
    return method->getReturnType();
  }
  for (size_t i = 0; i < node->getNumArguments(); ++i) {
    auto *argExpr = node->getArguments()[i];
    // An `atomic` parameter takes an `atomic` place's handle (the
    // prototype); any other value makes a new cell (checkAssignable).
    S.WantAtomicHandle =
        ast::isa<ast::AtomicType>(paramTys[i]) && Sema::isPlaceChain(argExpr);
    // (A generic method's arguments were typed to infer its own; one that
    // is to give its handle is typed again, now that its parameter is
    // known.)
    if (genericCall && Sema::movesPlace(argExpr))
      S.WantAtomicHandle = false; // typed once: what it moved, it moved
    auto *argTy = genericCall && !S.WantAtomicHandle
                      ? inferredFrom[i]
                      : visitExpecting(argExpr, paramTys[i]);
    S.WantAtomicHandle = false;
    if (!argTy)
      continue;
    if (!S.checkAssignable(paramTys[i], argTy, argExpr) &&
        !S.diagnoseOptionalNarrowing(argExpr->getLocation(), paramTys[i],
                                     argTy)) {
      S.error(argExpr->getLocation(),
              "argument " + std::to_string(i + 1) + " of '" +
                  node->getMethodName() + "' has type '" + typeName(argTy) +
                  "', expected '" + typeName(paramTys[i]) + "'" +
                  sliceArgNote(paramTys[i], argTy));
    }
  }
  // An async method starts a task: what it takes across is moved or copied,
  // the send check a free function's call makes first too.
  if (method->isAsync() && !S.checkSendArgs(node, method))
    return nullptr;
  // `equals` is lent its argument, as `==` lends it: comparing keeps nothing.
  const bool lends = node->getMethodName() == names::kMethodEquals;
  for (size_t i = 0; i < node->getNumArguments(); ++i)
    if (!lends && node->getArguments()[i]->getResolvedType() &&
        method->getParamModes().mode(i) == ast::ParamMode::Value &&
        !S.checkQualifiedFlow(method->getParamQual(i), node->getArguments()[i],
                              "parameter " + std::to_string(i + 1) + " of '" +
                                  node->getMethodName() + "'",
                              node->getArguments()[i]->getLocation()))
      return nullptr;
  // A borrow takes the qualifier its parameter says (the prototype).
  if (!S.checkBorrowQualifiers(method->getParamModes(), method->getParamQuals(),
                               node->getArguments(), node->getMethodName()))
    return nullptr;
  S.checkInoutArgs(node, method->getParamModes(), paramTys,
                   node->getArguments(), node->getMethodName(),
                   node->getReceiver(), method);
  // A builtin method reads its arguments; `equals`, like `==`, only
  // compares.
  if (!(ct && ct->isBuiltin()) && node->getMethodName() != names::kMethodEquals)
    S.checkViewArgs(method->getParamModes(), method->getParamTypes(),
                    node->getArguments(), node->getMethodName());
  S.noteBorrowResult(node, node->getMethodName(), method->getParamModes(),
                     paramTys, method->getReturnType(), node->getArguments(),
                     node->getReceiver());
  // An `inout` result is part of the receiver, which must outlive the call.
  if (method->getParamModes().Result == ast::ParamMode::Inout &&
      !S.placeRoot(node->getReceiver()) &&
      !S.isInoutResult(S.placeBase(node->getReceiver())))
    S.error(node->getReceiver()->getLocation(),
            "the receiver of '" + node->getMethodName() +
                "' must be a variable or a field: its 'inout' result is part "
                "of it");

  if (method->isAsync() && !S.checkAsyncMethodCall(node, method))
    return nullptr;
  node->setResolvedType(method->getReturnType());
  return method->getReturnType();
}

// static
ast::ClassType *Sema::findLowestCommonAncestor(ast::ClassType *a,
                                               ast::ClassType *b) {
  std::unordered_set<ast::ClassType *> aAncestors;
  for (auto *c = a; c; c = c->getSuperClass())
    aAncestors.insert(c);
  for (auto *c = b; c; c = c->getSuperClass())
    if (aAncestors.count(c))
      return c;
  return nullptr;
}

ast::Type *
Sema::ExprChecker::visitMemberAccessExpr(ast::MemberAccessExpr *node) {
  auto *recvTy = visit(node->getReceiver());
  if (!recvTy)
    return nullptr;
  if (!S.checkNotLocked(node->getReceiver(), node->getLocation()))
    return nullptr;

  // A field cannot be read through an optional receiver (it may be None).
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(recvTy)) {
    S.errorOptionalUnwrap(node->getLocation(), ot);
    return nullptr;
  }

  // A struct's field, by name.
  if (auto *st = ast::dyn_cast<ast::StructType>(recvTy)) {
    if (auto *fty = st->findField(node->getFieldName())) {
      if (!S.checkMemberAccess(st, node->getFieldName(), node->getLocation()))
        return nullptr;
      node->setResolvedType(fty);
      return fty;
    }
    if (S.ErroneousClasses.count(st->getName()))
      ++S.SuppressedFollowOns; // the struct declaration was already reported
    else
      S.error(node->getLocation(), "no field '" + node->getFieldName() +
                                       "' in " + describeStruct(st));
    return nullptr;
  }

  auto *ct = ast::dyn_cast<ast::ClassType>(recvTy);
  if (!ct) {
    S.error(node->getLocation(), "member access '." + node->getFieldName() +
                                     "' on non-class type '" +
                                     typeName(recvTy) + "'");
    return nullptr;
  }

  // Walk the class hierarchy (this class + all ancestors) for the field.
  if (auto *fty = ct->findField(node->getFieldName())) {
    if (!S.checkMemberAccess(S.fieldOwner(ct, node->getFieldName()),
                             node->getFieldName(), node->getLocation()))
      return nullptr;
    // An `atomic` field (the prototype): its handle where one is wanted,
    // otherwise its value, read atomically.
    if (auto *at = ast::dyn_cast<ast::AtomicType>(fty)) {
      if (std::exchange(S.WantAtomicHandle, false)) {
        node->setAtomicHandle(true);
        node->setResolvedType(at);
        return at;
      }
      if (!S.checkAtomicReadable(at, placeText(node), node->getLocation()))
        return nullptr;
      node->setResolvedType(at->getInnerType());
      return at->getInnerType();
    }
    node->setResolvedType(fty);
    return fty;
  }

  if (S.isErroneousClass(ct)) // the class declaration was already reported
    ++S.SuppressedFollowOns;
  else
    S.error(node->getLocation(), "no field '" + node->getFieldName() +
                                     "' in class '" + ct->getName() + "'");
  return nullptr;
}

ast::Type *
Sema::ExprChecker::visitArrayLiteralExpr(ast::ArrayLiteralExpr *node) {
  // The slot this literal flows into, if known (see visitExpecting).
  ast::Type *expected = std::exchange(S.LiteralExpectation, nullptr);
  if (auto *fixed = ast::dyn_cast<ast::FixedArrayType>(expected))
    return checkFixedArrayLiteral(node, fixed);
  if (node->isEmpty()) {
    S.error(node->getLocation(), "an empty array literal has no type: an "
                                 "array has at least one element");
    return nullptr;
  }
  const int64_t n = node->getLength();
  // Into a slice: a fixed-size array of the literal's own length.
  if (auto *slice = ast::dyn_cast<ast::SliceType>(expected))
    return checkFixedArrayLiteral(
        node, S.Ctx.getFixedArrayType(slice->getElementType(), n,
                                      slice->getElemQual()));

  // Anywhere else, an array of its own length of its elements' common type.
  std::vector<ast::Type *> elemTys;
  elemTys.reserve(node->getNumElements());
  const MovedMap movedBefore =
      node->isRepeat() ? S.MovedVars : MovedMap{}; // checkRepeatValue's
  for (ast::Expr *elem : node->getElements()) {
    auto *ty = visit(elem);
    if (!ty)
      return nullptr;
    if (ty == S.Ctx.getVoidTy()) {
      S.error(elem->getLocation(),
              "array element " + std::to_string(elemTys.size()) +
                  " has type 'void' (the expression produces no value)");
      return nullptr;
    }
    elemTys.push_back(ty);
  }
  ast::Type *elemTy = elemTys.front();
  for (size_t i = 1; i < elemTys.size(); ++i) {
    auto *ty = elemTys[i];
    if (!typesEqual(elemTy, ty)) {
      // int <-> float promotion across elements.
      if (elemTy == S.Ctx.getIntTy() && ty == S.Ctx.getFloatTy()) {
        elemTy = S.Ctx.getFloatTy();
      } else if (elemTy == S.Ctx.getFloatTy() && ty == S.Ctx.getIntTy()) {
        // keep elemTy = float
      } else {
        // For class types try the lowest common ancestor.
        auto *eCT = ast::dyn_cast<ast::ClassType>(elemTy);
        auto *tCT = ast::dyn_cast<ast::ClassType>(ty);
        if (eCT && tCT) {
          if (auto *lca = S.findLowestCommonAncestor(eCT, tCT)) {
            elemTy = lca;
            continue;
          }
        }
        // Values whose parts differ only in their forms join to the
        // stronger (#368): `[(1, s), (2, Node(2))]` holds `shared` parts,
        // which each element then converts into (checkAssignable).
        if (ast::Type *joined = S.joinPartForms(elemTy, ty)) {
          elemTy = joined;
          continue;
        }
        S.error(node->getElements()[i]->getLocation(),
                "array literal has inconsistent element types: '" +
                    typeName(elemTy) + "' and '" + typeName(ty) + "'");
        return nullptr;
      }
    }
  }
  if (ast::isa<ast::SliceType>(elemTy)) {
    S.errorFixedArrayHere(node->getLocation(), elemTy, "an array element");
    return nullptr;
  }
  if (fixedElementBytes(elemTy) < 0) {
    S.error(node->getLocation(),
            "an array of '" + typeName(elemTy) + "' is not supported yet");
    return nullptr;
  }
  // The elements' form (#368): `[s, t]` of `shared` references is an
  // array of them, which a unique temporary joins as a variable would.
  ast::Qualifier elemQual;
  if (ast::isRefType(elemTy))
    for (ast::Expr *e : node->getElements()) {
      const ast::Qualifier q = S.qualifierOf(e);
      if (q.Form != ast::QualForm::Shared || q == elemQual)
        continue;
      if (!elemQual.isUnique()) {
        S.error(e->getLocation(), "array literal has elements of two forms: '" +
                                      S.qualifiedName(elemQual, elemTy) +
                                      "' and '" + S.qualifiedName(q, elemTy) +
                                      "'");
        return nullptr;
      }
      elemQual = q;
    }
  auto *fixed = S.Ctx.getFixedArrayType(elemTy, n, elemQual);
  if (fixedElementBytes(fixed) < 0) {
    S.error(node->getLocation(),
            "array literal of type '" + typeName(fixed) +
                "' is too big: a fixed-size array holds at most " +
                std::to_string(kMaxFixedArrayElements) + " elements");
    return nullptr;
  }
  // Each element takes the common type (`[1, 2.5]` is a `float[2]`), and
  // the array holds it: a `view` that shares what it holds is not one.
  for (size_t i = 0; i < elemTys.size(); ++i) {
    S.checkAssignable(elemTy, elemTys[i], node->getElements()[i]);
    S.checkViewNotStored(node->getElements()[i], elemTys[i]);
    // The array owns its elements: a move-only one moves in (#369), and
    // each converts into the elements' form (#368).  A `[v; n]` of a place
    // that is not Copyable is checkRepeatValue's.
    if (!S.repeatsAPlace(node, elemTy))
      S.checkQualifiedFlow(elemQual, node->getElements()[i],
                           "an element of the array",
                           node->getElements()[i]->getLocation());
  }
  if (!S.checkRepeatValue(node, elemTy, movedBefore))
    return nullptr;
  node->setResolvedType(fixed);
  return fixed;
}

ast::Type *
Sema::ExprChecker::checkFixedArrayLiteral(ast::ArrayLiteralExpr *node,
                                          ast::FixedArrayType *fixed) {
  // A `_` length (a variable declaration's) is the literal's length; an
  // element's `_` lengths are the first element's.
  if (fixed->isInferred()) {
    if (node->isEmpty()) {
      S.error(node->getLocation(),
              "an empty array literal gives '" + typeName(fixed) +
                  "' no length: a fixed-size array has at least one element");
      return nullptr;
    }
    fixed = S.Ctx.getFixedArrayType(fixed->getElementType(), node->getLength(),
                                    fixed->getElemQual());
  }
  ast::Type *elemTy = fixed->getElementType();
  const ast::Qualifier elemQual = fixed->getElemQual();
  if (node->getLength() != fixed->getLength()) {
    S.error(node->getLocation(),
            "array literal has " + std::to_string(node->getLength()) +
                " element(s), but '" + typeName(fixed) + "' has " +
                std::to_string(fixed->getLength()));
    return nullptr;
  }
  bool ok = true;
  const MovedMap movedBefore =
      node->isRepeat() ? S.MovedVars : MovedMap{}; // checkRepeatValue's
  for (ast::Expr *elem : node->getElements()) {
    auto *ty = visitExpecting(elem, elemTy);
    if (!ty) {
      ok = false;
      continue;
    }
    // The array owns its elements: a move-only one moves in (#369), and
    // each converts into the elements' form (#368).  A `[v; n]` of a place
    // that is not Copyable is checkRepeatValue's.
    if (!S.repeatsAPlace(node, elemTy) &&
        !S.checkQualifiedFlow(elemQual, elem, "an element of the array",
                              elem->getLocation()))
      ok = false;
    if (hasInferredLength(elemTy)) {
      ast::Type *filled = S.fillLengths(elemTy, ty);
      if (!filled) {
        S.errorNoLengths(elem->getLocation(), elemTy, ty);
        return nullptr;
      }
      elemTy = filled;
      fixed = S.Ctx.getFixedArrayType(elemTy, fixed->getLength(), elemQual);
    }
    if (!S.checkAssignable(elemTy, ty, elem)) {
      S.error(elem->getLocation(),
              "array literal element of type '" + typeName(ty) +
                  "' does not match the element type '" + typeName(elemTy) +
                  "' of '" + typeName(fixed) + "'");
      ok = false;
    } else if (!S.checkViewNotStored(elem, ty)) {
      ok = false;
    }
  }
  if (!ok)
    return nullptr;
  // `[v; n]`, after the value's own errors.
  if (!S.checkRepeatValue(node, elemTy, movedBefore))
    return nullptr;
  node->setResolvedType(fixed);
  return fixed;
}

bool Sema::checkFixedIndex(ast::FixedArrayType *fixed, const ast::Expr *index) {
  // A constant index is checked here; any other at run time.
  int64_t i;
  if (const auto *lit = ast::dyn_cast<ast::IntegerLiteral>(index))
    i = lit->getValue();
  else if (const auto *neg = ast::dyn_cast<ast::UnaryExpr>(index);
           neg && neg->getOpcode() == ast::UnaryOpcode::Neg &&
           ast::isa<ast::IntegerLiteral>(neg->getOperand()))
    i = -ast::cast<ast::IntegerLiteral>(neg->getOperand())->getValue();
  else
    return true;
  if (i >= 0 && i < fixed->getLength())
    return true;
  error(index->getLocation(), "index " + std::to_string(i) +
                                  " is out of bounds for '" + typeName(fixed) +
                                  "' (its indexes are 0 to " +
                                  std::to_string(fixed->getLength() - 1) + ")");
  return false;
}

ast::Type *Sema::ExprChecker::visitSubscriptExpr(ast::SubscriptExpr *node) {
  // An `atomic` array is used through its handle, and its element's handle
  // is wanted by the method called on it.
  const bool wantElementHandle = std::exchange(S.WantAtomicHandle, false);
  S.WantAtomicHandle = Sema::isPlaceChain(node->getArray());
  auto *arrayTy = visit(node->getArray());
  S.WantAtomicHandle = false;
  if (!arrayTy)
    return nullptr;
  if (!S.ParallelStack.empty())
    S.noteParallelElementRead(node);
  if (!S.checkNotLocked(node->getArray(), node->getLocation()))
    return nullptr;
  if (auto *at = ast::dyn_cast<ast::AtomicType>(arrayTy))
    if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(at->getInnerType())) {
      auto *idx = visit(node->getIndex());
      if (!idx)
        return nullptr;
      if (idx != S.Ctx.getIntTy()) {
        S.error(node->getIndex()->getLocation(),
                "array index must be 'int', got '" + typeName(idx) + "'");
        return nullptr;
      }
      if (!S.checkFixedIndex(ft, node->getIndex()))
        return nullptr;
      node->setAtomicElement(true);
      ast::Type *elem = ft->getElementType();
      ast::Type *ty = wantElementHandle ? S.Ctx.getAtomicType(elem) : elem;
      node->setResolvedType(ty);
      return ty;
    }

  // An optional array / string cannot be indexed (it may be None).
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(arrayTy)) {
    S.errorOptionalUnwrap(node->getLocation(), ot);
    return nullptr;
  }

  // String subscript: str[idx] -> char
  if (arrayTy == S.Ctx.getStrTy()) {
    auto *idxTy = visit(node->getIndex());
    if (idxTy && idxTy != S.Ctx.getIntTy())
      S.error(node->getIndex()->getLocation(),
              "string index must be 'int', got '" + typeName(idxTy) + "'");
    node->setResolvedType(S.Ctx.getCharTy());
    return S.Ctx.getCharTy();
  }

  auto *fixed = ast::dyn_cast<ast::FixedArrayType>(arrayTy);
  auto *slice = ast::dyn_cast<ast::SliceType>(arrayTy);
  if (!fixed && !slice) {
    S.error(node->getLocation(), "subscript '[]' applied to non-array type '" +
                                     typeName(arrayTy) + "'");
    return nullptr;
  }

  auto *idxTy = visit(node->getIndex());
  if (!idxTy)
    return nullptr;
  if (idxTy != S.Ctx.getIntTy()) {
    S.error(node->getIndex()->getLocation(),
            "array index must be 'int', got '" + typeName(idxTy) + "'");
    return nullptr;
  }
  if (fixed && !S.checkFixedIndex(fixed, node->getIndex()))
    return nullptr;

  ast::Type *elemTy = fixed ? fixed->getElementType() : slice->getElementType();
  node->setResolvedType(elemTy);
  return elemTy;
}

ast::Type *Sema::ExprChecker::visitTernaryExpr(ast::TernaryExpr *node) {
  // A generic enum's variant or a generic function's call in an arm takes
  // its type arguments from the slot the value flows into, else from the
  // other arm's type: the arm that needs it less is typed first (`if c then
  // Maybe::Just(1) else Maybe::Nothing`, `if c then Maybe::Nothing else
  // Maybe::Just(1)`, `if c then none() else Maybe::Just(1)`).
  ast::Type *expected = std::exchange(S.VariantExpectation, nullptr);
  auto visitArm = [&](ast::Expr *arm, ast::Type *want) {
    struct OnSomePaths {
      unsigned &Depth;
      explicit OnSomePaths(unsigned &d) : Depth(d) { ++Depth; }
      ~OnSomePaths() { --Depth; }
    } onSomePaths(S.ConditionalOperand);
    // A literal arm's elements take the slot's (or the other arm's) types
    // and forms (#368), as a literal does where it flows.
    if (want && (ast::isa<ast::ArrayLiteralExpr>(arm) ||
                 ast::isa<ast::TupleLiteralExpr>(arm)))
      S.LiteralExpectation = want;
    else if (want &&
             (ast::isa<ast::EnumValueExpr>(arm) ||
              ast::isa<ast::CallExpr>(arm) || ast::isa<ast::TernaryExpr>(arm)))
      S.VariantExpectation = want;
    else if (want && ast::isa<ast::MethodCallExpr>(arm))
      S.MethodExpectations[arm] = want;
    return visit(arm);
  };
  auto *condTy = visit(node->getCondition());
  ast::Type *trueTy = nullptr;
  ast::Type *falseTy = nullptr;
  // (A method call in the first arm is typed in its place, whether or not
  // it turns out to be a generic method's: what an arm moves is seen in the
  // order the arms are written.)
  if (!expected && needsOtherSide(node->getTrueExpr()) >
                       needsOtherSide(node->getFalseExpr())) {
    falseTy = visitArm(node->getFalseExpr(), nullptr);
    trueTy = visitArm(node->getTrueExpr(), falseTy);
  } else {
    trueTy = visitArm(node->getTrueExpr(), expected);
    falseTy = visitArm(node->getFalseExpr(), expected ? expected : trueTy);
  }
  if (!condTy || !trueTy || !falseTy)
    return nullptr;

  if (condTy != S.Ctx.getBoolTy()) {
    S.error(node->getCondition()->getLocation(),
            "ternary condition must be 'bool', got '" + typeName(condTy) + "'");
    return nullptr;
  }

  if (trueTy == falseTy) {
    if (holdsFixedArray(trueTy)) {
      S.errorFixedArrayHere(node->getLocation(), trueTy,
                            "a conditional expression");
      return nullptr;
    }
    node->setResolvedType(trueTy);
    return trueTy;
  }

  // Optional unification (issue #5):
  //   if c then x else None   — `T?` when x is a reference type T (or T?);
  //   if c then a else b      — `T?` when either side is optional and the
  //                             wrapped types unify (equal, or class LCA).
  // A `None` branch is re-typed to the optional result (see checkAssignable)
  // so the lowering produces a null box for it.
  {
    ast::Expr *trueExpr = node->getTrueExpr();
    ast::Expr *falseExpr = node->getFalseExpr();
    bool trueNone = ast::isa<ast::NoneLiteral>(trueExpr);
    bool falseNone = ast::isa<ast::NoneLiteral>(falseExpr);
    ast::Type *result = nullptr;
    if (trueNone != falseNone) {
      ast::Type *valueTy = trueNone ? falseTy : trueTy;
      // `x` a `T?` already, a reference `T`, a function value, or a
      // primitive (`int` -> `int?`).
      if (ast::isa<ast::OptionalType>(valueTy))
        result = valueTy;
      else if (ast::isRefType(valueTy) || ast::isFunctionValue(valueTy) ||
               ast::isa<ast::EnumType>(valueTy) ||
               ast::isa<ast::TupleType>(valueTy) ||
               (ast::isa<ast::BuiltinType>(valueTy) &&
                valueTy != S.Ctx.getVoidTy()))
        result = S.Ctx.getOptionalType(valueTy);
    } else if (ast::isa<ast::OptionalType>(trueTy) ||
               ast::isa<ast::OptionalType>(falseTy)) {
      ast::Type *a = ast::stripOptional(trueTy);
      ast::Type *b = ast::stripOptional(falseTy);
      ast::Type *inner = nullptr;
      if (typesEqual(a, b))
        inner = a;
      else if (auto *ac = ast::dyn_cast<ast::ClassType>(a))
        if (auto *bc = ast::dyn_cast<ast::ClassType>(b))
          inner = S.findLowestCommonAncestor(ac, bc);
      if (inner)
        result = S.Ctx.getOptionalType(inner);
    }
    if (result) {
      if (trueNone)
        trueExpr->setResolvedType(result);
      if (falseNone)
        falseExpr->setResolvedType(result);
      // A plain primitive branch of an optional primitive result is its
      // present value.
      if (ast::wrapsInOptional(result, trueTy))
        trueExpr->setCoercedType(result);
      if (ast::wrapsInOptional(result, falseTy))
        falseExpr->setCoercedType(result);
      node->setResolvedType(result);
      return result;
    }
  }

  // Values whose parts differ only in their forms join to the stronger
  // (#368), when the arm that converts into it is a fresh value: a place's
  // unique parts stay its own.
  if (ast::Type *joined = S.joinPartForms(trueTy, falseTy);
      joined && (joined == trueTy || S.isFreshValue(node->getTrueExpr())) &&
      (joined == falseTy || S.isFreshValue(node->getFalseExpr()))) {
    node->setResolvedType(joined);
    return joined;
  }

  // For class types, find the lowest common ancestor in the hierarchy.
  auto *trueCT = ast::dyn_cast<ast::ClassType>(trueTy);
  auto *falseCT = ast::dyn_cast<ast::ClassType>(falseTy);
  if (trueCT && falseCT) {
    if (auto *lca = S.findLowestCommonAncestor(trueCT, falseCT)) {
      node->setResolvedType(lca);
      return lca;
    }
  }

  S.error(node->getLocation(), "ternary branches have incompatible types '" +
                                   typeName(trueTy) + "' and '" +
                                   typeName(falseTy) + "'");
  return nullptr;
}

// -- Tuples

// A tuple literal's type is the canonical tuple of its element types.  There
// is no contextual typing: `(1, "a")` is `(int, Str)` even when assigned to a
// `(int, Obj)` variable (assignability then applies element-wise, see
// isAssignable).
ast::Type *
Sema::ExprChecker::visitTupleLiteralExpr(ast::TupleLiteralExpr *node) {
  // The slot this literal flows into, if known (see visitExpecting): its
  // element types are the expectations of the elements.  The literal's own
  // type is still its elements' (checkAssignable adopts the slot's types).
  ast::Type *expected = std::exchange(S.LiteralExpectation, nullptr);
  auto *expectedTT =
      expected ? ast::dyn_cast<ast::TupleType>(ast::stripOptional(expected))
               : nullptr;
  // A struct slot (nominal) expects nothing of a tuple literal.
  if (expectedTT && (ast::isa<ast::StructType>(expectedTT) ||
                     expectedTT->getArity() != node->getNumElements()))
    expectedTT = nullptr;
  std::vector<ast::Type *> elemTys;
  std::vector<ast::Qualifier> elemQuals;
  elemTys.reserve(node->getNumElements());
  elemQuals.reserve(node->getNumElements());
  bool ok = true;
  for (size_t i = 0; i < node->getNumElements(); ++i) {
    ast::Expr *elem = node->getElements()[i];
    auto *ty = visitExpecting(elem, expectedTT ? expectedTT->getElementType(i)
                                               : nullptr);
    if (!ty) {
      ok = false;
      continue;
    }
    if (ty == S.Ctx.getVoidTy()) {
      S.error(elem->getLocation(), "tuple element " + std::to_string(i) +
                                       " has type 'void' (the expression "
                                       "produces no value)");
      ok = false;
      continue;
    }
    if (ast::isa<ast::TaskType>(ty)) {
      S.error(elem->getLocation(), "a tuple cannot hold a task yet: a task "
                                   "is held by a variable");
      ok = false;
      continue;
    }
    // The tuple owns its elements: a move-only one moves in (#369), and
    // each converts into its element's form (#368), the slot's or, with
    // none, its own `shared` one (`(1, s)` is an `(int, shared Node)`).
    ast::Qualifier q;
    if (expectedTT)
      q = expectedTT->getElemQual(i);
    else if (const ast::Qualifier own = S.qualifierOf(elem);
             own.Form == ast::QualForm::Shared && ast::isRefType(ty))
      q = own;
    if (!S.checkQualifiedFlow(q, elem, "an element of the tuple",
                              elem->getLocation()))
      ok = false;
    if (holdsFixedArray(ty)) {
      S.errorFixedArrayHere(elem->getLocation(), ty,
                            "tuple element " + std::to_string(i));
      ok = false;
      continue;
    }
    // The tuple holds it: a `view` that shares what it holds is not one.
    if (!S.checkViewNotStored(elem, ty))
      ok = false;
    elemTys.push_back(ty);
    elemQuals.push_back(q);
  }
  if (!ok)
    return nullptr;
  auto *tt = S.Ctx.getTupleType(std::move(elemTys), std::move(elemQuals));
  node->setResolvedType(tt);
  return tt;
}

ast::Type *Sema::ExprChecker::visitTupleIndexExpr(ast::TupleIndexExpr *node) {
  auto *recvTy = visit(node->getTuple());
  if (!recvTy)
    return nullptr;
  if (auto *st = ast::dyn_cast<ast::StructType>(recvTy)) {
    std::string fields;
    for (const std::string &f : st->getFieldNames())
      fields += (fields.empty() ? "" : ", ") + f;
    S.error(node->getLocation(),
            "tuple index '." + std::to_string(node->getIndex()) +
                "' applied to struct type '" + st->getName() +
                "': a struct's fields are named (" + fields + ")");
    return nullptr;
  }
  // An element cannot be read through an optional tuple (it may be None).
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(recvTy);
      ot && ast::isa<ast::TupleType>(ot->getInnerType())) {
    S.errorOptionalUnwrap(node->getLocation(), ot);
    return nullptr;
  }
  auto *tt = ast::dyn_cast<ast::TupleType>(recvTy);
  if (!tt) {
    S.error(node->getLocation(),
            "tuple index '." + std::to_string(node->getIndex()) +
                "' applied to non-tuple type '" + typeName(recvTy) + "'");
    return nullptr;
  }
  if (node->getIndex() >= tt->getArity()) {
    S.error(node->getLocation(),
            "tuple index '." + std::to_string(node->getIndex()) +
                "' is out of range for type '" + typeName(tt) +
                "' (valid indices are .0 to ." +
                std::to_string(tt->getArity() - 1) + ")");
    return nullptr;
  }
  // Record the receiver's type too: the lowering reads it to pick the element
  // representation without re-deriving the receiver's type.
  node->getTuple()->setResolvedType(tt);
  ast::Type *elemTy = tt->getElementType(node->getIndex());
  node->setResolvedType(elemTy);
  return elemTy;
}

// `a, b = e;` — e must be a tuple whose arity equals the number of targets.
// Each named target follows the binding rule of the corresponding
// single-variable statement: an annotated target is a fresh declaration
// (VarDecl rules: no redeclaration in the current scope, initializer
// assignable to the annotation), a bare name is declared on first use or
// re-assigned if already visible (AssignStmt rules), and `_` discards the
// element.
bool Sema::visitDestructureStmt(ast::DestructureStmt *node) {
  // After a reported failure (the value is in error, is not a tuple, or has
  // the wrong arity) the targets are still bound, so their uses are not
  // reported as undeclared: an annotated target whose type resolves takes that
  // type, any other new target is poisoned, and a visible variable keeps its
  // type.
  auto bindTargetsAfterError = [&]() {
    StringSet seen;
    for (const auto &target : node->getTargets()) {
      if (target.isSkip())
        continue;
      const std::string &name = target.getName();
      if (!seen.insert(name).second || isTypeNameForVariable(name))
        continue;
      // A function's name is reported all the same, and poisoned, as in
      // visitAssignStmt.
      if (isFunctionNameForVariable(name)) {
        if (!rebindsPoisonedFunctionName(name)) {
          checkBinderName(name, target.Loc);
          if (!CurrentScope->findOwner(name))
            declarePoisoned(CurrentScope, name);
        }
        continue;
      }
      if (target.DeclType) {
        if (CurrentScope->contains(name))
          continue; // a redeclaration: the existing binding stands
        // An annotation that does not resolve is a separate error of its own.
        if (auto *declTy = resolveType(target.DeclType, target.Loc,
                                       "destructuring target '" + name + "'")) {
          CurrentScope->declare(name, declTy);
          if (const ast::Qualifier q = takeQualifier(); !q.isUnique())
            CurrentScope->Quals[name] = q;
        } else {
          declarePoisoned(CurrentScope, name);
        }
        continue;
      }
      if (!CurrentScope->findOwner(name))
        declarePoisoned(CurrentScope, name);
    }
    return false;
  };

  auto *valTy = resolveExprType(node->getValue());
  if (!valTy)
    return bindTargetsAfterError();

  // Nor is an optional tuple destructured.
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(valTy);
      ot && ast::isa<ast::TupleType>(ot->getInnerType())) {
    errorOptionalUnwrap(node->getValue()->getLocation(), ot);
    return bindTargetsAfterError();
  }
  auto *tt = ast::dyn_cast<ast::TupleType>(valTy);
  if (!tt || ast::isa<ast::StructType>(valTy)) {
    error(node->getValue()->getLocation(),
          "cannot destructure a value of type '" + typeName(valTy) +
              "'; only tuples can be destructured");
    return bindTargetsAfterError();
  }
  if (tt->getArity() != node->getNumTargets()) {
    error(node->getLocation(),
          "cannot destructure a value of type '" + typeName(tt) + "' into " +
              std::to_string(node->getNumTargets()) + " targets (it has " +
              std::to_string(tt->getArity()) + " elements)");
    return bindTargetsAfterError();
  }
  // The lowering extracts the elements from this resolved type.
  node->getValue()->setResolvedType(tt);

  // Each element is stored in its target: a move-only one moves (#369).
  bool ok = true;
  for (size_t i = 0; i < tt->getArity() && ok; ++i)
    ok = checkViewNotStored(node->getValue(), tt->getElementType(i));
  if (ok && !checkCopyRule(node->getValue(), node->getValue()->getLocation()))
    return bindTargetsAfterError();
  // Element @p i's form (#368) into a target held as @p want: as it is,
  // or a unique reference into a `shared` one (the tuple is a temporary or
  // moved, under the copy rule); else the error, and false.
  auto elementFlows = [&](size_t i, ast::Qualifier want,
                          const std::string &name, ast::SourceLocation loc) {
    const ast::Qualifier have = tt->getElemQual(i);
    ast::Type *elemTy = tt->getElementType(i);
    if (have == want || !ast::isRefType(elemTy) ||
        (have.isUnique() && (want.Form == ast::QualForm::Unique ||
                             want.Form == ast::QualForm::Shared)))
      return true;
    error(loc,
          "cannot store element " + std::to_string(i) + " of the tuple (a '" +
              qualifiedName(have, elemTy) + "') in variable '" + name +
              "', a '" + qualifiedName(want, elemTy) + "': " +
              (have.isUnique() ? std::string("a destructuring makes no cell or "
                                             "frozen object")
                               : std::string("a '") + have.spelling() +
                                     "' reference converts only as it is"));
    return false;
  };
  StringSet seen;
  for (size_t i = 0; i < node->getNumTargets(); ++i) {
    const auto &target = node->getTargets()[i];
    if (target.isSkip())
      continue;
    const std::string &name = target.getName();
    ast::Type *elemTy = tt->getElementType(i);

    if (!seen.insert(name).second) {
      error(target.Loc, "duplicate target '" + name + "' in destructuring");
      ok = false;
      continue;
    }
    if (rebindsPoisonedFunctionName(name)) {
      ok = false;
      continue;
    }
    if (!checkBinderName(name, target.Loc) || !checkNotSelf(name, target.Loc)) {
      // A function's name is poisoned, as in visitAssignStmt.
      if (isFunctionNameForVariable(name) && !CurrentScope->findOwner(name))
        declarePoisoned(CurrentScope, name);
      ok = false;
      continue;
    }

    if (target.DeclType) {
      // Annotated target: a fresh declaration in the current scope.
      if (CurrentScope->contains(name)) {
        error(target.Loc, "redeclaration of variable '" + name + "'");
        ok = false;
        continue;
      }
      auto *declTy = resolveType(target.DeclType, target.Loc,
                                 "destructuring target '" + name + "'");
      const ast::Qualifier declQual = takeQualifier();
      if (!declTy) {
        declarePoisoned(CurrentScope, name);
        ok = false;
        continue;
      }
      // A destructured element is stored as-is: an `int` element cannot
      // fill an `int?` target (it would need wrapping; unwrap or re-declare).
      if (!isAssignable(declTy, elemTy) ||
          ast::wrapsInOptional(declTy, elemTy)) {
        error(target.Loc, "element " + std::to_string(i) + " of type '" +
                              typeName(elemTy) +
                              "' does not match declared type '" +
                              typeName(declTy) + "' for target '" + name + "'");
        // The annotation still types the variable.
        CurrentScope->declare(name, declTy);
        ok = false;
        continue;
      }
      CurrentScope->declare(name, declTy);
      if (!declQual.isUnique())
        CurrentScope->Quals[name] = declQual;
      if (!elementFlows(i, declQual, name, target.Loc))
        ok = false;
      continue;
    }

    // Bare name: re-assignment if visible, implicit declaration otherwise.
    // A new one is held as its element is (#368).
    auto *owner = CurrentScope->findOwner(name);
    if (!owner) {
      CurrentScope->set(name, elemTy);
      if (ast::Qualifier q = tt->getElemQual(i); !q.isUnique()) {
        q.FromArgument = isArgumentForm(q, elemTy); // a type argument's
        CurrentScope->Quals[name] = q;
      }
      continue;
    }
    if (!checkReassignable(name, target.Loc)) {
      ok = false;
      continue;
    }
    if (isOuterScope(owner)) {
      error(target.Loc, "cannot assign to '" + name +
                            "' in a 'parallel for' body: a variable of the "
                            "enclosing function changes only by a reduction");
      ok = false;
      continue;
    }
    auto *varTy = owner->lookup(name);
    // A poisoned variable is re-declared with the element's type.
    if (ast::isa<ast::PoisonType>(varTy)) {
      owner->set(name, elemTy);
      continue;
    }
    if (!isAssignable(varTy, elemTy) || ast::wrapsInOptional(varTy, elemTy)) {
      error(target.Loc, "cannot assign element " + std::to_string(i) +
                            " of type '" + typeName(elemTy) +
                            "' to variable '" + name + "' of type '" +
                            typeName(varTy) + "'");
      ok = false;
      continue;
    }
    // A weak variable names a `shared` (`shared mut`) object: an element
    // held so (#368), or a literal's None.
    const ast::Qualifier want = qualifierOfVar(name);
    if (const VarKind k = varKind(name);
        k == VarKind::Weak || k == VarKind::WeakView) {
      const auto *lit = ast::dyn_cast<ast::TupleLiteralExpr>(node->getValue());
      const ast::Expr *elem = lit && i < lit->getElements().size()
                                  ? lit->getElements()[i]
                                  : nullptr;
      if ((!elem || !ast::isa<ast::NoneLiteral>(elem)) &&
          !(tt->getElemQual(i) == want)) {
        const char *form = want.Mut ? "weak mut" : "weak";
        error(target.Loc, "cannot store element " + std::to_string(i) +
                              " of the tuple (a '" +
                              qualifiedName(tt->getElemQual(i), elemTy) +
                              "') in variable '" + name + "', a '" + form +
                              " " + typeName(varTy) + "': a '" + form +
                              "' reference names a '" +
                              qualifiedName(want, nullptr) + "' object");
        ok = false;
        continue;
      }
    } else if (!elementFlows(i, want, name, target.Loc)) {
      ok = false;
      continue;
    }
    MovedVars.erase(varKey(owner, name)); // assigned again after a move
  }
  return ok;
}

// -- Entry point

SemaContext Sema::run(ast::TranslationUnit *tu) {
  CurrentScope = nullptr;
  AccumulatedImportContexts.clear();
  OwnSourceName = Diags.getSourceName();
  OwnSourceLines = Diags.getSourceLines();

  // Printing and files are the standard library's (stdlib/io.pkn).

  // Register type-conversion builtins (take unique builtin types — no ownership
  // check needed).
  auto *StrTy = Ctx.getStrTy();
  // The conversions (`Str<int>(n)`, `int<float>(x)`, ...) are not functions:
  // checkConversion handles them (#64).
  declareFunction(names::kString, StrTy, {StrTy}, true);
  // `Str(s)` of a `Str` copies it (#369): it reads a `view`.
  FunctionTable[names::kString].Modes.Modes = {ast::ParamMode::View};
  FunctionTable[names::kString].Modes.Names = {"s"};

  registerBuiltinTraits();

  // Imports go through a resolver, so a prebuilt module (the standard
  // library's) is found too: without the caller's, one with no cache.
  if (!Resolver) {
    modules::ResolverOptions opts;
    opts.ProjectRoot = ProjectRoot;
    opts.FrontendName = FrontendName;
    opts.ReadCache = false;
    opts.WriteCache = false;
    opts.ReadFrontend = readImport;
    OwnedResolver = std::make_shared<modules::ModuleResolver>(
        std::move(opts), modules::ModuleResolver::hostIdentity());
    Resolver = OwnedResolver.get();
  }

  // Process imports before local declarations.
  StringSet localImportStack, localFailedModules;
  unsigned localImportDepth = 0;
  if (!ImportStack)
    ImportStack = &localImportStack;
  if (!FailedModules)
    FailedModules = &localFailedModules;
  if (!ImportDepth)
    ImportDepth = &localImportDepth;
  claimImportQualifiers(tu->getImports());
  for (auto *imp : tu->getImports())
    processImport(imp);

  visit(tu);
  // A binder is poisoned only after its declaration's error was reported, so
  // a program without errors has none, and the poison type never reaches the
  // lowering.  Guard that invariant: never hand such a program on.  An
  // import of a module whose failure another import reported counts no
  // error here (RepeatedImportFailures), and what this module takes from it
  // is poisoned: that report covers it, and the module fails all the same.
  if (PoisonedBindings && !hasErrors() && RepeatedImportFailures == 0) {
    assert(false && "a binder was poisoned without an error being reported");
    error(tu->getLocation(),
          "internal compiler error: a declaration failed without a diagnostic");
  }
  SemaContext result{nullptr,
                     &Ctx,
                     nullptr,
                     !hasErrors() && RepeatedImportFailures == 0,
                     getErrorCount(),
                     RepeatedImportFailures != 0,
                     Diags.getDiagnostics(),
                     std::move(AccumulatedImportContexts),
                     std::move(ForeignInstantiatedClassDecls),
                     std::move(ForeignInstantiatedFuncDecls),
                     std::move(ForeignInstantiatedStructDecls),
                     std::move(ForeignInstantiationSources),
                     OwnedResolver,
                     std::move(ImportedNatives),
                     std::move(ImportedInstanceClassDecls),
                     std::move(ImportedInstanceStructDecls),
                     {OwnSourceName, OwnSourceLines, nullptr, ""},
                     std::move(ForeignMethodClassDecls),
                     std::move(ForeignMethodStructDecls),
                     {}};
  // The structs with generic methods: one that is not generic by its own
  // table, an instance by its template's (whichever block they are in).
  for (const auto &[name, st] : Ctx.getStructTypes()) {
    if (name != st->getName())
      continue; // an alias
    auto own = MethodTemplates.find(name);
    bool has = own != MethodTemplates.end() && !own->second.empty();
    if (!has)
      if (const StructInstantiationInfo *info = structInstantiationInfo(st))
        if (auto of = TemplateMethodTemplates.find(info->Template);
            of != TemplateMethodTemplates.end())
          has = !of->second.empty();
    if (has)
      result.GenericMemberTypes.insert(name);
  }
  // The sets live in this frame: a Sema is run once.
  if (ImportStack == &localImportStack)
    ImportStack = nullptr;
  if (FailedModules == &localFailedModules)
    FailedModules = nullptr;
  if (ImportDepth == &localImportDepth)
    ImportDepth = nullptr;
  return result;
}

// -- Conversion constructors (#64, #88)
//
// `Target<Source>(value)` converts a value to a primitive or to `Str`.  Each
// primitive target has a fixed, closed set of specializations (the valid
// sources), and no other type, builtin or user-defined, can be one.  `Str`
// takes every type that has a text (hasText): a scalar through the runtime,
// any other type as its `toString()` (the lowering's showCallOf), and a `Str`
// is itself.  The source is written (`Str<int>(n)`, checkConversion) or
// inferred from the argument's exact type (`Str(n)`, inferConversion).  The
// numeric targets take the sources of their family
// (docs/language/04-strings-and-conversions.md, "Between numbers"), and the
// error of a pair a family leaves out names the rewrite.  The semantics live
// in the lowering (Lowering/LoweringExpr.cpp, emitConversion and the runtime
// table), which both backends inherit.  A primitive does not parse a `Str`:
// that is a method of the string (`s.toInt()`), which the error names.

struct ConversionPair {
  std::string Target;
  std::string Source;
  std::string Spelled; // the rebound callee the lowering dispatches on
};

namespace {

/// The primitive sources, in the order a diagnostic lists them.
constexpr ast::BuiltinType::Kind kConversionSources[] = {
    ast::BuiltinType::Int, ast::BuiltinType::Float, ast::BuiltinType::I8,
    ast::BuiltinType::I16, ast::BuiltinType::I32,   ast::BuiltinType::U8,
    ast::BuiltinType::U16, ast::BuiltinType::U32,   ast::BuiltinType::U64,
    ast::BuiltinType::F16, ast::BuiltinType::F32,   ast::BuiltinType::Bool,
    ast::BuiltinType::Char};

/// Whether `t(x)` takes an `s` (docs/language/04-strings-and-conversions.md,
/// "Between numbers"): every primitive its own value; the float family every
/// number; the unsigned family every integer, `bool` and `char`; the signed
/// family the same but `unsigned`, and every float; `bool` and `char` every
/// integer.
bool convertsFrom(ast::BuiltinType::Kind t, ast::BuiltinType::Kind s) {
  using B = ast::BuiltinType;
  if (t == s)
    return true;
  if (B::isFloatKind(t))
    return B::isNumericKind(s);
  if (B::isUnsignedKind(t))
    return B::isIntegerKind(s) || s == B::Bool || s == B::Char;
  if (B::isIntegerKind(t))
    return (B::isIntegerKind(s) && s != B::U64) || B::isFloatKind(s) ||
           s == B::Bool || s == B::Char;
  if (t == B::Bool || t == B::Char)
    return B::isIntegerKind(s);
  return false;
}

/// The conversions, grouped by target, each group in the order its
/// diagnostics list it: `Str` from every scalar (the runtime's; any other
/// type with a text is Sema::textConversion's), and each primitive from the
/// sources convertsFrom gives it, in kConversionSources' order.
const std::vector<ConversionPair> &conversions() {
  static const std::vector<ConversionPair> kTable = [] {
    using ast::BuiltinType;
    std::vector<ConversionPair> t;
    auto add = [&](const std::string &target, const std::string &source) {
      t.push_back({target, source, target + "<" + source + ">"});
    };
    for (BuiltinType::Kind source : kConversionSources)
      add(names::kString, ast::builtinKindName(source));
    for (BuiltinType::Kind target : kConversionSources)
      for (BuiltinType::Kind source : kConversionSources)
        if (convertsFrom(target, source))
          add(ast::builtinKindName(target), ast::builtinKindName(source));
    return t;
  }();
  return kTable;
}

/// The rewrite of a pair the families leave out, after the target and the
/// source: an `unsigned` into the signed family, a float into the unsigned
/// family, a `bool` or a `char` into the float family.  Empty for any other
/// pair.
std::string conversionHint(const std::string &target,
                           const std::string &source) {
  using B = ast::BuiltinType;
  B::Kind t = B::Void;
  B::Kind s = B::Void;
  if (!ast::builtinKindOfName(target, t) || !ast::builtinKindOfName(source, s))
    return {};
  const std::string bits = std::to_string(B::integerBits(t));
  if (B::isIntegerKind(t) && !B::isUnsignedKind(t) && s == B::U64) {
    if (t == B::Int)
      return ": an 'unsigned' can be larger than any 'int'; its method '" +
             std::string(names::kMethodToInt) +
             "()' returns 'int?', None above 9223372036854775807";
    return ": an 'unsigned' can be larger than any '" + target + "'; '" +
           target + "(u" + bits + "(...))' keeps its low " + bits + " bits";
  }
  if (B::isUnsignedKind(t) && B::isFloatKind(s))
    return ": convert the '" + source + "' to an 'int' first: '" + target +
           "(int(...))' truncates toward zero, then keeps the low " + bits +
           " bits";
  if (B::isFloatKind(t) && (s == B::Bool || s == B::Char))
    return ": convert the '" + source + "' to an 'int' first: '" + target +
           "(int(...))'";
  return {};
}

/// The specialization set of @p target, for a diagnostic: "int, float, ...".
std::string specializationList(const std::string &target) {
  std::string list;
  for (const auto &c : conversions())
    if (target == c.Target)
      list += (list.empty() ? "" : ", ") + c.Source;
  return list;
}

/// The pair `target<source>`, or nullptr when it is not a specialization.
const ConversionPair *findConversion(const std::string &target,
                                     const std::string &source) {
  for (const auto &c : conversions())
    if (target == c.Target && source == c.Source)
      return &c;
  return nullptr;
}

/// The error of a parse written as a conversion (`int(s)`, `u8<Str>(s)`):
/// no primitive parses a `Str`, the method of `Str` that parses into
/// @p target does (docs/language/04-strings-and-conversions.md, "Parsing a
/// string").  Empty when @p source is not `Str` or @p target has no parse.
std::string parseMovedMessage(const std::string &target,
                              const std::string &source) {
  ast::BuiltinType::Kind kind = ast::BuiltinType::Void;
  if (source != names::kString || !ast::builtinKindOfName(target, kind))
    return {};
  const char *method = ast::strParseMethod(kind);
  if (!method)
    return {};
  return "no specialization of '" + target +
         "' for 'Str': parse a 'Str' with its method '" + method +
         "()', which returns '" + target + "?'";
}

/// The no-match error, for the explicit and the inferred form alike: a
/// parse's names its method, a pair a family leaves out its rewrite
/// (conversionHint), any other lists the target's specializations.
/// Either way the call has no type, so nothing built on it is reported
/// again: a program written for the older parse gets one error at each.
std::string noSpecializationMessage(const std::string &target,
                                    const std::string &source) {
  if (std::string moved = parseMovedMessage(target, source); !moved.empty())
    return moved;
  const std::string miss =
      "no specialization of '" + target + "' for '" + source + "'";
  if (std::string hint = conversionHint(target, source); !hint.empty())
    return miss + hint;
  return miss + "; its specializations are " + specializationList(target);
}

/// The conversion target of a callee: the target itself (`int`), or the
/// target of the spelled form (`int<float>`) a checked call carries, under
/// its printed name (`u64` and `i64` are `unsigned` and `int`).
std::string conversionTargetOf(const std::string &callee) {
  std::string target = callee.substr(0, callee.find('<'));
  ast::BuiltinType::Kind kind;
  if (ast::builtinKindOfName(target, kind))
    return ast::builtinKindName(kind);
  return target;
}

} // namespace

bool Sema::isConversionTarget(std::string_view name) {
  // A call checked once already carries its spelled form (`int<float>`).
  const std::string_view target = name.substr(0, name.find('<'));
  if (target == names::kString)
    return true;
  ast::BuiltinType::Kind kind;
  return ast::builtinKindOfName(target, kind) && kind != ast::BuiltinType::Void;
}

bool Sema::isStrConstruction(ast::CallExpr *node,
                             const std::vector<ast::Type *> &argTypes) const {
  if (node->getCalleeName() != names::kString)
    return false;
  // A wrong argument count is the constructor's arity error; an argument
  // already in error is reported.
  if (argTypes.size() != 1 || !argTypes[0])
    return true;
  // A `Str` is itself; a `Str?` has a text, "None" or the string.
  return typesEqual(argTypes[0], Ctx.getStrTy());
}

bool Sema::textConversion(ast::Type *source, ast::SourceLocation loc,
                          ConversionPair &pair) {
  const std::string name = typeName(source);
  // `Str<Str>(s)` is `s`: the constructor.
  if (typesEqual(source, Ctx.getStrTy())) {
    pair = {names::kString, name, names::kString};
    return true;
  }
  if (!hasText(source)) {
    const std::string why = noTextReason(source);
    error(loc, std::string("'") + names::kString +
                   "' cannot convert a value of type '" + name +
                   "', which does not implement trait '" +
                   names::kTraitShowable + "'" +
                   (why.empty() ? "" : ": " + why));
    return false;
  }
  pair = {names::kString, name, std::string(names::kString) + "<" + name + ">"};
  return true;
}

ast::Type *Sema::inferConversion(ast::CallExpr *node,
                                 const std::vector<ast::Type *> &argTypes) {
  const std::string target = conversionTargetOf(node->getCalleeName());
  if (argTypes.size() != 1) {
    error(node->getLocation(), "conversion '" + target +
                                   "' takes exactly one argument, got " +
                                   std::to_string(argTypes.size()));
    return nullptr;
  }
  ast::Type *argTy = argTypes[0];
  if (!argTy)
    return nullptr; // already reported
  // The specialization whose source is exactly the argument's type: no
  // widening (an `int` is not a `float`), no unwrapping of an optional.
  const std::string sourceName = typeName(argTy);
  const ConversionPair *pair = findConversion(target, sourceName);
  ConversionPair text;
  if (!pair && target == names::kString) {
    if (!textConversion(argTy, node->getLocation(), text))
      return nullptr;
    pair = &text;
  }
  if (!pair) {
    error(node->getLocation(), noSpecializationMessage(target, sourceName));
    return nullptr;
  }
  return applyConversion(node, pair);
}

ast::Type *Sema::applyConversion(ast::CallExpr *node,
                                 const ConversionPair *pair) {
  ast::Type *result = Ctx.lookupType(pair->Target);
  node->setCalleeName(Ctx.intern(pair->Spelled));
  node->setResolvedType(result);
  return result;
}

ast::Type *Sema::checkConversion(ast::CallExpr *node,
                                 const std::vector<ast::Type *> &argTypes) {
  // The callee is the target (`int`), or its spelled form (`int<float>`) if
  // this call was checked before.
  const std::string target = conversionTargetOf(node->getCalleeName());
  if (node->getTypeArgs().size() != 1) {
    error(node->getLocation(),
          "conversion '" + target +
              "<...>' takes exactly one source type, got " +
              std::to_string(node->getTypeArgs().size()));
    return nullptr;
  }
  ast::Type *source = resolveType(node->getTypeArgs()[0], node->getLocation(),
                                  "source type of conversion '" + target + "'");
  if (!source)
    return nullptr;
  const std::string sourceName = typeName(source);
  const std::string spelled = target + "<" + sourceName + ">";

  // A primitive's specializations are a closed set: any other type
  // argument, a user class included, is an error.  `Str` takes every type
  // with a text.
  const ConversionPair *pair = findConversion(target, sourceName);
  ConversionPair text;
  if (!pair && target == names::kString) {
    if (!textConversion(source, node->getLocation(), text))
      return nullptr;
    pair = &text;
  }
  if (!pair) {
    error(node->getLocation(), noSpecializationMessage(target, sourceName));
    return nullptr;
  }

  if (argTypes.size() != 1) {
    error(node->getLocation(), "conversion '" + spelled +
                                   "' takes exactly one argument, got " +
                                   std::to_string(argTypes.size()));
    return nullptr;
  }
  ast::Type *argTy = argTypes[0];
  if (!argTy)
    return nullptr; // already reported
  // The source type is written out, so the argument must have exactly that
  // type: no int -> float promotion, no unwrapping of an optional.
  if (!typesEqual(argTy, source)) {
    if (auto *ot = ast::dyn_cast<ast::OptionalType>(argTy);
        ot && typesEqual(ot->getInnerType(), source)) {
      errorOptionalUnwrap(node->getArguments()[0]->getLocation(), ot);
      return nullptr;
    }
    error(node->getArguments()[0]->getLocation(),
          "argument of '" + spelled + "' has type '" + typeName(argTy) +
              "', expected '" + sourceName + "'");
    return nullptr;
  }
  // (`Str<Str>(s)` makes a new string, so it reads a `view` as any other
  // conversion does, #369.)

  return applyConversion(node, pair);
}

// -- Top-level

bool Sema::visitTranslationUnit(ast::TranslationUnit *node) {
  // The module's functions by name, before any binder is checked.
  for (const auto *decls :
       {&node->getFuncDecls(), &node->getGenericFuncDecls()})
    for (const ast::FuncDecl *fd : *decls)
      ModuleFunctionNames.insert(fd->getName());
  bool ok = rejectFreeViewFns(node);
  if (!rejectInferredMethods(node))
    ok = false;
  if (!rejectTopLevelTypePrivate(node))
    ok = false;
  if (!rejectBodyInits(node))
    ok = false;

  // Traits by name first: no other declaration may take one's name.
  if (!declareTraits(node))
    ok = false;
  // `impl` blocks by their class or struct, whose methods they join in
  // checkClassDecls / populateStructs (a generic class's, in
  // registerGenericTemplates, when instantiated).
  for (auto *im : node->getImplDecls())
    if (im->getTypeParams().empty())
      ImplsByClass[im->getTypeName()].push_back(im);

  Unit = node;

  // Register enum types first so that class fields, parameters, and variable
  // declarations can reference them by name during the passes that follow.
  for (auto *ed : node->getEnumDecls())
    if (!visitEnumDecl(ed))
      ok = false;

  // Then the structs' names, as stubs: a class field or a signature may
  // name one.  Their fields come after the classes (populateStructs), as
  // they may name those.
  if (!declareStructs(node))
    ok = false;

  // Register generic class / function templates by name.  They are not
  // checked here; every use below instantiates them on demand (resolveType /
  // resolveGenericCall), so this must precede the first type resolution.
  if (!registerGenericTemplates(node))
    ok = false;

  // Register class types, fields, method signatures, and constructors first, so
  // that function signatures below can name class types (e.g. a function that
  // takes or returns a class).  Method bodies are deferred (checkClassBodies).
  if (!node->getClassDecls().empty())
    if (!checkClassDecls(node->getClassDecls()))
      ok = false;
  if (!populateStructs())
    ok = false;
  // The optionals of tuples and structs resolved so far, now that every
  // struct's size is known; the rest are checked where they are resolved.
  StructsPopulated = true;
  for (const auto &c : DeferredOptionalChecks) {
    std::vector<InstantiationFrame> savedStack = std::move(InstantiationStack);
    InstantiationStack = c.Frames;
    {
      OriginSource inOrigin(*this, c.Origin);
      if (!checkOptionalValueSize(c.Inner, c.Loc, c.Context))
        ok = false;
    }
    InstantiationStack = std::move(savedStack);
  }
  DeferredOptionalChecks.clear();
  // Every type's name is known: the fields of a template that was not
  // instantiated yet are checked as an instantiation's would be, in the
  // order the templates are written.
  std::vector<const ast::Decl *> templates(node->getGenericClassDecls().begin(),
                                           node->getGenericClassDecls().end());
  templates.insert(templates.end(), node->getGenericStructDecls().begin(),
                   node->getGenericStructDecls().end());
  templates.insert(templates.end(), node->getGenericEnumDecls().begin(),
                   node->getGenericEnumDecls().end());
  // By position, never by address, so the order is deterministic; stable,
  // so templates without a position keep the AST's order.
  // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order)
  std::stable_sort(templates.begin(), templates.end(),
                   [](const ast::Decl *a, const ast::Decl *b) {
                     return std::pair(a->getLocation().getLineStart(),
                                      a->getLocation().getColumnStart()) <
                            std::pair(b->getLocation().getLineStart(),
                                      b->getLocation().getColumnStart());
                   });
  for (const ast::Decl *tmpl : templates)
    checkTemplateFieldNames(tmpl);
  // Then the traits' signatures, which may name the classes, and the
  // `impl` blocks, which record what implements a trait; only then can a
  // template's bounds be checked (SemaTraits.cpp).
  if (!checkTraitDecls(node))
    ok = false;
  if (!checkImplDecls(node))
    ok = false;
  if (!checkBoundDecls(node))
    ok = false;
  if (!checkPendingBounds())
    ok = false;
  // And what the async methods declared so far return (checkAsyncDecl).
  if (!checkPendingAsyncResults())
    ok = false;

  // Forward-declare every free function's signature next, so that any body —
  // a free function OR a class method — may call any module-level function
  // regardless of the order it is defined.  A function whose declaration was
  // rejected (bad signature, or a name taken by a builtin / class / enum) has
  // no entry of its own; its body is skipped so it is not checked against
  // whatever entry already owns the name.
  std::vector<ast::FuncDecl *> declaredFns;
  declaredFns.reserve(node->getFuncDecls().size());
  for (auto *fn : node->getFuncDecls()) {
    if (declareFunctionSignature(fn))
      declaredFns.push_back(fn);
    else
      ok = false;
  }

  // The functions written `=> expression` with no result type: their
  // bodies first, so every body below sees their result types.
  if (!inferResults(declaredFns))
    ok = false;

  if (!checkEntryPoint(node, declaredFns))
    ok = false;

  // Now check class and struct method bodies (they can resolve free
  // functions) …
  if (!node->getClassDecls().empty())
    if (!checkClassBodies())
      ok = false;
  if (!checkStructBodies())
    ok = false;

  // … and finally free-function bodies (signatures all registered above;
  // the ones with an inferred result were checked by inferResults).
  for (auto *fn : declaredFns)
    if (!fn->hasInferredResult() && !visitFuncDecl(fn))
      ok = false;

  // Bodies of the instantiations requested so far (and of any they request
  // in turn): checked last, like C++ instantiates at the end of the TU.
  if (!checkPendingInstantiations())
    ok = false;
  if (!checkBufferElements())
    ok = false;

  // Each of the module's templates once more, with a move-only class for
  // each parameter without bounds: its copies need `T: Copyable` (#368).
  if (ok && !hasErrors() && !checkCrossedClasses(node))
    ok = false;
  if (ok && !hasErrors() && !checkArchetypes())
    ok = false;

  // Hand the instantiations to the lowering as ordinary declarations.
  injectInstantiations(node);

  // Every body is checked now, and each instantiation is the module's: what
  // a `parallel for` body calls can be judged, and what importers will read.
  if (ok && !hasErrors() && !checkParSafety(node))
    ok = false;

  // The suggestions need every body checked, and are noise next to errors.
  if (ok && !hasErrors())
    warnMissingViewFns();
  return ok;
}

// A program's entry point is `fn main() -> int` or `fn main(args: view
// Str[]) -> int` (the lowering's `@main`, which the PIR verifier checks
// again).  Checked
// here so the error has a source location on every frontend (#132).
bool Sema::checkEntryPoint(ast::TranslationUnit *tu,
                           const std::vector<ast::FuncDecl *> &declaredFns) {
  if (EntryPointCheck == EntryPoint::None)
    return true;
  const std::string expected =
      "'fn main() -> int' or 'fn main(args: view Str[]) -> int'";
  for (auto *fn : tu->getGenericFuncDecls())
    if (fn->getName() == names::kMain) {
      // `fn main(args)` is generic only for leaving the type out.
      std::string_view untyped;
      for (const std::string *p : fn->getTypeParams())
        if (untyped.empty())
          untyped = names::untypedParamOf(*p);
      error(fn->getLocation(),
            (untyped.empty() ? std::string("'main' cannot be generic")
                             : "parameter '" + std::string(untyped) +
                                   "' of 'main' needs a type") +
                "; the program's entry point is " + expected);
      return false;
    }
  for (auto *fn : tu->getFuncDecls()) {
    if (fn->getName() != names::kMain)
      continue;
    // A declaration that was rejected was reported already.
    if (std::find(declaredFns.begin(), declaredFns.end(), fn) ==
        declaredFns.end())
      return false;
    const FunctionSig *sig = lookupFunction(names::kMain);
    if (!sig)
      return false;
    bool params = sig->ParamTypes.empty();
    if (sig->ParamTypes.size() == 1 &&
        sig->Modes.mode(0) == ast::ParamMode::View)
      if (auto *st = ast::dyn_cast<ast::SliceType>(sig->ParamTypes[0]))
        params = st->getElementType() == Ctx.getStrTy();
    ast::Type *ret = sig->ReturnType;
    if (auto *tt = ast::dyn_cast<ast::TaskType>(ret))
      ret = tt->getInnerType(); // `async fn main` (the prototype)
    if (ret == Ctx.getIntTy() && params)
      return true;
    std::string actual = "fn main(";
    for (size_t i = 0; i < sig->ParamTypes.size(); ++i) {
      if (i)
        actual += ", ";
      if (sig->Modes.mode(i) != ast::ParamMode::Value)
        actual += std::string(ast::paramModeName(sig->Modes.mode(i))) + " ";
      actual += typeName(sig->ParamTypes[i]);
    }
    actual += ") -> ";
    actual += typeName(sig->ReturnType);
    std::string msg = "the program's entry point must be ";
    msg += expected;
    msg += ", not '";
    msg += actual;
    msg += "'";
    error(fn->getLocation(), msg);
    return false;
  }
  if (EntryPointCheck != EntryPoint::Required)
    return true;
  // Located at the start of the file (which may be empty), not at whatever
  // declaration happens to come first.
  error(ast::SourceLocation(1, 1, 1, 1),
        "program has no entry point 'fn main() -> int' (or "
        "'fn main(args: view Str[]) -> int')");
  return false;
}

// -- Declarations

bool Sema::visitImplDecl(ast::ImplDecl *) { return true; }
bool Sema::visitTraitDecl(ast::TraitDecl *) { return true; }
bool Sema::visitEnumDecl(ast::EnumDecl *node) {
  // Reject names that collide with a builtin, class, function, or existing
  // enum (the primitive type keywords cannot be spelled as identifiers).
  if (!checkDeclNameAvailable(node->getName(), node->getLocation(),
                              DeclKind::Enum))
    return false;

  auto *enumTy = Ctx.registerEnumType(node->getName(), node->getLocation());
  assert(enumTy && "enum name was checked to be free above");

  bool ok = true;
  StringSet seen;
  for (size_t i = 0; i < node->getNumVariants(); ++i) {
    const std::string &variant = *node->getVariants()[i];
    if (!seen.insert(variant).second) {
      error(node->getLocation(), "duplicate variant '" + variant +
                                     "' in enum '" + node->getName() + "'");
      ok = false;
      continue;
    }
    enumTy->addVariant(variant);
    const auto &fields = node->getPayload(i);
    if (fields.empty())
      continue;
    // The variant's fields are a struct of their own, `E::V` (a name no
    // declaration can take), populated with the module's structs
    // (populateStructs): its fields resolve, and are held in place, as a
    // struct's are.  It has no constructor: `E::V(1)` makes the enum.
    const std::string &name =
        Ctx.intern(node->getName() + names::kQualSep + variant);
    auto *sd =
        Ctx.make<ast::StructDecl>(fields.front()->getLocation(), name, fields);
    auto *st = Ctx.registerStructType(name, sd->getLocation());
    st->setVariantOf(enumTy, enumTy->getNumVariants() - 1);
    enumTy->setPayload(enumTy->getNumVariants() - 1, st);
    DeclaredStructs.push_back(sd);
    EnumPayloadStructs[node->getName()].push_back(name);
  }
  return ok;
}

// -- Statements

bool Sema::visitCompoundStmt(ast::CompoundStmt *node) {
  ScopeGuard guard(*this);
  const bool ok = visitStatements(node->getStatements());
  // The block's task handles are spent where it ends (the prototype); a
  // `return`, `break` or `continue` that left it checked them itself.
  if (ok && !FlowEnded)
    checkTasksAwaited(CurrentScope, nullptr, node->getLocation());
  return ok;
}

bool Sema::visitDeclStmt(ast::DeclStmt *node) {
  // A local starts with its initializer's value, a `weak` one without one as
  // `None` (docs/grammar.md section 6).  The parser and the AST interchange
  // reader build no other; an AST that has one is rejected here, before the
  // lowering would leave a reference null.  The variable is still declared
  // with its type, so its uses are checked as usual.  (A for loop's variable
  // and a field are VarDecls with no initializer too, outside a DeclStmt.)
  auto *vd = ast::dyn_cast<ast::VarDecl>(node->getDecl());
  bool ok = true;
  if (vd && vd->getType() && !vd->getInitExpr() && !vd->isWeak()) {
    error(vd->getLocation(), "variable '" + vd->getName() +
                                 "' needs an initial value (only a 'weak' "
                                 "local starts without one, as None)");
    ok = false;
  }
  return visit(node->getDecl()) && ok;
}

bool Sema::visitExprStmt(ast::ExprStmt *node) {
  // Type-check the expression; we discard the type.
  ast::Type *ty = resolveExprType(node->getExpr());
  if (!ty)
    return false;
  if (ast::isa<ast::TaskType>(ty)) {
    error(node->getLocation(), "this task is never awaited: 'await' the call, "
                               "or hold its task in a variable that is");
    return false;
  }
  return true;
}

Sema::VarKind Sema::varKind(std::string_view name) const {
  for (const Scope *s = CurrentScope; s; s = s->Parent) {
    if (!s->contains(name))
      continue;
    auto it = s->Kinds.find(name);
    return it == s->Kinds.end() ? VarKind::Plain : it->second;
  }
  return VarKind::Plain;
}

bool Sema::checkReassignable(const std::string &name, ast::SourceLocation loc) {
  if (!checkNotBorrowed(name, loc))
    return false;
  if (std::string why = viewedBy(name); !why.empty()) {
    error(loc, why + "; it cannot be assigned");
    return false;
  }
  if (const std::string binding = lookedAtBy(name); !binding.empty()) {
    error(loc, "'" + name + "' cannot be assigned here: 'match' binding '" +
                   binding +
                   "' reads a value of it that runs a '__deinit__' where it "
                   "is replaced, until its arm ends");
    return false;
  }
  if (ast::dyn_cast<ast::SliceType>(CurrentScope->lookup(name))) {
    error(loc, "'" + name + "' is a slice; assign its elements ('" + name +
                   "[i] = ...')");
    return false;
  }
  switch (varKind(name)) {
  case VarKind::Let:
    error(loc,
          "'" + name + "' is declared with 'let' and cannot be reassigned");
    return false;
  case VarKind::ForVar:
    error(loc, "'" + name +
                   "' is the variable of a 'for' loop and cannot be assigned");
    return false;
  case VarKind::Captured:
  case VarKind::CapturedWeak:
    error(loc, "cannot assign to '" + name +
                   "': the function captures it by copy, and the copy is "
                   "read-only (assign it before the function is made, or "
                   "keep it in an object)");
    return false;
  case VarKind::View:
    error(loc, "cannot assign to 'view' parameter '" + name + "'");
    return false;
  case VarKind::ViewLocal:
    error(loc, "cannot assign to 'view' local '" + name + "'");
    return false;
  case VarKind::ViewBinding:
    error(loc, "cannot assign to '" + name + "': it is bound to a 'view'");
    return false;
  case VarKind::Weak:
  case VarKind::WeakView:
  case VarKind::Plain:
  case VarKind::Inout:
    break;
  }
  return true;
}

bool Sema::visitAssignStmt(ast::AssignStmt *node) {
  // The concurrency prototype: an `atomic` variable changes by its
  // operations; a variable of the function around a `parallel for` only by
  // a reduction.
  if (auto *owner = CurrentScope->findOwner(node->getVarName())) {
    ast::Type *varTy = owner->lookup(node->getVarName());
    if (auto *at = ast::dyn_cast<ast::AtomicType>(varTy)) {
      if (!LambdaStack.empty() && !captureVariable(node->getVarName(), varTy,
                                                   owner, node->getLocation()))
        return false;
      return checkAtomicAssign(node, at);
    }
    if (isOuterScope(owner)) {
      // A reduction changes the variable all the same.
      if (!LambdaStack.empty() && !captureVariable(node->getVarName(), varTy,
                                                   owner, node->getLocation()))
        return false;
      if (!checkReassignable(node->getVarName(), node->getLocation()))
        return false;
      return checkParallelAssign(node, varTy);
    }
  }
  // An existing variable's type is the value's expected type.
  ast::Type *expected = nullptr;
  if (auto *existing = CurrentScope->findOwner(node->getVarName()))
    expected = existing->lookup(node->getVarName());
  if (expected && ast::isa<ast::PoisonType>(expected))
    expected = nullptr; // a poisoned variable takes the value's own type
  // Inside a lambda, assigning a variable of the enclosing function
  // captures it first (and the capture, a read-only copy, is then
  // rejected by checkReassignable below).
  if (expected && !LambdaStack.empty() &&
      !captureVariable(node->getVarName(), expected,
                       CurrentScope->findOwner(node->getVarName()),
                       node->getLocation()))
    return false;
  // A first assignment from an `atomic` place (`m = n`, `h = st.hits`)
  // takes its handle, as `let m = n` does (visitVarDecl): a copy of an
  // atomic place is another handle to its cell.
  WantAtomicHandle = !CurrentScope->findOwner(node->getVarName()) &&
                     isPlaceChain(node->getValue());
  auto *valTy = resolveExprType(node->getValue(), expected);
  WantAtomicHandle = false;
  const auto &varName = node->getVarName();
  if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(node->getValue());
      se && se->isAtomicElement() && !CurrentScope->findOwner(varName)) {
    error(node->getValue()->getLocation(),
          "'" + placeText(se) + "' is an element of an '" +
              typeName(
                  const_cast<ast::Expr *>(se->getArray())->getResolvedType()) +
              "': it has no handle to share; declare '" + varName +
              "' with its type to read it");
    declarePoisoned(CurrentScope, varName);
    return false;
  }
  if (!valTy) {
    // The value's error was reported.  A first assignment still declares the
    // variable -- poisoned -- so its uses are not reported as undeclared; an
    // existing variable keeps its type.  A function's name is reported all
    // the same, binding it being an error of its own, so the binding that
    // poisons it always has its error (rebindsPoisonedFunctionName).
    if (isFunctionNameForVariable(varName) &&
        !rebindsPoisonedFunctionName(varName))
      checkBinderName(varName, node->getLocation());
    if (!isTypeNameForVariable(varName) && !CurrentScope->findOwner(varName))
      declarePoisoned(CurrentScope, varName);
    return false;
  }

  // Guard: the target name must not shadow a type, a function or a reserved
  // name.  A function's name is then poisoned, so its uses and assignments
  // that follow are not reported again.
  if (rebindsPoisonedFunctionName(varName))
    return false;
  if (!checkBinderName(varName, node->getLocation())) {
    if (isFunctionNameForVariable(varName) && !CurrentScope->findOwner(varName))
      declarePoisoned(CurrentScope, varName);
    return false;
  }
  // `self` is the method's object, or a struct method's receiver: it names
  // what the method was called on, and cannot name something else.
  if (varName == names::kSelf && CurrentClassCtx &&
      !CurrentClassCtx->MethodName.empty()) {
    error(node->getLocation(),
          std::string("cannot assign to 'self'") +
              (CurrentClassCtx->Struct ? ": assign its fields instead" : ""));
    return false;
  }
  // The variable is still declared after this error.  A `weak` local may
  // take a `view`: it owns nothing (a `weak` local of a `view` reads as
  // `view`s; a cursor walking a borrowed chain is the usual one).
  const bool viewOk = varKind(varName) == VarKind::WeakView ||
                      varKind(varName) == VarKind::Weak ||
                      checkViewNotStored(node->getValue(), valTy);

  // Look up the variable in all enclosing scopes.
  auto *owner = CurrentScope->findOwner(varName);
  if (!owner) {
    // First assignment: declare in the current scope.
    if (ast::isa<ast::SliceType>(valTy)) {
      errorFixedArrayHere(node->getLocation(), valTy,
                          "variable '" + varName + "'");
      declarePoisoned(CurrentScope, varName);
      return false;
    }
    if (!checkNotVoidValue(valTy, varName, node->getLocation())) {
      declarePoisoned(CurrentScope, varName);
      return false;
    }
    CurrentScope->set(varName, valTy);
    DeclLoopDepth[varKey(CurrentScope, varName)] = LoopDepth;
    if (ast::isa<ast::TaskType>(valTy))
      TaskLocals[varKey(CurrentScope, varName)] = node->getLocation();
    // The prototype: the new variable adopts the value's qualifier.
    const ast::Qualifier q =
        sharesStorage(valTy) ? qualifierOf(node->getValue()) : ast::Qualifier{};
    CurrentScope->Quals[varName] = q;
    if (!checkQualifiedFlow(q, node->getValue(), "variable '" + varName + "'",
                            node->getLocation()))
      return false;
    return viewOk;
  }

  if (!checkReassignable(varName, node->getLocation()))
    return false;
  // A task handle is spent before the variable takes another (the
  // prototype): a handle dropped unawaited has no one to wait for it.
  if (ast::Type *heldTy = owner->lookup(varName);
      heldTy && ast::isa<ast::TaskType>(heldTy)) {
    auto spent = MovedVars.find(varKey(owner, varName));
    if (spent == MovedVars.end() || spent->second.Maybe) {
      error(node->getLocation(),
            "task '" + varName +
                "' is replaced before it is awaited: await it (or move it) "
                "first");
      return false;
    }
  }
  MovedVars.erase(varKey(owner, varName)); // assigned again after a move
  auto *varTy = owner->lookup(varName);

  // A poisoned variable (its declaration failed) is re-declared by a valid
  // assignment, with the value's type, in the scope that owns it.
  if (ast::isa<ast::PoisonType>(varTy)) {
    owner->set(varName, valTy);
    return viewOk;
  }

  if (!checkAssignable(varTy, valTy, node->getValue())) {
    if (!diagnoseOptionalNarrowing(node->getLocation(), varTy, valTy))
      error(node->getLocation(), "cannot assign value of type '" +
                                     typeName(valTy) + "' to variable '" +
                                     varName + "' of type '" + typeName(varTy) +
                                     "'");
    return false;
  }
  // A `weak` variable owns nothing: assigning it copies no owner.  It
  // names a `shared` object (`weak mut`, a `shared mut` one).
  const VarKind kind = varKind(varName);
  if ((kind == VarKind::Weak || kind == VarKind::WeakView) &&
      !checkWeakStore(node->getValue(), qualifierOfVar(varName),
                      CurrentScope->lookup(varName),
                      "variable '" + varName + "'", node->getLocation()))
    return false;
  if (kind != VarKind::Weak && kind != VarKind::WeakView &&
      !checkQualifiedFlow(qualifierOfVar(varName), node->getValue(),
                          "variable '" + varName + "'", node->getLocation()))
    return false;

  return viewOk;
}

bool Sema::visitReturnStmt(ast::ReturnStmt *node) {
  if (ParallelStack.size() > ReturnParallelBase) {
    error(node->getLocation(), "'return' cannot leave a 'parallel for' body");
    return false;
  }
  if (LockDepth > ReturnLockBase) {
    error(node->getLocation(), "'return' cannot leave a 'lock' block");
    return false;
  }
  if (node->getReturnValue()) {
    // An `atomic` result (the prototype) takes an `atomic` place's handle;
    // any other value makes a new cell (checkAssignable).
    WantAtomicHandle = CurrentReturnType &&
                       ast::isa<ast::AtomicType>(CurrentReturnType) &&
                       isPlaceChain(node->getReturnValue());
    auto *valTy = resolveExprType(node->getReturnValue(), CurrentReturnType);
    WantAtomicHandle = false;
    if (!valTy)
      return false;
    if (CurrentResult.Mode != ast::ParamMode::Value &&
        !checkNotLocked(node->getReturnValue(), node->getLocation()))
      return false;
    // A function that returns nothing returns no value, not one of `void`
    // (an inferred result reports it with its inference).
    if (valTy == Ctx.getVoidTy() && CurrentReturnType == Ctx.getVoidTy()) {
      error(node->getLocation(),
            "return value has type 'void' (the expression produces no "
            "value): make it a statement, then 'return;'");
      return false;
    }
    // A returned local or by-value parameter leaves as by `move`, so its
    // value is fresh: its unique parts convert (#368).
    const ast::Expr *savedImplicit = std::exchange(
        ImplicitMoveSource, CurrentResult.Mode == ast::ParamMode::Value
                                ? node->getReturnValue()
                                : nullptr);
    const bool assignable =
        !CurrentReturnType ||
        checkAssignable(CurrentReturnType, valTy, node->getReturnValue());
    ImplicitMoveSource = savedImplicit;
    if (!assignable) {
      if (!diagnoseOptionalNarrowing(node->getLocation(), CurrentReturnType,
                                     valTy))
        error(node->getLocation(),
              "return value of type '" + typeName(valTy) +
                  "' does not match function return type '" +
                  typeName(CurrentReturnType) + "'");
      return false;
    }
    // An inferred result (`=> x`) is held as its value is, as an inferred
    // variable is (#368): `fn(n: shared Node) => n` gives a `shared Node`.
    if (!CurrentReturnType && CurrentResult.Mode == ast::ParamMode::Value)
      if (const ast::Qualifier q = qualifierOf(node->getReturnValue());
          q.isSharedForm() || q.Form == ast::QualForm::Locked)
        CurrentResultQual = q; // a type argument's mark too
    // The result is an owning slot of the caller's (the prototype).  A
    // returned local or by-value parameter leaves as by `move`: a `return`
    // is always its last use.
    if (CurrentResult.Mode == ast::ParamMode::Value &&
        !checkQualifiedFlow(CurrentResultQual, node->getReturnValue(),
                            "the result", node->getLocation(),
                            /*implicitMove=*/true))
      return false;
    // A borrowed result is a borrow of what the function was given.
    if (CurrentResult.Mode != ast::ParamMode::Value) {
      std::string why = borrowedResultError(node->getReturnValue());
      // The caller writes the storage as the result's type, so no
      // conversion can happen (not a subclass to its base).
      if (why.empty() && CurrentResult.Mode == ast::ParamMode::Inout &&
          !typesEqual(valTy, CurrentReturnType))
        why = "an 'inout' result has type '" + typeName(CurrentReturnType) +
              "', but what it returns has type '" + typeName(valTy) + "'";
      // Nor in form (#368), as an `inout` parameter takes it: a plain
      // `-> inout C` names a unique or a `shared mut` slot (an `inout`
      // takes either), and a type argument's form names exactly its own
      // (`-> inout T` with T = `shared mut Node`).
      if (const ast::Qualifier have = qualifierOf(node->getReturnValue());
          why.empty() && CurrentResult.Mode == ast::ParamMode::Inout &&
          ast::isRefType(valTy) &&
          !(CurrentResultQual.Form == ast::QualForm::Unique &&
            (have.Form == ast::QualForm::Unique ||
             have == ast::Qualifier{ast::QualForm::Shared, true})) &&
          !(have == CurrentResultQual))
        why = "an 'inout' result is held as '" +
              qualifiedName(CurrentResultQual, CurrentReturnType) +
              "', but what it returns is held as '" +
              qualifiedName(have, valTy) + "'";
      // A `view` of a type argument's shared form, a frozen object's or a
      // cell's lends exactly that form (#368): the template's code may keep
      // a copy of it, as the argument's.  A unique one (`mut Node`) copies
      // for no T, and a `view` of it reads any form.
      if (why.empty() && CurrentResult.Mode == ast::ParamMode::View &&
          CurrentResultQual.FromArgument &&
          (CurrentResultQual.isSharedForm() ||
           CurrentResultQual.Form == ast::QualForm::Locked) &&
          ast::isRefType(valTy))
        for (const ast::Expr *b : conditionalBranches(node->getReturnValue())) {
          const ast::Qualifier have = qualifierOf(b);
          if (ast::isa<ast::NoneLiteral>(b) ||
              (have.Form == CurrentResultQual.Form &&
               have.Mut == CurrentResultQual.Mut))
            continue;
          why = "a 'view' result of a type argument's form is held as '" +
                qualifiedName(CurrentResultQual, CurrentReturnType) +
                "', but what it returns is held as '" +
                qualifiedName(have, valTy) + "'";
          break;
        }
      // The caller keeps alive the storage it named, not an object reached
      // through a field of another one.
      if (why.empty() && CurrentResult.Mode == ast::ParamMode::Inout)
        why = separateObjectError(node->getReturnValue());
      if (why.empty())
        return true;
      error(node->getReturnValue()->getLocation(), why);
      return false;
    }
    // Every task handle of the function is spent before it returns (the
    // prototype); the result's own flow spent one returned as it is.
    checkTasksAwaitedUpTo(FunctionScope ? FunctionScope->Parent : nullptr,
                          "return", node->getLocation());
    return checkViewNotReturned(node->getReturnValue(), valTy);
  }
  // void return
  if (CurrentReturnType && CurrentReturnType != Ctx.getVoidTy()) {
    error(node->getLocation(), "non-void function must return a value");
    return false;
  }
  checkTasksAwaitedUpTo(FunctionScope ? FunctionScope->Parent : nullptr,
                        "return", node->getLocation());
  return true;
}

bool Sema::visitIfStmt(ast::IfStmt *node) {
  // Type-check the condition — must be bool.
  auto *condTy = resolveExprType(node->getCondition());
  if (!condTy)
    return false;
  if (condTy != Ctx.getBoolTy()) {
    error(node->getCondition()->getLocation(),
          "if condition must be 'bool', got '" + typeName(condTy) + "'");
    return false;
  }

  bool ok = true;
  // Each branch starts from the moves before the `if`; what follows the
  // `if` joins the branches that reach it (the prototype's move analysis).
  const MovedMap before = MovedVars;
  std::vector<MovedMap> paths;
  // Type-check the then branch.
  FlowEnded = false;
  if (!visit(node->getThenBranch()))
    ok = false;
  const bool thenEnded = FlowEnded;
  if (!thenEnded)
    paths.push_back(MovedVars);
  // Type-check the else branch (if present).
  MovedVars = before;
  FlowEnded = false;
  if (node->hasElse() && !visit(node->getElseBranch()))
    ok = false;
  const bool elseEnded = node->hasElse() && FlowEnded;
  if (!elseEnded)
    paths.push_back(std::move(MovedVars));
  joinMoves(paths, "if", node->getLocation());
  FlowEnded = thenEnded && elseEnded;
  return ok;
}

bool Sema::visitWhileStmt(ast::WhileStmt *node) {
  // The condition runs before every iteration, so what it moves or awaits
  // is moved in the loop: it is checked inside the loop's moves.
  beginLoopMoves(node->getLocation());
  // Type-check the condition — must be bool.
  auto *condTy = resolveExprType(node->getCondition());
  if (condTy && condTy != Ctx.getBoolTy()) {
    error(node->getCondition()->getLocation(),
          "while condition must be 'bool', got '" + typeName(condTy) + "'");
    condTy = nullptr;
  }
  if (!condTy) {
    FlowEnded = true; // nothing of the loop is checked
    finishLoopMoves();
    FlowEnded = false;
    return false;
  }

  // The loop ends after its condition has run, an iteration or none.
  LoopMoveStack.back().NoIteration = MovedVars;

  // Type-check the body inside a loop context.
  ++LoopDepth;
  bool ok = visit(node->getBody());
  --LoopDepth;
  finishLoopMoves();
  return ok;
}

bool Sema::visitForStmt(ast::ForStmt *node) {
  ast::VarDecl *var = node->getVar();
  // `for _ in ...` binds no name.
  const std::string &name = var->getName();
  const bool discard = name.empty();
  const std::string shown = discard ? "_" : name;
  bool ok = discard || (checkBinderName(name, var->getLocation()) &&
                        checkNotSelf(name, var->getLocation()));
  auto isInteger = [](ast::Type *t) {
    const auto *bt = ast::dyn_cast<ast::BuiltinType>(t);
    return bt && bt->isInteger();
  };

  // The variable's type: its annotation, or the range's.
  ast::Type *varTy = nullptr;
  if (var->getType()) {
    varTy = resolveType(var->getType(), var->getLocation(),
                        "variable '" + shown + "'");
    if (varTy && !isInteger(varTy)) {
      error(var->getLocation(), "'" + shown + "' has type '" + typeName(varTy) +
                                    "': a 'for' loop's variable is an integer");
      varTy = nullptr;
    }
    if (varTy)
      var->setType(varTy);
    else
      ok = false;
  }

  // The range and the step are evaluated once, before the loop, so they
  // see the names outside it.  A literal end takes the other end's type, as
  // an operand of `<` does, or the annotation's.
  ast::Expr *start = node->getStart();
  ast::Expr *end = node->getEnd();
  ast::Type *startTy = nullptr;
  ast::Type *endTy = nullptr;
  if (varTy) {
    startTy = resolveExprType(start, varTy);
    endTy = resolveExprType(end, varTy);
  } else if (ExprChecker::isNumericLiteral(start) &&
             !ExprChecker::isNumericLiteral(end)) {
    endTy = resolveExprType(end);
    startTy = resolveExprType(start, endTy);
  } else {
    startTy = resolveExprType(start);
    endTy = resolveExprType(end, startTy);
  }
  ast::Type *loopTy = nullptr;
  if (!startTy || !endTy) {
    ok = false;
  } else if (!isInteger(startTy) || !isInteger(endTy)) {
    ast::Expr *bad = isInteger(startTy) ? end : start;
    error(bad->getLocation(),
          "a 'for' loop's range is of integers, not '" +
              typeName(isInteger(startTy) ? endTy : startTy) + "'");
    ok = false;
  } else if (varTy && !typesEqual(startTy, varTy)) {
    error(start->getLocation(), "the start of the range has type '" +
                                    typeName(startTy) + "', but '" + shown +
                                    "' is a '" + typeName(varTy) + "'");
    ok = false;
  } else if (varTy && !typesEqual(endTy, varTy)) {
    error(end->getLocation(), "the end of the range has type '" +
                                  typeName(endTy) + "', but '" + shown +
                                  "' is a '" + typeName(varTy) + "'");
    ok = false;
  } else if (!typesEqual(startTy, endTy)) {
    error(start->getLocation(),
          "the ends of the range have types '" + typeName(startTy) + "' and '" +
              typeName(endTy) + "'; a 'for' loop's range has one type");
    ok = false;
  } else {
    loopTy = startTy;
  }

  // The step is an `int`: positive counts up, negative down.
  if (ast::Expr *step = node->getStep()) {
    ast::Type *stepTy = resolveExprType(step, Ctx.getIntTy());
    if (!stepTy) {
      ok = false;
    } else if (!typesEqual(stepTy, Ctx.getIntTy())) {
      error(step->getLocation(),
            "a 'for' loop's step is an 'int', not '" + typeName(stepTy) + "'");
      ok = false;
    } else if (node->constantStep() == 0) {
      error(step->getLocation(), "a 'for' loop's step cannot be 0");
      ok = false;
    }
  }

  // The variable is bound in a scope of the loop's own, around the body, and
  // is read-only there.
  ScopeGuard guard(*this);
  if (loopTy)
    var->setType(loopTy);
  if (discard) {
    // Nothing to bind.
  } else if (loopTy) {
    CurrentScope->set(name, loopTy);
  } else {
    declarePoisoned(CurrentScope, name);
  }
  if (!discard)
    CurrentScope->Kinds[name] = VarKind::ForVar;
  // A `parallel for` (the concurrency prototype): its variable, and what
  // its body declares, are an iteration's own.
  if (node->isParallel() && LockDepth > 0) {
    error(node->getLocation(), "a 'parallel for' cannot run in a 'lock' block");
    return false;
  }
  if (node->isParallel())
    ParallelStack.push_back({node, CurrentScope, LoopDepth + 1, false, {}, {}});
  if (!discard)
    DeclLoopDepth[varKey(CurrentScope, name)] = LoopDepth + 1;
  beginLoopMoves(node->getLocation());
  ++LoopDepth;
  bool bodyOk = visit(node->getBody());
  --LoopDepth;
  finishLoopMoves();
  if (node->isParallel()) {
    if (!checkReductionsNotRead(ParallelStack.back()) ||
        !checkWrittenArraysNotRead(ParallelStack.back()))
      bodyOk = false;
    ParallelStack.pop_back();
  }
  return ok && bodyOk;
}

// `lock c as v: inout|view { }` (the concurrency prototype): the cell is a
// `locked` place; the binding is a local borrow of its contents for the
// block, which holds the lock to its end.
bool Sema::visitLockStmt(ast::LockStmt *node) {
  if (LockDepth > 0) {
    error(node->getLocation(), "a 'lock' block cannot hold another 'lock'");
    return false;
  }
  // Only the operand itself is the cell the block takes: a cell on the way
  // to it, or read in an argument of it, is reached with no lock.
  LockOperand = node->getCell();
  ast::Type *cellTy = resolveExprType(node->getCell());
  LockOperand = nullptr;
  if (!cellTy)
    return false;
  const ast::Qualifier cellQ = qualifierOf(node->getCell());
  bool isCell = cellQ.Form == ast::QualForm::Locked;
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(node->getCell()))
    for (ast::Expr *b : conditionalBranches(te))
      isCell = isCell && qualifierOf(b).Form == ast::QualForm::Locked &&
               ast::stripOptional(b->getResolvedType()) ==
                   ast::stripOptional(cellTy);
  if (!isCell) {
    error(node->getCell()->getLocation(),
          "'" + placeText(node->getCell()) +
              "' is not 'locked': a 'lock' block takes a 'locked' variable, "
              "field or parameter");
    return false;
  }
  if (ast::isa<ast::OptionalType>(cellTy)) {
    error(node->getCell()->getLocation(),
          "'" + placeText(node->getCell()) +
              "' may be None: 'match' it, and lock the cell its arm binds");
    return false;
  }
  ast::VarDecl *b = node->getBinding();
  if (!checkBinderName(b->getName(), b->getLocation()) ||
      !checkNotSelf(b->getName(), b->getLocation()))
    return false;
  // The lock buys the writes: an `inout` block takes a `locked mut` cell.
  if (b->getMode() == ast::ParamMode::Inout && !cellQ.Mut) {
    error(b->getLocation(),
          "'" + placeText(node->getCell()) + "' is 'locked " +
              typeName(cellTy) +
              "', whose contents are never written; declare it 'locked mut " +
              typeName(cellTy) + "' for a 'lock ... as " + b->getName() +
              ": inout' block");
    return false;
  }
  b->setType(cellTy);
  ScopeGuard guard(*this);
  CurrentScope->declare(b->getName(), cellTy);
  CurrentScope->Quals[b->getName()] = ast::Qualifier{
      ast::QualForm::Unique, b->getMode() == ast::ParamMode::Inout};
  CurrentScope->Kinds[b->getName()] = b->getMode() == ast::ParamMode::View
                                          ? VarKind::ViewLocal
                                          : VarKind::Inout;
  DeclLoopDepth[varKey(CurrentScope, b->getName())] = LoopDepth;
  ++LockDepth;
  const unsigned savedLockLoops = std::exchange(LockLoopBase, LoopDepth);
  const bool ok = visit(node->getBody());
  LockLoopBase = savedLockLoops;
  --LockDepth;
  return ok;
}

bool Sema::visitBreakStmt(ast::BreakStmt *node) {
  if (LoopDepth == 0) {
    error(node->getLocation(), "'break' outside of a loop");
    return false;
  }
  if (LockDepth > 0 && LoopDepth <= LockLoopBase) {
    error(node->getLocation(), "'break' cannot leave a 'lock' block");
    return false;
  }
  // The `parallel for`'s own iterations cannot be left early; a loop in its
  // body can.
  if (ParallelStack.size() > ReturnParallelBase &&
      LoopDepth <= ParallelStack.back().LoopBase) {
    error(node->getLocation(),
          "'break' cannot be used in a 'parallel for' body");
    return false;
  }
  // The moves so far leave the loop by this path; the task handles of the
  // scopes inside the loop are spent before it (the prototype).
  if (!LoopMoveStack.empty()) {
    checkTasksAwaitedUpTo(LoopMoveStack.back().Outer, "break",
                          node->getLocation());
    LoopMoveStack.back().Breaks.push_back(MovedVars);
  }
  FlowEnded = true;
  return true;
}

bool Sema::visitContinueStmt(ast::ContinueStmt *node) {
  if (LoopDepth == 0) {
    error(node->getLocation(), "'continue' outside of a loop");
    return false;
  }
  if (LockDepth > 0 && LoopDepth <= LockLoopBase) {
    error(node->getLocation(), "'continue' cannot leave a 'lock' block");
    return false;
  }
  // It ends the iteration of a `parallel for` it is directly in.
  if (ParallelStack.size() > ReturnParallelBase &&
      LoopDepth <= ParallelStack.back().LoopBase)
    ParallelStack.back().SawContinue = true;
  // The next iteration starts here; the task handles of the scopes inside
  // the loop are spent before it (the prototype).
  if (!LoopMoveStack.empty()) {
    checkTasksAwaitedUpTo(LoopMoveStack.back().Outer, "continue",
                          node->getLocation());
    checkLoopIteration(LoopMoveStack.back(), node->getLocation());
  }
  FlowEnded = true;
  return true;
}

// -- Declarations

/// Returns true if every control-flow path through `stmts` ends in a
/// ReturnStmt: a conservative syntactic check that catches the common
/// "missing return" cases without a CFG.
bool detail::blockAlwaysReturns(const std::vector<ast::Stmt *> &stmts) {
  if (stmts.empty())
    return false;
  for (int i = (int)stmts.size() - 1; i >= 0; --i)
    if (detail::stmtAlwaysReturns(stmts[i]))
      return true;
  return false;
}

namespace {

/// Whether a `break` in @p s leaves the loop @p s is the body of: one in a
/// nested loop leaves that loop instead.
bool breaksOut(const ast::Stmt *s) {
  if (!s)
    return false;
  if (ast::isa<ast::BreakStmt>(s))
    return true;
  if (const auto *cs = ast::dyn_cast<ast::CompoundStmt>(s)) {
    for (const ast::Stmt *x : cs->getStatements())
      if (breaksOut(x))
        return true;
    return false;
  }
  if (const auto *is = ast::dyn_cast<ast::IfStmt>(s))
    return breaksOut(is->getThenBranch()) || breaksOut(is->getElseBranch());
  if (const auto *ms = ast::dyn_cast<ast::MatchStmt>(s)) {
    for (const ast::MatchArm *arm : ms->getArms())
      if (breaksOut(arm->getBody()))
        return true;
    return false;
  }
  return false; // a nested loop, or a statement that holds none
}

} // namespace

bool detail::stmtAlwaysReturns(ast::Stmt *s) {
  if (ast::isa<ast::ReturnStmt>(s))
    return true;
  // `while True` with no `break` out of it never falls through: it is left
  // only by a `return`.
  if (auto *ws = ast::dyn_cast<ast::WhileStmt>(s)) {
    const auto *cond = ast::dyn_cast<ast::BoolLiteral>(ws->getCondition());
    return cond && cond->getValue() && !breaksOut(ws->getBody());
  }
  if (auto *ifStmt = ast::dyn_cast<ast::IfStmt>(s)) {
    if (!ifStmt->getElseBranch())
      return false;
    return detail::stmtAlwaysReturns(ifStmt->getThenBranch()) &&
           detail::stmtAlwaysReturns(ifStmt->getElseBranch());
  }
  if (auto *cs = ast::dyn_cast<ast::CompoundStmt>(s))
    return detail::blockAlwaysReturns(cs->getStatements());
  if (auto *ms = ast::dyn_cast<ast::MatchStmt>(s)) {
    for (ast::MatchArm *arm : ms->getArms())
      if (!detail::blockAlwaysReturns(arm->getBody()->getStatements()))
        return false;
    return detail::matchIsExhaustive(ms);
  }
  return false;
}

bool detail::matchIsExhaustive(ast::MatchStmt *ms) {
  ast::Type *subjTy = ms->getSubject()->getResolvedType();
  auto *optTy = ast::dyn_cast<ast::OptionalType>(subjTy); // null-safe
  bool hasWildcard = false;
  unsigned variantArms = 0;               // enum-subject variant arms
  bool trueArm = false, falseArm = false; // bool-subject literal coverage
  bool noneArm = false, innerArm = false; // optional-subject coverage
  for (ast::MatchArm *arm : ms->getArms()) {
    if (arm->isWildcard()) {
      hasWildcard = true;
    } else if (!arm->isLiteral()) {
      ++variantArms; // an enum/type-name (bare-variant) arm
      // For `T?` the arm naming T itself matches every non-None value.
      if (optTy && arm->getArmType() == optTy->getInnerType())
        innerArm = true;
    } else if (ast::isa<ast::NoneLiteral>(arm->getLiteralPattern())) {
      noneArm = true;
    } else if (auto *bl =
                   ast::dyn_cast<ast::BoolLiteral>(arm->getLiteralPattern())) {
      if (bl->getValue())
        trueArm = true;
      else
        falseArm = true;
    }
  }
  if (hasWildcard)
    return true;
  // A wildcard-less match is exhaustive iff its arms cover the whole value
  // domain of the subject: every variant of an enum subject, both True and
  // False literals for a bool subject, or — for an optional subject — the
  // `None` case plus the wrapped type itself (a strict subclass arm only
  // covers its exact runtime type).  Sema has already validated the arms
  // (these analyses run only after the body type-checks cleanly), so each
  // variant arm names a distinct, valid variant — counting them suffices.
  if (!subjTy)
    return false;
  if (optTy)
    return noneArm && innerArm;
  if (auto *et = ast::dyn_cast<ast::EnumType>(subjTy))
    return variantArms == et->getNumVariants();
  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(subjTy))
    if (bt->getTypeKind() == ast::BuiltinType::Bool)
      return trueArm && falseArm;
  return false;
}

bool Sema::declareFunctionSignature(ast::FuncDecl *node) {
  // The name must not be taken by a builtin, a class (constructor), an enum,
  // or an earlier function.  Checked first so that the collision — not some
  // unrelated type error in the signature — is what gets reported.
  if (!checkDeclNameAvailable(node->getName(), node->getLocation(),
                              DeclKind::Function))
    return false;

  // Resolve return type.  Resolved annotations are written back into the
  // declaration so the lowering sees canonical types (never a parser stub or a
  // generic type application).
  // `fn double(x: int) => x * 2;` has no result type until its body is
  // checked (inferResults): none is registered, and what is checked of the
  // result here is checked then.
  const bool inferred = node->hasInferredResult() && !node->getReturnType();
  ast::Type *retTy = inferred ? nullptr : Ctx.getVoidTy();
  if (node->getReturnType()) {
    FixedArrayAllowed = true; // a result may be a fixed-size array
    retTy = resolveType(node->getReturnType(), node->getLocation(),
                        "function '" + node->getName() + "' return type");
    if (!retTy ||
        !checkFixedResult(retTy, node->getResultMode(), node->getLocation())) {
      // Calls by its name are follow-ons of the reported error (#89).
      ErroneousNames.insert(node->getName());
      return false;
    }
    if (ast::Qualifier q = takeQualifier(); !q.isUnique()) {
      if (!checkBorrowQualifier(q, node->getResultMode(),
                                "the result of '" + node->getName() + "'",
                                node->getLocation())) {
        ErroneousNames.insert(node->getName());
        return false;
      }
      node->setResultQual(q);
    }
    node->setReturnType(retTy);
  }

  // Resolve parameter types.  (An instantiation's are its template's,
  // already checked to be distinct.)
  if (node->getName().find('<') == std::string::npos &&
      !checkDistinctParams(node,
                           "function '" + displayName(node->getName()) + "'")) {
    ErroneousNames.insert(node->getName()); // as for the return type
    return false;
  }
  std::vector<ast::Type *> paramTypes;
  for (auto &p : node->getMutableParams()) {
    if (!checkBinderName(p.getName(), p.Loc, /*binds=*/!node->isNative(),
                         "a parameter name")) {
      ErroneousNames.insert(node->getName()); // as for the return type
      return false;
    }
    FixedArrayAllowed = true; // a parameter may be a fixed-size array
    SliceAllowed = p.Mode != ast::ParamMode::Value; // or a slice
    auto *ty =
        resolveValueType(p.ParamType, p.Loc, "parameter '" + p.getName() + "'");
    if (!ty) {
      ErroneousNames.insert(node->getName()); // as for the return type
      return false;
    }
    if (ast::Qualifier q = takeQualifier(); !q.isUnique()) {
      if (!checkBorrowQualifier(q, p.Mode, "parameter '" + p.getName() + "'",
                                p.Loc)) {
        ErroneousNames.insert(node->getName());
        return false;
      }
      p.Qual = q;
    }
    p.ParamType = ty;
    paramTypes.push_back(ty);
  }

  // A public function cannot hand an importer a type it could not name.
  if (!isModuleOwnName(node->getName()) &&
      node->getName().find('<') == std::string::npos) {
    const std::string who =
        "'" + displayName(node->getName()) + "' is public but ";
    bool pub = inferred || checkPublicSurface(who + "its result", "function",
                                              retTy, node->getLocation());
    for (auto &p : node->getParams())
      pub = checkPublicSurface(who + "parameter '" + p.getName() + "'",
                               "function", p.ParamType, node->getLocation()) &&
            pub;
    if (!pub) {
      ErroneousNames.insert(node->getName());
      return false;
    }
  }

  if (node->isNative() && !checkNativeSignature(node, retTy, paramTypes)) {
    ErroneousNames.insert(node->getName());
    return false;
  }

  // An `async fn` (the concurrency prototype): a call of it is a task of
  // its result.
  ast::Type *callTy = retTy;
  if (node->isAsync()) {
    // `@main` is the entry point's name, whatever module declares it.
    if (node->getName() == names::kMain && OpenWorld) {
      error(node->getLocation(), "'async fn main' is a program's entry point: "
                                 "a module that others import cannot declare "
                                 "one");
      ErroneousNames.insert(node->getName());
      return false;
    }
    if (!checkAsyncDecl(node, retTy)) {
      ErroneousNames.insert(node->getName());
      return false;
    }
    callTy = taskOf(node->getResultQual(), retTy);
  }
  // Register the function in the function table.
  declareFunction(node->getName(), callTy, paramTypes);
  if (inferred)
    InferredFunctions[node->getName()] = {node, Inference::Pending, nullptr};
  FunctionSig &sig = FunctionTable[node->getName()];
  sig.Modes = ast::paramModes(node->getParams());
  sig.Modes.Result = node->getResultMode();
  sig.Native = node->isNative();
  sig.Async = node->isAsync();
  for (const ast::Param &p : node->getParams())
    sig.ParamQuals.push_back(p.Qual);
  sig.ResultQual = node->getResultQual();
  return checkResultDecl(node, /*method=*/false);
}

// A `native fn` (#198) crosses into C with the runtime builtins' convention,
// which only covers scalars, `Str` and `Obj` (and, as a result, `Str?` and
// `Obj?`: a null box is None).  An instance of a generic one passes what its
// template typed by a bare type parameter by address instead, so any type
// crosses there.  Its parameters are checked by checkNativeParamModes (a
// template's when it is registered), and a `view` result is part of a
// `view` or `inout` one; `-> inout` would need C to return an address into
// Paykan storage.
bool Sema::checkNativeSignature(ast::FuncDecl *node, ast::Type *retTy,
                                const std::vector<ast::Type *> &paramTypes) {
  // An enum crosses as its variant's ordinal, an int64_t (one whose variants
  // have fields does not cross).
  auto scalarOrRef = [&](ast::Type *t) {
    if (t == Ctx.getStrTy() || t == Ctx.getObjTy() ||
        (ast::isa<ast::EnumType>(t) && !ast::isPayloadEnum(t)))
      return true;
    auto *bt = ast::dyn_cast<ast::BuiltinType>(t);
    return bt && t != Ctx.getVoidTy();
  };
  bool ok =
      !node->getTypeArgs().empty() || checkNativeParamModes(node, paramTypes);
  auto reject = [&](ast::Type *t, const std::string &what) {
    error(node->getLocation(),
          "native function '" + node->getName() + "': " + what + " '" +
              typeName(t) +
              "' cannot cross into C (int, float, bool, char, a sized "
              "numeric type, an enum without fields, Str and Obj can, Str? "
              "and Obj? as a result, and any type where a generic native "
              "function says 'T')");
    ok = false;
  };
  for (size_t i = 0; i < paramTypes.size(); ++i)
    if (!node->paramByAddress(i) && !scalarOrRef(paramTypes[i]))
      reject(paramTypes[i],
             "parameter '" + node->getParams()[i].getName() + "' of type");
  bool borrowed = false;
  for (const ast::Param &p : node->getParams())
    borrowed |= p.Mode != ast::ParamMode::Value;
  if (node->getResultMode() == ast::ParamMode::Inout) {
    error(node->getLocation(), "native function '" + node->getName() +
                                   "' cannot return 'inout'; a native "
                                   "function returns a copy or a view");
    ok = false;
  } else if (node->getResultMode() == ast::ParamMode::View && !borrowed) {
    error(node->getLocation(),
          "native function '" + node->getName() +
              "' returns a 'view' but has no 'view' or 'inout' parameter for "
              "it to be part of");
    ok = false;
  }
  ast::Type *ret = retTy;
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(retTy))
    ret = ot->getInnerType() == Ctx.getStrTy() ||
                  ot->getInnerType() == Ctx.getObjTy()
              ? ot->getInnerType()
              : retTy;
  if (ret != Ctx.getVoidTy() && !node->resultByAddress() && !scalarOrRef(ret))
    reject(retTy, "result type");
  if (node->getName() == names::kMain) {
    error(node->getLocation(), "'main' cannot be a native function");
    ok = false;
  }
  return ok && checkNativeSymbol(node);
}

// A C symbol names one C function, whose signature its every declaration in
// the program must give (or a backend would declare it twice): a native
// function binding a symbol another one of the module binds, or one the
// runtime defines, must bind it with the same C signature.  The same one
// twice is fine.
bool Sema::checkNativeSymbol(const ast::FuncDecl *node) {
  const std::string &symbol = node->getNativeSymbol();
  // The frontends reject such a symbol; an AST made another way may not.
  if (std::string_view why = names::cSymbolProblem(symbol); !why.empty()) {
    error(node->getLocation(),
          "the C symbol '" + symbol + "' of native function '" +
              displayName(node->getName()) + "' " + std::string(why));
    return false;
  }
  const pir::Signature sig = nativeSignature(Ctx, *node);
  if (pir::isRuntimeSymbol(symbol)) {
    if (const pir::Signature *rt = pir::runtimeSignature(symbol);
        rt && *rt == sig)
      return true;
    error(node->getLocation(),
          "native function '" + displayName(node->getName()) +
              "' binds C symbol '" + symbol +
              "', a function of the runtime, with a different signature");
    return false;
  }
  const std::string file = Diags.getSourceName();
  const std::vector<std::string> *lines = Diags.getSourceLines();
  auto [it, fresh] =
      NativeSymbols.try_emplace(symbol, NativeBinding{node, sig, file, lines});
  if (fresh || it->second.Sig == sig)
    return true;
  const NativeBinding &first = it->second;
  error(node->getLocation(), "native function '" +
                                 displayName(node->getName()) +
                                 "' binds C symbol '" + symbol + "', which '" +
                                 displayName(first.Decl->getName()) +
                                 "' already binds with a different signature");
  // Where the first one is, which may be another file: the program's own
  // native function, when the later one is an instance of a generic native
  // function of the standard library.
  Diags.setSourceInfo(first.File, first.Lines);
  note(first.Decl->getLocation(),
       "'" + displayName(first.Decl->getName()) + "' binds it here");
  Diags.setSourceInfo(file, lines);
  return false;
}

// Only a scalar crosses into C by value.  C borrows anything else, as a
// `view`, or changes it in place, as an `inout`, so a call never shares a
// reference with C.  @p types are the parameters' types; a template's are its
// annotations, so `x: T` is rejected whatever T will be.
bool Sema::checkNativeParamModes(const ast::FuncDecl *node,
                                 const std::vector<ast::Type *> &types) {
  bool ok = true;
  for (size_t i = 0; i < types.size(); ++i) {
    const ast::Param &p = node->getParams()[i];
    // An enum whose variants have fields crosses in no mode
    // (checkNativeSignature says so).
    if (p.Mode != ast::ParamMode::Value ||
        (ast::isa<ast::BuiltinType>(types[i]) && types[i] != Ctx.getVoidTy()) ||
        ast::isa<ast::EnumType>(types[i]))
      continue;
    const std::string ty = typeName(types[i]);
    error(node->getLocation(),
          "native function '" + node->getName() + "': parameter '" +
              p.getName() + "' must be 'view' or 'inout' ('" + p.getName() +
              ": view " + ty +
              "'); only int, float, bool, char, a sized numeric type and "
              "an enum without fields cross into C by value");
    ok = false;
  }
  return ok;
}

bool Sema::visitFuncDecl(ast::FuncDecl *node) {
  // The signature was registered by the forward-declaration pass in
  // visitTranslationUnit, which only visits bodies of successfully declared
  // functions.  Defensive: if the entry is absent anyway, the error was
  // already reported — skip the body to avoid duplicate diagnostics.
  if (!lookupFunction(node->getName()))
    return false;
  if (node->isNative())
    return true; // the body is C (#198)

  // A `static fn` of a class or struct (`C::make`) reaches its type's
  // private members, like a method.
  struct OwnerGuard {
    ast::Type *&Slot, *Saved;
    ~OwnerGuard() { Slot = Saved; }
  } ownerGuard{CurrentStaticOwner, CurrentStaticOwner};
  CurrentStaticOwner = nullptr;
  if (size_t sep = node->getName().rfind(names::kQualSep);
      sep != std::string::npos) {
    const std::string prefix = node->getName().substr(0, sep);
    CurrentStaticOwner = Ctx.lookupClassType(prefix);
    if (!CurrentStaticOwner)
      CurrentStaticOwner = Ctx.lookupStructType(prefix);
  }

  // Re-resolve the annotations to set up the body scope (resolveType is
  // idempotent and, for a registered function, is guaranteed to succeed —
  // declareFunctionSignature already wrote the canonical types back).  A
  // result to infer is none yet: the body is checked against none.
  const bool inferring = node->hasInferredResult() && !node->getReturnType();
  ast::Type *retTy = inferring ? nullptr : Ctx.getVoidTy();
  if (node->getReturnType()) {
    FixedArrayAllowed = true;
    retTy = resolveType(node->getReturnType(), node->getLocation(),
                        "function '" + node->getName() + "' return type");
  }

  std::vector<ast::Type *> paramTypes;
  for (auto &p : node->getParams()) {
    FixedArrayAllowed = true;
    paramTypes.push_back(resolveType(p.ParamType, node->getLocation(),
                                     "parameter '" + p.getName() + "'"));
  }

  // Type-check the body in a new scope with params.
  auto *savedRetTy = CurrentReturnType;
  const ResultContext savedResult = CurrentResult;
  CurrentReturnType = retTy;
  {
    ScopeGuard guard(*this);
    for (size_t i = 0; i < node->getParams().size(); ++i) {
      const ast::Param &p = node->getParams()[i];
      CurrentScope->declare(p.getName(), paramTypes[i]);
      ast::Qualifier q = p.Qual;
      // (a shared form, a template argument\'s, is its own: #368)
      if (p.Mode == ast::ParamMode::Inout && q.Form == ast::QualForm::Unique)
        q.Mut = true; // the borrow's writes
      CurrentScope->Quals[p.getName()] = q;
    }
    declareParamKinds(node);
    // The concurrency prototype: the function's own moves, task handles,
    // loop depths and result qualifier.
    Scope *savedFunctionScope = std::exchange(FunctionScope, CurrentScope);
    std::unordered_map<std::string, ast::SourceLocation> savedTasks =
        std::exchange(TaskLocals, {});
    for (size_t i = 0; i < node->getParams().size(); ++i)
      if (ast::isa<ast::TaskType>(paramTypes[i]))
        TaskLocals[varKey(CurrentScope, node->getParams()[i].getName())] =
            node->getParams()[i].Loc;
    const bool savedAsync = std::exchange(InAsyncFn, node->isAsync());
    const ast::Qualifier savedResultQual =
        std::exchange(CurrentResultQual, node->getResultQual());
    // (A generic instance is checked in the middle of another body: that
    // body's state comes back after.)
    MovedMap savedMoves = std::exchange(MovedVars, {});
    std::vector<LoopMoves> savedLoopMoves = std::exchange(LoopMoveStack, {});
    const bool savedFlowEnded = std::exchange(FlowEnded, false);
    DeclLoopDepth.clear();
    for (const ast::Param &p : node->getParams())
      DeclLoopDepth[varKey(CurrentScope, p.getName())] = 0;
    bool ok = visitBody(node->getBody()->getStatements());
    // The function's task handles are spent where its body ends.
    if (ok && !FlowEnded)
      checkTasksAwaited(CurrentScope, nullptr, node->getLocation());
    FunctionScope = savedFunctionScope;
    TaskLocals = std::move(savedTasks);
    MovedVars = std::move(savedMoves);
    LoopMoveStack = std::move(savedLoopMoves);
    FlowEnded = savedFlowEnded;
    InAsyncFn = savedAsync;
    const ast::Qualifier bodyResultQual =
        std::exchange(CurrentResultQual, savedResultQual);
    CurrentReturnType = savedRetTy;
    CurrentResult = savedResult;
    if (!ok)
      return false;
    if (inferring) {
      retTy = inferredResultOf(node);
      if (!retTy)
        return false;
      // The form its `return` gave it (visitReturnStmt).
      if (!bodyResultQual.isUnique()) {
        node->setResultQual(bodyResultQual);
        FunctionTable[node->getName()].ResultQual = bodyResultQual;
      }
    }
    // Non-void functions must always return a value on every path.
    if (retTy != Ctx.getVoidTy() &&
        !detail::blockAlwaysReturns(node->getBody()->getStatements())) {
      error(node->getLocation(), "non-void function '" + node->getName() +
                                     "' does not always return a value");
      return false;
    }
  }
  return true;
}

// -- Inferred results

ast::Type *Sema::returnedType(ast::CompoundStmt *body,
                              const std::string &subject,
                              const std::string &braces) {
  const auto &stmts = body->getStatements();
  const auto *ret =
      stmts.size() == 1 ? ast::dyn_cast<ast::ReturnStmt>(stmts[0]) : nullptr;
  ast::Expr *value = ret ? ret->getReturnValue() : nullptr;
  ast::Type *ty = value ? value->getResolvedType() : nullptr;
  if (!ty)
    return nullptr; // the parser makes the body one `return` of a value
  if (ty == Ctx.getVoidTy()) {
    error(value->getLocation(),
          subject +
              " is the value of its expression, which has none (its "
              "type is 'void'): write its body in braces, '" +
              braces + "'");
    return nullptr;
  }
  return ty;
}

/// How a diagnostic names the function shown as @p shown: an
/// instantiation by its template's name, as it was written.
static std::string writtenName(const std::string &shown) {
  return shown.substr(0, shown.find('<'));
}

ast::Type *Sema::inferredResultOf(ast::FuncDecl *node) {
  const std::string shown = writtenName(displayName(node->getName()));
  ast::Type *ty = returnedType(node->getBody(), "the result of '" + shown + "'",
                               "fn " + shown + "(...) { ...; }");
  if (!ty || !checkFixedResult(ty, node->getResultMode(), node->getLocation()))
    return nullptr;
  // What declareFunctionSignature checks of a written result.
  if (!isModuleOwnName(node->getName()) &&
      node->getName().find('<') == std::string::npos &&
      !checkPublicSurface("'" + shown + "' is public but its result",
                          "function", ty, node->getLocation()))
    return nullptr;
  node->setReturnType(ty);
  FunctionTable[node->getName()].ReturnType = ty;
  return ty;
}

bool Sema::inferResults(const std::vector<ast::FuncDecl *> &fns) {
  InferenceReady = true;
  bool ok = true;
  for (auto *fn : fns) {
    auto it = InferredFunctions.find(fn->getName());
    if (it == InferredFunctions.end() || it->second.Decl != fn)
      continue;
    if (it->second.State == Inference::Pending)
      inferResultOf(fn->getName(), fn->getLocation());
    if (it->second.State != Inference::Done)
      ok = false;
  }
  return ok;
}

bool Sema::inferResultOf(const std::string &name, ast::SourceLocation loc) {
  auto it = InferredFunctions.find(name);
  if (it == InferredFunctions.end())
    return true;
  InferredFunction &fn = it->second;
  switch (fn.State) {
  case Inference::Done:
    return true;
  case Inference::Failed:
    ++SuppressedFollowOns; // its own error was reported
    return false;
  case Inference::InProgress: {
    const std::string shown = writtenName(displayName(name));
    error(loc, "the result type of '" + shown +
                   "' is inferred from its expression, which uses '" + shown +
                   "' itself: write the result type, 'fn " + shown +
                   "(...) -> T => ...'");
    return false;
  }
  case Inference::Pending:
    if (!InferenceReady)
      return true; // the signatures are still being registered
    break;
  }
  // Its body is checked now, as a body of its own: nothing of the body that
  // uses it (its scopes, lambdas, borrows, loops, class) is in scope there.
  // An instantiation's is checked as checkPendingInstantiations would.
  Scope *savedScope = std::exchange(CurrentScope, nullptr);
  auto savedLambdas = std::exchange(LambdaStack, {});
  auto savedBorrows = std::exchange(Borrows, {});
  const unsigned savedLoopDepth = std::exchange(LoopDepth, 0u);
  // … nor is a `parallel for` or a `lock` it is first used in.
  auto savedParallel = std::exchange(ParallelStack, {});
  const size_t savedParallelBase = std::exchange(ReturnParallelBase, size_t{0});
  const unsigned savedLockDepth = std::exchange(LockDepth, 0u);
  const unsigned savedLockBase = std::exchange(ReturnLockBase, 0u);
  const unsigned savedLockLoopBase = std::exchange(LockLoopBase, 0u);
  ClassContext *savedClassCtx = std::exchange(CurrentClassCtx, nullptr);
  MethodUse *savedMethodUse = std::exchange(CurrentMethodUse, nullptr);
  const StringSet *savedTypeParams = std::exchange(CurrentTypeParams, nullptr);
  const ast::FuncDecl *savedImplMethod =
      std::exchange(ImplScopeMethod, nullptr);
  const size_t savedDepth = CurrentInstantiationDepth;
  fn.State = Inference::InProgress;
  bool ok;
  if (const PendingInstantiation *p = fn.Instance.get()) {
    InstantiationStack.push_back(
        {p->Name, p->RequestLoc, p->RequestFile, p->RequestLines});
    InstanceFrames[fn.Decl] = InstantiationStack;
    CurrentTypeParams = &p->TypeParams;
    CurrentInstantiationDepth = p->Depth;
    std::vector<InstantiatingTemplate> savedTemplates =
        std::exchange(InstantiatingTemplates, p->Templates);
    {
      OriginSource inOrigin(*this, p->Origin);
      InstanceCode instance(*this, &p->Args);
      ok = visitFuncDecl(fn.Decl);
    }
    InstantiatingTemplates = std::move(savedTemplates);
    InstantiationStack.pop_back();
  } else {
    InstanceCode program(*this, nullptr); // the program's own function
    ok = visitFuncDecl(fn.Decl);
  }
  CurrentInstantiationDepth = savedDepth;
  fn.State = ok ? Inference::Done : Inference::Failed;
  CurrentScope = savedScope;
  LambdaStack = std::move(savedLambdas);
  Borrows = std::move(savedBorrows);
  LoopDepth = savedLoopDepth;
  ParallelStack = std::move(savedParallel);
  ReturnParallelBase = savedParallelBase;
  LockDepth = savedLockDepth;
  ReturnLockBase = savedLockBase;
  LockLoopBase = savedLockLoopBase;
  CurrentClassCtx = savedClassCtx;
  CurrentMethodUse = savedMethodUse;
  CurrentTypeParams = savedTypeParams;
  ImplScopeMethod = savedImplMethod;
  if (!ok)
    ++SuppressedFollowOns; // the use, of a function whose error is reported
  return ok;
}

void Sema::reportInferredMethod(const ast::FuncDecl *fn) {
  error(fn->getLocation(), "the result type of method '" + fn->getName() +
                               "' is not inferred: write it, 'fn " +
                               fn->getName() + "(...) -> T => ...'");
}

bool Sema::rejectInferredMethods(ast::TranslationUnit *tu) {
  // A method of a template is declared (declareMethod) for each of its
  // instantiations, if any: reported here, once, and quietly there.
  bool ok = true;
  for (auto *cd : tu->getGenericClassDecls())
    for (auto *m : cd->getMethods())
      if (m->hasInferredResult()) {
        reportInferredMethod(m);
        ok = false;
      }
  for (auto *im : tu->getImplDecls())
    if (!im->getTypeParams().empty())
      for (auto *m : im->getMethods())
        if (!m->isStatic() && m->hasInferredResult()) {
          reportInferredMethod(m);
          ok = false;
        }
  return ok;
}

namespace {

/// Whether @p e is `None`, or a conditional each of whose branches is: a
/// value of no class.
bool isNoneValue(const ast::Expr *e) {
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e))
    return isNoneValue(te->getTrueExpr()) && isNoneValue(te->getFalseExpr());
  return ast::isa<ast::NoneLiteral>(e);
}

} // namespace

bool Sema::visitVarDecl(ast::VarDecl *node) {
  // The name must not shadow a type, a function or a reserved name (#126);
  // a function's is poisoned, as in visitAssignStmt.
  if (!checkBinderName(node->getName(), node->getLocation()) ||
      !checkNotSelf(node->getName(), node->getLocation())) {
    if (isFunctionNameForVariable(node->getName()) &&
        !CurrentScope->contains(node->getName()))
      declarePoisoned(CurrentScope, node->getName());
    return false;
  }

  // Check for duplicate declaration in the current scope.
  if (CurrentScope->contains(node->getName())) {
    error(node->getLocation(),
          "redeclaration of variable '" + node->getName() + "'");
    return false;
  }
  if (node->isLet())
    CurrentScope->Kinds[node->getName()] = VarKind::Let;
  // The concurrency prototype.
  DeclLoopDepth[varKey(CurrentScope, node->getName())] = LoopDepth;
  MovedVars.erase(varKey(CurrentScope, node->getName()));

  // Resolve the declared type.
  ast::Type *declTy = nullptr;
  // The qualifier (the concurrency prototype): the annotation's, or, in
  // `x: shared = e;`, written alone with the type inferred.
  ast::Qualifier qual;
  bool qualified = false;
  // Whether the qualifier stands alone (`x: shared = e;`): an annotation
  // with a type has its qualifier checked as the type resolves.
  bool bareQual = false;
  if (auto *qt = ast::dyn_cast<ast::QualifiedType>(node->getType());
      qt && !qt->getInnerType()) {
    qual = qt->getQualifier();
    qualified = true;
    bareQual = true;
    node->setType(nullptr);
    // As an annotation's qualifier is checked below: a borrow carries none.
    if (!checkBorrowQualifier(qual, node->getMode(),
                              "local '" + node->getName() + "'",
                              node->getLocation())) {
      declarePoisoned(CurrentScope, node->getName());
      return false;
    }
  }

  if (node->getType()) {
    FixedArrayAllowed = true; // a local may be a fixed-size array
    // and a length may be `_` where an initializer gives it
    InferredLengthAllowed = node->getInitExpr() != nullptr;
    ResolvingWeakDecl = node->isWeak();
    declTy = resolveValueType(node->getType(), node->getLocation(),
                              "variable '" + node->getName() + "'");
    ResolvingWeakDecl = false;
    if (!declTy) {
      declarePoisoned(CurrentScope, node->getName());
      return false;
    }
    node->setType(declTy); // canonical write-back (see VarDecl::setType)
    qual = takeQualifier();
    qualified = true;
    if (!checkBorrowQualifier(qual, node->getMode(),
                              "local '" + node->getName() + "'",
                              node->getLocation())) {
      declarePoisoned(CurrentScope, node->getName());
      return false;
    }
  }

  // Check the initializer type.
  bool viewOk = true;
  bool weakOfView = false;
  if (node->getInitExpr()) {
    unsigned errorsBefore = Diags.getErrorCount() + SuppressedFollowOns;
    // An `atomic` variable (the prototype) starts as a new cell holding
    // the value, or as another handle to an `atomic` variable's cell.
    ast::Type *expectedInit = declTy;
    if (!declTy || ast::isa<ast::AtomicType>(declTy)) {
      // An `atomic` variable, field or parameter named as the initializer
      // (`j = k`, `n: atomic int = st.hits`): its handle.  A `let` with no
      // type (`let j = k`) is a read-only copy: it takes the value.
      WantAtomicHandle =
          isPlaceChain(node->getInitExpr()) && !(node->isLet() && !declTy);
      if (auto *at = ast::dyn_cast<ast::AtomicType>(declTy))
        expectedInit = at->getInnerType();
    }
    auto *initTy = resolveExprType(node->getInitExpr(), expectedInit);
    WantAtomicHandle = false;
    // A `_` length is the initializer's: a literal fills it in as it is
    // checked (checkFixedArrayLiteral), anything else has it in its type.
    const bool inferred = declTy && hasInferredLength(declTy);
    if (!initTy) {
      // Only when the initializer failed silently: an error inside it (or
      // one suppressed as a follow-on) already explains the failure.
      if (Diags.getErrorCount() + SuppressedFollowOns == errorsBefore)
        error(node->getLocation(),
              "cannot determine type of initializer for variable '" +
                  node->getName() + "'");
      // A resolved annotation still types the variable; otherwise poison it.
      if (declTy && !inferred)
        CurrentScope->set(node->getName(), declTy);
      else
        declarePoisoned(CurrentScope, node->getName());
      return false;
    }
    if (inferred) {
      ast::Type *filled = fillLengths(declTy, initTy);
      if (!filled)
        errorNoLengths(node->getInitExpr()->getLocation(), declTy, initTy);
      else if (fixedElementBytes(filled) < 0)
        error(node->getLocation(),
              "variable '" + node->getName() + "' has type '" +
                  typeName(filled) +
                  "', which is too big: a fixed-size array holds at most " +
                  std::to_string(kMaxFixedArrayElements) + " elements");
      if (!filled || fixedElementBytes(filled) < 0) {
        declarePoisoned(CurrentScope, node->getName());
        return false;
      }
      declTy = filled;
      node->setType(declTy);
    }

    // The variable is still declared after this error.  A local borrow may
    // name a `view` (checkLocalBorrow), and so may a `weak` local: it takes
    // no reference of its own (a weak reference is not a copy of the
    // view).  What it reads is the view's object, so its reads are `view`s
    // too (VarKind::WeakView).
    if (node->getMode() == ast::ParamMode::Value && !node->isWeak())
      viewOk = checkViewNotStored(node->getInitExpr(), initTy);
    else if (node->isWeak())
      weakOfView =
          sharesStorage(initTy) && !viewOf(node->getInitExpr()).empty();
    if (declTy) {
      auto *atomic = ast::dyn_cast<ast::AtomicType>(declTy);
      if (atomic && initTy == atomic->getInnerType()) {
        // A new cell holding the value (the prototype).
      } else if (!checkAssignable(declTy, initTy, node->getInitExpr())) {
        if (!diagnoseOptionalNarrowing(node->getLocation(), declTy, initTy))
          error(node->getLocation(),
                "initializer of type '" + typeName(initTy) +
                    "' does not match declared type '" + typeName(declTy) +
                    "' for variable '" + node->getName() + "'");
        CurrentScope->set(node->getName(), declTy);
        return false;
      }
      if (node->getMode() == ast::ParamMode::View &&
          !checkViewLocalNotConverted(node->getName(), declTy,
                                      node->getInitExpr(), initTy))
        viewOk = false;
    } else {
      if (ast::isa<ast::SliceType>(initTy)) {
        errorFixedArrayHere(node->getLocation(), initTy,
                            "variable '" + node->getName() + "'");
        declarePoisoned(CurrentScope, node->getName());
        return false;
      }
      if (!checkNotVoidValue(initTy, node->getName(), node->getLocation())) {
        declarePoisoned(CurrentScope, node->getName());
        return false;
      }
      declTy = initTy;
    }
  }

  if (!declTy) {
    error(node->getLocation(),
          "variable '" + node->getName() +
              "' has no type annotation and no initializer");
    declarePoisoned(CurrentScope, node->getName());
    return false;
  }

  // A `weak` local holds a weak reference: its type is `T?` of a class `T`
  // (`y: weak = x;` with `x: T` or `x: T?` infers `T?`).  `None` names no
  // class: inferred, it would be a weak reference to the None value itself,
  // which never reads None.
  if (node->isWeak()) {
    if (!node->getType() && node->getInitExpr() &&
        isNoneValue(node->getInitExpr())) {
      error(node->getLocation(), "cannot infer the class of weak local '" +
                                     node->getName() + "' from None; write '" +
                                     node->getName() + ": weak T? = None'");
      declarePoisoned(CurrentScope, node->getName());
      return false;
    }
    if (!node->getType() && !ast::isa<ast::OptionalType>(declTy) &&
        ast::isa<ast::ClassType>(declTy))
      declTy = Ctx.getOptionalType(declTy);
    if (!checkWeakType(declTy, node->getLocation(),
                       "variable '" + node->getName() + "'")) {
      declarePoisoned(CurrentScope, node->getName());
      return false;
    }
    CurrentScope->Kinds[node->getName()] =
        weakOfView ? VarKind::WeakView : VarKind::Weak;
  }

  // The qualifier (the prototype): a value has none; an inferred one adopts
  // the initializer's (a copy of a `shared` reference is `shared`); a
  // qualified slot takes what converts into it.
  if (!sharesStorage(declTy) || node->isWeak()) {
    // One written alone (`x: shared = 1;`, `w: weak mut = n;`) is
    // checked as an annotation's is (resolveValueType).  A weak variable is
    // `weak` (the `shared` form) or `weak mut` (`shared mut`): it names a
    // `shared` object, or a `shared mut` one.
    if (node->isWeak() && qual.Form != ast::QualForm::Shared) {
      error(node->getLocation(),
            "weak variable '" + node->getName() +
                "' is 'weak' or 'weak mut': a weak reference names a 'shared' "
                "object");
      declarePoisoned(CurrentScope, node->getName());
      return false;
    }
    if (node->isWeak() && node->getInitExpr() &&
        !checkWeakStore(node->getInitExpr(), qual, declTy,
                        "variable '" + node->getName() + "'",
                        node->getLocation())) {
      declarePoisoned(CurrentScope, node->getName());
      return false;
    }
    if (bareQual && !checkQualifierApplies(qual, declTy, node->getLocation())) {
      declarePoisoned(CurrentScope, node->getName());
      return false;
    }
    // A weak variable holds nothing: it reads as the `shared` (`shared
    // mut`) reference it names, while its object lives.
    if (!node->isWeak())
      qual = ast::Qualifier{};
  } else if (!qualified && node->getInitExpr()) {
    qual = qualifierOf(node->getInitExpr());
  }
  if (bareQual && !checkQualifierApplies(qual, declTy, node->getLocation())) {
    declarePoisoned(CurrentScope, node->getName());
    return false;
  }
  if (node->getInitExpr() && node->getMode() == ast::ParamMode::Value &&
      !node->isWeak() &&
      !checkQualifiedFlow(qual, node->getInitExpr(),
                          "variable '" + node->getName() + "'",
                          node->getLocation()))
    viewOk = false;
  // A local borrow of a type argument's form (`v: view T` with T = `shared
  // Node`, #368) takes that form, as a parameter of it does.
  // A borrow of a cell would reach its contents with no lock.
  if (node->getInitExpr() && node->getMode() != ast::ParamMode::Value &&
      !checkNotLocked(node->getInitExpr(), node->getLocation()))
    viewOk = false;
  if (node->getInitExpr() && node->getMode() != ast::ParamMode::Value &&
      qual.FromArgument && ast::isRefType(declTy) &&
      ((node->getMode() == ast::ParamMode::Inout && !qual.isUnique()) ||
       qual.isSharedForm() || qual.Form == ast::QualForm::Locked) &&
      !checkBorrowForm(node->getInitExpr(), qual, node->getMode(),
                       "local '" + node->getName() + "'"))
    viewOk = false;
  if (node->getMode() == ast::ParamMode::Inout && node->getInitExpr() &&
      !checkParallelWrite(node->getInitExpr(), nullptr, "bound 'inout'",
                          node->getLocation()))
    viewOk = false;
  // A `view` of a part of a temporary would read it after the temporary,
  // which ends with the statement, ran the part's `__deinit__` (#378).
  if (node->getMode() == ast::ParamMode::View && node->getInitExpr() &&
      ast::holdsDeinit(declTy) && partLeavesTemporary(node->getInitExpr())) {
    error(node->getLocation(),
          "'" + node->getName() + "' cannot be a 'view' of '" +
              placeText(node->getInitExpr()) +
              "': it is a part of a temporary, which is destroyed with the "
              "statement, and a '" +
              typeName(declTy) +
              "' runs a '__deinit__' there; bind the value itself ('let " +
              node->getName() + " = ...')");
    viewOk = false;
  }
  if (std::string why = node->getMode() == ast::ParamMode::Inout
                            ? wholeSelfLent(node->getInitExpr())
                            : std::string();
      !why.empty()) {
    error(node->getLocation(), "'self' cannot be bound 'inout': " + why);
    viewOk = false;
  }
  // An `inout` local is the borrow's writes (a unique one's: a shared
  // form is its own, #368).
  if (node->getMode() == ast::ParamMode::Inout && sharesStorage(declTy) &&
      qual.Form == ast::QualForm::Unique)
    qual.Mut = true;
  node->setQualifier(qual);

  // A `let` cell (the prototype): nothing changes it through its name, and
  // an atomic matters only when threads change it.  Its value is the
  // alternative where one can be taken: from a value or a place, not from
  // a call's new cell.
  if (auto *at = ast::dyn_cast<ast::AtomicType>(declTy); at && node->isLet()) {
    const ast::Expr *init = node->getInitExpr();
    const bool valueForm =
        init && (isPlaceChain(init) || !init->getResolvedType() ||
                 !ast::isa<ast::AtomicType>(init->getResolvedType()));
    warning(node->getLocation(),
            "'" + node->getName() +
                "' is a 'let' atomic: nothing changes it through its name, "
                "and an atomic matters only when threads change it; declare "
                "it without 'let'" +
                (valueForm ? ", or as its value ('let " + node->getName() +
                                 ": " + typeName(at->getInnerType()) + "')"
                           : std::string()));
  }

  // Register the variable in the current scope.  An inferred type (`let x =
  // e;`) is written back like an annotation's, for the lowering.
  if (!node->getType())
    node->setType(declTy);
  CurrentScope->declare(node->getName(), declTy);
  CurrentScope->Quals[node->getName()] = qual;
  if (ast::isa<ast::TaskType>(declTy))
    TaskLocals[varKey(CurrentScope, node->getName())] = node->getLocation();
  return checkLocalBorrow(node) && viewOk;
}

bool Sema::checkWeakType(ast::Type *ty, ast::SourceLocation loc,
                         const std::string &what) {
  auto *ot = ast::dyn_cast<ast::OptionalType>(ty);
  if (ot && ast::isa<ast::ClassType>(ot->getInnerType()))
    return true;
  if (!ot && ast::isa<ast::ClassType>(ty))
    error(loc, "'weak' " + what +
                   " must be optional: a weak reference reads "
                   "as None once its object is destroyed; "
                   "write '" +
                   typeName(ty) + "?'");
  else
    error(loc, "'weak' " + what + " has type '" + typeName(ty) +
                   "'; a weak reference is an optional class type ('Node?'): "
                   "only an object is referred to weakly");
  return false;
}

namespace {

/// Whether @p copy is a copy of @p e made where @p e is: the same kind of
/// node at the same place in the source.
bool isCopyOf(const ast::Expr *copy, const ast::Expr *e) {
  const ast::SourceLocation a = copy->getLocation();
  const ast::SourceLocation b = e->getLocation();
  return copy->getKind() == e->getKind() && a.isValid() &&
         a.getLineStart() == b.getLineStart() &&
         a.getColumnStart() == b.getColumnStart() &&
         a.getLineEnd() == b.getLineEnd() &&
         a.getColumnEnd() == b.getColumnEnd();
}

/// A compound assignment `t op= v` is the assignment `t = t' op v`, where
/// `t'` is a copy of the target the parser made, at the target's place
/// (Parser::cloneTarget).  These are its `v`, or null for a plain
/// assignment: a subscript's `xs[i] op= v`, and a field's `r.f op= v`.
ast::Expr *compoundOperand(const ast::SubscriptAssignStmt *node) {
  const auto *be = ast::dyn_cast<ast::BinaryExpr>(node->getValue());
  const auto *se =
      be ? ast::dyn_cast<ast::SubscriptExpr>(be->getLHS()) : nullptr;
  return se && isCopyOf(se->getArray(), node->getArray()) &&
                 isCopyOf(se->getIndex(), node->getIndex())
             ? be->getRHS()
             : nullptr;
}

ast::Expr *compoundOperand(const ast::MemberAssignStmt *node) {
  const auto *be = ast::dyn_cast<ast::BinaryExpr>(node->getValue());
  const auto *ma =
      be ? ast::dyn_cast<ast::MemberAccessExpr>(be->getLHS()) : nullptr;
  return ma && ma->getFieldName() == node->getFieldName() &&
                 isCopyOf(ma->getReceiver(), node->getReceiver())
             ? be->getRHS()
             : nullptr;
}

} // namespace

bool Sema::visitMemberAssignStmt(ast::MemberAssignStmt *node) {
  const unsigned errorsBefore = Diags.getErrorCount() + SuppressedFollowOns;
  auto *recvTy = resolveExprType(node->getReceiver());
  if (!recvTy)
    return false;
  // An error inside the receiver (an argument of a call it makes) leaves
  // its type known.
  const bool recvFailed =
      Diags.getErrorCount() + SuppressedFollowOns != errorsBefore;
  // A cell's contents are written in a `lock` block (#368: `c.f = v` on a
  // `locked mut` cell wrote into the cell).
  if (!checkNotLocked(node->getReceiver(), node->getLocation()))
    return false;

  // A field cannot be assigned through an optional receiver (it may be None).
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(recvTy)) {
    errorOptionalUnwrap(node->getLocation(), ot);
    return false;
  }

  ast::Type *fieldTy = nullptr;
  ast::Qualifier fieldQual; // the prototype
  bool weakField = false;   // a `weak` field of a class
  if (auto *st = ast::dyn_cast<ast::StructType>(recvTy)) {
    // A struct's fields can be assigned, in place, where the struct is
    // changeable: not through a `view`, and not in a `let` local, whose
    // fields are part of its value.
    fieldTy = st->findField(node->getFieldName());
    fieldQual = st->findFieldQualifier(node->getFieldName());
    if (!fieldTy) {
      if (ErroneousClasses.count(st->getName()))
        ++SuppressedFollowOns; // the struct declaration was already reported
      else
        error(node->getLocation(), "no field '" + node->getFieldName() +
                                       "' in " + describeStruct(st));
      return false;
    }
    if (!checkMemberAccess(st, node->getFieldName(), node->getLocation()))
      return false;
    if (std::string let = letValueRoot(node->getReceiver()); !let.empty()) {
      error(node->getLocation(), "'" + let +
                                     "' is declared with 'let'; its fields "
                                     "are part of its value and cannot be "
                                     "assigned");
      return false;
    }
    // A struct held in a tuple is part of the tuple's value, which is
    // immutable: so are its fields (through any structs in between).
    if (const auto *ti = tupleElementHolding(node->getReceiver())) {
      error(node->getLocation(),
            "cannot assign to field '" + node->getFieldName() +
                "' of a struct in element '." + std::to_string(ti->getIndex()) +
                "' of type '" + typeName(ti->getTuple()->getResolvedType()) +
                "': tuples are immutable; build a new tuple instead");
      return false;
    }
  } else if (ast::isa<ast::TupleType>(recvTy)) {
    // `t.0 = v`: the parser hands tuple-index assignment over as a member
    // assignment whose field name is the index (docs/grammar.md) so that the
    // rejection is a typed diagnostic.  Tuples are immutable.
    error(node->getLocation(), "cannot assign to element '." +
                                   node->getFieldName() + "' of type '" +
                                   typeName(recvTy) +
                                   "': tuples are immutable; build a new tuple "
                                   "instead");
    return false;
  } else {
    auto *ct = ast::dyn_cast<ast::ClassType>(recvTy);
    if (!ct) {
      error(node->getLocation(), "member assignment '." + node->getFieldName() +
                                     "' on non-class type '" +
                                     typeName(recvTy) + "'");
      return false;
    }

    // Look up the field in the class hierarchy.
    fieldTy = ct->findField(node->getFieldName());
    fieldQual = ct->findFieldQualifier(node->getFieldName());
    weakField = ct->isWeakField(node->getFieldName());
    if (!fieldTy) {
      if (isErroneousClass(ct)) // the class declaration was already reported
        ++SuppressedFollowOns;
      else
        error(node->getLocation(), "no field '" + node->getFieldName() +
                                       "' in class '" + ct->getName() + "'");
      return false;
    }
    if (!checkMemberAccess(fieldOwner(ct, node->getFieldName()),
                           node->getFieldName(), node->getLocation()))
      return false;
  }

  // An `atomic` field (the prototype) is a cell: the assignment is one
  // atomic operation on it, through any receiver that reaches the field
  // (an immutable or a frozen object's cells are usable).
  if (auto *at = ast::dyn_cast<ast::AtomicType>(fieldTy))
    return checkAtomicMemberAssign(node, at);
  if (ast::isa<ast::StructType>(recvTy)
          ? !checkParallelWrite(node->getReceiver(), nullptr, "written",
                                node->getLocation(), &node->getFieldName(),
                                fieldTy)
          : !checkNotLookedAt(node->getReceiver(), "written",
                              node->getLocation(), &node->getFieldName()))
    return false;

  const bool path = ast::isa<ast::MemberAccessExpr>(node->getReceiver()) ||
                    ast::isa<ast::SubscriptExpr>(node->getReceiver()) ||
                    ast::isa<ast::TupleIndexExpr>(node->getReceiver());
  if (!checkChangeable(
          node->getReceiver(),
          path ? "cannot assign to '" + placeText(node->getReceiver()) + "." +
                     node->getFieldName() + "'"
               : "cannot assign to its field '" + node->getFieldName() + "'",
          node->getLocation()))
    return false;

  // The copy of the target in a compound assignment's value would report
  // the receiver's errors again: only the other operand is checked then.
  if (ast::Expr *operand = compoundOperand(node); operand && recvFailed) {
    resolveExprType(operand, fieldTy);
    return false;
  }
  auto *valTy = resolveExprType(node->getValue(), fieldTy);
  if (!valTy)
    return false;

  if (!checkAssignable(fieldTy, valTy, node->getValue())) {
    if (!diagnoseOptionalNarrowing(node->getLocation(), fieldTy, valTy))
      error(node->getLocation(),
            "cannot assign value of type '" + typeName(valTy) + "' to field '" +
                node->getFieldName() + "' of type '" + typeName(fieldTy) + "'");
    return false;
  }
  // A `weak` field owns nothing: storing a reference there copies no owner.
  // It names a `shared` object (`weak mut`, a `shared mut` one).
  if (weakField)
    return checkWeakStore(node->getValue(), fieldQual, fieldTy,
                          "field '" + node->getFieldName() + "'",
                          node->getLocation());
  if (!checkQualifiedFlow(fieldQual, node->getValue(),
                          "field '" + node->getFieldName() + "'",
                          node->getLocation()))
    return false;

  return checkViewNotStored(node->getValue(), valTy);
}

bool Sema::visitSubscriptAssignStmt(ast::SubscriptAssignStmt *node) {
  const unsigned errorsBefore = Diags.getErrorCount() + SuppressedFollowOns;
  WantAtomicHandle = isPlaceChain(node->getArray());
  auto *arrTy = resolveExprType(node->getArray());
  WantAtomicHandle = false;
  if (!arrTy)
    return false;
  if (auto *at = ast::dyn_cast<ast::AtomicType>(arrTy))
    if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(at->getInnerType()))
      return checkAtomicElementAssign(node, ft);
  // An optional cannot be indexed (it may be None).
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(arrTy)) {
    errorOptionalUnwrap(node->getLocation(), ot);
    return false;
  }
  auto *fixed = ast::dyn_cast<ast::FixedArrayType>(arrTy);
  auto *slice = ast::dyn_cast<ast::SliceType>(arrTy);
  if (!fixed && !slice) {
    error(node->getLocation(),
          "subscript assignment on non-array type '" + typeName(arrTy) + "'");
    return false;
  }
  auto *idxTy = resolveExprType(node->getIndex());
  if (idxTy && idxTy != Ctx.getIntTy()) {
    error(node->getIndex()->getLocation(),
          "array index must be 'int', got '" + typeName(idxTy) + "'");
  } else if (idxTy && fixed && !checkFixedIndex(fixed, node->getIndex())) {
    return false;
  }
  // A `parallel for` body writes an array of the enclosing function at the
  // loop's variable only: no two iterations then write one element.
  if (const ast::Identifier *root = placeRoot(node->getArray());
      root && !ParallelStack.empty())
    noteParallelNoElementRead(root);
  if (!checkParallelWrite(node->getArray(), node->getIndex(), "written",
                          node->getLocation()))
    return false;
  // An error in the array or the index, as a read of them reports it.
  const bool targetFailed =
      Diags.getErrorCount() + SuppressedFollowOns != errorsBefore;
  // A fixed-size array's elements are its value: a `let` one keeps them.
  if (fixed)
    if (std::string let = letValueRoot(node->getArray()); !let.empty()) {
      error(node->getLocation(), "'" + let +
                                     "' is declared with 'let'; its elements "
                                     "are part of its value and cannot be "
                                     "assigned");
      return false;
    }
  if (!checkChangeable(node->getArray(), "cannot assign to its elements",
                       node->getLocation()))
    return false;
  ast::Type *elemTy = fixed ? fixed->getElementType() : slice->getElementType();
  // As for a field: the copy of the target is not checked again.
  if (ast::Expr *operand = compoundOperand(node); operand && targetFailed) {
    resolveExprType(operand, elemTy);
    return false;
  }
  auto *valTy = resolveExprType(node->getValue(), elemTy);
  if (!valTy)
    return false;
  if (!checkAssignable(elemTy, valTy, node->getValue())) {
    if (!diagnoseOptionalNarrowing(node->getLocation(), elemTy, valTy))
      error(node->getLocation(), "cannot assign value of type '" +
                                     typeName(valTy) + "' to array of '" +
                                     typeName(elemTy) + "'");
    return false;
  }
  // The value converts into the elements' form (#368); the array's
  // writability is checked above.
  const ast::Qualifier elemQual =
      fixed ? fixed->getElemQual() : slice->getElemQual();
  if (!checkQualifiedFlow(elemQual, node->getValue(),
                          "an element of '" + placeText(node->getArray()) + "'",
                          node->getLocation()))
    return false;
  return checkViewNotStored(node->getValue(), valTy);
}

bool Sema::visitImportDecl(ast::ImportDecl *) {
  // Import processing happens in run() before visiting the TU.
  return true;
}

// Value-mode match: the subject is a primitive (int/float/bool/char) or Str.
// Every non-wildcard arm must be a literal whose type matches the subject.
bool Sema::checkValueMatch(ast::MatchStmt *node, ast::Type *subjectTy) {
  // A `locked Str`'s contents are compared in a `lock` block (#368).
  bool ok = checkNotLocked(node->getSubject(), node->getLocation());
  bool seenWildcard = false;

  for (ast::MatchArm *arm : node->getArms()) {
    if (seenWildcard) {
      error(arm->getLocation(),
            "unreachable arm: wildcard '_' must be the last arm");
      ok = false;
      continue;
    }

    ScopeGuard armGuard(*this);
    ArmMovesGuard armMoves(*this);

    if (arm->isWildcard()) {
      seenWildcard = true;
    } else if (arm->isLiteral()) {
      // Bindings are meaningless in value-mode; the grammar never produces a
      // binding on a literal arm, but guard anyway.
      if (arm->hasBinding()) {
        error(arm->getLocation(), "value-match arm cannot bind a variable");
        declarePoisoned(CurrentScope, arm->getBinding());
        ok = false;
      }
      // The pattern is a literal, as the grammar's is (docs/grammar.md
      // section 6): a switch case, or the value an equality compares with.
      // The parser and the AST interchange reader build no other; an AST that
      // has one is rejected here, before the lowering.
      ast::Expr *pattern = arm->getLiteralPattern();
      const bool literal = ast::isa<ast::IntegerLiteral>(pattern) ||
                           ast::isa<ast::FloatLiteral>(pattern) ||
                           ast::isa<ast::BoolLiteral>(pattern) ||
                           ast::isa<ast::CharLiteral>(pattern) ||
                           ast::isa<ast::StringLiteral>(pattern) ||
                           ast::isa<ast::NoneLiteral>(pattern);
      if (!literal) {
        error(arm->getLocation(),
              "a value-match arm's pattern must be a literal (a number, a "
              "character, a string, True, False or None)");
        ok = false;
      } else {
        // A numeric literal takes the subject's sized type when it fits.
        auto *litTy = resolveExprType(pattern, subjectTy);
        if (litTy && !typesEqual(litTy, subjectTy)) {
          error(arm->getLocation(), "match arm literal of type '" +
                                        typeName(litTy) +
                                        "' does not match subject type '" +
                                        typeName(subjectTy) + "'");
          ok = false;
        }
      }
    } else {
      // A type-named arm in a value-mode match.
      error(arm->getLocation(),
            "match on a value of type '" + typeName(subjectTy) +
                "' requires literal patterns, not type names");
      ok = false;
    }

    if (!visitStatements(arm->getBody()->getStatements()))
      ok = false;
  }

  return ok;
}

// Enum-mode match: the subject is an enum, and each non-wildcard arm names a
// bare variant.  The variant name is carried by the arm's type-name (a
// ClassType stub produced by the parser's `typeAnnotation: IDENT` rule).
bool Sema::checkEnumMatch(ast::MatchStmt *node, ast::EnumType *subjectTy) {
  bool ok = true;
  bool seenWildcard = false;
  StringSet seenVariants;

  for (ast::MatchArm *arm : node->getArms()) {
    if (seenWildcard) {
      error(arm->getLocation(),
            "unreachable arm: wildcard '_' must be the last arm");
      ok = false;
      continue;
    }

    ScopeGuard armGuard(*this);
    ArmMovesGuard armMoves(*this);

    if (arm->isWildcard()) {
      seenWildcard = true;
    } else if (arm->isLiteral()) {
      error(arm->getLocation(),
            "match on enum '" + subjectTy->getName() +
                "' requires bare variant names, not literal patterns");
      ok = false;
    } else {
      // The arm's type-name stub carries the variant name.
      std::string variant;
      if (auto *stub = ast::dyn_cast<ast::ClassType>(arm->getArmType()))
        variant = stub->getName();
      else if (auto *et = ast::dyn_cast<ast::EnumType>(arm->getArmType()))
        variant = et->getName();
      if (variant.empty() || subjectTy->findVariant(variant) < 0) {
        // Arms use bare variant names; say so when a qualified name
        // (`base::Color::Green`) ends in a real variant.
        auto sep = variant.rfind("::");
        std::string bare =
            sep == std::string::npos ? "" : variant.substr(sep + 2);
        if (!bare.empty() && subjectTy->findVariant(bare) >= 0) {
          std::string msg = "'" + variant;
          msg += "' is not a valid match arm; use the bare variant name '";
          msg += bare + "'";
          error(arm->getLocation(), msg);
        } else if (variant.empty()) {
          // A type that is no name (`int`, `(int, Str)`, `E?`, `int[3]`)
          // cannot be one.
          std::string msg = "'" + typeName(arm->getArmType()) +
                            "' is not a variant of enum '" +
                            subjectTy->getName() + "'";
          if (subjectTy->getNumVariants() != 0) {
            msg += ": an arm names one of its variants, such as '";
            msg += *subjectTy->getVariants().front();
            msg += "'";
          }
          error(arm->getLocation(), msg);
        } else {
          error(arm->getLocation(), "'" + variant +
                                        "' is not a variant of enum '" +
                                        subjectTy->getName() + "'");
        }
        ok = false;
      } else if (!seenVariants.insert(variant).second) {
        error(arm->getLocation(),
              "duplicate variant '" + variant + "' in enum match");
        ok = false;
      }
      // `x: V { }` binds the variant's fields, a struct: a copy, or with
      // `view` / `inout` the subject's own (bindArmName).
      if (arm->hasBinding()) {
        int64_t idx = variant.empty() ? -1 : subjectTy->findVariant(variant);
        ast::StructType *payload =
            idx < 0 ? nullptr : subjectTy->getPayload(static_cast<size_t>(idx));
        if (idx >= 0 && !payload) {
          std::string msg = "variant '" + variant + "' of enum '";
          msg += subjectTy->getName() + "' has no fields to bind: write the ";
          msg += "arm '" + variant + " { ... }'";
          error(arm->getLocation(), msg);
          ok = false;
        }
        if (!payload || ErroneousClasses.count(payload->getName())) {
          declarePoisoned(CurrentScope, arm->getBinding());
        } else if (!bindArmName(arm, node->getSubject(), payload)) {
          ok = false;
        } else if (!CurrentScope->declare(arm->getBinding(), payload)) {
          error(arm->getLocation(),
                "redeclaration of '" + arm->getBinding() + "' in match arm");
          ok = false;
        }
      }
    }

    if (!visitArmBody(arm, node->getSubject()))
      ok = false;
  }

  return ok;
}

bool Sema::visitMatchStmt(ast::MatchStmt *node) {
  auto *subjectTy = resolveExprType(node->getSubject());
  if (!subjectTy)
    return false;
  if (subjectTy == Ctx.getVoidTy()) {
    error(node->getSubject()->getLocation(),
          "match subject has type 'void' (the expression produces no value)");
    return false;
  }
  // An arm's name for a `view` that shares what it holds is a `view` too,
  // for this match only (a nested one decides its own).
  struct RestoreView {
    bool &Flag;
    bool Outer;
    ~RestoreView() { Flag = Outer; }
  } restoreView{MatchSubjectIsView, MatchSubjectIsView};
  MatchSubjectIsView =
      sharesStorage(subjectTy) && !viewOf(node->getSubject()).empty();
  // Record the subject type so the lowering can pick the right form (in
  // particular, distinguish an enum subject from a class subject).
  node->getSubject()->setResolvedType(subjectTy);
  // A part of a temporary matched on leaves it for the match (#378), as
  // one read into a variable does (checkCopyRule).
  if (!noteLeavesTemporary(node->getSubject()))
    return false;

  // The arms' moves join after the match (the prototype's move analysis).
  MatchMovesGuard matchMoves(*this, node);

  // A match is value-mode when its subject resolves to a builtin primitive
  // (int/float/bool/char) or to Str.  In value-mode, arms compare the subject
  // against literal patterns instead of dispatching on runtime type.  Str is a
  // ClassType internally, so it is steered into value-mode explicitly.
  bool valueMode =
      ast::isa<ast::BuiltinType>(subjectTy) || subjectTy == Ctx.getStrTy();
  if (valueMode)
    return checkValueMatch(node, subjectTy);

  // Enum-mode: the subject is an enum.  Arms name bare variants (each parsed as
  // a type-name arm whose ClassType-stub name is the variant name).
  if (auto *enumTy = ast::dyn_cast<ast::EnumType>(subjectTy))
    return checkEnumMatch(node, enumTy);

  // Optional-mode: the subject is `T?` — `match` is how an optional is
  // unwrapped (issue #5).
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(subjectTy);
      ot && ast::isa<ast::AtomicType>(ot->getInnerType())) {
    error(node->getSubject()->getLocation(),
          "a 'match' on an optional 'atomic' handle is not supported yet");
    return false;
  }
  if (auto *optTy = ast::dyn_cast<ast::OptionalType>(subjectTy))
    return checkOptionalMatch(node, optTy);

  // The subject must be a class type — matching on other builtins is not
  // supported.
  auto *subjectCt = ast::dyn_cast<ast::ClassType>(subjectTy);
  if (!subjectCt) {
    error(node->getSubject()->getLocation(),
          ast::isa<ast::StructType>(subjectTy)
              ? "match on a struct ('" + typeName(subjectTy) +
                    "') is not supported yet: a struct is a value, not an "
                    "object"
              : "match subject must be a class type, got '" +
                    typeName(subjectTy) + "'");
    return false;
  }

  // Helper: return true if `sub` is a subclass of (or equal to) `super`.
  auto isSubclassOf = [](ast::ClassType *sub, ast::ClassType *super) -> bool {
    for (auto *c = sub; c; c = c->getSuperClass())
      if (c == super)
        return true;
    return false;
  };

  bool ok = true;
  bool seenWildcard = false;

  for (ast::MatchArm *arm : node->getArms()) {
    if (seenWildcard) {
      error(arm->getLocation(),
            "unreachable arm: wildcard '_' must be the last arm");
      ok = false;
      continue;
    }

    // Open a new scope for each arm (the binding, if any, lives here).
    ScopeGuard armGuard(*this);
    ArmMovesGuard armMoves(*this);

    if (arm->isWildcard()) {
      seenWildcard = true;
    } else if (arm->isLiteral()) {
      // A literal pattern in a class-mode match.
      error(arm->getLocation(),
            "match on a class subject of type '" + subjectCt->getName() +
                "' requires type-name arms, not literal patterns");
      ok = false;
      if (!visitStatements(arm->getBody()->getStatements()))
        ok = false;
      continue;
    } else {
      // 2. Resolve the arm's type annotation.
      auto *resolvedArmTy =
          resolveType(arm->getArmType(), arm->getLocation(), "match arm");
      // A qualifier written on the arm's type is the binding's, which must
      // be the subject's form (bindArmName).
      const ast::Qualifier written = takeQualifier();
      arm->setArmType(resolvedArmTy); // write canonical pointer back
      auto *armCt = resolvedArmTy ? ast::dyn_cast<ast::ClassType>(resolvedArmTy)
                                  : nullptr;
      if (!armCt) {
        // Only emit a secondary diagnostic if resolution succeeded but the
        // type is not a class. If resolveType already emitted "unknown
        // type", resolvedArmTy is null and that's enough.
        if (resolvedArmTy)
          error(arm->getLocation(), "match arm type must be a class type");
        ok = false;
        // The binding is still in scope in the body, poisoned.
        if (arm->hasBinding())
          declarePoisoned(CurrentScope, arm->getBinding());
        // Still try to visit the body to surface further errors.
      } else {
        // 3. The arm type must be a subclass of the subject's static type.
        //    (Matching Obj against Obj arms is always allowed since every class
        //    descends from Obj; the subjectCt == Ctx.getObjTy() case passes
        //    trivially because every armCt IS a subclass of Obj.)
        if (!isSubclassOf(armCt, subjectCt)) {
          error(arm->getLocation(), "type '" + typeName(armCt) +
                                        "' is not a subclass of '" +
                                        subjectCt->getName() + "'");
          ok = false;
        } else if (armCt->isAbstract()) {
          // An arm is an exact-class test, and an object is of an abstract
          // class only while it is being constructed (its `__init__` has
          // assigned its fields, its subclass's has not), which no arm can
          // rely on.
          reportAbstractArm(arm, armCt);
          ok = false;
        } else if (!checkNotLocked(node->getSubject(), arm->getLocation())) {
          // The exact-class test reads a cell's contents, which a `lock`
          // block reaches (#368: it tested the cell).  The binding's uses
          // in the body follow from it.
          ok = false;
          armCt = nullptr;
        }
        // A qualifier a type argument gave an arm without a binding tests
        // nothing; one written must be the subject's form (#368).
        if (armCt && !arm->hasBinding() && !written.FromArgument &&
            !checkArmForm(node->getSubject(), armCt, written, "the arm matches",
                          arm->getLocation()))
          ok = false;

        // 4. Declare the binding variable with the narrowed (arm) type.  An
        //    arm naming a type parameter that is an optional `X?` binds the
        //    `X` it matches too (ASTCloner): the subject is never None, so an
        //    `inout` binding of a `T` could not go back to it.
        arm->setBindType(nullptr);
        if (!armCt) {
          if (arm->hasBinding())
            declarePoisoned(CurrentScope, arm->getBinding());
        } else if (arm->hasBinding() &&
                   !bindArmName(arm, node->getSubject(), armCt, written)) {
          ok = false;
        } else if (arm->hasBinding()) {
          if (!CurrentScope->declare(arm->getBinding(), armCt)) {
            error(arm->getLocation(),
                  "redeclaration of '" + arm->getBinding() + "' in match arm");
            ok = false;
          }
        }
      }
    }

    // 5. Recursively type-check the arm body.
    if (!visitArmBody(arm, node->getSubject()))
      ok = false;
  }

  return ok;
}

void Sema::reportAbstractArm(const ast::MatchArm *arm,
                             const ast::ClassType *armCt) {
  error(arm->getLocation(),
        "a 'match' arm cannot name abstract class '" +
            displayName(armCt->getName()) +
            "': an object is of it only while it is being constructed");
}

// Optional-mode match: the subject is `T?`.
//
//   match maybe {
//     n: T   { … }   // present: n is the unwrapped T (any runtime subtype)
//     s: Sub { … }   // present AND exactly of class Sub (a strict subclass)
//     None   { … }   // absent
//     _      { … }   // anything not matched above
//   }
//
// The arm naming the wrapped type T is the "present" pattern: it matches
// every non-None value regardless of runtime subtype (unlike class mode,
// where a type arm is an exact-type test) — that is what makes it an
// unwrap.  A strict-subclass arm keeps class-mode exact-type semantics.  The
// match is exhaustive with `_`, or with both a `None` arm and a T arm.
bool Sema::checkOptionalMatch(ast::MatchStmt *node,
                              ast::OptionalType *subjectTy) {
  ast::Type *inner = subjectTy->getInnerType();
  auto *innerCt = ast::dyn_cast<ast::ClassType>(inner);
  const std::string subjName = typeName(subjectTy);

  bool ok = true;
  bool seenWildcard = false, seenNone = false, seenInner = false;

  for (ast::MatchArm *arm : node->getArms()) {
    if (seenWildcard) {
      error(arm->getLocation(),
            "unreachable arm: wildcard '_' must be the last arm");
      ok = false;
      continue;
    }

    // Open a new scope for each arm (the binding, if any, lives here).
    ScopeGuard armGuard(*this);
    ArmMovesGuard armMoves(*this);

    if (arm->isWildcard()) {
      seenWildcard = true;
    } else if (arm->isLiteral()) {
      if (!ast::isa<ast::NoneLiteral>(arm->getLiteralPattern())) {
        error(arm->getLocation(),
              "match on optional '" + subjName +
                  "' requires type-name arms or 'None', not literal patterns");
        ok = false;
      } else if (seenNone) {
        error(arm->getLocation(),
              "duplicate 'None' arm in match on '" + subjName + "'");
        ok = false;
      } else {
        seenNone = true;
      }
    } else {
      auto *resolvedArmTy =
          resolveType(arm->getArmType(), arm->getLocation(), "match arm");
      // A qualifier written on the arm's type is the binding's, which must
      // be the subject's form (bindArmName).
      const ast::Qualifier written = takeQualifier();
      arm->setArmType(resolvedArmTy); // write canonical pointer back
      ast::Type *bindTy = nullptr;
      if (!resolvedArmTy) {
        ok = false; // resolveType reported the unknown type
      } else if (ast::isa<ast::OptionalType>(resolvedArmTy)) {
        error(arm->getLocation(),
              "match arm type '" + typeName(resolvedArmTy) +
                  "' cannot be optional; name '" + typeName(inner) +
                  "' for the present case and 'None' for the absent case");
        ok = false;
      } else if (seenInner) {
        error(arm->getLocation(),
              "unreachable arm: the '" + typeName(inner) +
                  "' arm above already matches every non-None value");
        ok = false;
      } else if (innerCt) {
        auto *armCt = ast::dyn_cast<ast::ClassType>(resolvedArmTy);
        if (!armCt || !armCt->isSubtypeOf(innerCt)) {
          error(arm->getLocation(),
                "type '" + typeName(resolvedArmTy) +
                    "' is not a subclass of '" + typeName(inner) +
                    "' (the match subject has type '" + subjName + "')");
          ok = false;
        } else {
          // The arm naming T unwraps any present value, so T may be
          // abstract; a strict subclass's arm is an exact-class test.
          if (armCt != innerCt && armCt->isAbstract()) {
            reportAbstractArm(arm, armCt);
            ok = false;
            bindTy = armCt;
          } else if (armCt != innerCt &&
                     !checkNotLocked(node->getSubject(), arm->getLocation())) {
            // The test reads a cell's contents (#368); the binding is
            // poisoned below.
            ok = false;
          } else {
            bindTy = armCt;
          }
        }
      } else if (ast::isa<ast::BuiltinType>(inner) ||
                 ast::isa<ast::FunctionType>(inner) ||
                 ast::isa<ast::EnumType>(inner) ||
                 ast::isa<ast::TupleType>(inner)) {
        // `int?`: the `int` arm unwraps the present value, a plain `int`;
        // likewise the function type of a `((int) -> int)?`, the enum of a
        // `Suit?`, and the tuple or the struct of a `(int, Str)?` / `P?`.
        if (resolvedArmTy != inner) {
          error(arm->getLocation(),
                "match arm type '" + typeName(resolvedArmTy) +
                    "' does not match the optional subject type '" + subjName +
                    "'");
          ok = false;
        } else {
          bindTy = inner;
        }
      }
      if (bindTy == inner)
        seenInner = true;
      // An arm of a template naming a type parameter `T` that is an optional
      // `X?` matches an `X`, and binds it as a `T` (ASTCloner): the template
      // uses it as the `T` it names, whatever `T` is.
      if (bindTy && arm->getBindType() != arm->getArmType()) {
        bindTy =
            resolveType(arm->getBindType(), arm->getLocation(), "match arm");
        arm->setBindType(bindTy);
        if (!bindTy)
          ok = false;
      }
      // An arm in error still binds its name in the body, poisoned.
      if (arm->hasBinding() && !bindTy)
        declarePoisoned(CurrentScope, arm->getBinding());
      if (!arm->hasBinding() && bindTy && !written.FromArgument &&
          !checkArmForm(node->getSubject(), bindTy, written, "the arm matches",
                        arm->getLocation()))
        ok = false;
      if (arm->hasBinding() && bindTy &&
          !bindArmName(arm, node->getSubject(), bindTy, written)) {
        ok = false;
      } else if (arm->hasBinding() && bindTy) {
        if (!CurrentScope->declare(arm->getBinding(), bindTy)) {
          error(arm->getLocation(),
                "redeclaration of '" + arm->getBinding() + "' in match arm");
          ok = false;
        }
      }
    }

    if (!visitArmBody(arm, node->getSubject()))
      ok = false;
  }

  return ok;
}

} // namespace sema
} // namespace paykan
