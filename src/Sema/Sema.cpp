// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "Sema.h"
#include "Names.h"
#include "SemaInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <unordered_set>
#include <utility>
#include <vector>

namespace paykan {
namespace sema {

// Static module cache.
StringMap<Sema::ModuleInfo> Sema::ModuleCache;

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
  FunctionTable[std::string(name)] = {
      retTy, std::move(paramTys), isBuiltin, {}};
}

const Sema::FunctionSig *Sema::lookupFunction(std::string_view name) const {
  auto it = FunctionTable.find(name);
  return it != FunctionTable.end() ? &it->second : nullptr;
}

bool Sema::checkDeclNameAvailable(const std::string &name,
                                  ast::SourceLocation loc, DeclKind kind) {
  // Compiler builtins are registered before any user declaration and must
  // never be replaced.  Which names are builtin is not spelled out here: a
  // class is builtin iff the ASTContext bootstrap flagged it
  // (ClassType::Builtin), and a function iff run() registered it so
  // (FunctionSig::IsBuiltin).  Classes are checked first so a name that is
  // both (`Str` is a class and a conversion function) reports as a class.
  if (auto *ct = Ctx.lookupClassType(name)) {
    if (ct->isBuiltin())
      error(loc, "'" + name + "' is a builtin class and cannot be redeclared");
    else if (kind == DeclKind::Class)
      // Local classes are checked before any of them is registered, so a
      // class hit while declaring a class can only come from an import.
      error(loc, "class '" + name +
                     "' conflicts with an imported type of the same name");
    else
      error(loc, "'" + name + "' is already declared as a class");
    return false;
  }
  if (const auto *sig = lookupFunction(name); sig && sig->IsBuiltin) {
    error(loc, "'" + name + "' is a builtin function and cannot be redeclared");
    return false;
  }
  if (Ctx.lookupStructType(name)) {
    if (kind == DeclKind::Struct && !LocalStructNames.count(name))
      // Local structs are checked before any of them is registered, so a
      // struct hit while declaring a struct can only come from an import
      // (or, for a generic struct, from a struct of the module).
      error(loc, "struct '" + name +
                     "' conflicts with an imported type of the same name");
    else if (kind == DeclKind::Class)
      error(loc,
            "class '" + name + "' conflicts with a struct of the same name");
    else
      error(loc, "'" + name + "' is already declared as a struct");
    return false;
  }
  if (Ctx.lookupEnumType(name)) {
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
  if (FuncTemplates.count(name)) {
    error(loc, "'" + name + "' is already declared as a generic function");
    return false;
  }
  return true;
}

void Sema::error(ast::SourceLocation loc, const std::string &msg) {
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
      if (!typesEqual(ta->getElementType(i), tb->getElementType(i)))
        return false;
    return true;
  }
  return false;
}

bool Sema::isAssignable(ast::Type *dst, ast::Type *src) const {
  if (dst == src)
    return true;

  // A slice takes a fixed-size array of exactly its element type, of any
  // length (and another slice, the same type).
  if (auto *dstST = ast::dyn_cast<ast::SliceType>(dst)) {
    auto *srcFT = ast::dyn_cast<ast::FixedArrayType>(src);
    return srcFT && srcFT->getElementType() == dstST->getElementType();
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

  // ClassType subtyping: src <: dst.
  if (auto *dstCT = ast::dyn_cast<ast::ClassType>(dst))
    if (auto *srcCT = ast::dyn_cast<ast::ClassType>(src))
      return srcCT->isSubtypeOf(dstCT);

  return false;
}

bool Sema::checkAssignable(ast::Type *dst, ast::Type *srcTy, ast::Expr *src) {
  // `None` into an optional slot: statically the literal is `Obj` (a design
  // decision — see docs/language/10-optionals.md), but in this position it
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
  if (!isAssignable(dst, srcTy))
    return false;
  // `int` -> `int?` (and `int` -> `float?`): the lowering makes the value
  // the optional's present value.  A fixed-size array into a slice is
  // passed as its address and length.
  if (ast::wrapsInOptional(dst, srcTy) || ast::dyn_cast<ast::SliceType>(dst))
    src->setCoercedType(dst);
  return true;
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
        ast::isa<ast::TupleLiteralExpr>(e))
      return adoptLiteralElements(d, t, e);
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
    auto *retyped = Ctx.getTupleType(std::move(elems));
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
  if (ast::isValueOptional(ty))
    return elementAlign(ast::cast<ast::OptionalType>(ty)->getInnerType());
  if (const auto *bt = ast::dyn_cast<ast::BuiltinType>(ty))
    return bt->getTypeKind() == ast::BuiltinType::Bool ||
                   bt->getTypeKind() == ast::BuiltinType::Char
               ? 1
               : 8;
  return 8; // an enum value, or a box
}

int64_t Sema::fixedElementBytes(const ast::Type *ty) {
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
  // A reference is held as its box.
  if (ast::isRefType(ty))
    return 8;
  // An optional value is a presence flag padded to its value's alignment,
  // then the value (`int?` takes 16 bytes, `bool?` 2).
  if (ast::isValueOptional(ty))
    if (const auto *inner = ast::dyn_cast<ast::BuiltinType>(
            ast::cast<ast::OptionalType>(ty)->getInnerType()))
      return 2 * fixedElementBytes(inner);
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
  return Ctx.getFixedArrayType(elem, ft->isInferred() ? fromFT->getLength()
                                                      : ft->getLength());
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
                 "a field for now");
  return false;
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

ast::Type *Sema::resolveType(ast::Type *ty, ast::SourceLocation loc,
                             const std::string &context) {
  const bool allowFixed = std::exchange(FixedArrayAllowed, false);
  const bool allowInferred = std::exchange(InferredLengthAllowed, false);
  const bool allowSlice = std::exchange(SliceAllowed, false);
  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(ty))
    return Ctx.getBuiltinType(bt->getTypeKind());
  // A parameter's slice, resolved already.
  if (ast::isa<ast::SliceType>(ty))
    return ty;

