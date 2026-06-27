// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "Sema.h"
#include "Names.h"
#include "SemaInternal.h"

#include <llvm/ADT/SmallPtrSet.h>

#include <functional>

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

Sema::ScopeGuard::ScopeGuard(Sema &s) : S(s), ScopeObj(s.CurrentScope) {
  S.CurrentScope = &ScopeObj;
}

Sema::ScopeGuard::~ScopeGuard() { S.CurrentScope = ScopeObj.Parent; }

// -- Helpers -----------------------------------------------------------------

Sema::Sema(ast::ASTContext &ctx, DiagEngine &diags,
           const std::string &projectRoot)
    : Diags(diags), Ctx(ctx), ProjectRoot(projectRoot) {}

void Sema::declareFunction(llvm::StringRef name, ast::Type *retTy,
                           std::vector<ast::Type *> paramTys, bool isVariadic,
                           bool isBuiltin) {
  FunctionTable[name] = {retTy, std::move(paramTys), isVariadic, isBuiltin};
}

const Sema::FunctionSig *Sema::lookupFunction(llvm::StringRef name) const {
  auto it = FunctionTable.find(name);
  return it != FunctionTable.end() ? &it->second : nullptr;
}

void Sema::error(ast::SourceLocation loc, const std::string &msg) {
  Diags.error(loc, msg);
}

void Sema::warning(ast::SourceLocation loc, const std::string &msg) {
  Diags.warning(loc, msg);
}

void Sema::note(ast::SourceLocation loc, const std::string &msg) {
  Diags.note(loc, msg);
}

std::string Sema::typeName(ast::Type *ty) {
  if (!ty)
    return "unknown";
  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(ty)) {
    switch (bt->getTypeKind()) {
    case ast::BuiltinType::Int:
      return names::kTypeInt;
    case ast::BuiltinType::Float:
      return names::kTypeFloat;
    case ast::BuiltinType::Bool:
      return names::kTypeBool;
    case ast::BuiltinType::Char:
      return names::kTypeChar;
    case ast::BuiltinType::Void:
      return names::kTypeVoid;
    }
  }
  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty))
    return ct->getName();
  if (auto *et = ast::dyn_cast<ast::EnumType>(ty))
    return et->getName();
  if (auto *at = ast::dyn_cast<ast::ArrayType>(ty))
    return typeName(at->getElementType()) + "[]";
  return "unknown";
}

// Structural type equality (pointer equality is insufficient for ArrayType
// nodes because each make<ArrayType>() call yields a fresh allocation).
static bool typesEqual(ast::Type *a, ast::Type *b) {
  if (a == b)
    return true;
  if (!a || !b)
    return false;
  if (a->getKind() != b->getKind())
    return false;
  if (auto *aa = ast::dyn_cast<ast::ArrayType>(a))
    return typesEqual(aa->getElementType(),
                      ast::cast<ast::ArrayType>(b)->getElementType());
  // BuiltinType / ClassType: pointer equality is canonical (singletons /
  // interned).
  return false;
}

bool Sema::isNumeric(ast::Type *ty) {
  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(ty))
    return bt->getTypeKind() == ast::BuiltinType::Int ||
           bt->getTypeKind() == ast::BuiltinType::Float;
  return false;
}

