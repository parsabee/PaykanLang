// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "Sema.h"
#include "Names.h"

#include <llvm/Support/raw_ostream.h>

namespace paykan {
namespace sema {

// -- Scope / ScopeGuard ------------------------------------------------------

Sema::Scope::Scope(Scope *parent) : Parent(parent) {}

ast::Type *Sema::Scope::lookup(llvm::StringRef name) const {
  auto it = Locals.find(name);
  if (it != Locals.end()) return it->second;
  return Parent ? Parent->lookup(name) : nullptr;
}

ast::Ownership Sema::Scope::lookupOwnership(llvm::StringRef name) const {
  auto it = OwnershipMap.find(name);
  if (it != OwnershipMap.end()) return it->second;
  return Parent ? Parent->lookupOwnership(name) : ast::Ownership::Unique;
}

bool Sema::Scope::isMoved(llvm::StringRef name) const {
  if (MovedSet.count(name)) return true;
  return Parent ? Parent->isMoved(name) : false;
}

void Sema::Scope::markMoved(llvm::StringRef name) {
  // Mark in the scope that owns the variable.
  if (Locals.count(name)) { MovedSet.insert(name); return; }
  if (Parent) Parent->markMoved(name);
}

bool Sema::Scope::declare(llvm::StringRef name, ast::Type *ty) {
  return Locals.try_emplace(name, ty).second;
}

bool Sema::Scope::declare(llvm::StringRef name, ast::Type *ty, ast::Ownership ownership,
                          bool isConst) {
  if (!Locals.try_emplace(name, ty).second)
    return false;
  OwnershipMap[name] = ownership;
  if (isConst)
    ConstSet.insert(name);
  return true;
}

bool Sema::Scope::isConst(llvm::StringRef name) const {
  if (ConstSet.count(name)) return true;
  return Parent ? Parent->isConst(name) : false;
}

void Sema::Scope::set(llvm::StringRef name, ast::Type *ty) {
  Locals[name] = ty;
}

bool Sema::Scope::contains(llvm::StringRef name) const {
  return Locals.count(name);
}

Sema::Scope *Sema::Scope::findOwner(llvm::StringRef name) {
  if (Locals.count(name)) return this;
  return Parent ? Parent->findOwner(name) : nullptr;
}

Sema::ScopeGuard::ScopeGuard(Sema &s)
    : S(s), ScopeObj(s.CurrentScope) {
  S.CurrentScope = &ScopeObj;
}

Sema::ScopeGuard::~ScopeGuard() { S.CurrentScope = ScopeObj.Parent; }

// -- Helpers -----------------------------------------------------------------

Sema::Sema(ast::ASTContext &ctx, llvm::raw_ostream &os)
    : OS(os), Ctx(ctx) {}

void Sema::declareFunction(llvm::StringRef name, ast::Type *retTy,
                           std::vector<ast::Type *> paramTys,
                           std::vector<ast::Ownership> paramOwns,
                           std::vector<bool> paramConst,
                           bool isVariadic, bool isBuiltin) {
  // Default all param ownerships to Unique if not provided.
  if (paramOwns.empty())
    paramOwns.resize(paramTys.size(), ast::Ownership::Unique);
  if (paramConst.empty())
    paramConst.resize(paramTys.size(), false);
  FunctionTable[name] = {retTy, std::move(paramTys), std::move(paramOwns),
                         std::move(paramConst), isVariadic, isBuiltin};
}

const Sema::FunctionSig *Sema::lookupFunction(llvm::StringRef name) const {
  auto it = FunctionTable.find(name);
  return it != FunctionTable.end() ? &it->second : nullptr;
}

void Sema::diag(Diagnostic::Severity level, ast::SourceLocation loc,
                const std::string &msg) {
  Diagnostics.push_back({level, loc, msg});
  if (level == Diagnostic::Error)
    ++ErrorCount;

  // Print immediately (clang-style one-liner).
  if (loc.isValid())
    OS << loc.getLineStart() << ":" << loc.getColumnStart() << ": ";

  switch (level) {
  case Diagnostic::Error:   OS << "error: ";   break;
  case Diagnostic::Warning: OS << "warning: "; break;
  case Diagnostic::Note:    OS << "note: ";    break;
  }
  OS << msg << "\n";
}

void Sema::error(ast::SourceLocation loc, const std::string &msg) {
  diag(Diagnostic::Error, loc, msg);
}

void Sema::warning(ast::SourceLocation loc, const std::string &msg) {
  diag(Diagnostic::Warning, loc, msg);
}

const char *Sema::typeName(ast::Type *ty) {
  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(ty)) {
    switch (bt->getTypeKind()) {
    case ast::BuiltinType::Int:   return "int";
    case ast::BuiltinType::Float: return "float";
    case ast::BuiltinType::Bool:  return "bool";
    case ast::BuiltinType::Void:  return "void";
    }
  }
  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty))
    return ct->getName().c_str();
  return "unknown";
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

  // int → float promotion.
  if (dst == Ctx.getFloatTy() && src == Ctx.getIntTy())
    return true;

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

  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty)) {
    if (auto *canonical = Ctx.lookupClassType(ct->getName()))
      return canonical;
    error(loc, context + " has unknown class type '" + ct->getName() + "'");
    return nullptr;
  }

  error(loc, context + " has unknown type");
  return nullptr;
}