  if (auto *et = ast::dyn_cast<ast::EnumType>(ty))
    return Ctx.lookupEnumType(et->getName());

  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty)) {
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
    // A private type of an imported module, spelled bare: it is that
    // module's own (a qualified spelling is reported below).
    if (names::isPrivateName(ct->getName()) &&
        ct->getName().find(names::kQualSep) == std::string::npos &&
        ct->getName().find('<') == std::string::npos) // not an instantiation
      if (auto o = ImportedTypeOrigins.find(ct->getName());
          o != ImportedTypeOrigins.end()) {
        error(loc, "'" + ct->getName() + "' is private to module '" +
                       o->second + "'");
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
        StructTemplates.count(ct->getName())) {
      error(loc,
            context + " names generic " +
                (ClassTemplates.count(ct->getName()) ? "class '" : "struct '") +
                ct->getName() + "' without type arguments (write '" +
                ct->getName() + "<...>')");
      return nullptr;
    }
    if (Traits.count(ct->getName())) {
      error(loc, context + " names trait '" + ct->getName() +
                     "', which is not a type (use a type parameter bounded "
                     "by it: '<T: " +
                     ct->getName() + ">')");
      return nullptr;
    }
    if (reportPrivateImport(ct->getName(), loc))
      return nullptr;
    if (!isFailedImportUse(ct->getName()))
      error(loc, context + " has unknown class type '" + ct->getName() + "'");
    return nullptr;
  }

  // `T[]` is a slice, the type of a `view` or `inout` parameter only.
  if (auto *at = ast::dyn_cast<ast::ArrayType>(ty)) {
    if (!allowSlice) {
      const std::string elem = typeName(at->getElementType());
      error(loc, context + " has type '" + typeName(at) +
                     "': a slice can only be a 'view' or 'inout' parameter, "
                     "and an array has a length ('" +
                     (ast::isa<ast::FunctionType>(at->getElementType())
                          ? "(" + elem + ")"
                          : elem) +
                     "[3]')");
      return nullptr;
    }
    FixedArrayAllowed = true; // a slice's elements may be fixed-size arrays
    auto *elemTy = resolveType(at->getElementType(), loc, context + " element");
    if (!elemTy)
      return nullptr;
    if (fixedElementBytes(elemTy) < 0) {
      error(loc, context + " has type '" + typeName(elemTy) +
                     "[]': a slice of '" + typeName(elemTy) +
                     "' is not supported yet");
      return nullptr;
    }
    return Ctx.getSliceType(elemTy);
  }

  if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty)) {
    // An array of arrays is one type, checked as a whole.
    std::vector<int64_t> lengths;
    ast::Type *elem = ft;
    while (auto *f = ast::dyn_cast<ast::FixedArrayType>(elem)) {
      lengths.push_back(f->getLength());
      elem = f->getElementType();
    }
    auto *base = resolveType(elem, loc, context + " element");
    if (!base)
      return nullptr;
    ast::Type *fixed = base;
    for (auto it = lengths.rbegin(); it != lengths.rend(); ++it)
      fixed = Ctx.getFixedArrayType(fixed, *it);
    if (!allowFixed) {
      errorFixedArrayHere(loc, fixed, context);
      return nullptr;
    }
    if (fixedElementBytes(base) < 0) {
      error(loc, context + " has type '" + typeName(fixed) +
                     "': a fixed-size array of '" + typeName(base) +
                     "' is not supported yet");
      return nullptr;
    }
    // Its size is checked once its lengths are inferred (visitVarDecl).
    if (hasInferredLength(fixed)) {
      if (allowInferred)
        return fixed;
      error(loc, context + " has type '" + typeName(fixed) +
                     "': a length can be '_' only in a variable declaration "
                     "with an initializer, which gives it");
      return nullptr;
    }
    if (fixedElementBytes(fixed) < 0) {
      error(loc, context + " has type '" + typeName(fixed) +
                     "', which is too big: a fixed-size array holds at most " +
                     std::to_string(kMaxFixedArrayElements) + " elements");
      return nullptr;
    }
    return fixed;
  }

  if (auto *gt = ast::dyn_cast<ast::GenericType>(ty)) {
    // Generic type application `Box<int>`: resolve the arguments to canonical
    // types, then instantiate (or fetch the cached instantiation of) the class
    // template.  The result is an ordinary ClassType.
    std::vector<ast::Type *> args;
    for (size_t i = 0; i < gt->getNumArgs(); ++i) {
      auto *arg =
          resolveType(gt->getArgs()[i], loc,
                      context + " type argument " + std::to_string(i + 1) +
                          " of '" + gt->getName() + "'");
      if (!arg)
        return nullptr;
      args.push_back(arg);
    }
    if (StructTemplates.count(gt->getName()))
      return instantiateStruct(gt->getName(), args, loc);
    return instantiateClass(gt->getName(), args, loc);
  }

  if (auto *ot = ast::dyn_cast<ast::OptionalType>(ty)) {
    auto *inner = resolveType(ot->getInnerType(), loc, context);
    if (!inner)
      return nullptr;
    // A `T?` of a reference type is T's own PaykanShared* box with NULL
    // meaning None; a primitive's (`int?`, `float?`, `bool?`, `char?`) is a
    // value, the primitive and a presence flag (#96).  `void` has no value
    // (the parser rejects the spelled `void?`; a type argument can still
    // produce it).  `Enum?` is not supported yet.  Nested optionals are
    // rejected because a `T??` would need a second None to distinguish `None`
    // from `Some(None)`.
    if (inner == Ctx.getVoidTy()) {
      error(loc, context + " has type 'void?': optional type 'void?' is not "
                           "supported");
      return nullptr;
    }
    if (ast::isa<ast::EnumType>(inner)) {
      error(loc, context + " has type '" + typeName(inner) +
                     "?': optional enum types are not supported yet");
      return nullptr;
    }
    if (ast::isa<ast::OptionalType>(inner)) {
      error(loc, context + " has nested optional type '" + typeName(inner) +
                     "?', which is not supported");
      return nullptr;
    }
    // An optional struct could not be unwrapped either (`match` takes no
    // struct subject or arm); before the tuple check, which a struct is.
    if (ast::isa<ast::StructType>(inner)) {
      error(loc, context + " has type '" + typeName(inner) +
                     "?': optional struct types are not supported yet (an "
                     "optional struct cannot be unwrapped with match)");
      return nullptr;
    }
    // An optional tuple could never be unwrapped: `match` rejects tuple type
    // arms, so the `T` arm that unwraps an optional does not exist for it.
    // Reject it until tuple patterns land.
    if (ast::isa<ast::TupleType>(inner)) {
      error(loc, context + " has type '" + typeName(inner) +
                     "?': optional tuple types are not supported yet (an "
                     "optional tuple cannot be unwrapped with match)");
      return nullptr;
    }
    // Resolve to the context's canonical (interned) instance, as for arrays.
    return Ctx.getOptionalType(inner);
  }

  // A struct type is canonical already (a resolved annotation, or a type
  // argument put into a template); before the tuple case, which it is too.
  if (ast::isa<ast::StructType>(ty))
    return ty;

  if (auto *fnt = ast::dyn_cast<ast::FunctionType>(ty)) {
    // `(int, view Str) -> bool`: the parameters as a function's (a slice
    // for a `view` or `inout` one, a fixed-size array for any), the result
    // any type but a fixed-size array; then the canonical instance.
    std::vector<ast::Type *> params;
    std::vector<ast::ParamMode> modes;
    for (size_t i = 0; i < fnt->getNumParams(); ++i) {
      const std::string what = context + " parameter " + std::to_string(i + 1);
      FixedArrayAllowed = true;
      SliceAllowed = fnt->getMode(i) != ast::ParamMode::Value;
      auto *pty = resolveType(fnt->getParamType(i), loc, what);
      if (!pty)
        return nullptr;
      if (pty == Ctx.getVoidTy()) {
        error(loc, what + " cannot have type 'void'");
        return nullptr;
      }
      params.push_back(pty);
      modes.push_back(fnt->getMode(i));
    }
    FixedArrayAllowed = true; // a result may be a fixed-size array
    auto *ret = resolveType(fnt->getReturnType(), loc, context + " result");
    if (!ret)
      return nullptr;
    return Ctx.getFunctionType(std::move(params), std::move(modes), ret);
  }

  if (auto *tt = ast::dyn_cast<ast::TupleType>(ty)) {
    std::vector<ast::Type *> elems;
    elems.reserve(tt->getArity());
    for (size_t i = 0; i < tt->getArity(); ++i) {
      auto *elemTy = resolveType(tt->getElementType(i), loc,
                                 context + " element " + std::to_string(i));
      if (!elemTy)
        return nullptr;
      if (elemTy == Ctx.getVoidTy()) {
        error(loc, context + " element " + std::to_string(i) +
                       " cannot have type 'void'");
        return nullptr;
      }
      elems.push_back(elemTy);
    }
    // Same canonicalisation as arrays: the interned per-element-list node.
    return Ctx.getTupleType(std::move(elems));
  }

  error(loc, context + " has unknown type");
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

bool Sema::checkBinderName(const std::string &name, ast::SourceLocation loc) {
  if (name == names::kStdin) {
    error(loc,
          "'" + name + "' is a reserved name and cannot be used as a variable");
    return false;
  }
  if (isTypeNameForVariable(name)) {
    error(loc,
          "'" + name + "' is a type name and cannot be used as a variable");
    return false;
  }
  return true;
}

bool Sema::bindArmName(ast::MatchArm *arm, const ast::Expr *subject,
                       ast::Type *bindTy) {
  const std::string &name = arm->getBinding();
  std::string msg;
  if (!checkBinderName(name, arm->getLocation())) {
    // Reported.
  } else if (arm->getMode() == ast::ParamMode::Inout) {
    // `n: inout T` is another name for the subject's storage, as an `inout`
    // local is for what it names; a T written through it fits there.
    const std::string what = "'inout' arm '" + name + "'";
    if (!ast::isa<ast::Identifier>(subject) &&
        !ast::isa<ast::MemberAccessExpr>(subject) &&
        !ast::isa<ast::SubscriptExpr>(subject))
      msg = what + " needs a variable or a field to match on";
    else if (ast::isa<ast::BuiltinType>(bindTy))
      msg = what + " cannot change the '" + typeName(bindTy) +
            "' inside an optional yet";
    else
      msg = inoutPlaceError(subject, "bound by " + what);
    if (msg.empty()) {
      CurrentScope->Kinds[name] = VarKind::Inout;
      return true;
    }
    error(subject->getLocation(), msg);
  } else {
    // `n: view T` is a `view` local of the subject, whatever the subject is.
    if (arm->getMode() == ast::ParamMode::View)
      CurrentScope->Kinds[name] = VarKind::ViewLocal;
    else if (MatchSubjectIsView)
      CurrentScope->Kinds[name] = VarKind::ViewBinding;
    return true;
  }
  // Its uses in the arm's body are follow-ons of the reported error.
  declarePoisoned(CurrentScope, arm->getBinding());
  return false;
}

