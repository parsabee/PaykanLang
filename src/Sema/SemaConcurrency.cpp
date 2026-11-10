// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The concurrency prototype's checks (docs/design/concurrency.md): what a
// `parallel for` body may read and write, what an async call takes across
// to its task (the send check), and assignments to `atomic` variables.

#include "Names.h"
#include "NativeABI.h"
#include "Sema.h"
#include "SemaInternal.h"

#include <algorithm>

namespace paykan::sema {

namespace {

std::string at(ast::SourceLocation loc) {
  return std::to_string(loc.getLineStart()) + ":" +
         std::to_string(loc.getColumnStart());
}

/// A value of plain parts: a number, bool, char, or an enum whose
/// variants' fields are plain (a payload's reference would be counted from
/// every iteration's thread, `E<shared mut Node>`).
bool isPlainValue(ast::Type *ty) {
  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(ty))
    return bt->getTypeKind() != ast::BuiltinType::Void;
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    return isPlainValue(ot->getInnerType());
  if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty))
    return isPlainValue(ft->getElementType());
  if (auto *tt = ast::dyn_cast<ast::TupleType>(ty)) { // a struct too
    for (ast::Type *e : tt->getElementTypes())
      if (!isPlainValue(e))
        return false;
    return true;
  }
  auto *et = ast::dyn_cast<ast::EnumType>(ty);
  if (!et)
    return false;
  for (size_t i = 0; i < et->getNumVariants(); ++i)
    if (ast::StructType *p = et->getPayload(i))
      if (!isPlainValue(p))
        return false;
  return true;
}

} // namespace

bool Sema::isOuterScope(Scope *owner) const {
  if (ParallelStack.empty() || !owner)
    return false;
  for (Scope *s = owner; s; s = s->Parent)
    if (s == ParallelStack.back().Boundary)
      return false; // bound by the loop, or inside its body
  return true;
}

bool Sema::checkParallelRead(const std::string &name, ast::Type *ty,
                             ast::SourceLocation loc) {
  if (!isOuterScope(CurrentScope->findOwner(name)))
    return true;
  if (isPlainValue(ty) || ast::isa<ast::AtomicType>(ty))
    return true;
  // A `mem::Buffer<T>` of plain values: read at any index, written at the
  // loop's variable only (checkParallelBufferCall), its object never
  // counted by the body.
  if (ast::Type *elem = bufferElementType(ty); elem && isPlainValue(elem))
    return true;
  // A handle: a `locked` cell or an `atomic shared` object.
  if (const ast::Qualifier q = qualifierOfVar(name);
      q.Form == ast::QualForm::Locked || q.Form == ast::QualForm::AtomicShared)
    return true;
  ast::Type *elem = nullptr;
  if (auto *a = ast::dyn_cast<ast::ArrayType>(ty))
    elem = a->getElementType();
  else if (auto *f = ast::dyn_cast<ast::FixedArrayType>(ty))
    elem = f->getElementType();
  else if (auto *s = ast::dyn_cast<ast::SliceType>(ty))
    elem = s->getElementType();
  if (elem && isPlainValue(elem))
    return true;
  error(loc, "'" + name + "' (a '" + typeName(ty) +
                 "') cannot be used in a 'parallel for' body: an iteration "
                 "uses only values, arrays of values and 'atomic' variables "
                 "of the enclosing function");
  return false;
}

bool Sema::checkParallelAssign(ast::AssignStmt *node, ast::Type *varTy) {
  const std::string &name = node->getVarName();
  ast::Expr *value = node->getValue();
  ast::Type *valTy = resolveExprType(value, varTy);
  if (!valTy)
    return false;
  // `s += e`, `s -= e` or `s *= e` on a number: a reduction.
  auto *bt = ast::dyn_cast<ast::BuiltinType>(varTy);
  auto *be = ast::dyn_cast<ast::BinaryExpr>(value);
  auto *lhs = be ? ast::dyn_cast<ast::Identifier>(be->getLHS()) : nullptr;
  const bool reduction = bt && bt->isNumeric() && lhs &&
                         lhs->getName() == name &&
                         (be->getOpcode() == ast::BinaryOpcode::Add ||
                          be->getOpcode() == ast::BinaryOpcode::Sub ||
                          be->getOpcode() == ast::BinaryOpcode::Mul);
  if (!reduction) {
    error(node->getLocation(),
          "cannot assign to '" + name +
              "' in a 'parallel for' body: a variable of the enclosing "
              "function changes only by a reduction ('" +
              name + " += e', '" + name + " -= e', '" + name +
              " *= e') or, if 'atomic', by its "
              "operations");
    return false;
  }
  if (!checkAssignable(varTy, valTy, value)) {
    error(node->getLocation(), "cannot assign value of type '" +
                                   typeName(valTy) + "' to variable '" + name +
                                   "' of type '" + typeName(varTy) + "'");
    return false;
  }
  ParallelStack.back().Loop->addReduction(Ctx.intern(name), be->getOpcode());
  return true;
}

// -- Atomic places (the prototype): a variable or a class field of an
// `atomic` type is a cell, and an assignment to it one atomic operation.

bool Sema::isPlaceChain(const ast::Expr *e) {
  if (ast::isa<ast::Identifier>(e))
    return true;
  if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e))
    return isPlaceChain(ma->getReceiver());
  return false;
}

bool Sema::samePlace(const ast::Expr *a, const ast::Expr *b) {
  const auto *ia = ast::dyn_cast<ast::Identifier>(a);
  const auto *ib = ast::dyn_cast<ast::Identifier>(b);
  if (ia || ib)
    return ia && ib && ia->getName() == ib->getName();
  const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(a);
  const auto *mb = ast::dyn_cast<ast::MemberAccessExpr>(b);
  return ma && mb && ma->getFieldName() == mb->getFieldName() &&
         samePlace(ma->getReceiver(), mb->getReceiver());
}

/// Whether @p e reads @p place: a walk of the operators, accesses and calls
/// of an expression (any read of an atomic place on the right of an `=` to
/// it would read and write the cell separately).
bool Sema::readsPlace(const ast::Expr *e, const ast::Expr *place) {
  if (!e)
    return false;
  if (samePlace(e, place))
    return true;
  if (const auto *be = ast::dyn_cast<ast::BinaryExpr>(e))
    return readsPlace(be->getLHS(), place) || readsPlace(be->getRHS(), place);
  if (const auto *ue = ast::dyn_cast<ast::UnaryExpr>(e))
    return readsPlace(ue->getOperand(), place);
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e))
    return readsPlace(te->getCondition(), place) ||
           readsPlace(te->getTrueExpr(), place) ||
           readsPlace(te->getFalseExpr(), place);
  if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e))
    return readsPlace(ma->getReceiver(), place);
  if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(e))
    return readsPlace(se->getArray(), place) ||
           readsPlace(se->getIndex(), place);
  if (const auto *ce = ast::dyn_cast<ast::CallExpr>(e)) {
    for (const ast::Expr *arg : ce->getArguments())
      if (readsPlace(arg, place))
        return true;
    return false;
  }
  if (const auto *mc = ast::dyn_cast<ast::MethodCallExpr>(e)) {
    if (readsPlace(mc->getReceiver(), place))
      return true;
    for (const ast::Expr *arg : mc->getArguments())
      if (readsPlace(arg, place))
        return true;
  }
  return false;
}

std::optional<ast::AssignStmt::AtomicKind>
Sema::checkAtomicValue(const ast::Expr *place, ast::Expr *value,
                       ast::AtomicType *at, ast::SourceLocation loc,
                       ast::Type *valTy) {
  using K = ast::AssignStmt::AtomicKind;
  const std::string what = placeText(place);
  ast::Type *inner = at->getInnerType();
  // `x += e` and `x -= e` (the parser's `x = x + e`): one atomic add.  The
  // other compound operators would read and write the cell separately.
  if (auto *be = ast::dyn_cast<ast::BinaryExpr>(value);
      be && samePlace(be->getLHS(), place)) {
    const bool add = be->getOpcode() == ast::BinaryOpcode::Add;
    const bool sub = be->getOpcode() == ast::BinaryOpcode::Sub;
    if (!add && !sub) {
      error(loc, "'" + what +
                     "' is 'atomic': this operator reads and writes "
                     "it separately; its atomic operations are '+=', '-=', "
                     "'exchange' and 'compareExchange'");
      return std::nullopt;
    }
    ast::Type *rhsTy = resolveExprType(be->getRHS(), inner);
    if (!rhsTy)
      return std::nullopt;
    if (!checkAssignable(inner, rhsTy, be->getRHS())) {
      error(loc, "cannot add a '" + typeName(rhsTy) + "' to '" + what +
                     "', an '" + typeName(at) + "'");
      return std::nullopt;
    }
    be->getLHS()->setResolvedType(inner); // the parser's copy: never read
    be->setResolvedType(inner);
    return add ? K::Add : K::Sub;
  }
  if (readsPlace(value, place)) {
    error(loc, "'" + what + "' is 'atomic': '" + what +
                   " = ...' reads and writes it separately; write '" + what +
                   " += e', or use 'exchange' or 'compareExchange'");
    return std::nullopt;
  }
  if (!valTy)
    valTy = resolveExprType(value, inner);
  if (!valTy)
    return std::nullopt;
  if (!checkAssignable(inner, valTy, value)) {
    error(loc, "cannot store a '" + typeName(valTy) + "' in '" + what +
                   "', an '" + typeName(at) + "'");
    return std::nullopt;
  }
  return K::Store;
}

bool Sema::checkAtomicAssign(ast::AssignStmt *node, ast::AtomicType *at) {
  // A `let` cell is read through its name and never changed by it: `=`,
  // `+=` and `-=` assign to it.
  if (varKind(node->getVarName()) == VarKind::Let) {
    error(node->getLocation(), "'" + node->getVarName() +
                                   "' is declared with 'let' and cannot be "
                                   "reassigned");
    return false;
  }
  auto kind = checkAtomicValue(node->getLHS(), node->getValue(), at,
                               node->getLocation());
  if (!kind)
    return false;
  node->setAtomicKind(*kind);
  warnAtomicEveryIteration(node->getLHS(), node->getLocation());
  return true;
}

