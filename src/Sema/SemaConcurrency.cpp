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

/// A value of plain parts: a number, bool, char or enum.
bool isPlainValue(ast::Type *ty) {
  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(ty))
    return bt->getTypeKind() != ast::BuiltinType::Void;
  return ast::isa<ast::EnumType>(ty);
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
  // A handle: a `locked` cell or an `atomic const` object.
  if (const ast::Qualifier q = qualifierOfVar(name);
      q.Form == ast::QualForm::Locked || q.Form == ast::QualForm::AtomicConst)
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
           q.Form == ast::QualForm::AtomicConst ||
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
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    return isCopyable(ot->getInnerType(), why);
  if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty)) {
    if (isCopyable(ft->getElementType(), why))
      return true;
    part("its elements", ft->getElementType(), {});
    return false;
  }
  // A struct's fields carry qualifiers (a `shared` one copies); a tuple's
  // elements do not.  Before the tuple case, which a struct is too.
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
      if (!isCopyable(types[i], why)) {
        part("element '." + std::to_string(i) + "'", types[i], {});
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
  // `Str`, a class, a function value, a task: move-only.
  return false;
}

/// The `at` of `mem::Buffer<T>` when @p st is that struct (one field,
/// `__data`, the runtime's buffer object as an `Obj`); nullptr otherwise.
static const ast::MethodDecl *bufferAt(const ast::StructType *st,
                                       const ast::ASTContext &ctx) {
  const std::string &n = st->getName();
  const size_t lt = n.find('<');
  if (lt == std::string::npos)
    return nullptr;
  const std::string base = n.substr(0, lt);
  const bool named =
      base == "Buffer" ||
      (base.size() > 8 && base.compare(base.size() - 8, 8, "::Buffer") == 0);
  if (!named || st->getNumFields() != 1 || st->getFieldName(0) != "__data" ||
      st->getElementTypes()[0] != ctx.getObjTy())
    return nullptr;
  return st->findMethod("at");
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
  if (auto *tt = ast::dyn_cast<ast::TaskType>(ty))
    return uncrossable(tt->getInnerType(), {}, path);
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    return uncrossable(ot->getInnerType(), q, path);
  if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty))
    return uncrossable(ft->getElementType(), q, path);
  if (auto *st = ast::dyn_cast<ast::StructType>(ty)) {
    // `mem::Buffer<T>` crosses when `T` does (the design's table): its one
    // field is the runtime's buffer object, whose walk freezes the
    // elements (PaykanBuffer_walk), so the check is of `T`, by `at`'s
    // result.
    if (const ast::MethodDecl *at = bufferAt(st, Ctx))
      return uncrossable(at->getReturnType(), {}, path);
    const auto &types = st->getElementTypes();
    for (size_t i = 0; i < types.size() && i < st->getNumFields(); ++i) {
      const std::string &f = st->getFieldName(i);
      std::string why = uncrossable(types[i], st->findFieldQualifier(f),
                                    path.empty() ? f : path + "." + f);
      if (!why.empty())
        return why;
    }
    return "";
  }
  if (auto *tt = ast::dyn_cast<ast::TupleType>(ty)) {
    const auto &types = tt->getElementTypes();
    for (size_t i = 0; i < types.size(); ++i) {
      const std::string e = "." + std::to_string(i);
      std::string why = uncrossable(types[i], {}, path + e);
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
  if (q.Form == ast::QualForm::Locked || q.Form == ast::QualForm::AtomicConst)
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
      const std::string sub = path.empty() ? f : path + "." + f;
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
    // A handle (`locked`, `atomic const`) is copied; a `shared` reference
    // never crosses; a unique or `const` one crosses by `move`, or as a
    // temporary of the caller's.
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
          q.Form == ast::QualForm::AtomicConst)
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
        error(arg->getLocation(),
              "cannot copy '" + text + "' (" +
                  (part.empty() ? "a move-only '" + typeName(ty) + "'"
                                : "a '" + typeName(ty) + "' holding " + part) +
                  ") into a task: write 'move " + text + "' to transfer it");
        ok = false;
        continue;
      }
    }
    // What crosses holds nothing of this thread: no `shared` or `weak`
    // part, no function value, no `Obj`.
    if (std::string why = uncrossable(ty, q, ""); !why.empty()) {
      error(arg->getLocation(), "cannot send '" + text + "' (a '" +
                                    qualifiedName(q, ty) + "'): " + why);
      ok = false;
    }
  }
  return ok;
}