bool Sema::isTypeNameForVariable(const std::string &name) const {
  ast::BuiltinType::Kind kind;
  return Ctx.lookupClassType(name) || Ctx.lookupEnumType(name) ||
         Ctx.lookupStructType(name) || ClassTemplates.count(name) ||
         StructTemplates.count(name) ||
         (CurrentTypeParams && CurrentTypeParams->count(name)) ||
         name == names::kObj || name == names::kString ||
         name == names::kFile ||
         (ast::builtinKindOfName(name, kind) &&
          kind != ast::BuiltinType::Void) ||
         name == names::kStdin;
}

// Visit an expression and return its resolved type (nullptr on error).
ast::Type *Sema::resolveExprType(ast::Expr *expr) { return EC.visit(expr); }
ast::Type *Sema::resolveExprType(ast::Expr *expr, ast::Type *expected) {
  return EC.visitExpecting(expr, expected);
}

// -- ExprVisitor

bool Sema::ExprChecker::isNumericLiteral(const ast::Expr *e) {
  if (const auto *u = ast::dyn_cast<ast::UnaryExpr>(e))
    return u->getOpcode() == ast::UnaryOpcode::Neg &&
           isNumericLiteral(u->getOperand());
  return ast::isa<ast::IntegerLiteral>(e) || ast::isa<ast::FloatLiteral>(e);
}

ast::Type *Sema::ExprChecker::sizedLiteralTarget(const ast::Expr *e,
                                                 ast::Type *expected) {
  if (!expected || !isNumericLiteral(e))
    return nullptr;
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(expected))
    expected = ot->getInnerType();
  auto *bt = ast::dyn_cast<ast::BuiltinType>(expected);
  if (!bt || !bt->isNumeric() || bt->getTypeKind() == ast::BuiltinType::Int ||
      bt->getTypeKind() == ast::BuiltinType::Float)
    return nullptr;
  // A float literal is never an integer.
  const ast::Expr *lit = e;
  if (const auto *u = ast::dyn_cast<ast::UnaryExpr>(lit))
    lit = u->getOperand();
  if (bt->isInteger() && ast::isa<ast::FloatLiteral>(lit))
    return nullptr;
  return bt;
}

namespace {

/// The range of the integer kind @p k, as [lo, hi].
void integerRange(ast::BuiltinType::Kind k, int64_t &lo, int64_t &hi) {
  const unsigned bits = ast::BuiltinType::integerBits(k);
  if (ast::BuiltinType::isUnsignedKind(k)) {
    lo = 0;
    // A literal is at most INT64_MAX; `unsigned` reaches it.
    hi = bits == 64 ? INT64_MAX : static_cast<int64_t>((1ULL << bits) - 1);
  } else {
    lo = bits == 64 ? INT64_MIN : -static_cast<int64_t>(1ULL << (bits - 1));
    hi =
        bits == 64 ? INT64_MAX : static_cast<int64_t>((1ULL << (bits - 1)) - 1);
  }
}

/// The text of the range of the integer kind @p k for a diagnostic:
/// `0 to 255`, `-128 to 127`, `0 to 18446744073709551615`.
std::string integerRangeText(ast::BuiltinType::Kind k) {
  int64_t lo, hi;
  integerRange(k, lo, hi);
  std::string hiText = std::to_string(hi);
  if (k == ast::BuiltinType::U64)
    hiText = "18446744073709551615";
  return std::to_string(lo) + " to " + hiText;
}

/// The text of a float literal for a diagnostic.
std::string floatText(double value) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%g", value);
  return buf;
}

/// Whether the finite @p value, rounded to the float kind @p k, stays finite.
bool floatFits(ast::BuiltinType::Kind k, double value) {
  if (std::isinf(value) || std::isnan(value))
    return true; // `inf` and `nan` are not literals; nothing to check
  switch (k) {
  case ast::BuiltinType::F32:
    return std::fabs(value) <= 3.4028235677973366e+38; // the f32 maximum,
                                                       // plus half an ulp
  case ast::BuiltinType::F16:
    return std::fabs(value) < 65520.0; // rounds to the f16 maximum, 65504
  default:
    return true;
  }
}

} // namespace

ast::Type *Sema::ExprChecker::checkSizedLiteral(ast::Expr *node,
                                                ast::Type *target,
                                                bool isInteger,
                                                int64_t intValue,
                                                double floatValue) {
  auto *bt = ast::cast<ast::BuiltinType>(target);
  const ast::BuiltinType::Kind k = bt->getTypeKind();
  if (bt->isInteger()) {
    int64_t lo, hi;
    integerRange(k, lo, hi);
    if (intValue < lo || intValue > hi) {
      S.error(node->getLocation(), "the literal " + std::to_string(intValue) +
                                       " does not fit in '" + typeName(bt) +
                                       "' (" + integerRangeText(k) + ")");
      return nullptr;
    }
    return bt;
  }
  const double value = isInteger ? static_cast<double>(intValue) : floatValue;
  if (!floatFits(k, value)) {
    S.error(node->getLocation(), "the literal " + floatText(value) +
                                     " does not fit in '" + typeName(bt) + "'");
    return nullptr;
  }
  return bt;
}

ast::Type *Sema::ExprChecker::visitIntegerLiteral(ast::IntegerLiteral *node) {
  if (ast::Type *target = std::exchange(S.LiteralExpectation, nullptr))
    return checkSizedLiteral(node, target, true, node->getValue(), 0.0);
  return S.Ctx.getIntTy();
}

ast::Type *Sema::ExprChecker::visitFloatLiteral(ast::FloatLiteral *node) {
  if (ast::Type *target = std::exchange(S.LiteralExpectation, nullptr))
    return checkSizedLiteral(node, target, false, 0, node->getValue());
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
  if (node->getName() == names::kStdin)
    return S.Ctx.getFileTy();
  // Inside an instantiation body, a bare type parameter name is a type, not a
  // value (a local variable of the same name shadows it, as usual).
  if (S.CurrentTypeParams && S.CurrentTypeParams->count(node->getName()) &&
      !(S.CurrentScope && S.CurrentScope->lookup(node->getName()))) {
    S.error(node->getLocation(), "type parameter '" + node->getName() +
                                     "' cannot be used as a value");
    return nullptr;
  }
  // A function used as a value (`f = square;`): its function type.
  if (!S.CurrentScope->lookup(node->getName())) {
    if (const FunctionSig *sig = S.lookupFunction(node->getName())) {
      if (!S.checkFunctionValue(node->getName(), *sig, node->getLocation()))
        return nullptr;
      node->setFunctionRef(true);
      auto *fnt = S.functionTypeOf(*sig);
      node->setResolvedType(fnt);
      return fnt;
    }
    if (S.FuncTemplates.count(node->getName())) {
      S.error(node->getLocation(),
              "'" + node->getName() +
                  "' is a generic function; a generic function cannot be a "
                  "function value yet (wrap a call to it in a lambda)");
      return nullptr;
    }
  }
  S.checkNotBorrowed(node->getName(), node->getLocation());
  ast::Type *ty = S.checkIdentLive(node->getName(), node->getLocation());
  // Inside a lambda, a variable of the function it is written in is
  // captured (by copy) when it is first used.
  if (ty && !S.LambdaStack.empty() &&
      !S.captureVariable(node->getName(), ty,
                         S.CurrentScope->findOwner(node->getName()),
                         node->getLocation()))
    return nullptr;
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
  ast::Type *ret = S.checkFunctionValueCall(fnt, node->getArguments(),
                                            typeName(fnt), node->getLocation());
  if (!ret)
    return nullptr;
  node->setResolvedType(ret);
  return ret;
}