void Sema::warnAtomicEveryIteration(const ast::Expr *place,
                                    ast::SourceLocation loc) {
  // The body's own statements run every iteration; one in an `if` or a
  // `match` inside it is in a scope of its own below the body's.
  if (ParallelStack.empty() ||
      CurrentScope->Parent != ParallelStack.back().Boundary)
    return;
  const ast::Identifier *root = placeRoot(place);
  if (!root || !isOuterScope(CurrentScope->findOwner(root->getName())))
    return;
  warning(loc, "'" + placeText(place) +
                   "' changes on every iteration of the 'parallel for': "
                   "each atomic operation moves its cache line between "
                   "cores, a thousand times a reduction's cost on 16 "
                   "threads (benchmarks/README.md); count in a reduction "
                   "(a plain variable of the enclosing function, '+='), "
                   "or change it less often");
}

bool Sema::checkAtomicMemberAssign(ast::MemberAssignStmt *node,
                                   ast::AtomicType *at) {
  using K = ast::AssignStmt::AtomicKind;
  ast::Expr *value = node->getValue();
  ast::Type *valTy = nullptr;
  // `self.f = p` in `__init__`, `p` an `atomic` place: the field takes its
  // cell (the default `__init__` is all such).
  const auto *recv = ast::dyn_cast<ast::Identifier>(node->getReceiver());
  const bool inInit = recv && recv->getName() == names::kSelf &&
                      CurrentClassCtx &&
                      CurrentClassCtx->MethodName == names::kMethodInit;
  if (inInit && isPlaceChain(value)) {
    WantAtomicHandle = true;
    valTy = resolveExprType(value, at->getInnerType());
    WantAtomicHandle = false;
    if (!valTy)
      return false;
    if (ast::isa<ast::AtomicType>(valTy)) {
      if (valTy != at) {
        error(node->getLocation(),
              "cannot give field '" + node->getFieldName() + "', an '" +
                  typeName(at) + "', the cell of an '" + typeName(valTy) + "'");
        return false;
      }
      node->setAtomicKind(K::Cell);
      return true;
    }
  }
  // Synthetic member access of the field: the place the value goes to.
  ast::MemberAccessExpr place(node->getLocation(), node->getReceiver(),
                              node->getFieldName());
  auto kind = checkAtomicValue(&place, value, at, node->getLocation(), valTy);
  if (!kind)
    return false;
  node->setAtomicKind(*kind);
  warnAtomicEveryIteration(&place, node->getLocation());
  return true;
}

ast::Type *Sema::checkAtomicMethodCall(ast::MethodCallExpr *node,
                                       ast::AtomicType *at) {
  const std::string &m = node->getMethodName();
  ast::Type *inner = at->getInnerType();
  const bool exchange = m == names::kMethodExchange;
  const bool compare = m == names::kMethodCompareExchange;
  if (!exchange && !compare) {
    error(node->getLocation(), "'" + typeName(at) + "' has no method '" + m +
                                   "': an atomic place has 'exchange(v)' and "
                                   "'compareExchange(expected, desired)'");
    return nullptr;
  }
  // Both change the cell, which a `let` variable's name never does (a cell
  // field of a `let` object is the object's own, and changes).
  if (const auto *id = ast::dyn_cast<ast::Identifier>(node->getReceiver());
      id && !id->isFunctionRef() && varKind(id->getName()) == VarKind::Let) {
    error(node->getLocation(), "'" + id->getName() +
                                   "' is declared with 'let' and cannot be "
                                   "changed by '" +
                                   m + "'");
    return nullptr;
  }
  const size_t want = exchange ? 1 : 2;
  if (node->getNumArguments() != want) {
    error(node->getLocation(), "method '" + m + "' expects " +
                                   std::to_string(want) + " argument(s), got " +
                                   std::to_string(node->getNumArguments()));
    return nullptr;
  }
  for (size_t i = 0; i < want; ++i) {
    ast::Expr *arg = node->getArguments()[i];
    ast::Type *ty = resolveExprType(arg, inner);
    if (!ty)
      return nullptr;
    if (!checkAssignable(inner, ty, arg)) {
      error(arg->getLocation(), "argument " + std::to_string(i + 1) + " of '" +
                                    m + "' has type '" + typeName(ty) +
                                    "', expected '" + typeName(inner) + "'");
      return nullptr;
    }
  }
  ast::Type *result = exchange ? inner : Ctx.getBoolTy();
  node->setResolvedType(result);
  return result;
}

// -- Copyable and the send check (docs/design/concurrency.md, "Copies and
// `move`", "The send check")

bool Sema::isCopyablePart(ast::Type *ty, ast::Qualifier q, std::string *why) {
  if (ty && ast::isRefType(ty))
    return q.Form == ast::QualForm::Shared ||
           q.Form == ast::QualForm::AtomicShared ||
           q.Form == ast::QualForm::Locked;
  return isCopyable(ty, why);
}

bool Sema::isCopyable(ast::Type *ty, std::string *why) {
  auto part = [&](const std::string &what, ast::Type *t, ast::Qualifier q) {
    if (why && why->empty())
      *why = what + " ('" + qualifiedName(q, t) + "')";
  };
  if (!ty || ast::isa<ast::PoisonType>(ty) || ast::isa<ast::BuiltinType>(ty) ||
      ast::isa<ast::AtomicType>(ty))
    return true;
  // A qualified type argument (#368): `T` with T = `shared Node` copies.
  if (auto *qt = ast::dyn_cast<ast::QualifiedType>(ty))
    return isCopyablePart(qt->getInnerType(), qt->getQualifier(), why);
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    return isCopyable(ot->getInnerType(), why);
  if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty)) {
    if (isCopyablePart(ft->getElementType(), ft->getElemQual(), why))
      return true;
    part("elements", ft->getElementType(), ft->getElemQual());
    return false;
  }
  // A container's storage, a `mem::Buffer<T>`: named as that, not by the
  // buffer's private field.
  if (ast::Type *elem = bufferElementType(ty)) {
    if (why && why->empty())
      *why = "storage ('" + typeName(ty) + "' of '" + typeName(elem) + "')";
    return false;
  }
  // A struct's fields carry qualifiers (a `shared` one copies), as a
  // tuple's elements do.  Before the tuple case, which a struct is too.
  if (auto *st = ast::dyn_cast<ast::StructType>(ty)) {
    const auto &types = st->getElementTypes();
    for (size_t i = 0; i < types.size() && i < st->getNumFields(); ++i) {
      const ast::Qualifier q = st->findFieldQualifier(st->getFieldName(i));
      if (!isCopyablePart(types[i], q, why)) {
        part("field '" + st->getFieldName(i) + "'", types[i], q);
        return false;
      }
    }
    return true;
  }
  if (auto *tt = ast::dyn_cast<ast::TupleType>(ty)) {
    const auto &types = tt->getElementTypes();
    for (size_t i = 0; i < types.size(); ++i)
      if (!isCopyablePart(types[i], tt->getElemQual(i), why)) {
        part("element '." + std::to_string(i) + "'", types[i],
             tt->getElemQual(i));
        return false;
      }
    return true;
  }
  if (auto *et = ast::dyn_cast<ast::EnumType>(ty)) {
    for (size_t i = 0; i < et->getNumVariants(); ++i)
      if (ast::StructType *p = et->getPayload(i); p && !isCopyable(p, why))
        return false;
    return true;
  }
  // A function value: its closure, when it has one, is shared by its
  // copies (a copy counts it), and holds copies of Copyable captures and
  // `weak` references.  One with no captures is a function's address.
  if (ast::isa<ast::FunctionType>(ty))
    return true;
  // `Str`, a class, a task: move-only.
  return false;
}

bool Sema::hasBuiltinTraitImpl(const ast::Type *ty, const char *trait) {
  auto has = [&](const ast::Type *t) {
    if (auto it = TraitImpls.find(t); it != TraitImpls.end())
      for (const auto &impl : it->second)
        if (impl.Trait->IsBuiltin && impl.Trait->Decl->getName() == trait)
          return true;
    // A block of this module not checked yet (checkTraitImpl records it):
    // the classes are populated before any is.
    const auto *ct = ast::dyn_cast<ast::ClassType>(t);
    const auto *st = ast::dyn_cast<ast::StructType>(t);
    if (!ct && !st)
      return false;
    auto it = ImplsByClass.find(ct ? ct->getName() : st->getName());
    if (it == ImplsByClass.end())
      return false;
    for (const ast::ImplDecl *im : it->second)
      if (im->getTrait() && namesBuiltinTrait(im->getTrait()) &&
          traitAnnotationName(im->getTrait()) == trait)
        return true;
    return false;
  };
  if (const auto *ct = ast::dyn_cast<ast::ClassType>(ty)) {
    for (const ast::ClassType *c = ct; c; c = c->getSuperClass())
      if (has(c))
        return true;
    return false;
  }
  return has(ty);
}

bool Sema::isCloneablePart(ast::Type *ty, ast::Qualifier q, std::string *why) {
  if (ty && ast::isRefType(ty) && q.Form != ast::QualForm::Unique)
    return true; // the same object in the clone
  return isCloneable(ty, why);
}

bool Sema::isCloneable(ast::Type *ty, std::string *why) {
  auto part = [&](const std::string &what, ast::Type *t, ast::Qualifier q) {
    if (why && why->empty())
      *why = "its " + what + " ('" + qualifiedName(q, t) + "') " +
             (what == "elements" ? "are" : "is") + " not Cloneable";
  };
  // Every Copyable type is Cloneable: its clone is the copy (one already
  // reported, with no type, too).
  if (!ty || isCopyable(ty) || ty == Ctx.getStrTy())
    return true;
  if (auto *qt = ast::dyn_cast<ast::QualifiedType>(ty))
    return isCloneablePart(qt->getInnerType(), qt->getQualifier(), why);
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    return isCloneable(ot->getInnerType(), why);
  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty)) {
    if (hasBuiltinTraitImpl(ct, names::kTraitCloneable))
      return true;
    if (why && why->empty())
      *why = "a class is Cloneable only with an 'impl Cloneable for " +
             ct->getName() + "' block";
    return false;
  }
  if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty)) {
    if (isCloneablePart(ft->getElementType(), ft->getElemQual()))
      return true;
    part("elements", ft->getElementType(), ft->getElemQual());
    return false;
  }
  // A struct with an `impl Cloneable for` block clones as it says; else a
  // struct's fields carry qualifiers.  Before the tuple case.
  if (auto *st = ast::dyn_cast<ast::StructType>(ty)) {
    if (hasBuiltinTraitImpl(st, names::kTraitCloneable))
      return true;
    const auto &types = st->getElementTypes();
    for (size_t i = 0; i < types.size() && i < st->getNumFields(); ++i) {
      const ast::Qualifier q = st->findFieldQualifier(st->getFieldName(i));
      if (!isCloneablePart(types[i], q)) {
        part("field '" + st->getFieldName(i) + "'", types[i], q);
        return false;
      }
    }
    return true;
  }
  if (auto *tt = ast::dyn_cast<ast::TupleType>(ty)) {
    const auto &types = tt->getElementTypes();
    for (size_t i = 0; i < types.size(); ++i)
      if (!isCloneablePart(types[i], tt->getElemQual(i))) {
        part("element '." + std::to_string(i) + "'", types[i],
             tt->getElemQual(i));
        return false;
      }
    return true;
  }
  if (ast::isa<ast::EnumType>(ty)) {
    if (why && why->empty())
      *why = "an enum whose payloads are not Copyable is not Cloneable yet";
    return false;
  }
  if (why && why->empty())
    *why = "a task is never Cloneable";
  return false;
}