bool Sema::checkBorrowQualifier(ast::Qualifier q, ast::ParamMode mode,
                                const std::string &what,
                                ast::SourceLocation loc) {
  if (mode == ast::ParamMode::Value || q.isUnique())
    return true;
  // A borrow is written with no qualifier: it borrows the object whatever
  // its qualifier and counts nothing.  A `view` is read-only already, and
  // an `inout` of a `const` object could not change it.
  const char *m = ast::paramModeName(mode);
  error(loc, what + " is '" + m + " " + q.spelling() +
                 "': a borrow carries no qualifier; a 'view' takes every "
                 "form of the object (read-only through it) and an 'inout' "
                 "a unique or 'shared' one, with no retain or release; "
                 "write '" +
                 m + " T'");
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
    // does, with no retain or release; a `const` one is frozen
    // (inoutPlaceError) and a cell is reached in a `lock` block.
    const ast::Qualifier q = qualifierOf(args[i]);
    (void)quals;
    const std::string text = placeText(args[i]);
    const char *mode = ast::paramModeName(modes.mode(i));
    if (q.Form == ast::QualForm::Locked) {
      error(args[i]->getLocation(),
            "'" + text +
                "' is 'locked': a cell's contents are reached in a "
                "'lock' block, not borrowed by '" +
                mode + "' parameter '" + modes.name(i) + "' of '" + callee +
                "'");
      ok = false;
    }
  }
  return ok;
}

// -- Moves (docs/design/concurrency.md, "Use after move")

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
  LoopMoveStack.push_back({loc, LoopDepth + 1, MovedVars, {}});
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
  for (const auto &[name, st] : MovedVars) {
    if (loop.Entry.count(name))
      continue; // moved before the loop: reported at its use
    // The body's own variables are declared anew each iteration.
    if (auto d = DeclLoopDepth.find(name);
        d != DeclLoopDepth.end() && d->second >= loop.Depth)
      continue;
    if (!LoopMovesReported.insert(name + "@" + at(loop.Loc)).second)
      continue;
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
  // Every link a unique field: a `shared` or `const` one is an alias, a
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
    if (!q.isUnique()) {
      error(loc, "cannot move '" + placeText(link) + "': it is '" +
                     qualifiedName(q, link->getResolvedType()) +
                     "', not a unique part of '" + name + "'");
      return nullptr;
    }
  }
  if (!ast::isRefType(ty)) {
    error(loc, "cannot move '" + what + "': it is a '" + typeName(ty) +
                   "', a value (a value is copied; a field of a reference "
                   "type moves out)");
    return nullptr;
  }
  if (std::string why = viewedBy(name); !why.empty()) {
    error(loc, why + "; it cannot be moved out of");
    return nullptr;
  }
  MovedVars[name] = MovedState{loc, false, ""};
  return ty;
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
    // `move` before a temporary (a call, a constructor, a literal): it is
    // owned already, and nothing happens.
    return resolveExprType(operand);
  }
  const std::string &name = id->getName();
  if (name == names::kSelf) {
    error(node->getLocation(),
          "cannot move 'self': a method borrows its receiver");
    return nullptr;
  }
  switch (varKind(name)) {
  case VarKind::Plain:
  case VarKind::Let:
  case VarKind::ForVar: // an integer: read as it is
    break;
  case VarKind::Inout:
    error(node->getLocation(), "cannot move '" + name +
                                   "': it is an 'inout' parameter, the "
                                   "caller's storage");
    return nullptr;
  case VarKind::View:
  case VarKind::ViewLocal:
  case VarKind::ViewBinding:
    error(node->getLocation(),
          "cannot move '" + name + "': it is a 'view', a borrow");
    return nullptr;
  case VarKind::Weak:
  case VarKind::WeakView:
    error(node->getLocation(), "cannot move '" + name +
                                   "': a 'weak' variable holds no strong "
                                   "reference");
    return nullptr;
  case VarKind::Captured:
  case VarKind::CapturedWeak:
    error(node->getLocation(),
          "cannot move '" + name + "': the function captures it by copy");
    return nullptr;
  }
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
  MovedVars[name] = MovedState{node->getLocation(), false, ""};
  return ty;
}

