// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "Sema.h"
#include "Names.h"
#include "SemaInternal.h"

#include <llvm/ADT/SmallPtrSet.h>

namespace paykan {
namespace sema {

// Static module cache.
llvm::StringMap<Sema::ModuleInfo> Sema::ModuleCache;

// -- Scope / ScopeGuard ------------------------------------------------------

Sema::Scope::Scope(Scope *parent) : Parent(parent) {}

ast::Type *Sema::Scope::lookup(llvm::StringRef name) const {
  auto it = Locals.find(name);
  if (it != Locals.end())
    return it->second;
  return Parent ? Parent->lookup(name) : nullptr;
}

bool Sema::Scope::declare(llvm::StringRef name, ast::Type *ty) {
  if (!Locals.try_emplace(name, ty).second)
    return false;
  return true;
}

void Sema::Scope::set(llvm::StringRef name, ast::Type *ty) {
  Locals[name] = ty;
}

bool Sema::Scope::contains(llvm::StringRef name) const {
  return Locals.count(name);
}

Sema::Scope *Sema::Scope::findOwner(llvm::StringRef name) {
  if (Locals.count(name))
    return this;
  return Parent ? Parent->findOwner(name) : nullptr;
}

void Sema::Scope::markMoved(llvm::StringRef name) {
  if (auto *owner = findOwner(name))
    owner->Moved.insert(name);
}

void Sema::Scope::clearMoved(llvm::StringRef name) {
  if (auto *owner = findOwner(name))
    owner->Moved.erase(name);
}

bool Sema::Scope::isMoved(llvm::StringRef name) const {
  if (Locals.count(name))
    return Moved.count(name) != 0;
  return Parent ? Parent->isMoved(name) : false;
}

Sema::ScopeGuard::ScopeGuard(Sema &s) : S(s), ScopeObj(s.CurrentScope) {
  S.CurrentScope = &ScopeObj;
}

Sema::ScopeGuard::~ScopeGuard() { S.CurrentScope = ScopeObj.Parent; }

// -- Flow-sensitive move tracking (see Sema.h for the rule) ------------------

Sema::MovedState Sema::saveMovedState() const {
  MovedState st;
  for (Scope *s = CurrentScope; s; s = s->Parent)
    st.emplace_back(s, s->Moved);
  return st;
}

void Sema::restoreMovedState(const MovedState &st) {
  for (auto &[scope, moved] : st)
    scope->Moved = moved;
}

void Sema::unionMovedState(MovedState &dst, const MovedState &src) {
  assert(dst.size() == src.size() && "snapshots must cover the same chain");
  for (size_t i = 0; i < dst.size(); ++i) {
    assert(dst[i].first == src[i].first && "scope chains diverged");
    for (const auto &name : src[i].second)
      dst[i].second.insert(name.getKey());
  }
}

Sema::MovedBranchMerger::MovedBranchMerger(Sema &s)
    : S(s), Entry(s.saveMovedState()) {}

void Sema::MovedBranchMerger::beginBranch() { S.restoreMovedState(Entry); }

void Sema::MovedBranchMerger::endBranch() {
  MovedState exit = S.saveMovedState();
  // Scopes opened inside the branch and still alive here (a match arm's own
  // scope wraps the endBranch call) sit innermost-first at the head of the
  // snapshot.  Drop them: their locals die with the branch, so their
  // moved-state cannot escape, and the merge must align with Entry's chain.
  assert(exit.size() >= Entry.size() && "branch closed scopes it did not open");
  exit.erase(exit.begin(), exit.begin() + (exit.size() - Entry.size()));
  if (!AnyBranch) {
    Merged = std::move(exit);
    AnyBranch = true;
  } else {
    unionMovedState(Merged, exit);
  }
}

void Sema::MovedBranchMerger::finish(bool coversAllPaths) {
  // When no branch is guaranteed to run, the entry state survives as the
  // implicit skip path: a re-assignment inside the branches cannot revive a
  // previously-moved name, but new moves inside them still count.
  if (!AnyBranch)
    Merged = Entry;
  else if (!coversAllPaths)
    unionMovedState(Merged, Entry);
  S.restoreMovedState(Merged);
}

// -- Helpers -----------------------------------------------------------------

Sema::Sema(ast::ASTContext &ctx, DiagEngine &diags,
           const std::string &projectRoot)
    : Diags(diags), Ctx(ctx), ProjectRoot(projectRoot) {}

void Sema::declareFunction(llvm::StringRef name, ast::Type *retTy,
                           std::vector<ast::Type *> paramTys, bool isBuiltin) {
  FunctionTable[name] = {retTy, std::move(paramTys), isBuiltin};
}

const Sema::FunctionSig *Sema::lookupFunction(llvm::StringRef name) const {
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
  // Generic templates share the namespace: a class name is also its
  // constructor, and a generic function is called like any other.
  if (ClassTemplates.count(name)) {
    error(loc, "'" + name + "' is already declared as a generic class");
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
  // and where it was requested, innermost first, like a C++ compiler.
  for (auto it = InstantiationStack.rbegin(); it != InstantiationStack.rend();
       ++it)
    Diags.note(it->RequestLoc,
               "in instantiation of '" + it->Name + "' requested here");
}

void Sema::warning(ast::SourceLocation loc, const std::string &msg) {
  Diags.warning(loc, msg);
}

void Sema::note(ast::SourceLocation loc, const std::string &msg) {
  Diags.note(loc, msg);
}

std::string Sema::typeName(ast::Type *ty) {
  // Thin forwarder kept for the many diagnostic call sites; the single shared
  // implementation lives with the type definitions (ASTContext.cpp).
  return ast::typeName(ty);
}

// Type equality.  Every type Sema compares is canonical -- builtin singletons,
// registered ClassType/EnumType instances, and ArrayTypes interned per element
// type by ASTContext::getArrayType -- so pointer identity is the whole test.
// The structural fallback for arrays guards against a parser-emitted
// (source-located) ArrayType that reaches a comparison without having been
// resolved; it never fires for resolved types.
bool Sema::typesEqual(ast::Type *a, ast::Type *b) {
  if (a == b)
    return true;
  if (!a || !b)
    return false;
  if (a->getKind() != b->getKind())
    return false;
  if (auto *aa = ast::dyn_cast<ast::ArrayType>(a))
    return typesEqual(aa->getElementType(),
                      ast::cast<ast::ArrayType>(b)->getElementType());
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

  // int -> float promotion.
  if (dst == Ctx.getFloatTy() && src == Ctx.getIntTy())
    return true;

  // Optional destinations (prototype, issue #5):
  //   T  -> T?   implicit widening (a present value);
  //   S? -> T?   when S -> T (covariant in the wrapped type).
  // The `None` literal is handled by checkAssignable, which sees the
  // expression; its static type is `Obj`, which is NOT assignable to `T?`.
  if (auto *dstOT = ast::dyn_cast<ast::OptionalType>(dst))
    return isAssignable(dstOT->getInnerType(), ast::stripOptional(src));

  // An optional source can only flow into a non-optional slot typed `Obj`,
  // which may hold `None` today anyway.  `T?` -> `T` requires a `match`.
  if (ast::isa<ast::OptionalType>(src))
    return dst == Ctx.getObjTy();

  // Any array type is assignable to Obj (arrays are heap-allocated objects).
  if (dst == Ctx.getObjTy() && ast::isa<ast::ArrayType>(src))
    return true;

  // Tuples: a tuple value is an instance of the builtin `Tuple` class (an Obj
  // subtype), so it is assignable to `Obj` (and to `Tuple` itself).  Between
  // tuple types, assignability is element-wise and *representation-
  // preserving*: primitive elements must match exactly (no int -> float
  // promotion — the runtime slot holds raw bits and no conversion is
  // emitted), while reference-typed elements may be covariant
  // (`(int, Str)` is assignable to `(int, Obj)`) because tuples are
  // immutable and every reference slot holds a box regardless of its
  // static type.
  if (auto *srcTT = ast::dyn_cast<ast::TupleType>(src)) {
    if (auto *dstCT = ast::dyn_cast<ast::ClassType>(dst))
      return Ctx.getTupleTy()->isSubtypeOf(dstCT);
    auto *dstTT = ast::dyn_cast<ast::TupleType>(dst);
    if (!dstTT || dstTT->getArity() != srcTT->getArity())
      return false;
    for (size_t i = 0; i < dstTT->getArity(); ++i) {
      ast::Type *d = dstTT->getElementType(i);
      ast::Type *s = srcTT->getElementType(i);
      if (d == s)
        continue;
      if (!ast::isRefType(d) || !ast::isRefType(s) || !isAssignable(d, s))
        return false;
    }
    return true;
  }

  // Array assignability: element types must be compatible.
  if (auto *dstAT = ast::dyn_cast<ast::ArrayType>(dst)) {
    if (auto *srcAT = ast::dyn_cast<ast::ArrayType>(src)) {
      // An empty literal (void element) is assignable to any array type.
      if (srcAT->getElementType() == Ctx.getVoidTy())
        return true;
      return isAssignable(dstAT->getElementType(), srcAT->getElementType());
    }
    return false;
  }

  // ClassType subtyping: src <: dst.
  if (auto *dstCT = ast::dyn_cast<ast::ClassType>(dst))
    if (auto *srcCT = ast::dyn_cast<ast::ClassType>(src))
      return srcCT->isSubtypeOf(dstCT);

  return false;
}

bool Sema::checkAssignable(ast::Type *dst, ast::Type *srcTy, ast::Expr *src) {
  // `None` into an optional slot: statically the literal is `Obj` (a design
  // decision — see proposals/optionals.md), but in this position it denotes
  // the absent `T?` value.  Record that contextual type on the literal so
  // CodeGen emits a null box rather than boxing the `None` singleton, which
  // is how a present `Obj` value spells None.
  if (ast::isa<ast::NoneLiteral>(src) && ast::isa<ast::OptionalType>(dst)) {
    src->setResolvedType(dst);
    return true;
  }
  if (!isAssignable(dst, srcTy))
    return false;
  // `T?` -> `Obj` (the only non-optional destination an optional may flow
  // into): CodeGen must turn a null box into the boxed `None` singleton so the
  // receiving `Obj` slot never holds a NULL box, which no `Obj` consumer
  // (method dispatch, `match`) expects today.
  if (ast::isa<ast::OptionalType>(srcTy) && !ast::isa<ast::OptionalType>(dst))
    src->setCoercedType(dst);
  return true;
}

bool Sema::diagnoseOptionalNarrowing(ast::SourceLocation loc, ast::Type *dst,
                                     ast::Type *srcTy) {
  auto *srcOT = ast::dyn_cast<ast::OptionalType>(srcTy);
  if (!srcOT || ast::isa<ast::OptionalType>(dst))
    return false;
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

ast::Type *Sema::resolveType(ast::Type *ty, ast::SourceLocation loc,
                             const std::string &context) {
  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(ty))
    return Ctx.getBuiltinType(bt->getTypeKind());

  if (auto *et = ast::dyn_cast<ast::EnumType>(ty))
    return Ctx.lookupEnumType(et->getName());

  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty)) {
    // The parser emits a ClassType stub for any unknown type name; it may in
    // fact be an enum, which is resolved here before falling back to classes.
    if (auto *enumTy = Ctx.lookupEnumType(ct->getName()))
      return enumTy;
    if (auto *canonical = Ctx.lookupClassType(ct->getName()))
      return canonical;
    if (ClassTemplates.count(ct->getName())) {
      error(loc, context + " names generic class '" + ct->getName() +
                     "' without type arguments (write '" + ct->getName() +
                     "<...>')");
      return nullptr;
    }
    error(loc, context + " has unknown class type '" + ct->getName() + "'");
    return nullptr;
  }