ast::Type *Sema::bufferElementType(ast::Type *ty) const {
  const auto *st = ty ? ast::dyn_cast<ast::StructType>(ty) : nullptr;
  if (!st)
    return nullptr;
  const std::string &n = st->getName();
  const size_t lt = n.find('<');
  if (lt == std::string::npos)
    return nullptr;
  const std::string base = n.substr(0, lt);
  const bool named =
      base == "Buffer" ||
      (base.size() > 8 && base.compare(base.size() - 8, 8, "::Buffer") == 0);
  if (!named || st->getNumFields() != 1 || st->getFieldName(0) != "__data" ||
      st->getElementTypes()[0] != Ctx.getObjTy())
    return nullptr;
  // `view fn at(i: unsigned) -> view T`: its result is `T`, qualified as
  // T's argument is (#368).
  const ast::MethodDecl *at = st->findMethod("at");
  return at ? Ctx.getQualifiedType(at->getResultQual(), at->getReturnType())
            : nullptr;
}

bool Sema::checkParallelBufferCall(const ast::MethodCallExpr *node,
                                   const std::string &name) {
  const std::string &m = node->getMethodName();
  if (m == "at" || m == "get" || m == "len")
    return true;
  const std::string &var = ParallelStack.back().Loop->getVar()->getName();
  if (m == "set" && node->getNumArguments() == 2) {
    // `set(i, v)`, or `set(unsigned<int>(i), v)`: the index is the loop's
    // variable, possibly converted.
    const ast::Expr *idx = node->getArguments()[0];
    if (const auto *conv = ast::dyn_cast<ast::CallExpr>(idx);
        conv && conv->getNumArguments() == 1 &&
        isConversionTarget(conv->getCalleeName()))
      idx = conv->getArguments()[0];
    const auto *id = ast::dyn_cast<ast::Identifier>(idx);
    if (id && id->getName() == var)
      return true;
    error(node->getArguments()[0]->getLocation(),
          "'" + name +
              "' is written in a 'parallel for' body at an index other than "
              "the loop's variable '" +
              var + "': two iterations could write one element");
    return false;
  }
  error(node->getLocation(),
        "'" + name + "' (a '" +
            typeName(node->getReceiver()->getResolvedType()) +
            "' of the enclosing function) is read ('at', 'get', 'len') and "
            "written at the loop's variable ('set') in a 'parallel for' "
            "body; '" +
            m + "' could move its storage under the other iterations");
  return false;
}

std::string Sema::uncrossable(ast::Type *ty, ast::Qualifier q,
                              const std::string &path) {
  auto at = [&](const std::string &what) {
    return path.empty() ? what : "field '" + path + "' " + what;
  };
  if (!ty || ast::isa<ast::PoisonType>(ty) || ast::isa<ast::BuiltinType>(ty) ||
      ast::isa<ast::AtomicType>(ty))
    return "";
  if (ast::isa<ast::FunctionType>(ty))
    return at("is a function value, whose captures are not known");
  // A qualified type argument (#368): `mem::Buffer<shared Node>`'s elements
  // are `shared`.
  if (auto *qt = ast::dyn_cast<ast::QualifiedType>(ty))
    return uncrossable(qt->getInnerType(), qt->getQualifier(), path);
  if (auto *tt = ast::dyn_cast<ast::TaskType>(ty))
    return uncrossable(tt->getInnerType(), {}, path);
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    return uncrossable(ot->getInnerType(), q, path);
  if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty))
    return uncrossable(ft->getElementType(),
                       ft->getElemQual().isUnique() ? q : ft->getElemQual(),
                       path);
  if (auto *st = ast::dyn_cast<ast::StructType>(ty)) {
    // `mem::Buffer<T>` crosses when `T` does (the design's table): its one
    // field is the runtime's buffer object, whose walk freezes the
    // elements (PaykanBuffer_walk), so the check is of `T`, by `at`'s
    // result.
    if (ast::Type *elem = bufferElementType(st))
      return uncrossable(elem, {}, path);
    const auto &types = st->getElementTypes();
    for (size_t i = 0; i < types.size() && i < st->getNumFields(); ++i) {
      const std::string &f = st->getFieldName(i);
      std::string sub = path;
      if (!sub.empty())
        sub += '.';
      sub += f;
      std::string why = uncrossable(types[i], st->findFieldQualifier(f), sub);
      if (!why.empty())
        return why;
    }
    return "";
  }
  if (auto *tt = ast::dyn_cast<ast::TupleType>(ty)) {
    const auto &types = tt->getElementTypes();
    for (size_t i = 0; i < types.size(); ++i) {
      const std::string e = "." + std::to_string(i);
      std::string why = uncrossable(types[i], tt->getElemQual(i), path + e);
      if (!why.empty())
        return why;
    }
    return "";
  }
  if (auto *et = ast::dyn_cast<ast::EnumType>(ty)) {
    for (size_t i = 0; i < et->getNumVariants(); ++i)
      if (ast::StructType *p = et->getPayload(i))
        if (std::string why = uncrossable(p, {}, path); !why.empty())
          return why;
    return "";
  }
  // A reference: by its qualifier, then by what the object holds.
  if (q.Form == ast::QualForm::Shared)
    return at("is '" + qualifiedName(q, ty) +
              "': a 'shared' reference is aliased on its own thread");
  if (q.Form == ast::QualForm::Locked || q.Form == ast::QualForm::AtomicShared)
    return ""; // a handle: its contents were checked when it was made
  auto *ct = ast::dyn_cast<ast::ClassType>(ty);
  if (!ct || ct == Ctx.getStrTy())
    return "";
  if (ct == Ctx.getObjTy())
    return at("is an 'Obj', which may be any class");
  // A class's own and inherited fields.  A class reached again on the way
  // (a list's `next: Node?`) is being checked already.
  static thread_local std::vector<const ast::ClassType *> onTheWay;
  if (std::find(onTheWay.begin(), onTheWay.end(), ct) != onTheWay.end())
    return "";
  onTheWay.push_back(ct);
  std::string why;
  for (const ast::ClassType *c = ct; c && why.empty(); c = c->getSuperClass())
    for (const auto &[f, fty] : c->getFields()) {
      std::string sub = path;
      if (!sub.empty())
        sub += '.';
      sub += f;
      if (c->isWeakField(f)) {
        why = "field '" + sub + "' is 'weak': a weak load is a plain retain";
        break;
      }
      why = uncrossable(fty, c->findFieldQualifier(f), sub);
      if (!why.empty())
        break;
    }
  onTheWay.pop_back();
  return why;
}

bool Sema::checkSendArgs(const ast::CallExpr *call, const FunctionSig &sig) {
  if (!ParallelStack.empty()) {
    error(call->getLocation(),
          "a task cannot be started in a 'parallel for' body");
    return false;
  }
  const std::vector<ast::Expr *> &args = call->getArguments();
  bool ok = true;
  for (size_t i = 0; i < args.size() && i < sig.ParamTypes.size(); ++i) {
    ast::Type *p = sig.ParamTypes[i];
    ast::Expr *arg = args[i];
    ast::Type *ty = arg->getResolvedType();
    if (!ty || ast::isa<ast::AtomicType>(p) || ast::isa<ast::AtomicType>(ty))
      continue; // a cell: its handle is shared
    const ast::Qualifier q = qualifierOf(arg);
    auto *u = ast::dyn_cast<ast::UnaryExpr>(arg);
    const bool moved = u && u->getOpcode() == ast::UnaryOpcode::Move;
    const ast::Expr *inner = moved ? u->getOperand() : arg;
    const bool place = isPlaceChain(inner) ||
                       ast::isa<ast::SubscriptExpr>(inner) ||
                       ast::isa<ast::TupleIndexExpr>(inner);
    const std::string text = placeText(inner);
    // A handle (`locked`, `atomic shared`) is copied; a `shared`
    // reference never crosses; a unique one (`mut` or not) crosses by `move`,
    // or as a temporary of the caller's.
    if (ast::isRefType(ty)) {
      if (q.Form == ast::QualForm::Shared) {
        error(arg->getLocation(),
              "'" + text + "' is '" + qualifiedName(q, ty) +
                  "': it never crosses to a task (a 'shared' reference is "
                  "aliased on its own thread)");
        ok = false;
        continue;
      }
      if (q.Form == ast::QualForm::Locked ||
          q.Form == ast::QualForm::AtomicShared)
        continue; // a handle: another reference to the same cell or object
      if (place && !moved) {
        error(arg->getLocation(),
              "cannot copy '" + text + "' (a unique '" + typeName(ty) +
                  "') into a task: " +
                  (ast::isa<ast::Identifier>(inner)
                       ? "write 'move " + text + "' to transfer it"
                       : "move it out first ('move " + text +
                             "' of an owned variable, or 'mem::replace')"));
        ok = false;
        continue;
      }
    } else if (!sharesStorage(ty)) {
      continue; // a plain value: copied
    } else if (place && !moved) {
      // A value holding references: a Copyable one (shared parts, handles)
      // copies; one holding a unique part moves.
      std::string part;
      if (!isCopyable(ty, &part)) {
        std::string msg = "cannot copy '";
        msg += text;
        msg += "' (";
        if (part.empty()) {
          msg += "a move-only '";
          msg += typeName(ty);
          msg += "'";
        } else {
          msg += "a '";
          msg += typeName(ty);
          msg += "' holding ";
          msg += part;
        }
        msg += ") into a task: write 'move ";
        msg += text;
        msg += "' to transfer it";
        error(arg->getLocation(), msg);
        ok = false;
        continue;
      }
    }
    // What crosses holds nothing of this thread: no `shared` or `weak`
    // part, no function value, no `Obj`.
    if (std::string why = uncrossable(ty, q, ""); !why.empty()) {
      std::string msg = "cannot send '";
      msg += text;
      msg += "' (a '";
      msg += qualifiedName(q, ty);
      msg += "'): ";
      msg += why;
      error(arg->getLocation(), msg);
      ok = false;
    }
  }
  return ok;
}