bool Sema::checkNotMoved(const std::string &name, ast::SourceLocation loc) {
  auto moved = MovedVars.find(name);
  if (moved == MovedVars.end())
    return true;
  const MovedState &st = moved->second;
  if (st.Maybe)
    error(loc, "'" + name + "' may have been moved (by " + st.By + ", at " +
                   at(st.At) + "); it cannot be used after a move");
  else
    error(loc, "'" + name + "' was moved at " + at(st.At) +
                   "; it cannot be used after the move");
  return false;
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
  if (const auto *te = ast::dyn_cast<ast::TernaryExpr>(e))
    return qualifierOf(te->getTrueExpr());
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
    if (const auto *ct = ast::dyn_cast<ast::ClassType>(rt))
      q = ct->findFieldQualifier(ma->getFieldName());
    else if (const auto *st = ast::dyn_cast<ast::StructType>(rt))
      q = st->findFieldQualifier(ma->getFieldName());
    // A field reached through a `const` reference reads as `const`.
    if (recv.isConst() &&
        (q.Form == ast::QualForm::Unique || q.Form == ast::QualForm::Shared))
      q.Const = true;
    return q;
  }
  if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(e))
    return qualifierOf(se->getArray()); // the elements carry the array's
  if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(e))
    return qualifierOf(ti->getTuple());
  if (const auto *ce = ast::dyn_cast<ast::CallExpr>(e)) {
    if (const FunctionSig *sig = lookupFunction(ce->getCalleeName()))
      return sig->ResultQual;
    return {};
  }
  if (const auto *mc = ast::dyn_cast<ast::MethodCallExpr>(e)) {
    ast::Type *rt = ast::stripOptional(
        const_cast<ast::Expr *>(mc->getReceiver())->getResolvedType());
    if (const auto *ct = ast::dyn_cast<ast::ClassType>(rt))
      if (const ast::MethodDecl *m = ct->findMethod(mc->getMethodName()))
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
  // thread, goes in a `mem::Box<T>`, an object (`atomic const
  // mem::Box<Buffer<float>>`).
  const bool ref = ast::isRefType(inner) || ast::isa<ast::AtomicType>(inner);
  const std::string value =
      "'" + typeName(inner) + "' is a value" +
      (sharesStorage(inner) ? ": box it ('mem::Box<" + typeName(inner) +
                                  ">') to share what it holds"
                            : "");
  switch (q.Form) {
  case ast::QualForm::Unique: // `const`
    if (!ref) {
      error(loc, "'const' applies to objects; " + value +
                     (sharesStorage(inner) ? "" : ", which 'let' fixes"));
      return false;
    }
    return true;
  case ast::QualForm::Shared:
    if (!ref) {
      error(loc, "'shared' qualifies a reference; " + value);
      return false;
    }
    return true;
  case ast::QualForm::AtomicConst:
    if (!ref) {
      error(loc, "'atomic const' qualifies a reference; " + value);
      return false;
    }
    return true;
  case ast::QualForm::Locked:
    if (!ast::isRefType(inner)) {
      error(loc, "'locked' holds an object in the prototype, not a '" +
                     typeName(inner) + "' (wrap the value in a class)");
      return false;
    }
    return true;
  }
  return true;
}