ast::Type *Sema::ExprChecker::visitEnumValueExpr(ast::EnumValueExpr *node) {
  auto *enumTy = S.Ctx.lookupEnumType(node->getEnumName());
  if (!enumTy) {
    // `m::f` or `S::f`, an imported function or a `static fn` used as a
    // value: its function type.
    const std::string qualified =
        node->getEnumName() + names::kQualSep + node->getVariantName();
    if (!S.checkStaticMemberUse(qualified, node->getLocation()))
      return nullptr;
    if (const FunctionSig *sig = S.lookupFunction(qualified)) {
      if (!S.checkFunctionValue(qualified, *sig, node->getLocation()))
        return nullptr;
      node->setFunctionRef(S.Ctx.intern(qualified));
      auto *fnt = S.functionTypeOf(*sig);
      node->setResolvedType(fnt);
      return fnt;
    }
    if (S.reportPrivateImport(qualified, node->getLocation()) ||
        S.reportPrivateImport(node->getEnumName(), node->getLocation()))
      return nullptr;
    if (!S.isFailedImportUse(node->getEnumName()))
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
  node->setResolvedEnum(enumTy);
  node->setValue(idx);
  node->setResolvedType(enumTy);
  return enumTy;
}

ast::Type *Sema::ExprChecker::visitUnaryExpr(ast::UnaryExpr *node) {
  // `-5` flowing into a sized numeric slot: the negated literal takes the
  // slot's type when it fits (`-128` fits an `i8`, `-1` no `u8`).
  if (ast::Type *target = std::exchange(S.LiteralExpectation, nullptr)) {
    ast::Expr *operand = node->getOperand();
    if (auto *il = ast::dyn_cast<ast::IntegerLiteral>(operand)) {
      // The literal's magnitude is at most INT64_MAX, so its negation fits.
      ast::Type *ty =
          checkSizedLiteral(node, target, true, -il->getValue(), 0.0);
      if (!ty)
        return nullptr;
      operand->setResolvedType(ty);
      return ty;
    }
    if (auto *fl = ast::dyn_cast<ast::FloatLiteral>(operand)) {
      ast::Type *ty =
          checkSizedLiteral(node, target, false, 0, -fl->getValue());
      if (!ty)
        return nullptr;
      operand->setResolvedType(ty);
      return ty;
    }
    // `-(-5)`: the inner negation takes the slot's type.
    S.LiteralExpectation = target;
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
    S.error(node->getLocation(), "unary '" + std::string(node->getOpcodeStr()) +
                                     "' is not defined for type '" +
                                     typeName(operandTy) + "'");
    return nullptr;
  }

  switch (node->getOpcode()) {
  case ast::UnaryOpcode::Neg:
    return operandTy;
  case ast::UnaryOpcode::Not:
    return S.Ctx.getBoolTy();
  case ast::UnaryOpcode::Count:
    break;
  }
  PAYKAN_UNREACHABLE("unknown UnaryOpcode");
}

ast::Type *Sema::ExprChecker::visitBinaryExpr(ast::BinaryExpr *node) {
  // A numeric literal operand takes the other operand's sized type (`x + 1`
  // with `x: u8` adds two `u8`), so the non-literal operand is typed first.
  ast::Type *lhsTy = nullptr;
  ast::Type *rhsTy = nullptr;
  if (isNumericLiteral(node->getLHS()) && !isNumericLiteral(node->getRHS())) {
    rhsTy = visit(node->getRHS());
    lhsTy = visitOperand(node->getLHS(), rhsTy);
  } else {
    lhsTy = visit(node->getLHS());
    rhsTy = visitOperand(node->getRHS(), lhsTy);
  }
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
      bool compatible = typesEqual(li, ri);
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

  if (!lhsTy->hasBinaryOp(node->getOpcode(), rhsTy)) {
    S.error(node->getLocation(),
            "operator '" + std::string(node->getOpcodeStr()) +
                "' is not defined for types '" + typeName(lhsTy) + "' and '" +
                typeName(rhsTy) + "'");
    return nullptr;
  }

  // Two numeric operands have one type; the one promotion is `int` with
  // `float` (docs/design/sized-types.md).
  auto *lhsBT = ast::dyn_cast<ast::BuiltinType>(lhsTy);
  auto *rhsBT = ast::dyn_cast<ast::BuiltinType>(rhsTy);
  const bool numericPair =
      lhsBT && rhsBT && lhsBT->isNumeric() && rhsBT->isNumeric();
  const bool intWithFloat =
      (lhsTy == S.Ctx.getIntTy() && rhsTy == S.Ctx.getFloatTy()) ||
      (lhsTy == S.Ctx.getFloatTy() && rhsTy == S.Ctx.getIntTy());
  if (numericPair && lhsTy != rhsTy && !intWithFloat) {
    S.error(node->getLocation(),
            "operator '" + std::string(node->getOpcodeStr()) +
                "' is not defined for types '" + typeName(lhsTy) + "' and '" +
                typeName(rhsTy) +
                "' (the operands must have one type: convert one with '" +
                typeName(lhsTy) + "(...)' or '" + typeName(rhsTy) + "(...)')");
    return nullptr;
  }

  // Equality requires compatible types.
  if ((node->getOpcode() == ast::BinaryOpcode::Eq ||
       node->getOpcode() == ast::BinaryOpcode::Ne)) {
    // Same type is always OK (types are canonical, so `(int, Str) == (int,
    // Str)` unifies while `(int, Str) == (Str, int)` stays an error).
    if (!typesEqual(lhsTy, rhsTy)) {
      // Allow class subtype comparisons (either direction).
      auto *lhsCT = ast::dyn_cast<ast::ClassType>(lhsTy);
      auto *rhsCT = ast::dyn_cast<ast::ClassType>(rhsTy);
      if (!lhsCT || !rhsCT ||
          (!lhsCT->isSubtypeOf(rhsCT) && !rhsCT->isSubtypeOf(lhsCT))) {
        S.error(node->getLocation(),
                "operands of '" + std::string(node->getOpcodeStr()) +
                    "' have mismatched types '" + typeName(lhsTy) + "' and '" +
                    typeName(rhsTy) + "'");
        return nullptr;
      }
    }
  }

  switch (node->getOpcode()) {
  // Arithmetic: result is float if either operand is float, else int.
  case ast::BinaryOpcode::Add:
    // String concatenation: String + String -> String.
    if (lhsTy == S.Ctx.getStrTy() && rhsTy == S.Ctx.getStrTy())
      return S.Ctx.getStrTy();
    [[fallthrough]];
  case ast::BinaryOpcode::Sub:
  case ast::BinaryOpcode::Mul:
  case ast::BinaryOpcode::Div:
  case ast::BinaryOpcode::Mod:
    // One numeric type gives that type; `int` with `float` gives `float`.
    if (numericPair && lhsTy == rhsTy)
      return lhsTy;
    if (lhsTy == S.Ctx.getFloatTy() || rhsTy == S.Ctx.getFloatTy())
      return S.Ctx.getFloatTy();
    return S.Ctx.getIntTy();

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
  case ast::BinaryOpcode::Count:
    break;
  }
  PAYKAN_UNREACHABLE("unknown BinaryOpcode");
}

ast::Type *Sema::ExprChecker::visitCallExpr(ast::CallExpr *node) {
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
    ast::Type *ret = S.checkFunctionValueCall(fnt, node->getArguments(), name,
                                              node->getLocation());
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
  const bool explicitInstance = node->hasTypeArgs() &&
                                !S.isConversionTarget(node->getCalleeName()) &&
                                S.isTemplate(node->getCalleeName());
  if (explicitInstance && !S.resolveGenericCall(node, {}))
    return nullptr;
  if (explicitInstance ||
      (!node->hasTypeArgs() && !S.isTemplate(node->getCalleeName())))
    if (const auto *sig = S.lookupFunction(node->getCalleeName());
        sig && sig->ParamTypes.size() == node->getNumArguments())
      known = sig;
  std::vector<ast::Type *> expectedArgs;
  if (known)
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
    auto *ty =
        visitExpecting(node->getArguments()[i],
                       i < expectedArgs.size() ? expectedArgs[i] : nullptr);
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
    auto *superInit = superClass->findMethod(names::kMethodInit);
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
      S.checkInoutArgs(superInit->getParamModes(), expectedParams,
                       node->getArguments(), names::kMethodSuper);
      S.checkViewArgs(superInit->getParamModes(), expectedParams,
                      node->getArguments(), names::kMethodSuper);
    }
    S.CurrentClassCtx->SuperInitCalled = true;
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
      (node->hasTypeArgs() || S.isTemplate(node->getCalleeName()))) {
    for (auto *ty : argTypes)
      if (!ty)
        return nullptr; // argument errors already reported; cannot infer
    if (!S.resolveGenericCall(node, argTypes))
      return nullptr;
  } else if (S.CurrentTypeParams &&
             S.CurrentTypeParams->count(node->getCalleeName())) {
    S.error(node->getLocation(),
            "type parameter '" + node->getCalleeName() +
                "' cannot be used as a value (a type parameter cannot be "
                "constructed)");
    return nullptr;
  }

  const auto *sig = S.lookupFunction(node->getCalleeName());
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
    if (S.reportPrivateImport(node->getCalleeName(), node->getLocation()))
      return nullptr;
    if (!S.isFailedImportUse(node->getCalleeName()))
      S.error(node->getLocation(), "call to undeclared function '" +
                                       displayName(node->getCalleeName()) +
                                       "'");
    return nullptr;
  }

  // Record "class A constructs class B" edges for instantiation ordering.
  if (S.CurrentClassCtx && S.CurrentClassCtx->ClassType &&
      S.Ctx.lookupClassType(node->getCalleeName()))
    S.ConstructsEdges[S.CurrentClassCtx->ClassType->getName()].insert(
        node->getCalleeName());

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

  // The print builtins print an optional too, `None` or its value, and a
  // tuple: they take one as it is, not as an `Obj` (neither is one).
  const bool printsOptionals =
      sig->IsBuiltin && (node->getCalleeName() == names::kPrint ||
                         node->getCalleeName() == names::kPrintln ||
                         node->getCalleeName() == names::kErrPrint ||
                         node->getCalleeName() == names::kErrPrintln);

  // Check argument types.
  for (size_t i = 0; i < argTypes.size(); ++i) {
    if (!argTypes[i])
      continue; // already reported
    auto *argExpr = node->getArguments()[i];
    if (printsOptionals && (ast::isa<ast::OptionalType>(argTypes[i]) ||
                            ast::isa<ast::TupleType>(argTypes[i])))
      continue;
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
  const std::string callee = displayName(node->getCalleeName());
  S.checkInoutArgs(sig->Modes, sig->ParamTypes, node->getArguments(), callee);
  S.checkViewArgs(sig->Modes, sig->ParamTypes, node->getArguments(), callee);
  S.noteBorrowResult(node, callee, sig->Modes, node->getArguments(), nullptr);

  node->setResolvedType(sig->ReturnType);
  return sig->ReturnType;
}