bool Sema::checkBorrowQualifier(ast::Qualifier q, ast::ParamMode mode,
                                const std::string &what,
                                ast::SourceLocation loc) {
  if (mode == ast::ParamMode::Value || q.isUnique() || q.FromArgument)
    return true;
  // A borrow is written with no qualifier: it borrows the object whatever
  // its qualifier and counts nothing.  A `view` is read-only already, and
  // an `inout` of an immutable object could not change it.
  const char *m = ast::paramModeName(mode);
  error(loc, what + " is '" + m + " " + q.spelling() +
                 "': a borrow carries no qualifier; a 'view' takes every "
                 "form of the object (read-only through it) and an 'inout' "
                 "a 'mut' or 'shared mut' one, with no retain or release; "
                 "write '" +
                 m + " T'");
  return false;
}

bool Sema::checkWeakStore(const ast::Expr *src, ast::Qualifier want,
                          ast::Type *slotTy, const std::string &what,
                          ast::SourceLocation loc) {
  if (!src || ast::isa<ast::NoneLiteral>(src))
    return true;
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(src))
    return checkWeakStore(te->getTrueExpr(), want, slotTy, what, loc) &&
           checkWeakStore(te->getFalseExpr(), want, slotTy, what, loc);
  const ast::Qualifier q = qualifierOf(src);
  if (q == want)
    return true;
  ast::Type *ty = const_cast<ast::Expr *>(src)->getResolvedType();
  const char *form = want.Mut ? "weak mut" : "weak";
  const auto *id = ast::dyn_cast<ast::Identifier>(src);
  std::string msg = "cannot store '" + placeText(src) + "' (a '" +
                    qualifiedName(q, ty) + "') in " + what + ", a '" + form +
                    " " + typeName(slotTy) + "': a '" + form +
                    "' reference names a '" + qualifiedName(want, nullptr) +
                    "' object";
  if (id && id->getName() == names::kSelf && CurrentClassCtx &&
      CurrentClassCtx->MethodName == names::kMethodInit)
    msg += "; an initializer's 'self' is the new object, which its caller "
           "holds as it declares";
  else if (id && id->getName() == names::kSelf)
    msg += std::string("; a '") + (want.Mut ? "shared fn" : "shared view fn") +
           "' has a '" + qualifiedName(want, nullptr) + "' 'self'";
  else if (id && q.Form == ast::QualForm::Unique)
    msg +=
        "; declare '" + id->getName() + "' '" + qualifiedName(want, ty) + "'";
  error(loc, msg);
  return false;
}

bool Sema::checkBorrowQualifiers(const ast::ParamModes &modes,
                                 const std::vector<ast::Qualifier> &quals,
                                 const std::vector<ast::Expr *> &args,
                                 const std::string &callee) {
  bool ok = true;
  for (size_t i = 0; i < args.size(); ++i) {
    if (modes.mode(i) == ast::ParamMode::Value)
      continue;
    ast::Type *ty = args[i]->getResolvedType();
    if (!ty || !ast::isRefType(ty))
      continue;
    // A `shared` object passes to a `view` or an `inout` as a unique one
    // does, with no retain or release; an immutable one is frozen
    // (inoutPlaceError) and a cell is reached in a `lock` block.
    const ast::Qualifier q = qualifierOf(args[i]);
    const std::string text = placeText(args[i]);
    const char *mode = ast::paramModeName(modes.mode(i));
    // A borrow whose form a template's type argument gave (`view T`,
    // `inout T` with T = `shared Node`, #368) reads as that form, and the
    // callee may keep a copy of it (its clone is the copy) or trade it for
    // another (`mem::replace`): it takes a reference of exactly that form.
    // A `view` also takes a fresh unique value, which nothing else holds and
    // which becomes that form (frozen, for `atomic shared`).  An `inout` of
    // a `mut` T takes a unique one, a `view` of one any form.
    const ast::Qualifier want = i < quals.size() ? quals[i] : ast::Qualifier{};
    const bool copied =
        want.isSharedForm() || want.Form == ast::QualForm::Locked;
    if (want.FromArgument &&
        ((modes.mode(i) == ast::ParamMode::Inout && !want.isUnique()) ||
         (modes.mode(i) == ast::ParamMode::View && copied))) {
      if (!checkBorrowForm(args[i], want, modes.mode(i),
                           "parameter '" + modes.name(i) + "' of '" + callee +
                               "'"))
        ok = false;
      continue;
    }
    // A `view shared` parameter is a weak field's in a generated `__init__`
    // (a program writes none): a `weak` field names a `shared` object, a
    // `weak mut` one a `shared mut` object.
    if (modes.mode(i) == ast::ParamMode::View && i < quals.size() &&
        quals[i].Form == ast::QualForm::Shared &&
        !ast::isa<ast::NoneLiteral>(args[i]) && !(q == quals[i])) {
      const char *form = quals[i].Mut ? "weak mut" : "weak";
      std::string msg = "cannot pass '";
      msg += text;
      msg += "' (a '";
      msg += qualifiedName(q, ty);
      msg += "') to parameter '";
      msg += modes.name(i);
      msg += "' of '";
      msg += callee;
      msg += "', a '";
      msg += form;
      msg += "' field's: a '";
      msg += form;
      msg += "' reference names a '";
      msg += qualifiedName(quals[i], nullptr);
      msg += "' object";
      error(args[i]->getLocation(), msg);
      ok = false;
      continue;
    }
    if (q.Form == ast::QualForm::Locked) {
      std::string msg = "'";
      msg += text;
      msg += "' is 'locked': a cell's contents are reached in a 'lock' "
             "block, not borrowed by '";
      msg += mode;
      msg += "' parameter '";
      msg += modes.name(i);
      msg += "' of '";
      msg += callee;
      msg += "'";
      error(args[i]->getLocation(), msg);
      ok = false;
    }
  }
  return ok;
}

bool Sema::isFreshValue(const ast::Expr *e) {
  if (!e)
    return false;
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e)) {
    // A returned conditional hands on the locals its branches name.
    const ast::Expr *returning = ImplicitMoveSource;
    if (e == returning)
      ImplicitMoveSource = te->getTrueExpr();
    const bool t = isFreshValue(te->getTrueExpr());
    if (e == returning)
      ImplicitMoveSource = te->getFalseExpr();
    const bool f = isFreshValue(te->getFalseExpr());
    ImplicitMoveSource = returning;
    return t && f;
  }
  if (ast::isa<ast::TupleLiteralExpr>(e) ||
      ast::isa<ast::ArrayLiteralExpr>(e) || ast::isa<ast::NoneLiteral>(e))
    return true;
  if (const auto *u = ast::dyn_cast<ast::UnaryExpr>(e))
    return u->getOpcode() == ast::UnaryOpcode::Move;
  // A local or a by-value parameter a `return` hands on (its last use).
  if (e == ImplicitMoveSource && isOwnedBinder(e))
    return true;
  // A call's result by value is the caller's own; a borrowed one is not.
  if (ast::isa<ast::CallExpr>(e) || ast::isa<ast::MethodCallExpr>(e) ||
      ast::isa<ast::CallValueExpr>(e))
    return !borrowResultOf(e) && !e->isInoutResult();
  return false;
}

bool Sema::checkBorrowForm(ast::Expr *arg, ast::Qualifier want,
                           ast::ParamMode mode, const std::string &what) {
  if (!arg || ast::isa<ast::NoneLiteral>(arg))
    return true;
  // Each branch of a conditional is a reference the borrow may name.
  if (auto *te = ast::dyn_cast<ast::TernaryExpr>(arg)) {
    const bool t = checkBorrowForm(te->getTrueExpr(), want, mode, what);
    return checkBorrowForm(te->getFalseExpr(), want, mode, what) && t;
  }
  const ast::Qualifier have = qualifierOf(arg);
  if (have == want)
    return true;
  // A fresh unique value (`Node(1)`, a unique result, `move u`) becomes
  // the form, as it would flowing into a slot of it.
  if (mode == ast::ParamMode::View && have.Form == ast::QualForm::Unique &&
      isFreshValue(arg))
    return checkQualifiedFlow(want, arg, what, arg->getLocation(),
                              /*implicitMove=*/false, /*materialize=*/true);
  ast::Type *ty = arg->getResolvedType();
  error(arg->getLocation(),
        "cannot pass '" + placeText(arg) + "' (a '" + qualifiedName(have, ty) +
            "') to " + what + ", " +
            (mode == ast::ParamMode::Inout ? "an '" : "a '") +
            ast::paramModeName(mode) + " " + qualifiedName(want, ty) +
            "': a borrow of a type argument's form takes that form, which the "
            "callee may keep a copy of");
  return false;
}

// -- Moves (docs/design/concurrency.md, "Use after move")

std::string Sema::varKey(const Scope *owner, std::string_view name) {
  std::string key(name);
  key += '#';
  key += std::to_string(owner ? owner->Id : 0);
  return key;
}

std::string Sema::varKey(std::string_view name) const {
  return varKey(CurrentScope ? CurrentScope->findOwner(name) : nullptr, name);
}

std::string_view Sema::keyName(std::string_view key) {
  return key.substr(0, key.rfind('#'));
}

unsigned Sema::keyScope(std::string_view key) {
  return static_cast<unsigned>(
      std::stoul(std::string(key.substr(key.rfind('#') + 1))));
}

Sema::MatchMovesGuard::MatchMovesGuard(Sema &s, ast::MatchStmt *node) : S(s) {
  S.MatchMoveStack.push_back({node, S.MovedVars, {}, true});
}

Sema::MatchMovesGuard::~MatchMovesGuard() {
  MatchMoves m = std::move(S.MatchMoveStack.back());
  S.MatchMoveStack.pop_back();
  // A match no arm of which takes the subject falls through as it came.
  if (!detail::matchIsExhaustive(m.Node)) {
    m.Arms.push_back(std::move(m.Entry));
    m.AllEnded = false;
  }
  S.joinMoves(m.Arms, "match", m.Node->getLocation());
  S.FlowEnded = m.AllEnded;
}

Sema::ArmMovesGuard::ArmMovesGuard(Sema &s) : S(s) {
  S.MovedVars = S.MatchMoveStack.back().Entry;
  S.FlowEnded = false;
}

Sema::ArmMovesGuard::~ArmMovesGuard() {
  MatchMoves &m = S.MatchMoveStack.back();
  if (S.FlowEnded)
    return;
  m.Arms.push_back(S.MovedVars);
  m.AllEnded = false;
}