// Check that a variable is declared and has not been moved.
ast::Type *Sema::checkIdentLive(llvm::StringRef name, ast::SourceLocation loc) {
  auto *ty = CurrentScope->lookup(name);
  if (!ty) {
    error(loc, "use of undeclared variable '" + std::string(name) + "'");
    return nullptr;
  }
  if (CurrentScope->isMoved(name)) {
    error(loc, "use of moved variable '" + std::string(name) + "'");
    return nullptr;
  }
  return ty;
}

// Check ownership compatibility of a single call argument.
bool Sema::checkArgOwnership(const FunctionSig &sig, size_t i,
                             ast::Expr *argExpr,
                             const std::string &calleeName) {
  auto paramOwn = sig.ParamOwnerships[i];
  bool paramConst = sig.ParamIsConst[i];
  bool isMov = ast::isa<ast::MovExpr>(argExpr);
  bool isRef = ast::isa<ast::RefExpr>(argExpr);

  // Determine the argument's ownership and name.
  ast::Ownership argOwn = ast::Ownership::Unique;
  bool argIsIdent = false;
  std::string argName;
  if (auto *id = ast::dyn_cast<ast::Identifier>(argExpr)) {
    argOwn = CurrentScope->lookupOwnership(id->getName());
    argIsIdent = true;
    argName = id->getName();
  } else if (isRef) {
    if (auto *id = ast::cast<ast::RefExpr>(argExpr)->getIdentOperand()) {
      argOwn = CurrentScope->lookupOwnership(id->getName());
      argName = id->getName();
    }
  }

  auto err = [&](const std::string &msg) {
    error(argExpr->getLocation(),
          "argument " + std::to_string(i + 1) + " of '" + calleeName + "': " + msg);
  };

  switch (paramOwn) {
  case ast::Ownership::Unique:
    if (!isMov) {
      if (argIsIdent && argOwn == ast::Ownership::Shared)
        err("shared variable '" + argName + "' cannot be passed to unique parameter");
      else if (argIsIdent && argOwn == ast::Ownership::Reference)
        err("reference '" + argName + "' cannot be passed to unique parameter");
      else
        err("unique ownership must be transferred with 'mov'");
      return false;
    }
    break;

  case ast::Ownership::Shared:
    if (isMov) {
      // mov → shared: OK
    } else if (argIsIdent) {
      if (argOwn == ast::Ownership::Unique) {
        err("unique variable '" + argName + "' must be 'mov'd to shared parameter");
        return false;
      } else if (argOwn == ast::Ownership::Reference) {
        err("reference '" + argName + "' cannot be passed to shared parameter");
        return false;
      }
      // shared → shared: OK
    } else {
      err("unique ownership must be transferred with 'mov' to shared parameter");
      return false;
    }
    break;

  case ast::Ownership::Reference:
    if (isMov) {
      err("'mov' cannot be used when passing to a reference parameter");
      return false;
    } else if (isRef) {
      // &x — check source ownership for non-const ref.
      if (!paramConst) {
        if (auto *id = ast::cast<ast::RefExpr>(argExpr)->getIdentOperand()) {
          if (CurrentScope->lookupOwnership(id->getName()) == ast::Ownership::Shared) {
            err("shared variable '" + std::string(id->getName()) +
                "' cannot be passed to non-const reference parameter");
            return false;
          }
        }
      }
    } else if (argIsIdent && argOwn != ast::Ownership::Reference) {
      err("reference parameter requires '&' on the argument");
      return false;
    } else if (!argIsIdent && !paramConst) {
      err("cannot pass rvalue to non-const reference parameter");
      return false;
    }
    break;
  }
  return true;
}

// Visit an expression and return its resolved type (nullptr on error).
ast::Type *Sema::resolveExprType(ast::Expr *expr) {
  return EC.visit(expr);
}

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

ast::Type *Sema::ExprChecker::visitStringLiteral(ast::StringLiteral *) {
  return S.Ctx.getStringTy();
}