ast::Type *Sema::ExprChecker::visitMethodCallExpr(ast::MethodCallExpr *node) {
  // Resolve receiver type.
  auto *recvTy = visit(node->getReceiver());
  if (!recvTy)
    return nullptr;

  // A method cannot be called on an optional receiver (it may be None).
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(recvTy)) {
    S.errorOptionalUnwrap(node->getLocation(), ot);
    return nullptr;
  }

  // A fixed-size array, and a slice of one, has one method, `len()`, its
  // length.
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
    node->setResolvedType(S.Ctx.getIntTy());
    return S.Ctx.getIntTy();
  }

  // Resolve the ClassType, or the StructType, to look up the method on.
  ast::ClassType *ct = ast::dyn_cast<ast::ClassType>(recvTy);
  ast::StructType *st = ast::dyn_cast<ast::StructType>(recvTy);

  // `n.toString()` on an int, float, bool or char, or on a tuple or a
  // struct: the builtin `Show`; `n.hash()` on an int, bool or char, and
  // `s.hash()` on a Str: the builtin `Hashable`.
  const std::string &mname = node->getMethodName();
  ast::Type *builtinResult = nullptr;
  if (!ct &&
      ((ast::isa<ast::BuiltinType>(recvTy) && recvTy != S.Ctx.getVoidTy()) ||
       ast::isa<ast::TupleType>(recvTy)) &&
      mname == names::kMethodToString)
    builtinResult = S.Ctx.getStrTy();
  else if (mname == names::kMethodHash &&
           ((ast::isa<ast::BuiltinType>(recvTy) &&
             (ast::cast<ast::BuiltinType>(recvTy)->isInteger() ||
              recvTy == S.Ctx.getBoolTy() || recvTy == S.Ctx.getCharTy())) ||
            ct == S.Ctx.getStrTy()))
    builtinResult = S.Ctx.getIntTy();
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

  // A struct's methods are those of its `impl` blocks, called directly.
  ast::MethodDecl *method = ct ? ct->findMethod(node->getMethodName())
                               : st->findMethod(node->getMethodName());
  std::string ownerName = ct ? ct->getName() : st->getName();

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
    if (fnt) {
      if (!S.checkMemberAccess(ct ? static_cast<ast::Type *>(
                                        S.fieldOwner(ct, node->getMethodName()))
                                  : static_cast<ast::Type *>(st),
                               node->getMethodName(), node->getLocation()))
        return nullptr;
      ast::Type *ret =
          S.checkFunctionValueCall(fnt, node->getArguments(),
                                   node->getMethodName(), node->getLocation());
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
    S.error(node->getLocation(), "no method '" + node->getMethodName() +
                                     "' on type '" + ownerName + "'");
    return nullptr;
  }

  // A private method is its type's own.
  if (!S.checkMemberAccess(ct ? static_cast<ast::Type *>(
                                    S.declaringClass(ct, node->getMethodName()))
                              : static_cast<ast::Type *>(st),
                           node->getMethodName(), node->getLocation()))
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

  // Only a `view fn` may be called on a `view`.
  S.checkMethodReceiver(node, method);

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
    auto *argTy = visitExpecting(argExpr, paramTys[i]);
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
  S.checkInoutArgs(method->getParamModes(), paramTys, node->getArguments(),
                   node->getMethodName(), node->getReceiver(), method);
  // A builtin method reads its arguments; `equals`, like `==`, only
  // compares.
  if (!(ct && ct->isBuiltin()) && node->getMethodName() != names::kMethodEquals)
    S.checkViewArgs(method->getParamModes(), method->getParamTypes(),
                    node->getArguments(), node->getMethodName());
  S.noteBorrowResult(node, node->getMethodName(), method->getParamModes(),
                     node->getArguments(), node->getReceiver());
  // An `inout` result is part of the receiver, which must outlive the call.
  if (method->getParamModes().Result == ast::ParamMode::Inout &&
      !S.placeRoot(node->getReceiver()) &&
      !S.isInoutResult(S.placeBase(node->getReceiver())))
    S.error(node->getReceiver()->getLocation(),
            "the receiver of '" + node->getMethodName() +
                "' must be a variable or a field: its 'inout' result is part "
                "of it");

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
                                       "' in struct '" + st->getName() + "'");
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
        node, S.Ctx.getFixedArrayType(slice->getElementType(), n));

  // Anywhere else, an array of its own length of its elements' common type.
  std::vector<ast::Type *> elemTys;
  elemTys.reserve(node->getNumElements());
  for (ast::Expr *elem : node->getElements()) {
    auto *ty = visit(elem);
    if (!ty)
      return nullptr;
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
  auto *fixed = S.Ctx.getFixedArrayType(elemTy, n);
  if (fixedElementBytes(fixed) < 0) {
    S.error(node->getLocation(),
            "array literal of type '" + typeName(fixed) +
                "' is too big: a fixed-size array holds at most " +
                std::to_string(kMaxFixedArrayElements) + " elements");
    return nullptr;
  }
  // Each element takes the common type (`[1, 2.5]` is a `float[2]`).
  for (size_t i = 0; i < elemTys.size(); ++i)
    S.checkAssignable(elemTy, elemTys[i], node->getElements()[i]);
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
    fixed = S.Ctx.getFixedArrayType(fixed->getElementType(), node->getLength());
  }
  ast::Type *elemTy = fixed->getElementType();
  if (node->getLength() != fixed->getLength()) {
    S.error(node->getLocation(),
            "array literal has " + std::to_string(node->getLength()) +
                " element(s), but '" + typeName(fixed) + "' has " +
                std::to_string(fixed->getLength()));
    return nullptr;
  }
  bool ok = true;
  for (ast::Expr *elem : node->getElements()) {
    auto *ty = visitExpecting(elem, elemTy);
    if (!ty) {
      ok = false;
      continue;
    }
    if (hasInferredLength(elemTy)) {
      ast::Type *filled = S.fillLengths(elemTy, ty);
      if (!filled) {
        S.errorNoLengths(elem->getLocation(), elemTy, ty);
        return nullptr;
      }
      elemTy = filled;
      fixed = S.Ctx.getFixedArrayType(elemTy, fixed->getLength());
    }
    if (!S.checkAssignable(elemTy, ty, elem)) {
      S.error(elem->getLocation(),
              "array literal element of type '" + typeName(ty) +
                  "' does not match the element type '" + typeName(elemTy) +
                  "' of '" + typeName(fixed) + "'");
      ok = false;
    }
  }
  if (!ok)
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
  auto *arrayTy = visit(node->getArray());
  if (!arrayTy)
    return nullptr;

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
  auto *condTy = visit(node->getCondition());
  auto *trueTy = visit(node->getTrueExpr());
  auto *falseTy = visit(node->getFalseExpr());
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
      // `x` a `T?` already, a reference `T`, or a primitive (`int` -> `int?`).
      if (ast::isa<ast::OptionalType>(valueTy))
        result = valueTy;
      else if (ast::isRefType(valueTy) ||
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
  elemTys.reserve(node->getNumElements());
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
    if (holdsFixedArray(ty)) {
      S.errorFixedArrayHere(elem->getLocation(), ty,
                            "tuple element " + std::to_string(i));
      ok = false;
      continue;
    }
    elemTys.push_back(ty);
  }
  if (!ok)
    return nullptr;
  auto *tt = S.Ctx.getTupleType(std::move(elemTys));
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
      if (target.DeclType) {
        if (CurrentScope->contains(name))
          continue; // a redeclaration: the existing binding stands
        // An annotation that does not resolve is a separate error of its own.
        if (auto *declTy = resolveType(target.DeclType, target.Loc,
                                       "destructuring target '" + name + "'"))
          CurrentScope->declare(name, declTy);
        else
          declarePoisoned(CurrentScope, name);
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

  // Each element is stored in its target.
  bool ok = true;
  for (size_t i = 0; i < tt->getArity() && ok; ++i)
    ok = checkViewNotStored(node->getValue(), tt->getElementType(i));
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
    if (!checkBinderName(name, target.Loc)) {
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
      continue;
    }

    // Bare name: re-assignment if visible, implicit declaration otherwise.
    auto *owner = CurrentScope->findOwner(name);
    if (!owner) {
      CurrentScope->set(name, elemTy);
      continue;
    }
    if (!checkReassignable(name, target.Loc)) {
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
    }
  }
  return ok;
}