void Sema::joinMoves(const std::vector<MovedMap> &paths, const char *what,
                     ast::SourceLocation loc) {
  if (paths.empty())
    return; // nothing after is reached
  MovedMap joined;
  for (const MovedMap &p : paths)
    for (const auto &[name, st] : p) {
      auto it = joined.find(name);
      if (it == joined.end())
        joined[name] = st;
      else if (st.Maybe && !it->second.Maybe)
        it->second = st;
    }
  for (auto &[name, st] : joined) {
    if (st.Maybe)
      continue;
    const bool onEveryPath =
        std::all_of(paths.begin(), paths.end(),
                    [&](const MovedMap &p) { return p.count(name) != 0; });
    if (!onEveryPath) {
      st.Maybe = true;
      st.By = std::string("the '") + what + "' at " + at(loc);
    }
  }
  MovedVars = std::move(joined);
}

void Sema::beginLoopMoves(ast::SourceLocation loc) {
  LoopMoveStack.push_back({loc, LoopDepth + 1, MovedVars, {}, CurrentScope});
  FlowEnded = false;
}

void Sema::finishLoopMoves() {
  LoopMoves loop = std::move(LoopMoveStack.back());
  LoopMoveStack.pop_back();
  // The body's end is an iteration's end.
  if (!FlowEnded)
    checkLoopIteration(loop, loop.Loc);
  // After the loop: no iteration at all, the last one's end, or a `break`.
  std::vector<MovedMap> paths{std::move(loop.Entry)};
  if (!FlowEnded)
    paths.push_back(MovedVars);
  for (MovedMap &b : loop.Breaks)
    paths.push_back(std::move(b));
  joinMoves(paths, "loop", loop.Loc);
  FlowEnded = false;
}

void Sema::checkLoopIteration(const LoopMoves &loop, ast::SourceLocation loc) {
  // The scopes the loop stands in: their variables are the enclosing
  // function's.  The body's own (its locals, an arm's or a lock block's
  // binding) are declared anew each iteration.
  std::set<unsigned> outer;
  for (const Scope *s = loop.Outer; s; s = s->Parent)
    outer.insert(s->Id);
  for (const auto &[key, st] : MovedVars) {
    if (loop.Entry.count(key))
      continue; // moved before the loop: reported at its use
    if (auto d = DeclLoopDepth.find(key);
        (d != DeclLoopDepth.end() && d->second >= loop.Depth) ||
        !outer.count(keyScope(key)))
      continue;
    if (!LoopMovesReported.insert(key + "@" + at(loop.Loc)).second)
      continue;
    const std::string name(keyName(key));
    error(st.At, "'" + name + "' is moved in the loop at " + at(loop.Loc) +
                     " and not assigned again before the next iteration" +
                     (at(loc) == at(loop.Loc)
                          ? ""
                          : " (the 'continue' at " + at(loc) + ")"));
  }
}

ast::Type *Sema::checkPartialMove(ast::UnaryExpr *node,
                                  ast::MemberAccessExpr *ma) {
  const ast::SourceLocation loc = node->getLocation();
  const std::string what = placeText(ma);
  // The root: an owned local or by-value parameter.
  const ast::Expr *e = ma;
  while (const auto *link = ast::dyn_cast<ast::MemberAccessExpr>(e))
    e = link->getReceiver();
  const auto *root = ast::dyn_cast<ast::Identifier>(e);
  if (!root || root->getName() == names::kSelf) {
    error(loc, "cannot move '" + what +
                   "': 'move' takes a variable, or a field of one it owns" +
                   (root ? "; write 'mem::replace(" + what + ", v)'" : ""));
    return nullptr;
  }
  const std::string &name = root->getName();
  switch (varKind(name)) {
  case VarKind::Plain:
  case VarKind::Let:
    break;
  default:
    error(loc, "cannot move '" + what + "': '" + name +
                   "' is not an owned variable (a borrow, a capture or a "
                   "'weak' reference); write 'mem::replace(" +
                   what + ", v)'");
    return nullptr;
  }
  ast::Type *ty = resolveExprType(ma); // the root live, not moved, not lent
  if (!ty)
    return nullptr;
  // Every link a unique field: a `shared` one is an alias, a
  // `weak` one no owner.
  for (const ast::Expr *x = ma;
       const auto *link = ast::dyn_cast<ast::MemberAccessExpr>(x);
       x = link->getReceiver()) {
    const ast::Qualifier q = qualifierOf(link);
    const auto *ct = ast::dyn_cast<ast::ClassType>(
        ast::stripOptional(link->getReceiver()->getResolvedType()));
    if (ct && ct->isWeakField(link->getFieldName())) {
      error(loc, "cannot move '" + placeText(link) +
                     "': it is a 'weak' field, which owns nothing");
      return nullptr;
    }
    // In a template's code, the part moved holding a type argument's
    // Copyable form (`value: T` with T = `shared Node`, #368) moves as its
    // copy, as a Copyable value does, and stays: `move n.value` of a node of
    // a `LinkedList<shared Node>`.  The links on the way are unique.
    if (q.Form != ast::QualForm::Unique && x == ma && inInstanceCode() &&
        q.FromArgument && isCopyablePart(link->getResolvedType(), q)) {
      node->setMoveIsCopy(true);
      continue;
    }
    if (q.Form != ast::QualForm::Unique) {
      error(loc, "cannot move '" + placeText(link) + "': it is '" +
                     qualifiedName(q, link->getResolvedType()) +
                     "', not a unique part of '" + name + "'");
      return nullptr;
    }
  }
  // A value moves out as a copy of its bytes, which takes over the
  // references among them; a Copyable one is the copy, so that generic code
  // writes `move x.f` for any type (#369).  `x` ends either way.
  if (std::string why = viewedBy(name); !why.empty()) {
    error(loc, why + "; it cannot be moved out of");
    return nullptr;
  }
  MovedVars[varKey(name)] = MovedState{loc, false, ""};
  return ty;
}

bool Sema::checkMovable(const std::string &name, ast::SourceLocation loc,
                        const char *what) {
  const std::string head = std::string("cannot ") + what + " '" + name + "'";
  switch (varKind(name)) {
  case VarKind::Plain:
  case VarKind::Let:
  case VarKind::ForVar: // an integer: read as it is
    return true;
  case VarKind::Inout:
    error(loc, head + ": it is an 'inout' parameter, the caller's storage");
    return false;
  case VarKind::View:
  case VarKind::ViewLocal:
  case VarKind::ViewBinding:
    error(loc, head + ": it is a 'view', a borrow");
    return false;
  case VarKind::Weak:
  case VarKind::WeakView:
    error(loc, head + ": a 'weak' variable holds no strong reference");
    return false;
  case VarKind::Captured:
  case VarKind::CapturedWeak:
    error(loc, head + ": the function captures it by copy");
    return false;
  }
  return true;
}

ast::Type *Sema::checkMove(ast::UnaryExpr *node) {
  // `move x.f` (the prototype): the field leaves, and x ends.
  if (auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(node->getOperand()))
    return checkPartialMove(node, ma);
  auto *id = ast::dyn_cast<ast::Identifier>(node->getOperand());
  if (!id) {
    ast::Expr *operand = node->getOperand();
    if (ast::isa<ast::SubscriptExpr>(operand) ||
        ast::isa<ast::TupleIndexExpr>(operand)) {
      error(node->getLocation(),
            "cannot move '" + placeText(operand) +
                "': an element leaves by 'mem::replace' or 'mem::take'");
      return nullptr;
    }
    // A conditional moves a branch: write `move` in each (#368: as a whole
    // it moves neither).
    if (ast::isa<ast::TernaryExpr>(operand)) {
      error(node->getLocation(),
            "cannot move a conditional: write 'move' in each branch that "
            "names a variable");
      return nullptr;
    }
    // `move` before a temporary (a call, a constructor, a literal): it is
    // owned already, and nothing happens.  A borrowed result names storage
    // another owner holds, which a move would not take from it.
    ast::Type *ty = resolveExprType(operand);
    if (ty && (borrowResultOf(operand) || operand->isInoutResult())) {
      error(node->getLocation(),
            "cannot move '" + placeText(operand) +
                "': it is a borrow of storage its owner keeps; write "
                "'mem::replace' or 'mem::take' to take it from there");
      return nullptr;
    }
    return ty;
  }
  const std::string &name = id->getName();
  if (name == names::kSelf) {
    error(node->getLocation(),
          "cannot move 'self': a method borrows its receiver");
    return nullptr;
  }
  if (!checkMovable(name, node->getLocation(), "move"))
    return nullptr;
  ast::Type *ty = resolveExprType(id); // live, not moved, not lent inout
  if (!ty)
    return nullptr;
  // A move is a write: not while a `view` local reads the variable.
  if (std::string why = viewedBy(name); !why.empty()) {
    error(node->getLocation(), why + "; it cannot be moved");
    return nullptr;
  }
  // (A move in a loop of a variable declared outside it is checked at the
  // iteration's end: checkLoopIteration.)
  MovedVars[varKey(name)] = MovedState{node->getLocation(), false, ""};
  return ty;
}

bool Sema::checkNotMoved(const std::string &name, ast::SourceLocation loc) {
  auto moved = MovedVars.find(varKey(name));
  if (moved == MovedVars.end())
    return true;
  const MovedState &st = moved->second;
  if (st.Maybe)
    error(loc, "'" + name + "' may have been moved (by " + st.By + ", at " +
                   at(st.At) + "); it cannot be used after a move");
  else if (st.By == "await")
    error(loc, "'" + name + "' was awaited at " + at(st.At) +
                   "; a task handle is spent by its 'await'");
  else
    error(loc, "'" + name + "' was moved at " + at(st.At) +
                   "; it cannot be used after the move");
  return false;
}

void Sema::checkTasksAwaited(const Scope *s, const char *exit,
                             ast::SourceLocation loc) {
  for (const auto &[name, ty] : s->Locals) {
    if (!ty || !ast::isa<ast::TaskType>(ty))
      continue;
    auto moved = MovedVars.find(varKey(s, name));
    if (moved != MovedVars.end() && !moved->second.Maybe)
      continue;
    auto decl = TaskLocals.find(varKey(s, name));
    const ast::SourceLocation where = exit                       ? loc
                                      : decl != TaskLocals.end() ? decl->second
                                                                 : loc;
    const std::string when =
        exit ? std::string(" before this '") + exit + "'" : "";
    if (moved == MovedVars.end())
      error(where, "task '" + name + "' is " +
                       (exit ? "not awaited" + when : "never awaited") +
                       ": a task handle is spent by 'await' (or 'move') on "
                       "every path before its scope ends; the prototype "
                       "has no 'join', 'cancel' or 'detach'");
    else
      error(where, "task '" + name + "' may not be awaited" +
                       (exit ? when : " when its scope ends") +
                       ": it is spent on some paths only (by " +
                       moved->second.By + ", at " + at(moved->second.At) +
                       "); every path awaits it, or moves it");
  }
}