ast::Type *Sema::ExprChecker::visitIdentifier(ast::Identifier *node) {
  return S.checkIdentLive(node->getName(), node->getLocation());
}

ast::Type *Sema::ExprChecker::visitUnaryExpr(ast::UnaryExpr *node) {
  auto *operandTy = visit(node->getOperand());
  if (!operandTy)
    return nullptr;

  if (!operandTy->hasUnaryOp(node->getOpcode())) {
    S.error(node->getLocation(),
            "unary '" + std::string(node->getOpcodeStr()) +
                "' is not defined for type '" +
                std::string(typeName(operandTy)) + "'");
    return nullptr;
  }

  switch (node->getOpcode()) {
  case ast::UnaryOpcode::Neg: return operandTy;
  case ast::UnaryOpcode::Not: return S.Ctx.getBoolTy();
  }
  return nullptr;
}

ast::Type *Sema::ExprChecker::visitBinaryExpr(ast::BinaryExpr *node) {
  auto *lhsTy = visit(node->getLHS());
  auto *rhsTy = visit(node->getRHS());
  if (!lhsTy || !rhsTy)
    return nullptr;

  if (!lhsTy->hasBinaryOp(node->getOpcode(), rhsTy)) {
    S.error(node->getLocation(),
            "operator '" + std::string(node->getOpcodeStr()) +
                "' is not defined for types '" + typeName(lhsTy) +
                "' and '" + typeName(rhsTy) + "'");
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
                    "' have mismatched types '" + typeName(lhsTy) +
                    "' and '" + typeName(rhsTy) + "'");
        return nullptr;
      }
    }
  }

  switch (node->getOpcode()) {
  // Arithmetic: result is float if either operand is float, else int.
  case ast::BinaryOpcode::Add:
    // String concatenation: String + String → String.
    if (lhsTy == S.Ctx.getStringTy() && rhsTy == S.Ctx.getStringTy())
      return S.Ctx.getStringTy();
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
                  "' must be 'bool', got '" + std::string(typeName(lhsTy)) + "'");
      return nullptr;
    }
    if (rhsTy != S.Ctx.getBoolTy()) {
      S.error(node->getRHS()->getLocation(),
              "right operand of '" + std::string(node->getOpcodeStr()) +
                  "' must be 'bool', got '" + std::string(typeName(rhsTy)) + "'");
      return nullptr;
    }
    return S.Ctx.getBoolTy();
  }

  return nullptr;
}

ast::Type *Sema::ExprChecker::visitCallExpr(ast::CallExpr *node) {
  // Type-check all arguments first.
  std::vector<ast::Type *> argTypes;
  for (auto *arg : node->getArguments()) {
    auto *ty = visit(arg);
    argTypes.push_back(ty);
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
                  std::to_string(sig->ParamTypes.size()) + " argument(s), got " +
                  std::to_string(argTypes.size()));
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
    size_t paramIdx = (i < sig->ParamTypes.size()) ? i : sig->ParamTypes.size() - 1;
    if (!S.isAssignable(sig->ParamTypes[paramIdx], argTypes[i]))
      S.error(node->getArguments()[i]->getLocation(),
              "argument " + std::to_string(i + 1) + " of '" +
                  node->getCalleeName() + "' has type '" +
                  typeName(argTypes[i]) + "', expected '" +
                  typeName(sig->ParamTypes[paramIdx]) + "'");

    // ---- Ownership checking for class-type parameters ----
    if (i < sig->ParamTypes.size() &&
        ast::isa<ast::ClassType>(sig->ParamTypes[i]))
      S.checkArgOwnership(*sig, i, node->getArguments()[i],
                          node->getCalleeName());
  }

  return sig->ReturnType;
}

ast::Type *Sema::ExprChecker::visitMovExpr(ast::MovExpr *node) {
  auto *ident = node->getOperand();
  auto *ty = S.checkIdentLive(ident->getName(), ident->getLocation());
  if (!ty)
    return nullptr;
  auto ownership = S.CurrentScope->lookupOwnership(ident->getName());
  if (ownership == ast::Ownership::Reference) {
    S.error(node->getLocation(),
            "'mov' cannot be used on reference variables");
    return nullptr;
  }
  S.CurrentScope->markMoved(ident->getName());
  return ty;
}

ast::Type *Sema::ExprChecker::visitRefExpr(ast::RefExpr *node) {
  if (auto *ident = node->getIdentOperand())
    return S.checkIdentLive(ident->getName(), ident->getLocation());
  // '&' can only be applied to a variable.
  S.error(node->getLocation(),
          "'&' can only be applied to a variable");
  return nullptr;
}