bool Sema::checkQualifiedFlow(ast::Qualifier dst, ast::Expr *src,
                              const std::string &what,
                              ast::SourceLocation loc) {
  ast::Type *ty = src ? src->getResolvedType() : nullptr;
  if (!ty || !sharesStorage(ty) || ast::isa<ast::PoisonType>(ty))
    return true; // a value is copied
  if (ast::isa<ast::NoneLiteral>(src))
    return true;
  const ast::Expr *inner = src;
  bool moved = false;
  if (const auto *u = ast::dyn_cast<ast::UnaryExpr>(src);
      u && u->getOpcode() == ast::UnaryOpcode::Move) {
    moved = true;
    inner = u->getOperand();
  }
  const bool place = ast::isa<ast::Identifier>(inner) ||
                     ast::isa<ast::MemberAccessExpr>(inner) ||
                     ast::isa<ast::SubscriptExpr>(inner) ||
                     ast::isa<ast::TupleIndexExpr>(inner);
  const ast::Qualifier sq = qualifierOf(src);
  // The conversions: a unique value into any form, a `const` one into the
  // other `const` forms, a handle or a `shared` reference only as it is.
  bool ok = sq.isUnique() || sq == dst;
  if (!ok && sq.Form == ast::QualForm::Unique && sq.Const)
    ok = dst.isConst() && dst.Form != ast::QualForm::Locked;
  if (!ok) {
    error(loc, "cannot store a '" + qualifiedName(sq, ty) + "' in " + what +
                   ", a '" + qualifiedName(dst, ty) +
                   "': a reference converts only from unique, or from "
                   "'const' into the other 'const' forms");
    return false;
  }
  // A unique (or `const`) value that becomes a cell, or a frozen object,
  // is made into one at this use: the send check runs on what it holds.
  if (sq.Form == ast::QualForm::Unique && dst.Form != ast::QualForm::Unique &&
      dst.Form != ast::QualForm::Shared) {
    if (std::string why = uncrossable(ty, sq, ""); !why.empty()) {
      error(loc, "cannot make '" + placeText(inner) + "' (a '" +
                     qualifiedName(sq, ty) + "') '" + dst.spelling() +
                     "': " + why);
      return false;
    }
    src->setMaterialize(dst.Form);
  }
  // The copy rule (--strict-ownership): a move-only value read from a
  // place is moved, never copied.  A `shared` reference, a handle, and a
  // value of such parts are Copyable.
  std::string part;
  if (StrictOwnership && place && !moved && sq.Form == ast::QualForm::Unique &&
      !isCopyablePart(ty, sq, &part)) {
    const std::string text = placeText(inner);
    std::string msg = "cannot copy '" + text + "'";
    if (ast::isRefType(ty))
      msg += " (a unique '" + qualifiedName(sq, ty) + "')";
    else
      msg += ": '" + typeName(ty) + "' holds unique " + part;
    if (ast::isa<ast::Identifier>(inner))
      msg +=
          "; write 'move " + text + "' to transfer it" +
          (ast::isRefType(ty)
               ? ", or declare it '" +
                     qualifiedName(
                         ast::Qualifier{ast::QualForm::Shared, sq.Const}, ty) +
                     "'"
               : std::string());
    else
      msg +=
          "; a field or element leaves by 'mem::replace' or 'mem::take'" +
          (ast::isRefType(ty) ? ", or is declared 'shared " + typeName(ty) + "'"
                              : std::string());
    error(loc, msg);
    return false;
  }
  return true;
}

std::string Sema::constPlace(const ast::Expr *e) {
  for (const ast::Expr *p = e;;) {
    if (const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(p)) {
      ast::Type *rt = ast::stripOptional(
          const_cast<ast::Expr *>(ma->getReceiver())->getResolvedType());
      ast::Qualifier q;
      if (const auto *ct = ast::dyn_cast<ast::ClassType>(rt))
        q = ct->findFieldQualifier(ma->getFieldName());
      else if (const auto *st = ast::dyn_cast<ast::StructType>(rt))
        q = st->findFieldQualifier(ma->getFieldName());
      if (q.isConst())
        return "field '" + ma->getFieldName() + "' is '" + q.spelling() + "'";
      p = ma->getReceiver();
    } else if (const auto *se = ast::dyn_cast<ast::SubscriptExpr>(p)) {
      p = se->getArray();
    } else if (const auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(p)) {
      p = ti->getTuple();
    } else {
      break;
    }
  }
  if (const auto *id = ast::dyn_cast<ast::Identifier>(placeBase(e))) {
    const ast::Qualifier q = qualifierOfVar(id->getName());
    if (q.isConst())
      return "'" + id->getName() + "' is '" + q.spelling() + "'";
  }
  return "";
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