bool Sema::isAssignable(ast::Type *dst, ast::Type *src) const {
  if (dst == src)
    return true;

  // int -> float promotion.
  if (dst == Ctx.getFloatTy() && src == Ctx.getIntTy())
    return true;

  // Any array type is assignable to Obj (arrays are heap-allocated objects).
  if (dst == Ctx.getObjTy() && ast::isa<ast::ArrayType>(src))
    return true;

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
    error(loc, context + " has unknown class type '" + ct->getName() + "'");
    return nullptr;
  }

  if (auto *at = ast::dyn_cast<ast::ArrayType>(ty)) {
    auto *elemTy = resolveType(at->getElementType(), loc, context + " element");
    if (!elemTy)
      return nullptr;
    return Ctx.make<ast::ArrayType>(at->getLocation(), elemTy);
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
  return S.checkIdentLive(node->getName(), node->getLocation());
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
  auto *rhsTy = visit(node->getRHS());
  if (!lhsTy || !rhsTy)
    return nullptr;

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
    // Same type is always OK.
    if (lhsTy != rhsTy) {
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
        if (!S.isAssignable(expectedParams[i], argTypes[i]))
          S.error(node->getArguments()[i]->getLocation(),
                  "argument " + std::to_string(i + 1) + " of '" +
                      names::kMethodSuper + "' has type '" +
                      typeName(argTypes[i]) + "', expected '" +
                      typeName(expectedParams[i]) + "'");
      }
    }
    S.CurrentClassCtx->SuperInitCalled = true;
    return S.Ctx.getVoidTy();
  }

  const auto *sig = S.lookupFunction(node->getCalleeName());
  if (!sig) {
    S.error(node->getLocation(),
            "call to undeclared function '" + node->getCalleeName() + "'");
    return nullptr;
  }

  if (sig->IsVariadic) {
    if (argTypes.size() < sig->ParamTypes.size()) {
      S.error(node->getLocation(),
              "function '" + node->getCalleeName() + "' requires at least " +
                  std::to_string(sig->ParamTypes.size()) +
                  " argument(s), got " + std::to_string(argTypes.size()));
      return sig->ReturnType;
    }
  } else if (argTypes.size() != sig->ParamTypes.size()) {
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
    // For variadic functions, extra arguments are checked against the
    // last declared parameter type.
    size_t paramIdx =
        (i < sig->ParamTypes.size()) ? i : sig->ParamTypes.size() - 1;

    if (!S.isAssignable(sig->ParamTypes[paramIdx], argTypes[i])) {
      S.error(node->getArguments()[i]->getLocation(),
              "argument " + std::to_string(i + 1) + " of '" +
                  node->getCalleeName() + "' has type '" +
                  typeName(argTypes[i]) + "', expected '" +
                  typeName(sig->ParamTypes[paramIdx]) + "'");
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

  // Resolve the ClassType to look up the method on.
  // Array types dispatch through a per-element specialized ClassType so that
  // push/pop signatures are element-type-aware.
  ast::ClassType *ct = nullptr;
  if (auto *at = ast::dyn_cast<ast::ArrayType>(recvTy))
    ct = S.Ctx.getOrCreateSpecializedArrayType(at->getElementType());
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

  // `destroy` is the ARC destructor; it runs automatically when the last
  // reference is released. Calling it directly would free a still-referenced
  // object and lead to a use-after-free, so reject it at compile time.
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
    auto *argTy = visit(node->getArguments()[i]);
    if (!argTy)
      continue;
    if (!S.isAssignable(paramTys[i], argTy)) {
      S.error(node->getArguments()[i]->getLocation(),
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

  auto *ct = ast::dyn_cast<ast::ClassType>(recvTy);
  if (!ct) {
    S.error(node->getLocation(), "member access '." + node->getFieldName() +
                                     "' on non-class type '" +
                                     typeName(recvTy) + "'");
    return nullptr;
  }

  // Walk the class hierarchy (this class + all ancestors) for the field.
  for (auto *c = ct; c; c = c->getSuperClass()) {
    for (auto &[fname, fty] : c->getFields()) {
      if (fname == node->getFieldName()) {
        node->setResolvedType(fty);
        return fty;
      }
    }
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
    auto *arrTy =
        S.Ctx.make<ast::ArrayType>(node->getLocation(), S.Ctx.getVoidTy());
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
  auto *arrTy = S.Ctx.make<ast::ArrayType>(node->getLocation(), elemTy);
  node->setResolvedType(arrTy);
  return arrTy;
}

ast::Type *Sema::ExprChecker::visitSubscriptExpr(ast::SubscriptExpr *node) {
  auto *arrayTy = visit(node->getArray());
  if (!arrayTy)
    return nullptr;

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
    node->setResolvedType(trueTy);
    return trueTy;
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

// -- Entry point -------------------------------------------------------------

SemaContext Sema::run(ast::TranslationUnit *tu) {
  CurrentScope = nullptr;
  AccumulatedImportContexts.clear();

  // Bootstrap builtin functions — print/println take any object.
  declareFunction(names::kPrint, Ctx.getVoidTy(), {Ctx.getObjTy()},
                  /*isVariadic=*/true, /*isBuiltin=*/true);
  declareFunction(names::kPrintln, Ctx.getVoidTy(), {Ctx.getObjTy()},
                  /*isVariadic=*/true, /*isBuiltin=*/true);
  declareFunction(names::kErrPrint, Ctx.getVoidTy(), {Ctx.getObjTy()},
                  /*isVariadic=*/true, /*isBuiltin=*/true);
  declareFunction(names::kErrPrintln, Ctx.getVoidTy(), {Ctx.getObjTy()},
                  /*isVariadic=*/true, /*isBuiltin=*/true);

  // Register type-conversion builtins (take unique builtin types — no ownership
  // check needed).
  auto *StrTy = Ctx.getStrTy();
  declareFunction(names::kStrInt, StrTy, {Ctx.getIntTy()}, false, true);
  declareFunction(names::kStrFloat, StrTy, {Ctx.getFloatTy()}, false, true);
  declareFunction(names::kStrBool, StrTy, {Ctx.getBoolTy()}, false, true);
  declareFunction(names::kStrChar, StrTy, {Ctx.getCharTy()}, false, true);
  declareFunction(names::kString, StrTy, {StrTy}, false, true);
  declareFunction(names::kOpen, Ctx.getObjTy(), {StrTy, StrTy}, false, true);
  declareFunction(names::kIntStr, Ctx.getObjTy(), {StrTy}, false, true);
  declareFunction(names::kFloatStr, Ctx.getObjTy(), {StrTy}, false, true);

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

  // Register class types, fields, method signatures, and constructors first, so
  // that function signatures below can name class types (e.g. a function that
  // takes or returns a class).  Method bodies are deferred (checkClassBodies).
  if (!node->getClassDecls().empty())
    if (!checkClassDecls(node->getClassDecls()))
      ok = false;

  // Forward-declare every free function's signature next, so that any body —
  // a free function OR a class method — may call any module-level function
  // regardless of the order it is defined.
  for (auto *fn : node->getFuncDecls())
    if (!declareFunctionSignature(fn))
      ok = false;

  // Now check class method bodies (they can resolve free functions) …
  if (!node->getClassDecls().empty())
    if (!checkClassBodies())
      ok = false;

  // … and finally free-function bodies (signatures all registered above).
  for (auto *fn : node->getFuncDecls())
    if (!visitFuncDecl(fn))
      ok = false;

  return ok;
}

// -- Declarations ------------------------------------------------------------

bool Sema::visitEnumDecl(ast::EnumDecl *node) {
  // Reject names that collide with a builtin, class, or existing enum.
  if (Ctx.lookupType(node->getName())) {
    error(node->getLocation(),
          "redefinition of type '" + node->getName() + "'");
    return false;
  }

  auto *enumTy = Ctx.registerEnumType(node->getName(), node->getLocation());
  if (!enumTy) {
    error(node->getLocation(),
          "redefinition of enum '" + node->getName() + "'");
    return false;
  }

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
      varName == names::kObj || varName == names::kString ||
      varName == names::kFile || varName == names::kTypeInt ||
      varName == names::kTypeBool || varName == names::kTypeFloat ||
      varName == names::kTypeChar || varName == names::kStdin) {
    error(node->getLocation(),
          "'" + varName + "' is a type name and cannot be used as a variable");
    return false;
  }

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
  // type.
  if (auto *at = ast::dyn_cast<ast::ArrayType>(valTy)) {
    if (at->getElementType() == Ctx.getVoidTy() &&
        !ast::isa<ast::ArrayType>(varTy)) {
      error(node->getLocation(),
            "cannot infer element type of empty array literal '[]'; "
            "add an explicit type annotation");
      return false;
    }
  }

  if (!isAssignable(varTy, valTy)) {
    error(node->getLocation(), "cannot assign value of type '" +
                                   typeName(valTy) + "' to variable '" +
                                   varName + "' of type '" + typeName(varTy) +
                                   "'");
    return false;
  }

  return true;
}

bool Sema::visitReturnStmt(ast::ReturnStmt *node) {
  if (node->getReturnValue()) {
    auto *valTy = resolveExprType(node->getReturnValue());
    if (!valTy)
      return false;
    if (CurrentReturnType && !isAssignable(CurrentReturnType, valTy)) {
      error(node->getLocation(), "return value of type '" + typeName(valTy) +
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
  // Type-check the then branch.
  if (!visit(node->getThenBranch()))
    ok = false;
  // Type-check the else branch (if present).
  if (node->hasElse()) {
    if (!visit(node->getElseBranch()))
      ok = false;
  }
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
    bool hasWildcard = false;
    unsigned variantArms = 0;
    for (ast::MatchArm *arm : ms->getArms()) {
      if (arm->isWildcard())
        hasWildcard = true;
      else if (!arm->isLiteral())
        ++variantArms; // an enum/type-name (bare-variant) arm
      if (!detail::blockAlwaysReturns(arm->getBody()->getStatements()))
        return false;
    }
    if (hasWildcard)
      return true;
    // A wildcard-less enum match is exhaustive iff its arms cover every
    // variant.  Sema has already validated the arms (visitFuncDecl runs this
    // analysis only after the body type-checks cleanly), so each variant arm
    // names a distinct, valid variant — counting them suffices.
    if (auto *subjTy = ms->getSubject()->getResolvedType())
      if (auto *et = ast::dyn_cast<ast::EnumType>(subjTy))
        return variantArms == et->getNumVariants();
    return false;
  }
  return false;
}

bool Sema::declareFunctionSignature(ast::FuncDecl *node) {
  // Resolve return type.
  ast::Type *retTy = Ctx.getVoidTy();
  if (node->getReturnType()) {
    retTy = resolveType(node->getReturnType(), node->getLocation(),
                        "function '" + node->getName() + "' return type");
    if (!retTy)
      return false;
  }

  // Resolve parameter types.
  std::vector<ast::Type *> paramTypes;
  for (auto &p : node->getParams()) {
    auto *ty = resolveType(p.ParamType, node->getLocation(),
                           "parameter '" + p.getName() + "'");
    if (!ty)
      return false;
    paramTypes.push_back(ty);
  }

  // Register the function in the function table.
  if (lookupFunction(node->getName())) {
    error(node->getLocation(),
          "redefinition of function '" + node->getName() + "'");
    return false;
  }
  declareFunction(node->getName(), retTy, paramTypes);
  return true;
}

bool Sema::visitFuncDecl(ast::FuncDecl *node) {
  // The signature was registered by the forward-declaration pass in
  // visitTranslationUnit.  If it is absent, signature resolution failed earlier
  // (the error was already reported) — skip the body to avoid duplicate
  // diagnostics.
  if (!lookupFunction(node->getName()))
    return false;

  // Re-resolve the annotations to set up the body scope (resolveType is
  // idempotent and, for a registered function, is guaranteed to succeed).
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
      if (auto *at = ast::dyn_cast<ast::ArrayType>(initTy)) {
        if (at->getElementType() == Ctx.getVoidTy() &&
            !ast::isa<ast::ArrayType>(declTy)) {
          error(node->getLocation(),
                "cannot infer element type of empty array literal '[]'; "
                "add an explicit type annotation");
          CurrentScope->set(node->getName(), declTy);
          return false;
        }
      }
      if (!isAssignable(declTy, initTy)) {
        error(node->getLocation(), "initializer of type '" + typeName(initTy) +
                                       "' does not match declared type '" +
                                       typeName(declTy) + "' for variable '" +
                                       node->getName() + "'");
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
        if (lit->isEmpty() && ast::isa<ast::ArrayType>(declTy))
          lit->setResolvedType(declTy);
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

  auto *ct = ast::dyn_cast<ast::ClassType>(recvTy);
  if (!ct) {
    error(node->getLocation(), "member assignment '." + node->getFieldName() +
                                   "' on non-class type '" + typeName(recvTy) +
                                   "'");
    return false;
  }

  // Look up the field in the class hierarchy.
  ast::Type *fieldTy = nullptr;
  for (auto *c = ct; c; c = c->getSuperClass()) {
    for (auto &[fname, fty] : c->getFields()) {
      if (fname == node->getFieldName()) {
        fieldTy = fty;
        break;
      }
    }
    if (fieldTy)
      break;
  }

  if (!fieldTy) {
    error(node->getLocation(), "no field '" + node->getFieldName() +
                                   "' in class '" + ct->getName() + "'");
    return false;
  }

  auto *valTy = resolveExprType(node->getValue());
  if (!valTy)
    return false;

  if (!isAssignable(fieldTy, valTy)) {
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
  if (!isAssignable(at->getElementType(), valTy)) {
    error(node->getLocation(), "cannot assign value of type '" +
                                   typeName(valTy) + "' to array of '" +
                                   typeName(at->getElementType()) + "'");
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
  }

  return ok;
}

// Enum-mode match: the subject is an enum, and each non-wildcard arm names a
// bare variant.  The variant name is carried by the arm's type-name (a
// ClassType stub produced by the parser's `typeAnnotation: IDENT` rule).
bool Sema::checkEnumMatch(ast::MatchStmt *node, ast::EnumType *subjectTy) {
  bool ok = true;
  bool seenWildcard = false;
  llvm::StringSet<> seenVariants;

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
  }

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
      for (auto *stmt : arm->getBody()->getStatements())
        if (!visit(stmt))
          ok = false;
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
  }

  return ok;
}

} // namespace sema
} // namespace paykan