  if (auto *at = ast::dyn_cast<ast::ArrayType>(ty)) {
    auto *elemTy = resolveType(at->getElementType(), loc, context + " element");
    if (!elemTy)
      return nullptr;
    // The parser's ArrayType carries the annotation's source location; the
    // resolved type is the context's canonical (interned) instance so that
    // array types compare by pointer and nested arrays share element nodes.
    return Ctx.getArrayType(elemTy);
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
    return instantiateClass(gt->getName(), args, loc);
  }

  if (auto *ot = ast::dyn_cast<ast::OptionalType>(ty)) {
    auto *inner = resolveType(ot->getInnerType(), loc, context);
    if (!inner)
      return nullptr;
    // Only reference types may be optional: a `T?` reuses T's PaykanShared*
    // box with NULL meaning None, and value types have no box.  The parser
    // already rejects the spelled builtins (`int?`); an enum is only known
    // here.  Nested optionals are rejected for the same reason (a `T??` would
    // need a second None to distinguish `None` from `Some(None)`).
    if (ast::isa<ast::BuiltinType>(inner) || ast::isa<ast::EnumType>(inner)) {
      error(loc, context + " has type '" + typeName(inner) +
                     "?': optional primitive types are not supported yet");
      return nullptr;
    }
    if (ast::isa<ast::OptionalType>(inner)) {
      error(loc, context + " has nested optional type '" + typeName(inner) +
                     "?', which is not supported");
      return nullptr;
    }
    // An optional tuple would be representable (a tuple is a boxed reference
    // value), but it could never be unwrapped: `match` rejects tuple type
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
ast::Type *Sema::checkIdentLive(llvm::StringRef name, ast::SourceLocation loc) {
  auto *ty = CurrentScope->lookup(name);
  if (!ty) {
    error(loc, "use of undeclared variable '" + std::string(name) + "'");
    return nullptr;
  }
  return ty;
}

// Visit an expression and return its resolved type (nullptr on error).
ast::Type *Sema::resolveExprType(ast::Expr *expr) { return EC.visit(expr); }

// -- ExprVisitor -------------------------------------------------------------

ast::Type *Sema::ExprChecker::visitIntegerLiteral(ast::IntegerLiteral *) {
  return S.Ctx.getIntTy();
}

ast::Type *Sema::ExprChecker::visitFloatLiteral(ast::FloatLiteral *) {
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
  if (S.CurrentScope && S.CurrentScope->isMoved(node->getName())) {
    S.error(node->getLocation(),
            "use of moved variable '" + node->getName() +
                "'; it was consumed by 'mov' and can only be used again after "
                "re-assignment");
    return nullptr;
  }
  // Inside an instantiation body, a bare type parameter name is a type, not a
  // value (a local variable of the same name shadows it, as usual).
  if (S.CurrentTypeParams && S.CurrentTypeParams->count(node->getName()) &&
      !(S.CurrentScope && S.CurrentScope->lookup(node->getName()))) {
    S.error(node->getLocation(), "type parameter '" + node->getName() +
                                     "' cannot be used as a value");
    return nullptr;
  }
  return S.checkIdentLive(node->getName(), node->getLocation());
}

ast::Type *Sema::ExprChecker::visitMovExpr(ast::MovExpr *node) {
  ast::Expr *operand = node->getOperand();

  // A member variable or array element may not be moved: moving out of an
  // aggregate slot would leave a dangling hole whose lifetime `mov` cannot
  // track.  Only a local variable or a temporary value may be moved.
  if (ast::isa<ast::MemberAccessExpr>(operand)) {
    S.error(node->getLocation(),
            "cannot 'mov' a member variable; move a local variable or a "
            "temporary value instead");
    return nullptr;
  }
  if (ast::isa<ast::SubscriptExpr>(operand)) {
    S.error(node->getLocation(),
            "cannot 'mov' an array element; move a local variable or a "
            "temporary value instead");
    return nullptr;
  }
  if (ast::isa<ast::TupleIndexExpr>(operand)) {
    S.error(node->getLocation(),
            "cannot 'mov' a tuple element; move a local variable or a "
            "temporary value instead");
    return nullptr;
  }

  // `self` is a borrowed reference to the receiver, not an owned local — the
  // method does not own it, so there is no ownership to transfer.  (Ordinary
  // parameters ARE owned by the callee frame and may be moved.)
  if (auto *id = ast::dyn_cast<ast::Identifier>(operand)) {
    if (id->getName() == names::kSelf) {
      S.error(node->getLocation(),
              "cannot 'mov' 'self'; it is a borrowed reference to the "
              "receiver, not an owned local variable");
      return nullptr;
    }
  }

  // Type-check the operand first.  For an identifier this also rejects reading
  // an already-moved variable.
  ast::Type *ty = visit(operand);
  if (!ty)
    return nullptr;

  // Moving a local variable consumes it: forbid any later use until it is
  // re-assigned.  Any other operand is a temporary that is simply forwarded.
  if (auto *id = ast::dyn_cast<ast::Identifier>(operand))
    S.CurrentScope->markMoved(id->getName());

  node->setResolvedType(ty);
  return ty;
}

ast::Type *Sema::ExprChecker::visitEnumValueExpr(ast::EnumValueExpr *node) {
  auto *enumTy = S.Ctx.lookupEnumType(node->getEnumName());
  if (!enumTy) {
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
  llvm_unreachable("unknown UnaryOpcode");
}

ast::Type *Sema::ExprChecker::visitBinaryExpr(ast::BinaryExpr *node) {
  auto *lhsTy = visit(node->getLHS());
  ast::Type *rhsTy = nullptr;
  if (node->getOpcode() == ast::BinaryOpcode::And ||
      node->getOpcode() == ast::BinaryOpcode::Or) {
    // Short-circuit: the RHS runs only on one outcome of the LHS, so it is a
    // branch whose entry state is the state after the LHS.  Nothing runs
    // instead of it, so the skip path is the entry state itself: a `mov` in
    // the RHS is a "maybe" afterwards and therefore counts as moved.
    MovedBranchMerger merger(S);
    merger.beginBranch();
    rhsTy = visit(node->getRHS());
    merger.endBranch();
    merger.finish(/*coversAllPaths=*/false);
  } else {
    rhsTy = visit(node->getRHS());
  }
  if (!lhsTy || !rhsTy)
    return nullptr;

  // Record operand types so CodeGen can distinguish reference-typed equality
  // (lowered to a virtual `equals` call) from primitive/enum equality without
  // re-deriving the type.
  node->getLHS()->setResolvedType(lhsTy);
  node->getRHS()->setResolvedType(rhsTy);

  // Optional operands (prototype, issue #5).  The only operators defined on a
  // `T?` are == and !=, in exactly two forms:
  //   x == None / None != x   — a null check on the box (CodeGen never
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

  // Equality requires compatible types.
  if ((node->getOpcode() == ast::BinaryOpcode::Eq ||
       node->getOpcode() == ast::BinaryOpcode::Ne)) {
    // Same type is always OK (`int[] == int[]` unifies because array types are
    // canonical per element type, while `int[] == Str[]` stays an error).
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
  llvm_unreachable("unknown BinaryOpcode");
}

ast::Type *Sema::ExprChecker::visitCallExpr(ast::CallExpr *node) {
  // Type-check all arguments first.
  std::vector<ast::Type *> argTypes;
  for (auto *arg : node->getArguments()) {
    auto *ty = visit(arg);
    argTypes.push_back(ty);
  }

  // -- __super__(args): superclass initializer call -------------------------
  if (node->getCalleeName() == names::kMethodSuper) {
    if (!S.CurrentClassCtx ||
        S.CurrentClassCtx->MethodName != names::kMethodInit) {
      S.error(node->getLocation(), std::string("'") + names::kMethodSuper +
                                       "' can only be called inside '" +
                                       names::kMethodInit + "'");
      return nullptr;
    }
    auto *superClass = S.CurrentClassCtx->ClassType->getSuperClass();
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
                      typeName(expectedParams[i]) + "'");
      }
    }
    S.CurrentClassCtx->SuperInitCalled = true;
    return S.Ctx.getVoidTy();
  }

  // -- Generic call: instantiate the template and rebind the callee ---------
  if (node->hasTypeArgs() || S.ClassTemplates.count(node->getCalleeName()) ||
      S.FuncTemplates.count(node->getCalleeName())) {
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
  if (!sig) {
    S.error(node->getLocation(),
            "call to undeclared function '" + node->getCalleeName() + "'");
    return nullptr;
  }

  // Record "class A constructs class B" edges for instantiation ordering.
  if (S.CurrentClassCtx && S.Ctx.lookupClassType(node->getCalleeName()))
    S.ConstructsEdges[S.CurrentClassCtx->ClassType->getName()].insert(
        node->getCalleeName());

  if (argTypes.size() != sig->ParamTypes.size()) {
    S.error(node->getLocation(),
            "function '" + node->getCalleeName() + "' expects " +
                std::to_string(sig->ParamTypes.size()) + " argument(s), got " +
                std::to_string(argTypes.size()));
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
                  node->getCalleeName() + "' has type '" +
                  typeName(argTypes[i]) + "', expected '" +
                  typeName(sig->ParamTypes[i]) + "'");
    }
  }

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