ast::Type *Sema::ExprChecker::visitTernaryExpr(ast::TernaryExpr *node) {
  auto *condTy = visit(node->getCondition());
  auto *trueTy = visit(node->getTrueExpr());
  auto *falseTy = visit(node->getFalseExpr());
  if (!condTy || !trueTy || !falseTy)
    return nullptr;

  if (condTy != S.Ctx.getBoolTy()) {
    S.error(node->getCondition()->getLocation(),
            "ternary condition must be 'bool', got '" +
                std::string(typeName(condTy)) + "'");
    return nullptr;
  }

  if (trueTy != falseTy) {
    S.error(node->getLocation(),
            "ternary branches have mismatched types '" +
                std::string(typeName(trueTy)) + "' and '" +
                std::string(typeName(falseTy)) + "'");
    return nullptr;
  }

  return trueTy;
}



// -- Entry point -------------------------------------------------------------

bool Sema::run(ast::TranslationUnit *tu) {
  CurrentScope = nullptr;
  Diagnostics.clear();
  ErrorCount = 0;

  // Bootstrap builtin functions — out/err take const T&.
  declareFunction(names::kOut, Ctx.getVoidTy(), {Ctx.getObjectTy()},
                  {ast::Ownership::Reference}, {true}, /*variadic=*/true, /*builtin=*/true);
  declareFunction(names::kErr, Ctx.getVoidTy(), {Ctx.getObjectTy()},
                  {ast::Ownership::Reference}, {true}, /*variadic=*/true, /*builtin=*/true);

  // Register type-conversion builtins (take unique builtin types — no ownership check needed).
  auto *StrTy = Ctx.getStringTy();
  declareFunction(names::kStringInt,   StrTy, {Ctx.getIntTy()},   {}, {}, false, true);
  declareFunction(names::kStringFloat, StrTy, {Ctx.getFloatTy()}, {}, {}, false, true);
  declareFunction(names::kStringBool,  StrTy, {Ctx.getBoolTy()},  {}, {}, false, true);
  declareFunction(names::kString,      StrTy, {StrTy},
                  {ast::Ownership::Reference}, {true}, false, true);

  visit(tu);
  return !hasErrors();
}

// -- Top-level ---------------------------------------------------------------

bool Sema::visitTranslationUnit(ast::TranslationUnit *node) {
  bool ok = true;
  for (auto *fn : node->getFuncDecls())
    if (!visitFuncDecl(fn))
      ok = false;
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

bool Sema::visitDeclStmt(ast::DeclStmt *node) {
  return visit(node->getDecl());
}

bool Sema::visitExprStmt(ast::ExprStmt *node) {
  // Type-check the expression; we discard the type.
  return resolveExprType(node->getExpr()) != nullptr;
}

bool Sema::visitAssignStmt(ast::AssignStmt *node) {
  auto *valTy = resolveExprType(node->getValue());
  if (!valTy)
    return false;

  // Look up the variable in all enclosing scopes.
  auto *owner = CurrentScope->findOwner(node->getVarName());
  if (!owner) {
    // First assignment — declare in the current scope.
    CurrentScope->set(node->getVarName(), valTy);
    return true;
  }

  auto *varTy = owner->lookup(node->getVarName());
  auto varOwnership = CurrentScope->lookupOwnership(node->getVarName());

  // Reject assignment to const variables.
  if (CurrentScope->isConst(node->getVarName())) {
    error(node->getLocation(),
          "cannot assign to const variable '" + node->getVarName() + "'");
    return false;
  }

  // Reject reassignment of unique class-type variables (would create two owners).
  if (varOwnership == ast::Ownership::Unique && varTy &&
      !ast::isa<ast::BuiltinType>(varTy)) {
    error(node->getLocation(),
          "cannot reassign unique variable '" + node->getVarName() +
              "'; consider using 'shared' ownership");
    return false;
  }

  if (!isAssignable(varTy, valTy)) {
    error(node->getLocation(),
          "cannot assign value of type '" + std::string(typeName(valTy)) +
              "' to variable '" + node->getVarName() + "' of type '" +
              typeName(varTy) + "'");
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
      error(node->getLocation(),
            "return value of type '" + std::string(typeName(valTy)) +
                "' does not match function return type '" +
                typeName(CurrentReturnType) + "'");
      return false;
    }
    return true;
  }
  // void return
  if (CurrentReturnType && CurrentReturnType != Ctx.getVoidTy()) {
    error(node->getLocation(),
          "non-void function must return a value");
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
          "if condition must be 'bool', got '" +
              std::string(typeName(condTy)) + "'");
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
          "while condition must be 'bool', got '" +
              std::string(typeName(condTy)) + "'");
    return false;
  }

  // Type-check the body.
  return visit(node->getBody());
}