void Sema::checkTasksAwaitedUpTo(const Scope *stop, const char *exit,
                                 ast::SourceLocation loc) {
  for (const Scope *s = CurrentScope; s && s != stop; s = s->Parent)
    checkTasksAwaited(s, exit, loc);
}

// -- Qualifiers on places (docs/design/concurrency.md, "The qualifiers")

ast::Qualifier Sema::qualifierOfVar(std::string_view name) const {
  for (const Scope *s = CurrentScope; s; s = s->Parent) {
    if (!s->contains(name))
      continue;
    auto it = s->Quals.find(name);
    return it == s->Quals.end() ? ast::Qualifier{} : it->second;
  }
  return {};
}

ast::Qualifier Sema::qualifierOf(const ast::Expr *e) {
  if (!e)
    return {};
  // A conditional is held as both its branches are: a unique one joins a
  // `shared` one's form (it converts into it); two writable unique ones
  // are writable.  Branches of two other forms are each checked as they
  // flow (checkQualifiedFlow).
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e)) {
    ast::Qualifier t = qualifierOf(te->getTrueExpr());
    const ast::Qualifier f = qualifierOf(te->getFalseExpr());
    if (t.Form == ast::QualForm::Unique && f.Form == ast::QualForm::Unique) {
      t.Mut = t.Mut && f.Mut;
      return t;
    }
    if (t.Form == ast::QualForm::Unique)
      return f;
    return t;
  }
  if (const auto *u = ast::dyn_cast<ast::UnaryExpr>(e))
    return u->getOpcode() == ast::UnaryOpcode::Move
               ? qualifierOf(u->getOperand())
               : ast::Qualifier{};
  if (const auto *id = ast::dyn_cast<ast::Identifier>(e))
    return id->isFunctionRef() ? ast::Qualifier{}
                               : qualifierOfVar(id->getName());
  if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e)) {
    const ast::Qualifier recv = qualifierOf(ma->getReceiver());
    ast::Type *rt = ast::stripOptional(
        const_cast<ast::Expr *>(ma->getReceiver())->getResolvedType());
    ast::Qualifier q;
    bool recvMut = recv.Mut;
    if (const auto *ct = ast::dyn_cast<ast::ClassType>(rt)) {
      q = ct->findFieldQualifier(ma->getFieldName());
    } else if (const auto *st = ast::dyn_cast<ast::StructType>(rt)) {
      q = st->findFieldQualifier(ma->getFieldName());
      // A struct is a value: the objects it holds follow the variable
      // that holds it (its `let`), or the object it is part of.
      recvMut = valueRootWritable(ma->getReceiver());
    }
    // A unique field follows its root: written under a `mut` receiver,
    // immutable under an immutable one.  A `shared` field is the object's
    // own form, kept through any root (`shared mut` is written through
    // every alias); a cell and a frozen field are what they are.
    if (q.Form == ast::QualForm::Unique)
      q.Mut = recvMut;
    return q;
  }
  if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(e)) {
    // An element declared `shared` (#368) is that object's form through
    // any root, as a `shared` field is.
    const ast::Type *arr = se->getArray()->getResolvedType();
    ast::Qualifier part;
    if (const auto *ft =
            arr ? ast::dyn_cast<ast::FixedArrayType>(arr) : nullptr)
      part = ft->getElemQual();
    else if (const auto *st =
                 arr ? ast::dyn_cast<ast::SliceType>(arr) : nullptr)
      part = st->getElemQual();
    if (!part.isUnique()) {
      // A type argument's form, in a template's code (an element of a
      // `T[]` with T = `shared Node`).
      part.FromArgument = isArgumentForm(
          part, const_cast<ast::SubscriptExpr *>(se)->getResolvedType());
      return part;
    }
    // An element of a fixed-size array (a value) follows the variable that
    // holds the array; a slice's its array's writability (`inout T[]`).
    ast::Qualifier q = qualifierOf(se->getArray());
    if (q.Form == ast::QualForm::Unique)
      q.Mut = valueRootWritable(se->getArray());
    return q;
  }
  if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(e)) {
    const ast::Type *tup = ti->getTuple()->getResolvedType();
    if (const auto *tt = tup ? ast::dyn_cast<ast::TupleType>(tup) : nullptr;
        tt && !tt->getElemQual(ti->getIndex()).isUnique()) {
      ast::Qualifier part = tt->getElemQual(ti->getIndex());
      part.FromArgument =
          isArgumentForm(part, tt->getElementType(ti->getIndex()));
      return part;
    }
    ast::Qualifier q = qualifierOf(ti->getTuple());
    if (q.Form == ast::QualForm::Unique)
      q.Mut = valueRootWritable(ti->getTuple());
    return q;
  }
  // A call through a function value: its type's result (#368).
  if (auto it = ValueCallResultQuals.find(e); it != ValueCallResultQuals.end())
    return it->second;
  if (const auto *ce = ast::dyn_cast<ast::CallExpr>(e)) {
    if (ce->calleeIsValue())
      return {};
    if (const FunctionSig *sig = lookupFunction(ce->getCalleeName()))
      return sig->ResultQual;
    return {};
  }
  if (const auto *mc = ast::dyn_cast<ast::MethodCallExpr>(e)) {
    // A Copyable reference's clone is the copy: the same object, held as
    // the receiver is.
    if (mc->cloneIsCopy())
      return qualifierOf(mc->getReceiver());
    ast::Type *rt = ast::stripOptional(
        const_cast<ast::Expr *>(mc->getReceiver())->getResolvedType());
    if (const auto *ct = ast::dyn_cast<ast::ClassType>(rt))
      if (const ast::MethodDecl *m = ct->findMethod(mc->getMethodName()))
        return m->getResultQual();
    // A struct's method too (`mem::Buffer<shared Node>`'s `pop`).
    if (const auto *st = ast::dyn_cast<ast::StructType>(rt))
      if (const ast::MethodDecl *m = st->findMethod(mc->getMethodName()))
        return m->getResultQual();
    return {};
  }
  return {};
}

std::string Sema::qualifiedName(ast::Qualifier q, ast::Type *ty) {
  std::string s = q.spelling();
  if (!s.empty() && ty)
    s += ' ';
  return s + (ty ? typeName(ty) : std::string());
}

bool Sema::checkQualifierApplies(ast::Qualifier q, ast::Type *inner,
                                 ast::SourceLocation loc) {
  if (q.isUnique() || !inner)
    return true;
  // A struct, a tuple or an array is a value wherever it is, whatever it
  // holds: it takes no qualifier.  One to share, or to freeze for every
  // thread, goes in a `mem::Box<T>`, an object (`atomic shared
  // mem::Box<Buffer<float>>`).
  const bool ref = ast::isRefType(inner) || ast::isa<ast::AtomicType>(inner);
  // A weak declaration's form is its parser's: what it may name is
  // checkWeakType's to say.
  if (ResolvingWeakDecl && q.Form == ast::QualForm::Shared && !ref)
    return true;
  // A function value is one kind of value: its closure, when it has one,
  // is shared by its copies, and nothing changes it.
  if (ast::isFunctionValue(inner)) {
    error(loc, std::string("a function value takes no qualifier ('") +
                   q.spelling() + " " + typeName(inner) +
                   "'): it is Copyable, and its copies share its closure");
    return false;
  }
  const std::string value =
      "'" + typeName(inner) + "' is a value" +
      (sharesStorage(inner) ? ": box it ('mem::Box<" + typeName(inner) +
                                  ">') to share what it holds"
                            : "");
  switch (q.Form) {
  case ast::QualForm::Unique: // `mut`
    if (!ref) {
      error(loc, "'mut' applies to objects; '" + typeName(inner) +
                     "' is a value: its storage is rewritten by assignment, "
                     "which 'let' fixes");
      return false;
    }
    return true;
  case ast::QualForm::Shared:
    if (!ref) {
      error(loc, std::string("'") + q.spelling() + "' qualifies a reference; " +
                     value);
      return false;
    }
    return true;
  case ast::QualForm::AtomicShared:
    if (!ref) {
      error(loc, "'atomic shared' qualifies a reference; " + value);
      return false;
    }
    return true;
  case ast::QualForm::Locked:
    if (!ast::isRefType(inner)) {
      error(loc, std::string("'") + q.spelling() +
                     "' holds an object in the prototype, not a '" +
                     typeName(inner) + "' (wrap the value in a class)");
      return false;
    }
    // The lock buys the writes: without `mut` it locks what nothing
    // writes, which a frozen object gives with no lock.
    if (!q.Mut)
      warning(loc, "'locked " + typeName(inner) +
                       "' locks an object nothing writes; 'atomic shared " +
                       typeName(inner) +
                       "' is read from every thread with no lock");
    return true;
  }
  return true;
}

bool Sema::isArgumentForm(ast::Qualifier q, ast::Type *ty) const {
  if (!CurrentInstanceArgs || !ty || q.isUnique() ||
      !(q.isSharedForm() || q.Form == ast::QualForm::Locked))
    return false;
  for (const ast::Type *a : *CurrentInstanceArgs)
    if (const auto *qt = ast::dyn_cast<ast::QualifiedType>(a);
        qt && qt->getQualifier() == q && qt->getInnerType() == ty)
      return true;
  return false;
}

bool Sema::joinQualifiers(ast::Qualifier outer, ast::Qualifier inner,
                          ast::Qualifier &out) {
  // docs/design/concurrency.md, "Generics and Copyable": a qualifier
  // written on `T` joins T's own, as `T?` joins T's None.
  const ast::Qualifier mut{ast::QualForm::Unique, true};
  if (outer.isUnique() || outer == inner) {
    out = inner;
    return true;
  }
  if (inner.isUnique() || inner == mut) { // a unique argument takes it
    out = outer;
    return true;
  }
  // `mut T` with T = `shared mut Node` writes through what is written
  // through already.
  if (outer == mut && inner == ast::Qualifier{ast::QualForm::Shared, true}) {
    out = inner;
    return true;
  }
  return false;
}