// -- Entry point

SemaContext Sema::run(ast::TranslationUnit *tu) {
  CurrentScope = nullptr;
  AccumulatedImportContexts.clear();

  // Bootstrap builtin functions — the print family takes a single Obj
  // (printed via toString), or an optional (`None` or its value, see
  // visitCallExpr); compose multiple pieces with `+`.  Paykan has no
  // variadic functions or overloading, so these are ordinary one-arg builtins.
  declareFunction(names::kPrint, Ctx.getVoidTy(), {Ctx.getObjTy()},
                  /*isBuiltin=*/true);
  declareFunction(names::kPrintln, Ctx.getVoidTy(), {Ctx.getObjTy()},
                  /*isBuiltin=*/true);
  declareFunction(names::kErrPrint, Ctx.getVoidTy(), {Ctx.getObjTy()},
                  /*isBuiltin=*/true);
  declareFunction(names::kErrPrintln, Ctx.getVoidTy(), {Ctx.getObjTy()},
                  /*isBuiltin=*/true);

  // Register type-conversion builtins (take unique builtin types — no ownership
  // check needed).
  auto *StrTy = Ctx.getStrTy();
  // The conversions (`Str<int>(n)`, `int<Str>(s)`, ...) are not functions:
  // checkConversion handles them (#64).
  declareFunction(names::kString, StrTy, {StrTy}, true);
  declareFunction(names::kOpen, Ctx.getObjTy(), {StrTy, StrTy}, true);
  // They only read their arguments, which are `view` parameters, so a `view`
  // can be printed (checkViewArgs).  Not `Str(s)`, which returns `s` itself.
  for (const char *fn : {names::kPrint, names::kPrintln, names::kErrPrint,
                         names::kErrPrintln, names::kOpen}) {
    FunctionSig &sig = FunctionTable[fn];
    sig.Modes.Modes.assign(sig.ParamTypes.size(), ast::ParamMode::View);
    sig.Modes.Names.assign(sig.ParamTypes.size(), "value");
  }

  registerBuiltinTraits();

  // Process imports before local declarations.
  StringSet localImportStack, localFailedModules;
  if (!ImportStack)
    ImportStack = &localImportStack;
  if (!FailedModules)
    FailedModules = &localFailedModules;
  for (auto *imp : tu->getImports())
    processImport(imp);

  visit(tu);
  // A binder is poisoned only after its declaration's error was reported, so
  // a program without errors has none, and the poison type never reaches the
  // lowering.  Guard that invariant: never hand such a program on.
  if (PoisonedBindings && !hasErrors()) {
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
                     std::move(ForeignInstantiatedStructDecls)};
  // The sets live in this frame: a Sema is run once.
  if (ImportStack == &localImportStack)
    ImportStack = nullptr;
  if (FailedModules == &localFailedModules)
    FailedModules = nullptr;
  return result;
}

// -- Conversion constructors (#64, #88)
//
// `Target<Source>(value)` converts between the primitives and `Str`.  Each
// builtin target has a fixed, closed set of specializations (the valid
// sources), and no other type, builtin or user-defined, can be one. The source
// is written (`Str<int>(n)`, checkConversion) or inferred from the argument's
// exact type (`Str(n)`, inferConversion).  The semantics live in the lowering
// (Lowering/LoweringExpr.cpp, emitConversion and the runtime table), which both
// backends inherit.

struct ConversionPair {
  std::string Target;
  std::string Source;
  std::string Spelled; // the rebound callee the lowering dispatches on
  bool Parses;         // a parse of a Str: the result is the optional target
};