// -- Declarations ------------------------------------------------------------

bool Sema::visitFuncDecl(ast::FuncDecl *node) {
  // Resolve return type.
  ast::Type *retTy = Ctx.getVoidTy();
  if (node->getReturnType()) {
    retTy = resolveType(node->getReturnType(), node->getLocation(),
                        "function '" + node->getName() + "' return type");
    if (!retTy)
      return false;
  }

  // Resolve parameter types and ownerships.
  std::vector<ast::Type *> paramTypes;
  std::vector<ast::Ownership> paramOwns;
  std::vector<bool> paramConst;
  for (auto &p : node->getParams()) {
    auto *ty = resolveType(p.ParamType, node->getLocation(),
                           "parameter '" + p.Name + "'");
    if (!ty)
      return false;
    paramTypes.push_back(ty);
    paramOwns.push_back(p.Own);
    paramConst.push_back(p.IsConst);
  }

  // Register the function in the function table.
  if (lookupFunction(node->getName())) {
    error(node->getLocation(),
          "redefinition of function '" + node->getName() + "'");
    return false;
  }
  declareFunction(node->getName(), retTy, paramTypes, paramOwns, paramConst);

  // Type-check the body in a new scope with params.
  auto *savedRetTy = CurrentReturnType;
  CurrentReturnType = retTy;
  {
    ScopeGuard guard(*this);
    for (size_t i = 0; i < node->getParams().size(); ++i)
      CurrentScope->declare(node->getParams()[i].Name, paramTypes[i],
                            node->getParams()[i].Own, node->getParams()[i].IsConst);
    bool ok = true;
    for (auto *stmt : node->getBody()->getStatements())
      if (!visit(stmt))
        ok = false;
    CurrentReturnType = savedRetTy;
    if (!ok) return false;
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

  auto ownership = node->getOwnership();

  // Reject ownership qualifiers on builtin types.
  if (ownership != ast::Ownership::Unique &&
      node->getType() && ast::isa<ast::BuiltinType>(node->getType())) {
    const char *qual = ownership == ast::Ownership::Shared
                           ? "shared"
                           : "reference (&)";
    error(node->getLocation(),
          std::string(qual) + " qualifier is not allowed on builtin type");
    CurrentScope->set(node->getName(), node->getType());
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

  // Reference must bind via & to an existing variable (lvalue), not a temporary.
  if (ownership == ast::Ownership::Reference) {
    auto *initExpr = node->getInitExpr();
    ast::RefExpr *refExpr = initExpr ? ast::dyn_cast<ast::RefExpr>(initExpr) : nullptr;
    if (!refExpr || !refExpr->getIdentOperand()) {
      error(node->getLocation(),
            "reference variable '" + node->getName() +
                "' must be initialized with '&' on an existing variable");
      if (declTy) CurrentScope->set(node->getName(), declTy);
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
      if (!isAssignable(declTy, initTy)) {
        error(node->getLocation(),
              "initializer of type '" + std::string(typeName(initTy)) +
                  "' does not match declared type '" + typeName(declTy) +
                  "' for variable '" + node->getName() + "'");
        CurrentScope->set(node->getName(), declTy);
        return false;
      }
    } else {
      declTy = initTy;
    }

    // If initializing a unique var from another unique var without mov, reject.
    if (ownership == ast::Ownership::Unique && declTy &&
        !ast::isa<ast::BuiltinType>(declTy)) {
      if (auto *ident = ast::dyn_cast<ast::Identifier>(node->getInitExpr())) {
        auto srcOwnership = CurrentScope->lookupOwnership(ident->getName());
        if (srcOwnership == ast::Ownership::Unique) {
          error(node->getLocation(),
                "cannot copy unique variable '" + ident->getName() +
                    "'; use 'mov' to transfer ownership");
          CurrentScope->declare(node->getName(), declTy, ownership);
          return false;
        }
      }
    }
  }

  if (!declTy) {
    error(node->getLocation(),
          "variable '" + node->getName() +
              "' has no type annotation and no initializer");
    CurrentScope->set(node->getName(), Ctx.getVoidTy());
    return false;
  }

  // Register the variable in the current scope with ownership.
  CurrentScope->declare(node->getName(), declTy, ownership, node->isConst());
  return true;
}

bool Sema::visitMethodDecl(ast::MethodDecl *) {
  // MethodDecl nodes are not produced by the parser yet.
  return true;
}

} // namespace sema
} // namespace paykan