  // Resolve the ClassType to look up the method on.
  // Array types dispatch through a per-element specialized ClassType so that
  // push/pop signatures are element-type-aware.
  ast::ClassType *ct = nullptr;
  if (auto *at = ast::dyn_cast<ast::ArrayType>(recvTy))
    ct = S.Ctx.getOrCreateSpecializedArrayType(at->getElementType());
  else if (auto *tt = ast::dyn_cast<ast::TupleType>(recvTy))
    // Tuples expose only Obj's methods (toString / equals) through their
    // per-element-list specialization.
    ct = S.Ctx.getOrCreateSpecializedTupleType(tt);
  else
    ct = ast::dyn_cast<ast::ClassType>(recvTy);

  if (!ct) {
    S.error(node->getLocation(),
            "method call on non-class type '" + typeName(recvTy) + "'");
    return nullptr;
  }

  ast::MethodDecl *method = ct->findMethod(node->getMethodName());
  std::string ownerName = ct->getName();

  if (!method) {
    S.error(node->getLocation(), "no method '" + node->getMethodName() +
                                     "' on type '" + ownerName + "'");
    return nullptr;
  }

  // `destroy` is the compiler-generated ARC destructor; it runs automatically
  // when the last reference is released. Calling it directly would free a
  // still-referenced object and lead to a use-after-free, so reject it.
  if (node->getMethodName() == names::kMethodDestroy) {
    S.error(node->getLocation(),
            "'destroy' cannot be called directly; an object is destroyed "
            "automatically when its last reference is released");
    return method->getReturnType();
  }

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
    auto *argTy = visit(argExpr);
    if (!argTy)
      continue;
    if (!S.checkAssignable(paramTys[i], argTy, argExpr) &&
        !S.diagnoseOptionalNarrowing(argExpr->getLocation(), paramTys[i],
                                     argTy)) {
      S.error(argExpr->getLocation(),
              "argument " + std::to_string(i + 1) + " of '" +
                  node->getMethodName() + "' has type '" + typeName(argTy) +
                  "', expected '" + typeName(paramTys[i]) + "'");
    }
  }