namespace {

/// The conversions, grouped by target, each group in the order its
/// diagnostics list it: `Str` from every scalar; `int`, `float` and the
/// sized numeric types from a `Str` (a parse) and from every other numeric
/// type; `bool` and `char` from `int` only (docs/design/sized-types.md).
const std::vector<ConversionPair> &conversions() {
  static const std::vector<ConversionPair> kTable = [] {
    using ast::BuiltinType;
    std::vector<ConversionPair> t;
    auto add = [&](const std::string &target, const std::string &source,
                   bool parses) {
      t.push_back({target, source, target + "<" + source + ">", parses});
    };
    // The numeric types, in the order the diagnostics list them.
    std::vector<std::string> numerics;
    for (BuiltinType::Kind k :
         {BuiltinType::Int, BuiltinType::Float, BuiltinType::I8,
          BuiltinType::I16, BuiltinType::I32, BuiltinType::U8, BuiltinType::U16,
          BuiltinType::U32, BuiltinType::U64, BuiltinType::F16,
          BuiltinType::F32})
      numerics.push_back(ast::builtinKindName(k));
    add(names::kString, names::kTypeInt, false);
    add(names::kString, names::kTypeFloat, false);
    add(names::kString, names::kTypeBool, false);
    add(names::kString, names::kTypeChar, false);
    for (size_t i = 2; i < numerics.size(); ++i)
      add(names::kString, numerics[i], false);
    for (const std::string &target : numerics) {
      add(target, names::kString, true);
      for (const std::string &source : numerics)
        if (source != target)
          add(target, source, false);
      // `int` also converts the two non-numeric scalars, as before the sized
      // types (so its list reads `Str, float, bool, char, i8, ...`).
      if (target == names::kTypeInt) {
        ConversionPair boolPair{target, names::kTypeBool,
                                target + "<" + names::kTypeBool + ">", false};
        ConversionPair charPair{target, names::kTypeChar,
                                target + "<" + names::kTypeChar + ">", false};
        auto at = t.end() - static_cast<long>(numerics.size() - 2);
        t.insert(at, {boolPair, charPair});
      }
    }
    add(names::kTypeBool, names::kTypeInt, false);
    add(names::kTypeBool, names::kString, true);
    add(names::kTypeChar, names::kTypeInt, false);
    return t;
  }();
  return kTable;
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

/// The no-match error, for the explicit and the inferred form alike.
std::string noSpecializationMessage(const std::string &target,
                                    const std::string &source) {
  return "no specialization of '" + target + "' for '" + source +
         "'; its specializations are " + specializationList(target);
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
  // A `Str`, or a `Str?` (reported by the constructor as an unwrap).
  ast::Type *inner = argTypes[0];
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(inner))
    inner = ot->getInnerType();
  return typesEqual(inner, Ctx.getStrTy());
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
  if (!pair) {
    error(node->getLocation(), noSpecializationMessage(target, sourceName));
    return nullptr;
  }
  return applyConversion(node, pair);
}

ast::Type *Sema::applyConversion(ast::CallExpr *node,
                                 const ConversionPair *pair) {
  // The result is the target type; a parse (`int<Str>`, `bool<Str>`, ...) can
  // fail, so it returns the optional (None for an invalid string).
  ast::Type *result = Ctx.lookupType(pair->Target);
  if (pair->Parses)
    result = Ctx.getOptionalType(result);
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

  // The specializations are a closed set: any other type argument, a user
  // class included, is an error (`toString()` stringifies an object).
  const ConversionPair *pair = findConversion(target, sourceName);
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

  return applyConversion(node, pair);
}

// -- Top-level

bool Sema::visitTranslationUnit(ast::TranslationUnit *node) {
  bool ok = rejectFreeViewFns(node);
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

  if (!checkEntryPoint(node, declaredFns))
    ok = false;

  // Now check class and struct method bodies (they can resolve free
  // functions) …
  if (!node->getClassDecls().empty())
    if (!checkClassBodies())
      ok = false;
  if (!checkStructBodies())
    ok = false;

  // … and finally free-function bodies (signatures all registered above).
  for (auto *fn : declaredFns)
    if (!visitFuncDecl(fn))
      ok = false;

  // Bodies of the instantiations requested so far (and of any they request
  // in turn): checked last, like C++ instantiates at the end of the TU.
  if (!checkPendingInstantiations())
    ok = false;

  // Hand the instantiations to the lowering as ordinary declarations.
  injectInstantiations(node);

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
      error(fn->getLocation(), "'main' cannot be generic; the program's entry "
                               "point is " +
                                   expected);
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
    if (sig->ReturnType == Ctx.getIntTy() && params)
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
  for (const auto *variant : node->getVariants()) {
    if (!seen.insert(*variant).second) {
      error(node->getLocation(), "duplicate variant '" + *variant +
                                     "' in enum '" + node->getName() + "'");
      ok = false;
      continue;
    }
    enumTy->addVariant(*variant);
  }
  return ok;
}

// -- Statements

bool Sema::visitCompoundStmt(ast::CompoundStmt *node) {
  ScopeGuard guard(*this);
  return visitStatements(node->getStatements());
}

bool Sema::visitDeclStmt(ast::DeclStmt *node) { return visit(node->getDecl()); }

bool Sema::visitExprStmt(ast::ExprStmt *node) {
  // Type-check the expression; we discard the type.
  return resolveExprType(node->getExpr()) != nullptr;
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
  case VarKind::Captured:
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
  case VarKind::Plain:
  case VarKind::Inout:
    break;
  }
  return true;
}

bool Sema::visitAssignStmt(ast::AssignStmt *node) {
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
  auto *valTy = resolveExprType(node->getValue(), expected);
  const auto &varName = node->getVarName();
  if (!valTy) {
    // The value's error was reported.  A first assignment still declares the
    // variable -- poisoned -- so its uses are not reported as undeclared; an
    // existing variable keeps its type.
    if (!isTypeNameForVariable(varName) && !CurrentScope->findOwner(varName))
      declarePoisoned(CurrentScope, varName);
    return false;
  }

  // Guard: the target name must not shadow a type or reserved name.
  if (!checkBinderName(varName, node->getLocation()))
    return false;
  // `self` is the method's object, or a struct method's receiver: it names
  // what the method was called on, and cannot name something else.
  if (varName == names::kSelf && CurrentClassCtx &&
      !CurrentClassCtx->MethodName.empty()) {
    error(node->getLocation(),
          std::string("cannot assign to 'self'") +
              (CurrentClassCtx->Struct ? ": assign its fields instead" : ""));
    return false;
  }
  // The variable is still declared after this error.
  const bool viewOk = checkViewNotStored(node->getValue(), valTy);

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
    CurrentScope->set(varName, valTy);
    return viewOk;
  }

  if (!checkReassignable(varName, node->getLocation()))
    return false;
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

  return viewOk;
}

bool Sema::visitReturnStmt(ast::ReturnStmt *node) {
  if (node->getReturnValue()) {
    auto *valTy = resolveExprType(node->getReturnValue(), CurrentReturnType);
    if (!valTy)
      return false;
    if (CurrentReturnType &&
        !checkAssignable(CurrentReturnType, valTy, node->getReturnValue())) {
      if (!diagnoseOptionalNarrowing(node->getLocation(), CurrentReturnType,
                                     valTy))
        error(node->getLocation(),
              "return value of type '" + typeName(valTy) +
                  "' does not match function return type '" +
                  typeName(CurrentReturnType) + "'");
      return false;
    }
    // A borrowed result is a borrow of what the function was given.
    if (CurrentResult.Mode != ast::ParamMode::Value) {
      std::string why = borrowedResultError(node->getReturnValue());
      // The caller writes the storage as the result's type, so no
      // conversion can happen (not a subclass to its base).
      if (why.empty() && CurrentResult.Mode == ast::ParamMode::Inout &&
          !typesEqual(valTy, CurrentReturnType))
        why = "an 'inout' result has type '" + typeName(CurrentReturnType) +
              "', but what it returns has type '" + typeName(valTy) + "'";
      if (why.empty())
        return true;
      error(node->getReturnValue()->getLocation(), why);
      return false;
    }
    return checkViewNotReturned(node->getReturnValue(), valTy);
  }
  // void return
  if (CurrentReturnType && CurrentReturnType != Ctx.getVoidTy()) {
    error(node->getLocation(), "non-void function must return a value");
    return false;
  }
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
  // Type-check the then branch.
  if (!visit(node->getThenBranch()))
    ok = false;
  // Type-check the else branch (if present).
  if (node->hasElse() && !visit(node->getElseBranch()))
    ok = false;
  return ok;
}

bool Sema::visitWhileStmt(ast::WhileStmt *node) {
  // Type-check the condition — must be bool.
  auto *condTy = resolveExprType(node->getCondition());
  if (!condTy)
    return false;
  if (condTy != Ctx.getBoolTy()) {
    error(node->getCondition()->getLocation(),
          "while condition must be 'bool', got '" + typeName(condTy) + "'");
    return false;
  }

  // Type-check the body inside a loop context.
  ++LoopDepth;
  bool ok = visit(node->getBody());
  --LoopDepth;
  return ok;
}

bool Sema::visitBreakStmt(ast::BreakStmt *node) {
  if (LoopDepth == 0) {
    error(node->getLocation(), "'break' outside of a loop");
    return false;
  }
  return true;
}