bool Sema::checkPartQualifier(ast::Qualifier q, PartKind kind,
                              ast::SourceLocation loc) {
  if (q.isUnique())
    return true;
  const std::string spelled = std::string("'") + q.spelling() + "'";
  switch (kind) {
  case PartKind::TypeArgument:
    // A template's code reaches its `T` as it is: a cell's contents are
    // reached in a `lock` block, which it does not know.
    if (q.Form == ast::QualForm::Locked) {
      error(loc, spelled +
                     " cannot qualify a type argument yet: a cell's contents "
                     "are reached in a 'lock' block, which a template's code "
                     "does not write");
      return false;
    }
    return true;
  case PartKind::Element:
    if (q.Form == ast::QualForm::Unique) { // `mut`
      error(loc, "an element takes no 'mut': it is written as its variable "
                 "is (a 'let' variable's never), as a unique field is as its "
                 "object is");
      return false;
    }
    if (q.Form != ast::QualForm::Shared) {
      error(loc, spelled +
                     " cannot qualify an element yet: a tuple's or an array's "
                     "element is 'shared' or 'shared mut' (a frozen object "
                     "or a cell goes in a struct's field)");
      return false;
    }
    return true;
  case PartKind::FunctionPart:
    if (q.Form == ast::QualForm::AtomicShared ||
        q.Form == ast::QualForm::Locked) {
      error(loc, spelled +
                     " cannot qualify a function type's parameter or result "
                     "yet: it takes 'mut', 'shared' or 'shared mut'");
      return false;
    }
    return true;
  }
  return true;
}

ast::Type *Sema::qualifyElements(ast::Qualifier q, ast::Type *ty,
                                 ast::SourceLocation loc, bool &failed) {
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(ty)) {
    ast::Type *inner = qualifyElements(q, ot->getInnerType(), loc, failed);
    return inner ? Ctx.getOptionalType(inner) : nullptr;
  }
  // The innermost array's elements, joined with their own qualifier (a
  // template's `shared T[4]` with T = `shared Node`).
  auto element = [&](ast::Type *elem, ast::Qualifier held,
                     ast::Qualifier &out) {
    if (!ast::isRefType(elem))
      return false;
    if (joinQualifiers(q, held, out) &&
        checkPartQualifier(out, PartKind::Element, loc))
      return true;
    if (!failed)
      error(loc, std::string("'") + q.spelling() +
                     "' cannot qualify elements held '" + held.spelling() +
                     "': a type takes one qualifier");
    failed = true;
    return false;
  };
  if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty)) {
    if (ast::isa<ast::FixedArrayType>(ft->getElementType())) {
      ast::Type *inner = qualifyElements(q, ft->getElementType(), loc, failed);
      return inner ? Ctx.getFixedArrayType(inner, ft->getLength()) : nullptr;
    }
    ast::Qualifier out;
    return element(ft->getElementType(), ft->getElemQual(), out)
               ? Ctx.getFixedArrayType(ft->getElementType(), ft->getLength(),
                                       out)
               : nullptr;
  }
  if (auto *st = ast::dyn_cast<ast::SliceType>(ty)) {
    if (ast::isa<ast::FixedArrayType>(st->getElementType())) {
      ast::Type *inner = qualifyElements(q, st->getElementType(), loc, failed);
      return inner ? Ctx.getSliceType(inner) : nullptr;
    }
    ast::Qualifier out;
    return element(st->getElementType(), st->getElemQual(), out)
               ? Ctx.getSliceType(st->getElementType(), out)
               : nullptr;
  }
  return nullptr;
}

bool Sema::isOwnedBinder(const ast::Expr *e) {
  const auto *id = e ? ast::dyn_cast<ast::Identifier>(e) : nullptr;
  if (!id || id->isFunctionRef() || id->getName() == names::kSelf ||
      !CurrentScope || !CurrentScope->findOwner(id->getName()))
    return false;
  const VarKind k = varKind(id->getName());
  return k == VarKind::Plain || k == VarKind::Let;
}

bool Sema::checkQualifiedFlow(ast::Qualifier dst, ast::Expr *src,
                              const std::string &what, ast::SourceLocation loc,
                              bool implicitMove, bool materialize) {
  // A conditional's value is one of its branches: each flows as it would
  // alone; the conditional is what becomes a cell or a frozen object.
  if (auto *te = src ? ast::dyn_cast<ast::TernaryExpr>(src) : nullptr) {
    bool ok = checkQualifiedFlow(dst, te->getTrueExpr(), what, loc,
                                 implicitMove, /*materialize=*/false);
    ok = checkQualifiedFlow(dst, te->getFalseExpr(), what, loc, implicitMove,
                            /*materialize=*/false) &&
         ok;
    // Made a cell or frozen as a whole, so its branches are both new values
    // or both of the form: a cell beside a new object would be one, or two.
    // Every branch, through nested conditionals; a None is neither.
    bool anyNew = false, anyForm = false;
    std::function<void(const ast::Expr *)> branches = [&](const ast::Expr *b) {
      if (const auto *nt = ast::dyn_cast<ast::TernaryExpr>(b)) {
        branches(nt->getTrueExpr());
        branches(nt->getFalseExpr());
      } else if (!ast::isa<ast::NoneLiteral>(b)) {
        (qualifierOf(b).Form == ast::QualForm::Unique ? anyNew : anyForm) =
            true;
      }
    };
    branches(te);
    if (ok && materialize &&
        (dst.Form == ast::QualForm::Locked ||
         dst.Form == ast::QualForm::AtomicShared) &&
        anyNew && anyForm) {
      error(loc, "a conditional flowing into " + what + " (" +
                     qualifiedName(dst, src->getResolvedType()) +
                     ") has a branch of that form and a new value: make the "
                     "new value '" +
                     dst.spelling() + "' first, so both branches are");
      return false;
    }
    if (ok && materialize)
      materializeFlow(dst, src, loc);
    return ok;
  }
  ast::Type *ty = src ? src->getResolvedType() : nullptr;
  // A value (a function value too) copies: the forms are of references,
  // so it skips the conversions below, but not the copy rule, which sees
  // the unique references it holds.
  const bool value = ty && !ast::isa<ast::PoisonType>(ty) &&
                     !ast::isRefType(ast::stripOptional(ty)) &&
                     !ast::isa<ast::AtomicType>(ty) &&
                     !ast::isa<ast::TaskType>(ty);
  // A task handle read from a variable into an owning slot (a result,
  // another variable) leaves with it: the variable is spent, as by
  // `await` (the prototype's typestate, checkTasksAwaited).
  if (ty && ast::isa<ast::TaskType>(ty))
    if (const auto *id = ast::dyn_cast<ast::Identifier>(src);
        id && !id->isFunctionRef() && CurrentScope &&
        CurrentScope->findOwner(id->getName()))
      MovedVars[varKey(id->getName())] =
          MovedState{src->getLocation(), false, "its use as a value"};
  if (!ty || !sharesStorage(ty) || ast::isa<ast::PoisonType>(ty))
    return true; // a value is copied
  if (ast::isa<ast::NoneLiteral>(src))
    return true;
  const ast::Expr *inner = src;
  bool moved = implicitMove && isOwnedBinder(src);
  if (const auto *u = ast::dyn_cast<ast::UnaryExpr>(src);
      u && u->getOpcode() == ast::UnaryOpcode::Move) {
    moved = true;
    inner = u->getOperand();
  }
  const bool place = isCopyPlace(inner);
  const ast::Qualifier sq = qualifierOf(src);
  // The conversions: a unique reference flows into any form, and gains
  // `mut` only by `move` (or as a temporary, which nobody else holds); a
  // `shared` reference, a frozen object and a cell convert only as they
  // are.
  bool ok = value || sq == dst;
  if (!ok && sq.Form == ast::QualForm::Unique)
    ok = sq.Mut || !dst.Mut || moved || !place;
  if (!ok) {
    auto article = [](const std::string &s) {
      return std::string(!s.empty() && s[0] == 'a' ? "an '" : "a '") + s + "'";
    };
    error(loc, "cannot store " + article(qualifiedName(sq, ty)) + " in " +
                   what + ", " + article(qualifiedName(dst, ty)) + ": " +
                   (sq.Form == ast::QualForm::Unique
                        ? "a reference gains 'mut' only by 'move'"
                        : "a 'shared' reference, a frozen object and a cell "
                          "convert only as they are"));
    return false;
  }
  if (materialize && !value && !materializeFlow(dst, src, loc))
    return false;
  return checkCopyRule(src, loc, implicitMove);
}

bool Sema::materializeFlow(ast::Qualifier dst, ast::Expr *src,
                           ast::SourceLocation loc) {
  // A unique value (`mut` or not) that becomes a cell, or a frozen object,
  // is made into one at this use: the send check runs on what it holds.
  const ast::Qualifier sq = qualifierOf(src);
  if (sq.Form != ast::QualForm::Unique || dst.Form == ast::QualForm::Unique ||
      dst.Form == ast::QualForm::Shared)
    return true;
  const ast::Expr *inner = src;
  if (const auto *u = ast::dyn_cast<ast::UnaryExpr>(src);
      u && u->getOpcode() == ast::UnaryOpcode::Move)
    inner = u->getOperand();
  ast::Type *ty = src->getResolvedType();
  if (std::string why = uncrossable(ty, sq, ""); !why.empty()) {
    error(loc, "cannot make '" + placeText(inner) + "' (a '" +
                   qualifiedName(sq, ty) + "') '" + dst.spelling() +
                   "': " + why);
    return false;
  }
  src->setMaterialize(dst.Form);
  return true;
}

bool Sema::isCopyPlace(const ast::Expr *e) {
  // An `inout` result names the storage it borrows (#368): what it holds
  // stays there, so a copy of it is another owner.
  if (e && e->isInoutResult())
    return true;
  // A named function is no place: naming it makes a new function value.
  if (const auto *id = ast::dyn_cast<ast::Identifier>(e))
    return !id->isFunctionRef();
  // A part of a temporary (`make().a`) is no place either: the temporary
  // ends with the statement, so its part leaves it.
  return (ast::isa<ast::MemberAccessExpr>(e) ||
          ast::isa<ast::SubscriptExpr>(e) ||
          ast::isa<ast::TupleIndexExpr>(e)) &&
         placeRoot(e) != nullptr;
}