  node->setResolvedType(method->getReturnType());
  return method->getReturnType();
}

// static
ast::ClassType *Sema::findLowestCommonAncestor(ast::ClassType *a,
                                               ast::ClassType *b) {
  llvm::SmallPtrSet<ast::ClassType *, 8> aAncestors;
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

  auto *ct = ast::dyn_cast<ast::ClassType>(recvTy);
  if (!ct) {
    S.error(node->getLocation(), "member access '." + node->getFieldName() +
                                     "' on non-class type '" +
                                     typeName(recvTy) + "'");
    return nullptr;
  }

  // Walk the class hierarchy (this class + all ancestors) for the field.
  if (auto *fty = ct->findField(node->getFieldName())) {
    node->setResolvedType(fty);
    return fty;
  }

  S.error(node->getLocation(), "no field '" + node->getFieldName() +
                                   "' in class '" + ct->getName() + "'");
  return nullptr;
}

ast::Type *
Sema::ExprChecker::visitArrayLiteralExpr(ast::ArrayLiteralExpr *node) {
  // Empty literal [] is valid only where an explicit array type annotation is
  // present (e.g. a: int[] = []).  Return ArrayType(void) as a sentinel;
  // isAssignable() treats it as compatible with any array destination.
  if (node->isEmpty()) {
    auto *arrTy = S.Ctx.getArrayType(S.Ctx.getVoidTy());
    node->setResolvedType(arrTy);
    return arrTy;
  }

  // Non-empty: resolve all elements and unify to a common element type.
  ast::Type *elemTy = nullptr;
  for (size_t i = 0; i < node->getNumElements(); ++i) {
    auto *ty = visit(node->getElements()[i]);
    if (!ty)
      return nullptr;
    if (!elemTy) {
      elemTy = ty;
    } else if (!typesEqual(elemTy, ty)) {
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
  auto *arrTy = S.Ctx.getArrayType(elemTy);
  node->setResolvedType(arrTy);
  return arrTy;
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

  auto *at = ast::dyn_cast<ast::ArrayType>(arrayTy);
  if (!at) {
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

  node->setResolvedType(at->getElementType());
  return at->getElementType();
}

ast::Type *Sema::ExprChecker::visitTernaryExpr(ast::TernaryExpr *node) {
  // The condition always runs first, so its moves are visible to both
  // branches.  The branches are then siblings exactly like an if/else
  // statement's: each is checked against the state after the condition, and
  // the state after the expression is their union (one of them always runs).
  auto *condTy = visit(node->getCondition());
  MovedBranchMerger merger(S);
  merger.beginBranch();
  auto *trueTy = visit(node->getTrueExpr());
  merger.endBranch();
  merger.beginBranch();
  auto *falseTy = visit(node->getFalseExpr());
  merger.endBranch();
  merger.finish(/*coversAllPaths=*/true);
  if (!condTy || !trueTy || !falseTy)
    return nullptr;

  if (condTy != S.Ctx.getBoolTy()) {
    S.error(node->getCondition()->getLocation(),
            "ternary condition must be 'bool', got '" + typeName(condTy) + "'");
    return nullptr;
  }

  if (trueTy == falseTy) {
    node->setResolvedType(trueTy);
    return trueTy;
  }

  // Optional unification (prototype, issue #5):
  //   if c then x else None   — `T?` when x is a reference type T (or T?);
  //   if c then a else b      — `T?` when either side is optional and the
  //                             wrapped types unify (equal, or class LCA).
  // A `None` branch is re-typed to the optional result (see checkAssignable)
  // so CodeGen produces a null box for it.
  {
    ast::Expr *trueExpr = node->getTrueExpr();
    ast::Expr *falseExpr = node->getFalseExpr();
    bool trueNone = ast::isa<ast::NoneLiteral>(trueExpr);
    bool falseNone = ast::isa<ast::NoneLiteral>(falseExpr);
    ast::Type *result = nullptr;
    if (trueNone != falseNone) {
      ast::Type *valueTy = trueNone ? falseTy : trueTy;
      if (ast::isRefType(valueTy))
        result = ast::isa<ast::OptionalType>(valueTy)
                     ? valueTy
                     : S.Ctx.getOptionalType(valueTy);
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

// -- Tuples (prototype) ------------------------------------------------------

// A tuple literal's type is the canonical tuple of its element types.  There
// is no contextual typing: `(1, "a")` is `(int, Str)` even when assigned to a
// `(int, Obj)` variable (assignability then applies element-wise, see
// isAssignable).  Elements are checked left to right in one straight-line
// path, so `mov` state simply flows through them.
ast::Type *
Sema::ExprChecker::visitTupleLiteralExpr(ast::TupleLiteralExpr *node) {
  std::vector<ast::Type *> elemTys;
  elemTys.reserve(node->getNumElements());
  bool ok = true;
  for (size_t i = 0; i < node->getNumElements(); ++i) {
    ast::Expr *elem = node->getElements()[i];
    auto *ty = visit(elem);
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
    // An empty array literal has no element type of its own and nothing
    // inside a tuple literal can supply one (unlike an annotated variable),
    // so codegen could not pick the right array representation.
    if (auto *at = ast::dyn_cast<ast::ArrayType>(ty);
        at && at->getElementType() == S.Ctx.getVoidTy()) {
      S.error(elem->getLocation(),
              "cannot infer element type of empty array literal '[]' inside a "
              "tuple literal; bind it to an annotated variable first");
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
  // Record the receiver's type too: CodeGen reads it to pick the element
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
  auto *valTy = resolveExprType(node->getValue());
  if (!valTy)
    return false;

  auto *tt = ast::dyn_cast<ast::TupleType>(valTy);
  if (!tt) {
    error(node->getValue()->getLocation(),
          "cannot destructure a value of type '" + typeName(valTy) +
              "'; only tuples can be destructured");
    return false;
  }
  if (tt->getArity() != node->getNumTargets()) {
    error(node->getLocation(),
          "cannot destructure a value of type '" + typeName(tt) + "' into " +
              std::to_string(node->getNumTargets()) + " targets (it has " +
              std::to_string(tt->getArity()) + " elements)");
    return false;
  }
  // CodeGen extracts the elements from this resolved type.
  node->getValue()->setResolvedType(tt);

  bool ok = true;
  llvm::StringSet<> seen;
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
    // Same guard as visitAssignStmt: a target must not shadow a type name.
    if (Ctx.lookupClassType(name) || Ctx.lookupEnumType(name) ||
        name == names::kObj || name == names::kString || name == names::kFile ||
        name == names::kTypeInt || name == names::kTypeBool ||
        name == names::kTypeFloat || name == names::kTypeChar ||
        name == names::kStdin) {
      error(target.Loc,
            "'" + name + "' is a type name and cannot be used as a variable");
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
        ok = false;
        continue;
      }
      if (!isAssignable(declTy, elemTy)) {
        error(target.Loc, "element " + std::to_string(i) + " of type '" +
                              typeName(elemTy) +
                              "' does not match declared type '" +
                              typeName(declTy) + "' for target '" + name + "'");
        ok = false;
        continue;
      }
      CurrentScope->declare(name, declTy);
      continue;
    }

    // Bare name: re-assignment if visible, implicit declaration otherwise.
    CurrentScope->clearMoved(name);
    auto *owner = CurrentScope->findOwner(name);
    if (!owner) {
      CurrentScope->set(name, elemTy);
      continue;
    }
    auto *varTy = owner->lookup(name);
    if (!isAssignable(varTy, elemTy)) {
      error(target.Loc, "cannot assign element " + std::to_string(i) +
                            " of type '" + typeName(elemTy) +
                            "' to variable '" + name + "' of type '" +
                            typeName(varTy) + "'");
      ok = false;
    }
  }
  return ok;
}

// -- Entry point -------------------------------------------------------------

SemaContext Sema::run(ast::TranslationUnit *tu) {
  CurrentScope = nullptr;
  AccumulatedImportContexts.clear();

  // Bootstrap builtin functions — the print family takes a single Obj
  // (printed via toString); compose multiple pieces with `+`.  Paykan has no
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
  declareFunction(names::kStrInt, StrTy, {Ctx.getIntTy()}, true);
  declareFunction(names::kStrFloat, StrTy, {Ctx.getFloatTy()}, true);
  declareFunction(names::kStrBool, StrTy, {Ctx.getBoolTy()}, true);
  declareFunction(names::kStrChar, StrTy, {Ctx.getCharTy()}, true);
  declareFunction(names::kString, StrTy, {StrTy}, true);
  declareFunction(names::kOpen, Ctx.getObjTy(), {StrTy, StrTy}, true);
  declareFunction(names::kIntStr, Ctx.getObjTy(), {StrTy}, true);
  declareFunction(names::kFloatStr, Ctx.getObjTy(), {StrTy}, true);

  // Process imports before local declarations.
  llvm::StringSet<> localImportStack;
  if (!ImportStack)
    ImportStack = &localImportStack;
  for (auto *imp : tu->getImports())
    processImport(imp);

  visit(tu);
  return SemaContext{nullptr,
                     &Ctx,
                     nullptr,
                     !Diags.hasErrors(),
                     Diags.getErrorCount(),
                     Diags.getDiagnostics(),
                     std::move(AccumulatedImportContexts)};
}

// -- Top-level ---------------------------------------------------------------

bool Sema::visitTranslationUnit(ast::TranslationUnit *node) {
  bool ok = true;

  // Register enum types first so that class fields, parameters, and variable
  // declarations can reference them by name during the passes that follow.
  for (auto *ed : node->getEnumDecls())
    if (!visitEnumDecl(ed))
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

  // Now check class method bodies (they can resolve free functions) …
  if (!node->getClassDecls().empty())
    if (!checkClassBodies())
      ok = false;

  // … and finally free-function bodies (signatures all registered above).
  for (auto *fn : declaredFns)
    if (!visitFuncDecl(fn))
      ok = false;

  // Bodies of the instantiations requested so far (and of any they request
  // in turn): checked last, like C++ instantiates at the end of the TU.
  if (!checkPendingInstantiations())
    ok = false;

  // Hand the instantiations to CodeGen as ordinary declarations.
  injectInstantiations(node);

  return ok;
}

// -- Declarations ------------------------------------------------------------

bool Sema::visitEnumDecl(ast::EnumDecl *node) {
  // Reject names that collide with a builtin, class, function, or existing
  // enum (the primitive type keywords cannot be spelled as identifiers).
  if (!checkDeclNameAvailable(node->getName(), node->getLocation(),
                              DeclKind::Enum))
    return false;

  auto *enumTy = Ctx.registerEnumType(node->getName(), node->getLocation());
  assert(enumTy && "enum name was checked to be free above");

  bool ok = true;
  llvm::StringSet<> seen;
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

// -- Statements --------------------------------------------------------------

bool Sema::visitCompoundStmt(ast::CompoundStmt *node) {
  ScopeGuard guard(*this);
  bool ok = true;
  for (auto *stmt : node->getStatements()) {
    if (!visit(stmt))
      ok = false;
  }
  return ok;
}

bool Sema::visitDeclStmt(ast::DeclStmt *node) { return visit(node->getDecl()); }

bool Sema::visitExprStmt(ast::ExprStmt *node) {
  // Type-check the expression; we discard the type.
  return resolveExprType(node->getExpr()) != nullptr;
}

bool Sema::visitAssignStmt(ast::AssignStmt *node) {
  auto *valTy = resolveExprType(node->getValue());
  if (!valTy)
    return false;

  // Guard: the target name must not shadow a registered type name.
  const auto &varName = node->getVarName();
  if (Ctx.lookupClassType(varName) || Ctx.lookupEnumType(varName) ||
      ClassTemplates.count(varName) ||
      (CurrentTypeParams && CurrentTypeParams->count(varName)) ||
      varName == names::kObj || varName == names::kString ||
      varName == names::kFile || varName == names::kTypeInt ||
      varName == names::kTypeBool || varName == names::kTypeFloat ||
      varName == names::kTypeChar || varName == names::kStdin) {
    error(node->getLocation(),
          "'" + varName + "' is a type name and cannot be used as a variable");
    return false;
  }

  // Re-assigning a moved variable revives it (the RHS was already checked
  // above, so a self-referential RHS like `a = a + 1` still errors).
  CurrentScope->clearMoved(varName);

  // Look up the variable in all enclosing scopes.
  auto *owner = CurrentScope->findOwner(varName);
  if (!owner) {
    // First assignment — declare in the current scope.
    // An empty array literal without an explicit type annotation is ambiguous.
    if (auto *at = ast::dyn_cast<ast::ArrayType>(valTy)) {
      if (at->getElementType() == Ctx.getVoidTy()) {
        error(node->getLocation(),
              "cannot infer element type of empty array literal '[]'; "
              "add an explicit type annotation");
        return false;
      }
    }
    CurrentScope->set(varName, valTy);
    return true;
  }

  auto *varTy = owner->lookup(varName);

  // Reject empty array literal when the target type can't supply the element
  // type.  An optional array variable (`xs: Str[]?`) supplies its wrapped
  // array type.
  ast::Type *varArrTy = ast::stripOptional(varTy);
  if (auto *at = ast::dyn_cast<ast::ArrayType>(valTy)) {
    if (at->getElementType() == Ctx.getVoidTy() &&
        !ast::isa<ast::ArrayType>(varArrTy)) {
      error(node->getLocation(),
            "cannot infer element type of empty array literal '[]'; "
            "add an explicit type annotation");
      return false;
    }
  }

  if (!checkAssignable(varTy, valTy, node->getValue())) {
    if (!diagnoseOptionalNarrowing(node->getLocation(), varTy, valTy))
      error(node->getLocation(), "cannot assign value of type '" +
                                     typeName(valTy) + "' to variable '" +
                                     varName + "' of type '" + typeName(varTy) +
                                     "'");
    return false;
  }

  // Propagate the target's array type onto an empty literal (see
  // visitVarDecl for why CodeGen needs the element type).
  if (auto *lit = ast::dyn_cast<ast::ArrayLiteralExpr>(node->getValue()))
    if (lit->isEmpty() && ast::isa<ast::ArrayType>(varArrTy))
      lit->setResolvedType(varArrTy);

  return true;
}

bool Sema::visitReturnStmt(ast::ReturnStmt *node) {
  if (node->getReturnValue()) {
    auto *valTy = resolveExprType(node->getReturnValue());
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
    return true;
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
  // Each branch sees the moved-state at entry (a `mov` in the then-branch is
  // on an impossible path for the else-branch); afterwards the state is the
  // union over branches — plus the fall-through path when there is no else.
  MovedBranchMerger merger(*this);
  // Type-check the then branch.
  merger.beginBranch();
  if (!visit(node->getThenBranch()))
    ok = false;
  merger.endBranch();
  // Type-check the else branch (if present).
  if (node->hasElse()) {
    merger.beginBranch();
    if (!visit(node->getElseBranch()))
      ok = false;
    merger.endBranch();
  }
  merger.finish(/*coversAllPaths=*/node->hasElse());
  return ok;
}

bool Sema::visitWhileStmt(ast::WhileStmt *node) {
  // Back-edge soundness rule for `mov`: snapshot the moved-state before the
  // condition (both the condition and the body re-execute every iteration).
  // Any name that is (a) owned by a scope OUTSIDE the loop and (b) still
  // moved when control reaches the back edge would be read-after-consume on
  // iteration 2 — codegen nulls the slot on mov — so it is a compile error
  // unless the body definitely re-assigned it before the back edge.  The
  // check is conservative: the whole-body exit state stands in for the back
  // edge, so a move that only reaches `break` is also rejected.  Names owned
  // by scopes INSIDE the body are re-declared fresh each iteration and are
  // exempt (their scopes are not on the chain in the entry snapshot).
  MovedState entry = saveMovedState();

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

  // Diagnose every name newly moved across the iteration (moved at the back
  // edge but live at loop entry).
  MovedState exit = saveMovedState();
  assert(entry.size() == exit.size() && "snapshots must cover the same chain");
  for (size_t i = 0; i < exit.size(); ++i) {
    for (const auto &name : exit[i].second) {
      if (entry[i].second.count(name.getKey()))
        continue;
      error(node->getLocation(),
            "variable '" + std::string(name.getKey()) +
                "' is declared outside the loop but consumed by 'mov' inside "
                "it; it would already be moved on the next iteration — "
                "re-assign it before the loop repeats or declare it inside "
                "the loop");
      ok = false;
    }
  }

  // After the loop: the body may run zero times, so a name moved at entry
  // stays moved even if every iteration re-assigns it (union of both paths).
  unionMovedState(exit, entry);
  restoreMovedState(exit);
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

// -- Declarations ------------------------------------------------------------

/// Returns true if every control-flow path through `stmts` ends in a
/// ReturnStmt.  This is a conservative syntactic check — it catches the common
/// "missing return" cases without requiring full CFG analysis.
/// Returns true if `stmt` (a single statement) always returns on every path.
bool detail::blockAlwaysReturns(llvm::ArrayRef<ast::Stmt *> stmts) {
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
  // declaration so CodeGen sees canonical types (never a parser stub or a
  // generic type application).
  ast::Type *retTy = Ctx.getVoidTy();
  if (node->getReturnType()) {
    retTy = resolveType(node->getReturnType(), node->getLocation(),
                        "function '" + node->getName() + "' return type");
    if (!retTy)
      return false;
    node->setReturnType(retTy);
  }

  // Resolve parameter types.
  std::vector<ast::Type *> paramTypes;
  for (auto &p : node->getMutableParams()) {
    auto *ty = resolveType(p.ParamType, node->getLocation(),
                           "parameter '" + p.getName() + "'");
    if (!ty)
      return false;
    p.ParamType = ty;
    paramTypes.push_back(ty);
  }

  // Register the function in the function table.
  declareFunction(node->getName(), retTy, paramTypes);
  return true;
}

bool Sema::visitFuncDecl(ast::FuncDecl *node) {
  // The signature was registered by the forward-declaration pass in
  // visitTranslationUnit, which only visits bodies of successfully declared
  // functions.  Defensive: if the entry is absent anyway, the error was
  // already reported — skip the body to avoid duplicate diagnostics.
  if (!lookupFunction(node->getName()))
    return false;

  // Re-resolve the annotations to set up the body scope (resolveType is
  // idempotent and, for a registered function, is guaranteed to succeed —
  // declareFunctionSignature already wrote the canonical types back).
  ast::Type *retTy = Ctx.getVoidTy();
  if (node->getReturnType())
    retTy = resolveType(node->getReturnType(), node->getLocation(),
                        "function '" + node->getName() + "' return type");

  std::vector<ast::Type *> paramTypes;
  for (auto &p : node->getParams())
    paramTypes.push_back(resolveType(p.ParamType, node->getLocation(),
                                     "parameter '" + p.getName() + "'"));

  // Type-check the body in a new scope with params.
  auto *savedRetTy = CurrentReturnType;
  CurrentReturnType = retTy;
  {
    ScopeGuard guard(*this);
    for (size_t i = 0; i < node->getParams().size(); ++i)
      CurrentScope->declare(node->getParams()[i].getName(), paramTypes[i]);
    bool ok = true;
    for (auto *stmt : node->getBody()->getStatements())
      if (!visit(stmt))
        ok = false;
    CurrentReturnType = savedRetTy;
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
  // Check for duplicate declaration in the current scope.
  if (CurrentScope->contains(node->getName())) {
    error(node->getLocation(),
          "redeclaration of variable '" + node->getName() + "'");
    return false;
  }

  // Resolve the declared type.
  ast::Type *declTy = nullptr;

  if (node->getType()) {
    declTy = resolveType(node->getType(), node->getLocation(),
                         "variable '" + node->getName() + "'");
    if (!declTy) {
      CurrentScope->set(node->getName(), Ctx.getVoidTy());
      return false;
    }
    node->setType(declTy); // canonical write-back (see VarDecl::setType)
  }

  // Check the initializer type.
  if (node->getInitExpr()) {
    auto *initTy = resolveExprType(node->getInitExpr());
    if (!initTy) {
      error(node->getLocation(),
            "cannot determine type of initializer for variable '" +
                node->getName() + "'");
      CurrentScope->set(node->getName(), declTy ? declTy : Ctx.getVoidTy());
      return false;
    }

    if (declTy) {
      // Reject an empty array literal when the declared type cannot supply the
      // element type (e.g. `a: Obj = []` — the element type is uninferable).
      // An optional array (`xs: Str[]? = []`) supplies its wrapped type.
      ast::Type *declArrTy = ast::stripOptional(declTy);
      if (auto *at = ast::dyn_cast<ast::ArrayType>(initTy)) {
        if (at->getElementType() == Ctx.getVoidTy() &&
            !ast::isa<ast::ArrayType>(declArrTy)) {
          error(node->getLocation(),
                "cannot infer element type of empty array literal '[]'; "
                "add an explicit type annotation");
          CurrentScope->set(node->getName(), declTy);
          return false;
        }
      }
      if (!checkAssignable(declTy, initTy, node->getInitExpr())) {
        if (!diagnoseOptionalNarrowing(node->getLocation(), declTy, initTy))
          error(node->getLocation(),
                "initializer of type '" + typeName(initTy) +
                    "' does not match declared type '" + typeName(declTy) +
                    "' for variable '" + node->getName() + "'");
        CurrentScope->set(node->getName(), declTy);
        return false;
      }

      // Propagate the declared array type onto an empty array literal `[]`
      // (whose own resolved type is the ArrayType(void) sentinel).  Without
      // this, codegen cannot tell an empty `Obj[]` from an empty `int[]` and
      // emits a primitive array whose destructor never releases the elements
      // pushed into it later — a leak.
      if (auto *lit =
              ast::dyn_cast<ast::ArrayLiteralExpr>(node->getInitExpr())) {
        if (lit->isEmpty() && ast::isa<ast::ArrayType>(declArrTy))
          lit->setResolvedType(declArrTy);
      }
    } else {
      // Infer type from initializer — but reject bare [] with no annotation.
      if (auto *at = ast::dyn_cast<ast::ArrayType>(initTy)) {
        if (at->getElementType() == Ctx.getVoidTy()) {
          error(node->getLocation(),
                "cannot infer element type of empty array literal '[]'; "
                "add an explicit type annotation");
          CurrentScope->set(node->getName(), Ctx.getVoidTy());
          return false;
        }
      }
      declTy = initTy;
    }
  }

  if (!declTy) {
    error(node->getLocation(),
          "variable '" + node->getName() +
              "' has no type annotation and no initializer");
    CurrentScope->set(node->getName(), Ctx.getVoidTy());
    return false;
  }

  // Register the variable in the current scope.
  CurrentScope->declare(node->getName(), declTy);
  return true;
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

  // `t.0 = v`: the parser hands tuple-index assignment over as a member
  // assignment whose field name is the index (see Parser.ypp) so that the
  // rejection is a typed diagnostic.  Prototype tuples are immutable.
  if (ast::isa<ast::TupleType>(recvTy)) {
    error(node->getLocation(), "cannot assign to element '." +
                                   node->getFieldName() + "' of type '" +
                                   typeName(recvTy) +
                                   "': tuples are immutable; build a new tuple "
                                   "instead");
    return false;
  }

  auto *ct = ast::dyn_cast<ast::ClassType>(recvTy);
  if (!ct) {
    error(node->getLocation(), "member assignment '." + node->getFieldName() +
                                   "' on non-class type '" + typeName(recvTy) +
                                   "'");
    return false;
  }

  // Look up the field in the class hierarchy.
  ast::Type *fieldTy = ct->findField(node->getFieldName());
  if (!fieldTy) {
    error(node->getLocation(), "no field '" + node->getFieldName() +
                                   "' in class '" + ct->getName() + "'");
    return false;
  }

  auto *valTy = resolveExprType(node->getValue());
  if (!valTy)
    return false;

  if (!checkAssignable(fieldTy, valTy, node->getValue())) {
    if (!diagnoseOptionalNarrowing(node->getLocation(), fieldTy, valTy))
      error(node->getLocation(),
            "cannot assign value of type '" + typeName(valTy) + "' to field '" +
                node->getFieldName() + "' of type '" + typeName(fieldTy) + "'");
    return false;
  }

  return true;
}

bool Sema::visitSubscriptAssignStmt(ast::SubscriptAssignStmt *node) {
  auto *arrTy = resolveExprType(node->getArray());
  if (!arrTy)
    return false;
  // An optional array cannot be indexed (it may be None).
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(arrTy)) {
    errorOptionalUnwrap(node->getLocation(), ot);
    return false;
  }
  auto *at = ast::dyn_cast<ast::ArrayType>(arrTy);
  if (!at) {
    error(node->getLocation(),
          "subscript assignment on non-array type '" + typeName(arrTy) + "'");
    return false;
  }
  auto *idxTy = resolveExprType(node->getIndex());
  if (idxTy && idxTy != Ctx.getIntTy()) {
    error(node->getIndex()->getLocation(),
          "array index must be int, got '" + typeName(idxTy) + "'");
  }
  auto *valTy = resolveExprType(node->getValue());
  if (!valTy)
    return false;
  if (!checkAssignable(at->getElementType(), valTy, node->getValue())) {
    if (!diagnoseOptionalNarrowing(node->getLocation(), at->getElementType(),
                                   valTy))
      error(node->getLocation(), "cannot assign value of type '" +
                                     typeName(valTy) + "' to array of '" +
                                     typeName(at->getElementType()) + "'");
    return false;
  }
  return true;
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

  // Sibling arms are mutually exclusive: each is checked against the
  // moved-state at match entry, and the state afterwards is the union over
  // arms (plus the no-arm-taken path when there is no wildcard).
  MovedBranchMerger merger(*this);

  for (ast::MatchArm *arm : node->getArms()) {
    if (seenWildcard) {
      error(arm->getLocation(),
            "unreachable arm: wildcard '_' must be the last arm");
      ok = false;
      continue;
    }

    merger.beginBranch();
    ScopeGuard armGuard(*this);

    if (arm->isWildcard()) {
      seenWildcard = true;
    } else if (arm->isLiteral()) {
      // Bindings are meaningless in value-mode; the grammar never produces a
      // binding on a literal arm, but guard anyway.
      if (arm->hasBinding()) {
        error(arm->getLocation(), "value-match arm cannot bind a variable");
        ok = false;
      }
      auto *litTy = resolveExprType(arm->getLiteralPattern());
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

    for (auto *stmt : arm->getBody()->getStatements())
      if (!visit(stmt))
        ok = false;
    merger.endBranch();
  }

  merger.finish(/*coversAllPaths=*/seenWildcard);
  return ok;
}

// Enum-mode match: the subject is an enum, and each non-wildcard arm names a
// bare variant.  The variant name is carried by the arm's type-name (a
// ClassType stub produced by the parser's `typeAnnotation: IDENT` rule).
bool Sema::checkEnumMatch(ast::MatchStmt *node, ast::EnumType *subjectTy) {
  bool ok = true;
  bool seenWildcard = false;
  llvm::StringSet<> seenVariants;

  // Per-arm moved-state isolation + union merge (see checkValueMatch).
  MovedBranchMerger merger(*this);

  for (ast::MatchArm *arm : node->getArms()) {
    if (seenWildcard) {
      error(arm->getLocation(),
            "unreachable arm: wildcard '_' must be the last arm");
      ok = false;
      continue;
    }

    merger.beginBranch();
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
        ok = false;
      }
      std::string variant;
      if (auto *stub = ast::dyn_cast<ast::ClassType>(arm->getArmType()))
        variant = stub->getName();
      else if (auto *et = ast::dyn_cast<ast::EnumType>(arm->getArmType()))
        variant = et->getName();
      if (variant.empty() || subjectTy->findVariant(variant) < 0) {
        error(arm->getLocation(), "'" + variant +
                                      "' is not a variant of enum '" +
                                      subjectTy->getName() + "'");
        ok = false;
      } else if (!seenVariants.insert(variant).second) {
        error(arm->getLocation(),
              "duplicate variant '" + variant + "' in enum match");
        ok = false;
      }
    }

    for (auto *stmt : arm->getBody()->getStatements())
      if (!visit(stmt))
        ok = false;
    merger.endBranch();
  }

  // NOTE: an enum match covering every variant without a wildcard is
  // exhaustive at runtime, but the merge stays conservative (the entry state
  // is kept as a possible path) — reviving a moved name in every arm of a
  // wildcard-less match does not revive it after the match.
  merger.finish(/*coversAllPaths=*/seenWildcard);
  return ok;
}

bool Sema::visitMatchStmt(ast::MatchStmt *node) {
  auto *subjectTy = resolveExprType(node->getSubject());
  if (!subjectTy)
    return false;
  // Record the subject type so CodeGen can pick the right lowering (in
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
  // unwrapped (prototype, issue #5).
  if (auto *optTy = ast::dyn_cast<ast::OptionalType>(subjectTy))
    return checkOptionalMatch(node, optTy);

  // The subject must be a class type — matching on other builtins is not
  // supported.
  auto *subjectCt = ast::dyn_cast<ast::ClassType>(subjectTy);
  if (!subjectCt) {
    error(node->getSubject()->getLocation(),
          "match subject must be a class type, got '" + typeName(subjectTy) +
              "'");
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

  // Per-arm moved-state isolation + union merge (see checkValueMatch).
  MovedBranchMerger merger(*this);

  for (ast::MatchArm *arm : node->getArms()) {
    if (seenWildcard) {
      error(arm->getLocation(),
            "unreachable arm: wildcard '_' must be the last arm");
      ok = false;
      continue;
    }

    merger.beginBranch();
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
      for (auto *stmt : arm->getBody()->getStatements())
        if (!visit(stmt))
          ok = false;
      merger.endBranch();
      continue;
    } else {
      // 2. Resolve the arm's type annotation.
      auto *resolvedArmTy =
          resolveType(arm->getArmType(), arm->getLocation(), "match arm");
      arm->setArmType(resolvedArmTy); // write canonical pointer back
      auto *armCt = resolvedArmTy ? ast::dyn_cast<ast::ClassType>(resolvedArmTy)
                                  : nullptr;
      auto *armAt = resolvedArmTy ? ast::dyn_cast<ast::ArrayType>(resolvedArmTy)
                                  : nullptr;
      if (!armCt && !armAt) {
        // Only emit a secondary diagnostic if resolution succeeded but the
        // type is not a class or array. If resolveType already emitted
        // "unknown type", resolvedArmTy is null and that's enough.
        if (resolvedArmTy)
          error(arm->getLocation(),
                "match arm type must be a class or array type");
        ok = false;
        // Still try to visit the body to surface further errors.
      } else if (armAt) {
        // Array arm: the subject must be Obj (arrays are dispatched as Obj
        // at runtime — they share the Obj vtable header).
        if (subjectCt != Ctx.getObjTy()) {
          error(arm->getLocation(),
                "array match arm requires an 'Obj' subject, got '" +
                    subjectCt->getName() + "'");
          ok = false;
        }
        if (arm->hasBinding()) {
          if (!CurrentScope->declare(arm->getBinding(), armAt)) {
            error(arm->getLocation(),
                  "redeclaration of '" + arm->getBinding() + "' in match arm");
            ok = false;
          }
        }
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
        if (arm->hasBinding()) {
          if (!CurrentScope->declare(arm->getBinding(), armCt)) {
            error(arm->getLocation(),
                  "redeclaration of '" + arm->getBinding() + "' in match arm");
            ok = false;
          }
        }
      }
    }

    // 5. Recursively type-check the arm body.
    for (auto *stmt : arm->getBody()->getStatements())
      if (!visit(stmt))
        ok = false;
    merger.endBranch();
  }

  merger.finish(/*coversAllPaths=*/seenWildcard);
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
  auto *innerAt = ast::dyn_cast<ast::ArrayType>(inner);
  const std::string subjName = typeName(subjectTy);

  bool ok = true;
  bool seenWildcard = false, seenNone = false, seenInner = false;

  // Per-arm moved-state isolation + union merge (see checkValueMatch).
  MovedBranchMerger merger(*this);

  for (ast::MatchArm *arm : node->getArms()) {
    if (seenWildcard) {
      error(arm->getLocation(),
            "unreachable arm: wildcard '_' must be the last arm");
      ok = false;
      continue;
    }

    merger.beginBranch();
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
      } else if (innerAt) {
        if (!typesEqual(resolvedArmTy, innerAt)) {
          error(arm->getLocation(),
                "match arm type '" + typeName(resolvedArmTy) +
                    "' does not match the optional subject type '" + subjName +
                    "'");
          ok = false;
        } else {
          bindTy = innerAt;
        }
      }
      if (bindTy == inner)
        seenInner = true;
      if (arm->hasBinding() && bindTy) {
        if (!CurrentScope->declare(arm->getBinding(), bindTy)) {
          error(arm->getLocation(),
                "redeclaration of '" + arm->getBinding() + "' in match arm");
          ok = false;
        }
      }
    }

    for (auto *stmt : arm->getBody()->getStatements())
      if (!visit(stmt))
        ok = false;
    merger.endBranch();
  }

  merger.finish(/*coversAllPaths=*/seenWildcard || (seenNone && seenInner));
  return ok;
}

} // namespace sema
} // namespace paykan