bool Sema::visitContinueStmt(ast::ContinueStmt *node) {
  if (LoopDepth == 0) {
    error(node->getLocation(), "'continue' outside of a loop");
    return false;
  }
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

bool detail::stmtAlwaysReturns(ast::Stmt *s) {
  if (ast::isa<ast::ReturnStmt>(s))
    return true;
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
  ast::Type *retTy = Ctx.getVoidTy();
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
    node->setReturnType(retTy);
  }

  // Resolve parameter types.
  std::vector<ast::Type *> paramTypes;
  for (auto &p : node->getMutableParams()) {
    if (!checkBinderName(p.getName(), node->getLocation())) {
      ErroneousNames.insert(node->getName()); // as for the return type
      return false;
    }
    FixedArrayAllowed = true; // a parameter may be a fixed-size array
    SliceAllowed = p.Mode != ast::ParamMode::Value; // or a slice
    auto *ty = resolveType(p.ParamType, node->getLocation(),
                           "parameter '" + p.getName() + "'");
    if (!ty) {
      ErroneousNames.insert(node->getName()); // as for the return type
      return false;
    }
    p.ParamType = ty;
    paramTypes.push_back(ty);
  }

  // A public function cannot hand an importer a type it could not name.
  if (!isModulePrivateName(node->getName()) &&
      node->getName().find('<') == std::string::npos) {
    const std::string who =
        "'" + displayName(node->getName()) + "' is public but ";
    bool pub = checkPublicSurface(who + "its result", "function", retTy,
                                  node->getLocation());
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

  // Register the function in the function table.
  declareFunction(node->getName(), retTy, paramTypes);
  ast::ParamModes &modes = FunctionTable[node->getName()].Modes;
  modes = ast::paramModes(node->getParams());
  modes.Result = node->getResultMode();
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
  auto scalarOrRef = [&](ast::Type *t) {
    if (t == Ctx.getStrTy() || t == Ctx.getObjTy())
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
              "' cannot cross into C (int, float, bool, char, Str and Obj "
              "can, Str? and Obj? as a result, and any type where a generic "
              "native function says 'T')");
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
  return ok;
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
    if (p.Mode != ast::ParamMode::Value ||
        (ast::isa<ast::BuiltinType>(types[i]) && types[i] != Ctx.getVoidTy()))
      continue;
    const std::string ty = typeName(types[i]);
    error(node->getLocation(),
          "native function '" + node->getName() + "': parameter '" +
              p.getName() + "' must be 'view' or 'inout' ('" + p.getName() +
              ": view " + ty +
              "'); only int, float, bool and char cross into C by value");
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
  // declareFunctionSignature already wrote the canonical types back).
  ast::Type *retTy = Ctx.getVoidTy();
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
    for (size_t i = 0; i < node->getParams().size(); ++i)
      CurrentScope->declare(node->getParams()[i].getName(), paramTypes[i]);
    declareParamKinds(node);
    bool ok = visitBody(node->getBody()->getStatements());
    CurrentReturnType = savedRetTy;
    CurrentResult = savedResult;
    if (!ok)
      return false;
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

bool Sema::visitVarDecl(ast::VarDecl *node) {
  // The name must not shadow a type or reserved name (#126).
  if (!checkBinderName(node->getName(), node->getLocation()))
    return false;

  // Check for duplicate declaration in the current scope.
  if (CurrentScope->contains(node->getName())) {
    error(node->getLocation(),
          "redeclaration of variable '" + node->getName() + "'");
    return false;
  }
  if (node->isLet())
    CurrentScope->Kinds[node->getName()] = VarKind::Let;

  // Resolve the declared type.
  ast::Type *declTy = nullptr;

  if (node->getType()) {
    FixedArrayAllowed = true; // a local may be a fixed-size array
    // and a length may be `_` where an initializer gives it
    InferredLengthAllowed = node->getInitExpr() != nullptr;
    declTy = resolveType(node->getType(), node->getLocation(),
                         "variable '" + node->getName() + "'");
    if (!declTy) {
      declarePoisoned(CurrentScope, node->getName());
      return false;
    }
    node->setType(declTy); // canonical write-back (see VarDecl::setType)
  }

  // Check the initializer type.
  bool viewOk = true;
  if (node->getInitExpr()) {
    unsigned errorsBefore = Diags.getErrorCount() + SuppressedFollowOns;
    auto *initTy = resolveExprType(node->getInitExpr(), declTy);
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
    // view), and what it reads later is a fresh reference to the object.
    if (node->getMode() == ast::ParamMode::Value && !node->isWeak())
      viewOk = checkViewNotStored(node->getInitExpr(), initTy);
    if (declTy) {
      if (!checkAssignable(declTy, initTy, node->getInitExpr())) {
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
  // (`weak y = x;` with `x: T` or `x: T?` infers `T?`).
  if (node->isWeak()) {
    if (!node->getType() && !ast::isa<ast::OptionalType>(declTy) &&
        ast::isa<ast::ClassType>(declTy))
      declTy = Ctx.getOptionalType(declTy);
    if (!checkWeakType(declTy, node->getLocation(),
                       "variable '" + node->getName() + "'")) {
      declarePoisoned(CurrentScope, node->getName());
      return false;
    }
    CurrentScope->Kinds[node->getName()] = VarKind::Weak;
  }

  // Register the variable in the current scope.  An inferred type (`let x =
  // e;`) is written back like an annotation's, for the lowering.
  if (!node->getType())
    node->setType(declTy);
  CurrentScope->declare(node->getName(), declTy);
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

bool Sema::visitMemberAssignStmt(ast::MemberAssignStmt *node) {
  auto *recvTy = resolveExprType(node->getReceiver());
  if (!recvTy)
    return false;

  // A field cannot be assigned through an optional receiver (it may be None).
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(recvTy)) {
    errorOptionalUnwrap(node->getLocation(), ot);
    return false;
  }

  ast::Type *fieldTy = nullptr;
  if (auto *st = ast::dyn_cast<ast::StructType>(recvTy)) {
    // A struct's fields can be assigned, in place, where the struct is
    // changeable: not through a `view`, and not in a `let` local, whose
    // fields are part of its value.
    fieldTy = st->findField(node->getFieldName());
    if (!fieldTy) {
      if (ErroneousClasses.count(st->getName()))
        ++SuppressedFollowOns; // the struct declaration was already reported
      else
        error(node->getLocation(), "no field '" + node->getFieldName() +
                                       "' in struct '" + st->getName() + "'");
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

  if (!checkChangeable(node->getReceiver(),
                       "cannot assign to its field '" + node->getFieldName() +
                           "'",
                       node->getLocation()))
    return false;

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

  return checkViewNotStored(node->getValue(), valTy);
}

bool Sema::visitSubscriptAssignStmt(ast::SubscriptAssignStmt *node) {
  auto *arrTy = resolveExprType(node->getArray());
  if (!arrTy)
    return false;
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
          "array index must be int, got '" + typeName(idxTy) + "'");
  } else if (idxTy && fixed && !checkFixedIndex(fixed, node->getIndex())) {
    return false;
  }
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
  return checkViewNotStored(node->getValue(), valTy);
}

bool Sema::visitImportDecl(ast::ImportDecl *) {
  // Import processing happens in run() before visiting the TU.
  return true;
}

// Value-mode match: the subject is a primitive (int/float/bool/char) or Str.
// Every non-wildcard arm must be a literal whose type matches the subject.
bool Sema::checkValueMatch(ast::MatchStmt *node, ast::Type *subjectTy) {
  bool ok = true;
  bool seenWildcard = false;

  for (ast::MatchArm *arm : node->getArms()) {
    if (seenWildcard) {
      error(arm->getLocation(),
            "unreachable arm: wildcard '_' must be the last arm");
      ok = false;
      continue;
    }

    ScopeGuard armGuard(*this);

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
      // A numeric literal takes the subject's sized type when it fits.
      auto *litTy = resolveExprType(arm->getLiteralPattern(), subjectTy);
      if (litTy && !typesEqual(litTy, subjectTy)) {
        error(arm->getLocation(), "match arm literal of type '" +
                                      typeName(litTy) +
                                      "' does not match subject type '" +
                                      typeName(subjectTy) + "'");
        ok = false;
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

    if (arm->isWildcard()) {
      seenWildcard = true;
    } else if (arm->isLiteral()) {
      error(arm->getLocation(),
            "match on enum '" + subjectTy->getName() +
                "' requires bare variant names, not literal patterns");
      ok = false;
    } else {
      // The arm's type-name stub carries the variant name.
      if (arm->hasBinding()) {
        error(arm->getLocation(), "enum-match arm cannot bind a variable");
        declarePoisoned(CurrentScope, arm->getBinding());
        ok = false;
      }
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
    }

    if (!visitStatements(arm->getBody()->getStatements()))
      ok = false;
  }

  return ok;
}

bool Sema::visitMatchStmt(ast::MatchStmt *node) {
  auto *subjectTy = resolveExprType(node->getSubject());
  if (!subjectTy)
    return false;
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
        }

        // 4. Declare the binding variable with the narrowed (arm) type.
        if (arm->hasBinding() && !bindArmName(arm, node->getSubject(), armCt)) {
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
          bindTy = armCt;
        }
      } else if (ast::isa<ast::BuiltinType>(inner) ||
                 ast::isa<ast::FunctionType>(inner)) {
        // `int?`: the `int` arm unwraps the present value, a plain `int`;
        // likewise the function type of a `((int) -> int)?`.
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
      // An arm in error still binds its name in the body, poisoned.
      if (arm->hasBinding() && !bindTy)
        declarePoisoned(CurrentScope, arm->getBinding());
      if (arm->hasBinding() && bindTy &&
          !bindArmName(arm, node->getSubject(), bindTy)) {
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