bool Sema::checkCopyRule(ast::Expr *src, ast::SourceLocation loc,
                         bool implicitMove) {
  if (auto *te = src ? ast::dyn_cast<ast::TernaryExpr>(src) : nullptr) {
    bool ok = checkCopyRule(te->getTrueExpr(), loc, implicitMove);
    return checkCopyRule(te->getFalseExpr(), loc, implicitMove) && ok;
  }
  ast::Type *ty = src ? src->getResolvedType() : nullptr;
  if (!ty || !sharesStorage(ty) || ast::isa<ast::PoisonType>(ty) ||
      ast::isa<ast::NoneLiteral>(src))
    return true;
  const ast::Expr *inner = src;
  bool moved = implicitMove && isOwnedBinder(src);
  if (const auto *u = ast::dyn_cast<ast::UnaryExpr>(src);
      u && u->getOpcode() == ast::UnaryOpcode::Move) {
    moved = true;
    inner = u->getOperand();
  }
  if (moved || !isCopyPlace(inner))
    return true;
  // The copy rule: a move-only value read from a place is moved, never
  // copied.  A `shared` reference, a handle, and a value of such parts are
  // Copyable.  A borrow, or a part reached through one, is the borrow
  // check's (checkViewNotStored, the parameter checks): it says where a
  // `view` may go, before this rule would.
  if (const ast::Identifier *root = placeRoot(inner)) {
    const VarKind rk = varKind(root->getName());
    if (rk == VarKind::View || rk == VarKind::ViewLocal ||
        rk == VarKind::ViewBinding || rk == VarKind::WeakView)
      return true;
    // So is the read-only `self` of a `view fn`.
    if (root->getName() == names::kSelf && CurrentClassCtx &&
        CurrentClassCtx->ViewMethod)
      return true;
  }
  const ast::Qualifier sq = qualifierOf(src);
  std::string part;
  if (sq.Form != ast::QualForm::Unique || isCopyablePart(ty, sq, &part))
    return true;
  const std::string text = placeText(inner);
  std::string msg = "cannot copy '" + text + "'";
  if (ast::isRefType(ty))
    msg += " (a unique '" + qualifiedName(sq, ty) + "')";
  else if (part.empty())
    msg += " (a '" + typeName(ty) + "')";
  else
    msg += ": '" + typeName(ty) + "' holds unique " + part;
  const auto *id = ast::dyn_cast<ast::Identifier>(inner);
  const VarKind kind = id ? varKind(id->getName()) : VarKind::Plain;
  // A Cloneable value has a copy too (#369).
  const std::string clone =
      isCloneable(ty) ? "'" + text + ".clone()' to copy it" : std::string();
  const std::string shared =
      ast::isRefType(ty)
          ? qualifiedName(ast::Qualifier{ast::QualForm::Shared, sq.Mut}, ty)
          : std::string();
  if (id && id->getName() == names::kSelf)
    msg += "; a method borrows its receiver" +
           (clone.empty() ? "" : ": write " + clone) +
           " (a 'shared fn' hands on a shared one)";
  else if (kind == VarKind::Inout)
    msg += "; it is an 'inout' borrow of the caller's storage" +
           (clone.empty() ? "" : ": write " + clone);
  else if (id)
    msg += "; write 'move " + text + "' to transfer it" +
           (clone.empty() ? "" : (shared.empty() ? ", or " : ", ") + clone) +
           (shared.empty() ? "" : ", or declare it '" + shared + "'");
  else if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(inner);
           se &&
           ast::isa<ast::FixedArrayType>(se->getArray()->getResolvedType()))
    // A fixed-size array's element is an `inout` place.
    msg += "; an element leaves its array by 'mem::replace' or 'mem::take'" +
           (clone.empty() ? "" : ", or " + clone);
  else if (ast::isa<ast::SubscriptExpr>(inner))
    msg += std::string("; an element stays its container's") +
           (clone.empty() ? "" : ": write " + clone);
  else
    msg += "; a field leaves by 'mem::replace' or 'mem::take'" +
           (clone.empty() ? "" : ", " + clone) +
           (shared.empty() ? "" : ", or is declared '" + shared + "'");
  error(loc, msg);
  return false;
}

/// Why @p what (a variable, a field) cannot be written: its form @p q, of
/// an object of type @p ty, is immutable, with the form that would allow it.
static std::string immutableText(const std::string &what,
                                 const ast::Qualifier &q,
                                 const std::string &ty) {
  switch (q.Form) {
  case ast::QualForm::Unique:
    return what + " is immutable (a '" + ty + "'; write 'mut " + ty +
           "' to change it)";
  case ast::QualForm::Shared:
    return what + " is immutable (a 'shared " + ty + "'; write 'shared mut " +
           ty + "' to change it)";
  case ast::QualForm::AtomicShared:
    return what + " is frozen (an 'atomic shared " + ty + "')";
  case ast::QualForm::Locked:
    return what + " is 'locked " + ty +
           "', whose contents are never written; declare it 'locked mut " + ty +
           "' to change them in a 'lock' block";
  }
  return "";
}

bool Sema::valueRootWritable(const ast::Expr *e) {
  // Through the value's links (a struct's field, an array's or a tuple's
  // element) to what holds the value: a variable, writable unless `let`
  // (or a borrow, a loop's copy, a capture), or an object, writable when
  // `mut`.
  for (const ast::Expr *p = e;;) {
    if (const auto *id = ast::dyn_cast<ast::Identifier>(p)) {
      if (id->isFunctionRef())
        return false;
      ast::Type *ty =
          CurrentScope ? CurrentScope->lookup(id->getName()) : nullptr;
      if (ty && ast::isRefType(ast::stripOptional(ty)))
        return qualifierOfVar(id->getName()).Mut;
      switch (varKind(id->getName())) {
      case VarKind::Plain:
      case VarKind::Inout:
        return true;
      default:
        return false;
      }
    }
    if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(p)) {
      ast::Type *rt = ast::stripOptional(
          const_cast<ast::Expr *>(ma->getReceiver())->getResolvedType());
      if (rt && ast::isa<ast::ClassType>(rt))
        return qualifierOf(ma->getReceiver()).Mut;
      p = ma->getReceiver();
    } else if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(p)) {
      ast::Type *at =
          const_cast<ast::Expr *>(se->getArray())->getResolvedType();
      if (at && ast::isa<ast::SliceType>(at))
        return varKind(placeRoot(se->getArray())
                           ? placeRoot(se->getArray())->getName()
                           : "") == VarKind::Inout;
      p = se->getArray();
    } else if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(p)) {
      p = ti->getTuple();
    } else {
      return false; // a temporary: written by nobody
    }
  }
}

std::string Sema::constPlace(const ast::Expr *e) {
  // The place as reached (qualifierOf: a unique field follows its root, a
  // `shared` one reads immutable under an immutable root).  A value (a
  // struct, an array, a tuple, a number) is written as its `let` allows,
  // and so are the objects it holds.  A unique link that is immutable is
  // so because of its root, which is what to change, as is a `shared mut`
  // field read under an immutable root: the walk reports the first link
  // that is immutable by its own declaration, else the root.  A `locked`
  // cell's contents are reached in a `lock` block (checkNotLocked).
  bool throughObject = false; // an object reached from the value
  for (const ast::Expr *p = e;;) {
    ast::Type *ty = const_cast<ast::Expr *>(p)->getResolvedType();
    const bool ref = ty && ast::isRefType(ast::stripOptional(ty));
    if (const auto *id = ast::dyn_cast<ast::Identifier>(p)) {
      if (!ref) {
        // The value's own storage is `let`'s business (letValueRoot); the
        // objects it holds follow it too.
        if (throughObject && varKind(id->getName()) == VarKind::Let)
          return "'" + id->getName() +
                 "' is declared with 'let', and so are the objects it holds; "
                 "drop 'let' to change them";
        return "";
      }
      const ast::Qualifier q = qualifierOfVar(id->getName());
      if (q.Form == ast::QualForm::Locked)
        return "";
      return q.isConst()
                 ? immutableText("'" + id->getName() + "'", q, typeName(ty))
                 : "";
    }
    const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(p);
    if (ref) {
      throughObject = true;
      const ast::Qualifier q = qualifierOf(p);
      if (!q.isConst() || q.Form == ast::QualForm::Locked)
        return "";
      if (ma && q.Form != ast::QualForm::Unique) {
        // Immutable by its own declaration, or only under its root?
        ast::Type *rt = ast::stripOptional(
            const_cast<ast::Expr *>(ma->getReceiver())->getResolvedType());
        ast::Qualifier declared;
        if (const auto *ct = ast::dyn_cast<ast::ClassType>(rt))
          declared = ct->findFieldQualifier(ma->getFieldName());
        else if (const auto *st = ast::dyn_cast<ast::StructType>(rt))
          declared = st->findFieldQualifier(ma->getFieldName());
        if (!declared.Mut)
          return immutableText("field '" + ma->getFieldName() + "'", q,
                               typeName(ty));
      } else if (q.Form != ast::QualForm::Unique &&
                 (ast::isa<ast::SubscriptExpr>(p) ||
                  ast::isa<ast::TupleIndexExpr>(p))) {
        // An element declared `shared` (#368) is immutable by its own
        // declaration, as a field is; a unique one follows its root.
        if (const ast::Qualifier declared = elementQualifier(p);
            !declared.isUnique() && !declared.Mut)
          return immutableText("element '" + placeText(p) + "'", q,
                               typeName(ty));
      } else if (q.Form != ast::QualForm::Unique &&
                 !ast::isa<ast::Identifier>(p) && !ma) {
        // What a call gives held `shared` or frozen (a template's `T`'s
        // form, #368): its own form, through no root.
        return immutableText("'" + placeText(p) + "'", q, typeName(ty));
      }
    }
    if (ma)
      p = ma->getReceiver();
    else if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(p))
      p = se->getArray();
    else if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(p))
      p = ti->getTuple();
    else
      return "";
  }
}

ast::Qualifier Sema::elementQualifier(const ast::Expr *e) {
  if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(e)) {
    const ast::Type *arr = se->getArray()->getResolvedType();
    if (const auto *ft =
            arr ? ast::dyn_cast<ast::FixedArrayType>(arr) : nullptr)
      return ft->getElemQual();
    if (const auto *st = arr ? ast::dyn_cast<ast::SliceType>(arr) : nullptr)
      return st->getElemQual();
  }
  if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(e)) {
    const ast::Type *tup = ti->getTuple()->getResolvedType();
    if (const auto *tt = tup ? ast::dyn_cast<ast::TupleType>(tup) : nullptr)
      return tt->getElemQual(ti->getIndex());
  }
  return {};
}

bool Sema::checkNotLocked(const ast::Expr *place, ast::SourceLocation loc) {
  if (LockOperand || !place)
    return true;
  if (qualifierOf(place).Form != ast::QualForm::Locked)
    return true;
  const std::string text = placeText(place);
  error(loc, "'" + text + "' is 'locked': reach its contents in 'lock " + text +
                 " as c: view { }' (or 'inout')");
  return false;
}

} // namespace paykan::sema
