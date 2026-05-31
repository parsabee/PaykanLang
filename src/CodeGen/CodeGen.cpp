// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "CodeGen.h"
#include "CodeGenNames.h"
#include "ModuleUtils.h"
#include "Names.h"
#include "ParserDriver.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>

#include <llvm/Passes/PassBuilder.h>

namespace paykan {
namespace codegen {

using namespace names;

/// Map Paykan-level builtin function names to their C runtime symbols.
static const llvm::StringMap<const char *> kBuiltinNames = {
    {kPrint,      kPaykanPrint},
    {kPrintln,    kPaykanPrintln},
    {kErrPrint,   kPaykanErrPrint},
    {kErrPrintln, kPaykanErrPrintln},
};

// -- Constructor -------------------------------------------------------------

CodeGen::CodeGen(const sema::SemaContext &semaCtx, llvm::LLVMContext &llvmCtx,
                 llvm::StringRef moduleName,
                 const std::string &projectRoot)
    : ASTCtx(*semaCtx.ASTCtx), LLVMCtx(llvmCtx),
      SemaCtx(semaCtx),
      Module(std::make_unique<llvm::Module>(moduleName, llvmCtx)),
      Builder(llvmCtx), Classes(*this), ProjectRoot(projectRoot) {
}

// -- Scope / ScopeGuard ------------------------------------------------------

CodeGen::Scope::Scope(Scope *parent) : Parent(parent) {}

llvm::AllocaInst *CodeGen::Scope::lookup(llvm::StringRef name) const {
  auto it = Locals.find(name);
  if (it != Locals.end())
    return it->second;
  return Parent ? Parent->lookup(name) : nullptr;
}

bool CodeGen::Scope::isOwned(llvm::StringRef name) const {
  if (SharedVars.count(name)) return true;
  // If the name is declared in THIS scope (but not in SharedVars), it is
  // explicitly NOT owned — stop here and do not walk up to a parent scope
  // that may have an owned variable with the same name.
  if (Locals.count(name)) return false;
  return Parent ? Parent->isOwned(name) : false;
}

void CodeGen::Scope::set(llvm::StringRef name, llvm::AllocaInst *alloca) {
  Locals[name] = alloca;
}

void CodeGen::Scope::declare(llvm::StringRef name, llvm::AllocaInst *alloca,
                             ast::Type *astTy) {
  Locals[name] = alloca;
  if (astTy)
    ASTTypeMap[name] = astTy;
  if (astTy && ast::isRefType(astTy)) {
    SharedVars.insert(name);
    DeclOrder.push_back({alloca, astTy});
  }
}

void CodeGen::Scope::declareUnowned(llvm::StringRef name,
                                    llvm::AllocaInst *alloca,
                                    ast::Type *astTy) {
  // Register the variable and its type but do NOT mark it as owned
  // (i.e. skip SharedVars). Used for None-initialised class vars which
  // hold a raw PaykanObject* singleton rather than a PaykanShared* box.
  Locals[name] = alloca;
  if (astTy)
    ASTTypeMap[name] = astTy;
}

ast::Type *CodeGen::Scope::lookupASTType(llvm::StringRef name) const {
  auto it = ASTTypeMap.find(name);
  if (it != ASTTypeMap.end())
    return it->second;
  return Parent ? Parent->lookupASTType(name) : nullptr;
}

void CodeGen::Scope::updateASTType(llvm::StringRef name, ast::Type *newTy) {
  // Walk up to the scope that owns this variable and update its type entry.
  if (ASTTypeMap.count(name)) {
    ASTTypeMap[name] = newTy;
    return;
  }
  if (Parent)
    Parent->updateASTType(name, newTy);
}

void CodeGen::Scope::promoteToOwned(llvm::StringRef name, ast::Type *astTy) {
  if (ASTTypeMap.count(name) || Locals.count(name)) {
    if (astTy)
      ASTTypeMap[name] = astTy;
    SharedVars.insert(name);
    if (auto *alloca = lookup(name))
      DeclOrder.push_back({alloca, astTy});
    return;
  }
  if (Parent)
    Parent->promoteToOwned(name, astTy);
}

CodeGen::Scope *CodeGen::Scope::findOwner(llvm::StringRef name) {
  if (Locals.count(name))
    return this;
  return Parent ? Parent->findOwner(name) : nullptr;
}

CodeGen::ScopeGuard::ScopeGuard(CodeGen &cg)
    : CG(cg), ScopeObj(cg.CurrentScope) {
  CG.CurrentScope = &ScopeObj;
}

CodeGen::ScopeGuard::~ScopeGuard() {
  // Only emit cleanup if the current block is not yet terminated.
  if (!CG.Builder.GetInsertBlock()->getTerminator())
    CG.emitScopeCleanup(ScopeObj);
  CG.CurrentScope = ScopeObj.Parent;
}

// -- Helpers -----------------------------------------------------------------

llvm::Type *CodeGen::toLLVMType(ast::Type *ty) {
  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(ty)) {
    switch (bt->getTypeKind()) {
    case ast::BuiltinType::Int:
      return llvm::Type::getInt64Ty(LLVMCtx);
    case ast::BuiltinType::Float:
      return llvm::Type::getDoubleTy(LLVMCtx);
    case ast::BuiltinType::Bool:
      return llvm::Type::getInt1Ty(LLVMCtx);
    case ast::BuiltinType::Void:
      return llvm::Type::getVoidTy(LLVMCtx);
    }
  }
  if (ast::isRefType(ty)) {
    // All class/array types are represented as opaque pointers (ptr) for now.
    return llvm::PointerType::getUnqual(LLVMCtx);
  }
  return nullptr;
}

llvm::AllocaInst *CodeGen::createEntryAlloca(llvm::Function *fn,
                                             llvm::StringRef name,
                                             llvm::Type *ty) {
  llvm::IRBuilder<> entryBuilder(&fn->getEntryBlock(),
                                 fn->getEntryBlock().begin());
  return entryBuilder.CreateAlloca(ty, nullptr, name);
}

llvm::Function *CodeGen::declareFunction(llvm::StringRef name,
                                         llvm::FunctionType *fnTy) {
  if (auto *existing = Module->getFunction(name))
    return existing;
  return llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                name, Module.get());
}

llvm::Value *CodeGen::wrapStringLiteral(llvm::Value *rawStr, size_t len) {
  auto *lenVal = llvm::ConstantInt::get(llvm::Type::getInt64Ty(LLVMCtx), len);
  auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);
  auto *i64Ty = llvm::Type::getInt64Ty(LLVMCtx);
  auto *fnTy = llvm::FunctionType::get(ptrTy, {ptrTy, i64Ty}, false);
  auto *callee = declareFunction(kPaykanStringNew, fnTy);
  return Builder.CreateCall(callee, {rawStr, lenVal}, kIRStr);
}

void CodeGen::emitScopeCleanup(Scope &scope) {
  auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);

  // Walk in reverse declaration order for proper LIFO cleanup.
  for (auto it = scope.DeclOrder.rbegin(); it != scope.DeclOrder.rend(); ++it) {
    assert(it->ASTType && ast::isRefType(it->ASTType) &&
           "DeclOrder must only contain ref-typed variables");
    emitRelease(Builder.CreateLoad(ptrTy, it->Alloca));
  }
}

// Emit cleanup for all active scopes up through the function body.
// Called by visitReturnStmt before emitting the ret instruction so that
// every owned variable declared in the function (across nested scopes) is
// released regardless of where the return appears.
void CodeGen::emitAllScopesCleanup() {
  for (Scope *s = CurrentScope; s != nullptr; s = s->Parent)
    emitScopeCleanup(*s);
}

llvm::Value *CodeGen::emitExpr(ast::Expr *expr) {
  return Emitter.visit(expr);
}

// -- Optimization -----------------------------------------------------------

void CodeGen::optimize(llvm::OptimizationLevel level) {
  if (level == llvm::OptimizationLevel::O0)
    return;

  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;

  llvm::PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  llvm::ModulePassManager MPM =
      PB.buildPerModuleDefaultPipeline(level);
  MPM.run(*Module, MAM);
}

// -- Entry point -------------------------------------------------------------

bool CodeGen::run(ast::TranslationUnit *tu) {
  CurrentScope = nullptr;
  bootstrapBuiltins();
  processImports(tu);
  visit(tu);
  return !llvm::verifyModule(*Module, &llvm::errs());
}

/// Returns true when `expr` already produces a PaykanShared* — i.e. the
/// expression is a call/method-call/ternary whose resolved type is a class.
bool CodeGen::exprAlreadyShared(ast::Expr *expr) const {
  // ArrayLiteralExpr always produces a PaykanShared* wrapping a PaykanArray*.
  if (ast::isa<ast::ArrayLiteralExpr>(expr))
    return true;
  // TernaryExpr and MethodCallExpr always produce PaykanShared* when ref-typed.
  if (auto *e = ast::dyn_cast<ast::MethodCallExpr>(expr))
    return e->getResolvedType() && ast::isRefType(e->getResolvedType());
  if (auto *e = ast::dyn_cast<ast::TernaryExpr>(expr))
    return e->getResolvedType() && ast::isRefType(e->getResolvedType());
  // MemberAccessExpr: ref-typed fields store PaykanShared* boxes.
  if (auto *e = ast::dyn_cast<ast::MemberAccessExpr>(expr))
    return e->getResolvedType() && ast::isRefType(e->getResolvedType());
  // CallExpr: only user-defined functions (not builtins) return PaykanShared*.
  if (auto *ce = ast::dyn_cast<ast::CallExpr>(expr)) {
    if (!ce->getResolvedType() || !ast::isRefType(ce->getResolvedType()))
      return false;
    // Builtins in FunctionTable or IdentityCtors return raw pointers — not shared.
    if (FunctionTable.count(ce->getCalleeName()) || IdentityCtors.count(ce->getCalleeName()))
      return false;
    return true;
  }
  return false;
}

void CodeGen::emitRetain(llvm::Value *shared) {
  auto *ptrTy  = llvm::PointerType::getUnqual(LLVMCtx);
  auto *voidTy = llvm::Type::getVoidTy(LLVMCtx);
  auto *fnTy   = llvm::FunctionType::get(voidTy, {ptrTy}, false);
  Builder.CreateCall(declareFunction(kPaykanRetain, fnTy), {shared});
}

void CodeGen::emitRelease(llvm::Value *shared) {
  auto *ptrTy  = llvm::PointerType::getUnqual(LLVMCtx);
  auto *voidTy = llvm::Type::getVoidTy(LLVMCtx);
  auto *fnTy   = llvm::FunctionType::get(voidTy, {ptrTy}, false);
  Builder.CreateCall(declareFunction(kPaykanRelease, fnTy), {shared});
}

llvm::Value *CodeGen::emitSharedNew(llvm::Value *raw, llvm::StringRef name) {
  auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);
  auto *fnTy  = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
  return Builder.CreateCall(declareFunction(kPaykanSharedNew, fnTy), {raw}, name);
}

void CodeGen::bootstrapBuiltins() {
  auto *i64Ty = llvm::Type::getInt64Ty(LLVMCtx);
  auto *voidTy = llvm::Type::getVoidTy(LLVMCtx);
  auto *varArgFnTy = llvm::FunctionType::get(voidTy, {i64Ty}, /*isVarArg=*/true);

  for (auto &[paykanName, runtimeName] : kBuiltinNames)
    FunctionTable[paykanName] = {runtimeName, varArgFnTy, /*IsVariadic=*/true};

  // Register type-conversion builtins.
  auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);
  auto *dblTy = llvm::Type::getDoubleTy(LLVMCtx);

  auto *ptrFromI64  = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
  auto *ptrFromDbl  = llvm::FunctionType::get(ptrTy, {dblTy}, false);

  FunctionTable[kStrInt]   = {kPaykanStringFromInt,   ptrFromI64, false};
  FunctionTable[kStrFloat] = {kPaykanStringFromFloat, ptrFromDbl, false};
  FunctionTable[kStrBool]  = {kPaykanStringFromBool,  ptrFromI64, false};

  // open(path: Str, mode: Str) -> Obj  (PaykanShared*)
  auto *ptrFromPtrPtr = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
  FunctionTable[kOpen] = {kPaykanFileOpen, ptrFromPtrPtr, false};

  // Identity constructors — just return the argument as-is.
  IdentityCtors.insert(kString);
}

// -- Top-level ---------------------------------------------------------------

llvm::Value *CodeGen::visitTranslationUnit(ast::TranslationUnit *node) {
  for (auto *cls : node->getClassDecls())
    visitClassDecl(cls);
  for (auto *fn : node->getFuncDecls())
    visitFuncDecl(fn);
  return nullptr;
}

// -- Statements --------------------------------------------------------------

llvm::Value *CodeGen::visitCompoundStmt(ast::CompoundStmt *node) {
  ScopeGuard guard(*this);
  llvm::Value *last = nullptr;
  for (auto *stmt : node->getStatements())
    last = visit(stmt);
  return last;
}

llvm::Value *CodeGen::visitDeclStmt(ast::DeclStmt *node) {
  return visit(node->getDecl());
}

llvm::Value *CodeGen::visitExprStmt(ast::ExprStmt *node) {
  llvm::Value *val = emitExpr(node->getExpr());
  // If the expression produces a PaykanShared* that is not bound to any
  // variable (e.g. `A();`), release it immediately to avoid a memory leak.
  if (val && exprAlreadyShared(node->getExpr())) {
    auto *ptrTy  = llvm::PointerType::getUnqual(LLVMCtx);
    auto *voidTy = llvm::Type::getVoidTy(LLVMCtx);
    auto *relFnTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
    Builder.CreateCall(declareFunction(kPaykanRelease, relFnTy), {val});
  }
  return val;
}

llvm::Value *CodeGen::emitImplicitVarDecl(llvm::StringRef name,
                                           ast::Expr *rhsExpr,
                                           llvm::Value *val) {
  auto *fn = Builder.GetInsertBlock()->getParent();
  bool isNoneRHS = ast::isa<ast::NoneLiteral>(rhsExpr);

  // Determine the AST class type for the RHS (for isOwned / method dispatch).
  ast::ClassType *rhsAstTy = nullptr;
  if (auto *id = ast::dyn_cast<ast::Identifier>(rhsExpr))
    rhsAstTy = ast::dyn_cast<ast::ClassType>(
        CurrentScope->lookupASTType(id->getName()));
  // For constructor calls / method calls / member access — use resolved type.
  if (!rhsAstTy) {
    ast::Type *resolved = nullptr;
    if (auto *ce = ast::dyn_cast<ast::CallExpr>(rhsExpr))
      resolved = ce->getResolvedType();
    else if (auto *mce = ast::dyn_cast<ast::MethodCallExpr>(rhsExpr))
      resolved = mce->getResolvedType();
    else if (auto *mae = ast::dyn_cast<ast::MemberAccessExpr>(rhsExpr))
      resolved = mae->getResolvedType();
    if (resolved)
      rhsAstTy = ast::dyn_cast<ast::ClassType>(resolved);
  }
  if (!rhsAstTy && val->getType()->isPointerTy())
    rhsAstTy = ASTCtx.getObjTy(); // conservative fallback
  // Canonicalize: parser stubs have no fields/methods.
  if (rhsAstTy) {
    if (auto *canonical = ASTCtx.lookupClassType(rhsAstTy->getName()))
      rhsAstTy = canonical;
  }

  if (!isNoneRHS && val->getType()->isPointerTy() && rhsAstTy) {
    auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);
    if (exprAlreadyShared(rhsExpr)) {
      // val is already a PaykanShared* (from a call/method-call/ternary) — use as-is.
      // Do NOT call emitAsShared here — that would re-emit the expression and call
      // the function a second time.
    } else if (auto *id = ast::dyn_cast<ast::Identifier>(rhsExpr);
               id && CurrentScope->isOwned(id->getName())) {
      // Owned identifier: visitIdentifier already unwrapped to raw; load the
      // PaykanShared* box directly from the alloca and retain it.
      val = Builder.CreateLoad(ptrTy, CurrentScope->lookup(id->getName()),
                               id->getName());
      emitRetain(val);
    } else {
      // Raw value (string literal, unowned identifier, etc.) — box it.
      val = emitSharedNew(val, kIRShared);
    }
  }

  auto *alloca = createEntryAlloca(fn, name, val->getType());
  Builder.CreateStore(val, alloca);

  if (isNoneRHS)
    CurrentScope->declareUnowned(name, alloca, rhsAstTy);
  else
    CurrentScope->declare(name, alloca, static_cast<ast::Type *>(rhsAstTy));
  return val;
}

void CodeGen::emitClassVarRebind(llvm::AllocaInst *alloca, ast::Expr *rhsExpr,
                                  llvm::Value *val) {
  auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);

  llvm::Value *newBox = nullptr;
  // RHS is another owned variable — retain its box (share the reference).
  if (auto *ident = ast::dyn_cast<ast::Identifier>(rhsExpr)) {
    if (CurrentScope->isOwned(ident->getName())) {
      newBox = Builder.CreateLoad(ptrTy, CurrentScope->lookup(ident->getName()),
                                  ident->getName());
      emitRetain(newBox);
    }
  }
  if (!newBox) {
    if (exprAlreadyShared(rhsExpr))
      newBox = val; // already a PaykanShared* — use directly (refcount already = 1)
    else
      newBox = emitSharedNew(val, kIRNewBox); // fresh value — refcount = 1
  }

  // Release the old box and store the new one.
  emitRelease(Builder.CreateLoad(ptrTy, alloca, kIROldBox));
  Builder.CreateStore(newBox, alloca);
}

llvm::Value *CodeGen::visitAssignStmt(ast::AssignStmt *node) {
  llvm::Value *val = emitExpr(node->getValue());
  if (!val)
    return nullptr;

  // Wrap raw string literal into a PaykanString* object.
  if (auto *sl = ast::dyn_cast<ast::StringLiteral>(node->getValue()))
    val = wrapStringLiteral(val, sl->getValue().size());

  auto *owner = CurrentScope->findOwner(node->getVarName());
  if (!owner)
    return emitImplicitVarDecl(node->getVarName(), node->getValue(), val);

  // Variable exists — store into its alloca.
  auto *alloca = owner->lookup(node->getVarName());
  llvm::Type *allocaTy = alloca->getAllocatedType();

  // Implicit int -> float promotion.
  if (allocaTy->isDoubleTy() && val->getType()->isIntegerTy(64))
    val = Builder.CreateSIToFP(val, llvm::Type::getDoubleTy(LLVMCtx),
                               kInt2FPName);

  // Class-type assignment: rebind the local variable to a new shared box.
  auto *astTy = CurrentScope->lookupASTType(node->getVarName());
  if (astTy && ast::isRefType(astTy)) {
    bool wasUnowned = !CurrentScope->isOwned(node->getVarName());
    if (wasUnowned) {
      // Variable was None-initialised (raw ptr) — just box the new value
      // without releasing the old one (it's a singleton, not a shared box).
      llvm::Value *newBox = nullptr;
      if (exprAlreadyShared(node->getValue()))
        newBox = val;
      else
        newBox = emitSharedNew(val, kIRNewBox);
      Builder.CreateStore(newBox, alloca);
    } else {
      emitClassVarRebind(alloca, node->getValue(), val);
    }
    // Narrow the scope type to the concrete RHS type (enables vtable dispatch
    // through base-type variables, e.g. `obj: Obj = MyObj(...)`).
    ast::ClassType *rhsCT = nullptr;
    if (auto *ce = ast::dyn_cast<ast::CallExpr>(node->getValue()))
      rhsCT = ast::dyn_cast<ast::ClassType>(ce->getResolvedType());
    else if (auto *mce = ast::dyn_cast<ast::MethodCallExpr>(node->getValue()))
      rhsCT = ast::dyn_cast<ast::ClassType>(mce->getResolvedType());
    else if (auto *mae = ast::dyn_cast<ast::MemberAccessExpr>(node->getValue()))
      rhsCT = ast::dyn_cast<ast::ClassType>(mae->getResolvedType());
    else if (auto *id = ast::dyn_cast<ast::Identifier>(node->getValue()))
      rhsCT = ast::dyn_cast<ast::ClassType>(
          CurrentScope->lookupASTType(id->getName()));
    if (rhsCT) {
      if (auto *canonical = ASTCtx.lookupClassType(rhsCT->getName()))
        rhsCT = canonical;
      CurrentScope->updateASTType(node->getVarName(), rhsCT);
    }
    if (wasUnowned)
      CurrentScope->promoteToOwned(node->getVarName(),
                                   rhsCT ? static_cast<ast::Type *>(rhsCT) : astTy);
    return val;
  }

  Builder.CreateStore(val, alloca);
  return val;
}

// Emit `expr` as a PaykanShared*.  Handles:
//   - owned identifier  -> retain + return existing shared ptr
//   - None literal      -> wrap the singleton in a fresh shared (refcount=1)
//   - string literal    -> PaykanString_new + PaykanShared_new
//   - call/ternary that already returns PaykanShared* -> pass through
//   - any other raw pointer -> PaykanShared_new
llvm::Value *CodeGen::emitAsShared(ast::Expr *expr) {
  auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);

  // Owned identifier: retain + return the existing PaykanShared*.
  if (auto *id = ast::dyn_cast<ast::Identifier>(expr)) {
    auto *alloca = CurrentScope->lookup(id->getName());
    if (alloca && CurrentScope->isOwned(id->getName())) {
      auto *sharedPtr = Builder.CreateLoad(ptrTy, alloca, id->getName());
      emitRetain(sharedPtr);
      return sharedPtr;
    }
  }

  // Call/ternary that already returns PaykanShared* — pass through.
  if (exprAlreadyShared(expr))
    return emitExpr(expr);

  // Everything else: emit raw, wrap string literals, then PaykanShared_new.
  llvm::Value *val = emitExpr(expr);
  if (auto *sl = ast::dyn_cast<ast::StringLiteral>(expr))
    val = wrapStringLiteral(val, sl->getValue().size());
  return emitSharedNew(val);
}

llvm::Value *CodeGen::visitReturnStmt(ast::ReturnStmt *node) {
  if (node->getReturnValue()) {
    auto *retExpr = node->getReturnValue();
    llvm::Value *val;
    if (CurrentFuncReturnASTType && ast::isRefType(CurrentFuncReturnASTType)) {
      val = emitAsShared(retExpr);
    } else {
      val = emitExpr(retExpr);
    }
    emitAllScopesCleanup();
    return Builder.CreateRet(val);
  }
  emitAllScopesCleanup();
  return Builder.CreateRetVoid();
}

llvm::Value *CodeGen::visitIfStmt(ast::IfStmt *node) {
  llvm::Value *condVal = emitExpr(node->getCondition());
  if (!condVal)
    return nullptr;

  auto *parentFn = Builder.GetInsertBlock()->getParent();
  auto *thenBB = llvm::BasicBlock::Create(LLVMCtx, kIRIfThen, parentFn);
  auto *elseBB = node->hasElse()
                     ? llvm::BasicBlock::Create(LLVMCtx, kIRIfElse, parentFn)
                     : nullptr;
  auto *mergeBB = llvm::BasicBlock::Create(LLVMCtx, kIRIfEnd, parentFn);

  Builder.CreateCondBr(condVal, thenBB, elseBB ? elseBB : mergeBB);

  // Emit then branch.
  Builder.SetInsertPoint(thenBB);
  visit(node->getThenBranch());
  if (!Builder.GetInsertBlock()->getTerminator())
    Builder.CreateBr(mergeBB);

  // Emit else branch.
  if (node->hasElse()) {
    Builder.SetInsertPoint(elseBB);
    visit(node->getElseBranch());
    if (!Builder.GetInsertBlock()->getTerminator())
      Builder.CreateBr(mergeBB);
  }

  Builder.SetInsertPoint(mergeBB);
  return nullptr;
}

llvm::Value *CodeGen::visitWhileStmt(ast::WhileStmt *node) {
  auto *parentFn = Builder.GetInsertBlock()->getParent();
  auto *condBB = llvm::BasicBlock::Create(LLVMCtx, kIRWhileCond, parentFn);
  auto *bodyBB = llvm::BasicBlock::Create(LLVMCtx, kIRWhileBody, parentFn);
  auto *endBB  = llvm::BasicBlock::Create(LLVMCtx, kIRWhileEnd, parentFn);

  // Push loop context for break/continue.
  LoopStack.push_back({condBB, endBB});

  // Branch to the condition block.
  Builder.CreateBr(condBB);

  // Emit condition.
  Builder.SetInsertPoint(condBB);
  llvm::Value *condVal = emitExpr(node->getCondition());
  Builder.CreateCondBr(condVal, bodyBB, endBB);

  // Emit body.
  Builder.SetInsertPoint(bodyBB);
  visit(node->getBody());
  if (!Builder.GetInsertBlock()->getTerminator())
    Builder.CreateBr(condBB);

  // Pop loop context.
  LoopStack.pop_back();

  // Continue after loop.
  Builder.SetInsertPoint(endBB);
  return nullptr;
}

llvm::Value *CodeGen::visitBreakStmt(ast::BreakStmt *) {
  assert(!LoopStack.empty() && "break outside loop");
  Builder.CreateBr(LoopStack.back().EndBB);
  // Create an unreachable block for any code after break.
  auto *deadBB = llvm::BasicBlock::Create(
      LLVMCtx, kIRBreakDead, Builder.GetInsertBlock()->getParent());
  Builder.SetInsertPoint(deadBB);
  return nullptr;
}

llvm::Value *CodeGen::visitContinueStmt(ast::ContinueStmt *) {
  assert(!LoopStack.empty() && "continue outside loop");
  Builder.CreateBr(LoopStack.back().CondBB);
  // Create an unreachable block for any code after continue.
  auto *deadBB = llvm::BasicBlock::Create(
      LLVMCtx, kIRContDead, Builder.GetInsertBlock()->getParent());
  Builder.SetInsertPoint(deadBB);
  return nullptr;
}

// -- Declarations ------------------------------------------------------------

llvm::Value *CodeGen::visitVarDecl(ast::VarDecl *node) {
  auto *fn = Builder.GetInsertBlock()->getParent();

  // Resolve LLVM type from the explicit annotation if present,
  // otherwise fall back to the initializer's type.
  llvm::Type *llvmTy = nullptr;
  if (node->getType()) {
    // Canonicalize ClassType: the parser may have created a stub before Sema
    // populated the canonical ClassType in the registry.
    ast::Type *annoTy = node->getType();
    if (auto *ct = ast::dyn_cast<ast::ClassType>(annoTy))
      if (auto *canonical = ASTCtx.lookupClassType(ct->getName()))
        annoTy = canonical;
    llvmTy = toLLVMType(annoTy);
  }

  llvm::Value *initVal = nullptr;
  if (node->getInitExpr()) {
    bool isNoneInit = ast::isa<ast::NoneLiteral>(node->getInitExpr());

    // For class-typed (non-None) VarDecls, use emitAsShared to get a properly
    // owned PaykanShared* — handles owned-identifier retain, already-shared
    // call pass-through, string literal wrapping, and raw->shared boxing.
    bool isClassDecl = !isNoneInit && node->getType() &&
                       ast::isRefType(node->getType());
    if (isClassDecl) {
      initVal = emitAsShared(node->getInitExpr());
      if (!llvmTy)
        llvmTy = initVal->getType();
    } else {
      initVal = emitExpr(node->getInitExpr());

      // Wrap raw string literal into a PaykanString* object.
      if (auto *sl = ast::dyn_cast<ast::StringLiteral>(node->getInitExpr()))
        initVal = wrapStringLiteral(initVal, sl->getValue().size());

      // If no explicit type, infer from the initializer value.
      if (!llvmTy)
        llvmTy = initVal->getType();

      // Implicit int -> float promotion.
      if (llvmTy->isDoubleTy() && initVal->getType()->isIntegerTy(64))
        initVal = Builder.CreateSIToFP(initVal, llvm::Type::getDoubleTy(LLVMCtx),
                                       kInt2FPName);
    }
  }

  // If we still have no type (shouldn't happen after Sema), default to i64.
  if (!llvmTy)
    llvmTy = llvm::Type::getInt64Ty(LLVMCtx);

  auto *alloca = createEntryAlloca(fn, node->getName(), llvmTy);

  if (initVal)
    Builder.CreateStore(initVal, alloca);
  else
    Builder.CreateStore(llvm::Constant::getNullValue(llvmTy), alloca);

  // Pass nullptr for AST type when None-init so the var is NOT marked owned
  // (it holds a raw PaykanObject* singleton, not a PaykanShared*).
  bool isNoneVar = node->getInitExpr() &&
                   ast::isa<ast::NoneLiteral>(node->getInitExpr());

  // Canonicalize ClassType to the registry entry (the parser may have created
  // a stub with no fields before Sema populated the canonical ClassType).
  ast::Type *scopeTy = node->getType();
  if (auto *ct = ast::dyn_cast<ast::ClassType>(scopeTy)) {
    if (auto *canonical = ASTCtx.lookupClassType(ct->getName()))
      scopeTy = canonical;
  }
  // Narrow the scope type to the concrete RHS type when the declared type is a
  // base class (e.g. `obj: Obj = MyObj(...)`). This ensures method dispatch
  // uses the concrete vtable convention, not the builtin Obj/Str convention.
  if (node->getInitExpr() && scopeTy && ast::isa<ast::ClassType>(scopeTy)) {
    ast::ClassType *rhsCT = nullptr;
    if (auto *ce = ast::dyn_cast<ast::CallExpr>(node->getInitExpr()))
      rhsCT = ast::dyn_cast<ast::ClassType>(ce->getResolvedType());
    if (rhsCT) {
      if (auto *canonical = ASTCtx.lookupClassType(rhsCT->getName()))
        rhsCT = canonical;
      if (rhsCT && rhsCT != ASTCtx.getObjTy() && rhsCT != ASTCtx.getStrTy())
        scopeTy = rhsCT;
    }
  }

  if (isNoneVar)
    CurrentScope->declareUnowned(node->getName(), alloca, scopeTy);
  else
    CurrentScope->declare(node->getName(), alloca, scopeTy);
  return alloca;
}

// -- Expression emitter ------------------------------------------------------

llvm::Value *CodeGen::ExprEmitter::visitIntegerLiteral(ast::IntegerLiteral *node) {
  return llvm::ConstantInt::get(llvm::Type::getInt64Ty(CG.LLVMCtx),
                                node->getValue(), /*isSigned=*/true);
}

llvm::Value *CodeGen::ExprEmitter::visitFloatLiteral(ast::FloatLiteral *node) {
  return llvm::ConstantFP::get(llvm::Type::getDoubleTy(CG.LLVMCtx),
                               node->getValue());
}

llvm::Value *CodeGen::ExprEmitter::visitBoolLiteral(ast::BoolLiteral *node) {
  return llvm::ConstantInt::get(llvm::Type::getInt1Ty(CG.LLVMCtx),
                                node->getValue() ? 1 : 0);
}

llvm::Value *CodeGen::ExprEmitter::visitNoneLiteral(ast::NoneLiteral *) {
  // None is a global singleton PaykanObject with its own vtable.
  // We declare it as an external global and return its address directly
  // (as a raw PaykanObject* — no PaykanShared wrapper, it's immortal).
  auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
  auto *noneGlobal = CG.Module->getOrInsertGlobal(kPaykanObjectNone, ptrTy);
  return noneGlobal;
}

llvm::Value *CodeGen::ExprEmitter::visitStringLiteral(ast::StringLiteral *node) {
  llvm::StringRef content = node->getValue();

  // Return the existing global if we already emitted this string.
  auto it = CG.InternedStrings.find(content);
  if (it != CG.InternedStrings.end())
    return it->second;

  // Emit a new null-terminated i8 array global and intern it.
  auto *gv = CG.Builder.CreateGlobalString(content, kStrGlobalName,
                                           /*AddressSpace=*/0, CG.Module.get());
  gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  CG.InternedStrings.insert({content, gv});
  return gv;
}

llvm::Value *CodeGen::ExprEmitter::visitIdentifier(ast::Identifier *node) {
  auto *alloca = CG.CurrentScope->lookup(node->getName());
  assert(alloca && "Sema should have caught undeclared variable");

  llvm::Value *val = CG.Builder.CreateLoad(alloca->getAllocatedType(), alloca,
                                           node->getName());

  // Owned class/array vars store a PaykanShared* — unwrap to get the underlying object.
  auto *astTy = CG.CurrentScope->lookupASTType(node->getName());
  if (ast::isRefType(astTy) && CG.CurrentScope->isOwned(node->getName())) {
    auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
    auto *getFnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    auto *getFn = CG.declareFunction(kPaykanSharedGet, getFnTy);
    val = CG.Builder.CreateCall(getFn, {val}, node->getName() + ".obj");
  }

  return val;
}

llvm::Value *CodeGen::ExprEmitter::visitMemberAccessExpr(ast::MemberAccessExpr *node) {
  return CG.Classes.visitMemberAccessExpr(node);
}

llvm::Value *CodeGen::ExprEmitter::visitArrayLiteralExpr(ast::ArrayLiteralExpr *node) {
  auto *ptrTy  = llvm::PointerType::getUnqual(CG.LLVMCtx);
  auto *i64Ty  = llvm::Type::getInt64Ty(CG.LLVMCtx);
  auto *voidTy = llvm::Type::getVoidTy(CG.LLVMCtx);

  // Determine element type from the ArrayType resolved by Sema.
  ast::Type *elemTy = nullptr;
  if (auto *at = ast::dyn_cast<ast::ArrayType>(node->getResolvedType()))
    elemTy = at->getElementType();

  // An object array holds PaykanShared* slots; a primitive array holds raw values.
  bool isObjectArray = elemTy &&
                       (ast::isa<ast::ClassType>(elemTy) ||
                        ast::isa<ast::ArrayType>(elemTy));

  size_t len = node->getNumElements();
  auto *lenVal = llvm::ConstantInt::get(i64Ty, (uint64_t)len);

  // Allocate the backing array.
  // For primitive literals we may switch to PaykanArray_new_from_data below;
  // for object arrays and dynamic primitives we allocate here.
  auto *newFnTy = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
  const char *newFnName = isObjectArray ? kPaykanArrayNewObj : kPaykanArrayNew;
  // arr is reassigned below for constant-primitive arrays.
  llvm::Value *arr = (!isObjectArray && len > 0)
                         ? nullptr // defer: may use new_from_data
                         : CG.Builder.CreateCall(
                               CG.declareFunction(newFnName, newFnTy),
                               {lenVal}, kIRArr);

  if (isObjectArray) {
    // Object array: each element needs retain semantics — emit per-element set_obj.
    for (size_t i = 0; i < len; ++i) {
      auto *elemExpr = node->getElements()[i];
      auto *idxVal   = llvm::ConstantInt::get(i64Ty, (uint64_t)i);
      llvm::Value *v = CG.emitAsShared(elemExpr);
      if (!v) return nullptr;
      auto *setFnTy = llvm::FunctionType::get(voidTy, {ptrTy, i64Ty, ptrTy}, false);
      CG.Builder.CreateCall(
          CG.declareFunction(kPaykanArraySetObj, setFnTy), {arr, idxVal, v});
    }
  } else if (len > 0) {
    arr = emitPrimitiveArrayLiteral(node, len, lenVal, newFnTy);
    if (!arr) return nullptr;
  }

  // Box the raw PaykanArray* in a PaykanShared*.
  return CG.emitSharedNew(arr, kIRArrShared);
}

llvm::Value *CodeGen::ExprEmitter::emitPrimitiveArrayLiteral(
    ast::ArrayLiteralExpr *node, size_t len,
    llvm::Value *lenVal, llvm::FunctionType *newFnTy) {
  auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
  auto *i64Ty = llvm::Type::getInt64Ty(CG.LLVMCtx);
  auto *voidTy = llvm::Type::getVoidTy(CG.LLVMCtx);

  // Constant fast path: collect all elements as LLVM constants.
  // If every element is a compile-time constant, emit a single static
  // [N x i64] global and call PaykanArray_new_from_data — one memcpy
  // instead of N individual set calls.
  std::vector<llvm::Constant *> elems;
  elems.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    auto *elemExpr = node->getElements()[i];
    llvm::Value *v = visit(elemExpr);
    if (!v) return nullptr;
    if (auto *sl = ast::dyn_cast<ast::StringLiteral>(elemExpr))
      v = CG.wrapStringLiteral(v, sl->getValue().size());
    if (v->getType()->isDoubleTy())
      v = CG.Builder.CreateBitCast(v, i64Ty, kIRF64Bits);
    else if (v->getType()->isIntegerTy(1))
      v = CG.Builder.CreateZExt(v, i64Ty, kIRBoolExtArr);
    if (auto *c = llvm::dyn_cast<llvm::Constant>(v))
      elems.push_back(c);
    else {
      elems.clear(); // non-constant element: fall through to dynamic path
      break;
    }
  }

  if (!elems.empty()) {
    // Deduplicate identical constant arrays via InternedArrayData.
    // Key: '|'-separated decimal values of the element words.
    std::string key;
    key.reserve(len * 5);
    for (auto *c : elems) {
      key += std::to_string(llvm::cast<llvm::ConstantInt>(c)->getZExtValue());
      key += '|';
    }

    llvm::GlobalVariable *global = nullptr;
    auto internIt = CG.InternedArrayData.find(key);
    if (internIt != CG.InternedArrayData.end()) {
      global = internIt->second;
    } else {
      auto *arrTy    = llvm::ArrayType::get(i64Ty, len);
      auto *initData = llvm::ConstantArray::get(arrTy, elems);
      global = new llvm::GlobalVariable(
          *CG.Module, arrTy, /*isConstant=*/true,
          llvm::GlobalValue::PrivateLinkage, initData, kIRArrData);
      global->setAlignment(llvm::Align(8));
      global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
      CG.InternedArrayData.insert({key, global});
    }

    auto *fromDataFnTy = llvm::FunctionType::get(ptrTy, {i64Ty, ptrTy}, false);
    return CG.Builder.CreateCall(
        CG.declareFunction(kPaykanArrayNewFromData, fromDataFnTy),
        {lenVal, global}, kIRArr);
  }

  // Dynamic path: allocate then fill per element.
  llvm::Value *arr = CG.Builder.CreateCall(
      CG.declareFunction(kPaykanArrayNew, newFnTy), {lenVal}, kIRArr);
  for (size_t i = 0; i < len; ++i) {
    auto *elemExpr = node->getElements()[i];
    auto *idxVal   = llvm::ConstantInt::get(i64Ty, (uint64_t)i);
    llvm::Value *v = visit(elemExpr);
    if (!v) return nullptr;
    if (auto *sl = ast::dyn_cast<ast::StringLiteral>(elemExpr))
      v = CG.wrapStringLiteral(v, sl->getValue().size());
    if (v->getType()->isDoubleTy())
      v = CG.Builder.CreateBitCast(v, i64Ty, kIRF64Bits);
    else if (v->getType()->isIntegerTy(1))
      v = CG.Builder.CreateZExt(v, i64Ty, kIRBoolExtArr);
    auto *setFnTy = llvm::FunctionType::get(voidTy, {ptrTy, i64Ty, i64Ty}, false);
    CG.Builder.CreateCall(
        CG.declareFunction(kPaykanArraySet, setFnTy), {arr, idxVal, v});
  }
  return arr;
}

llvm::Value *CodeGen::ExprEmitter::visitSubscriptExpr(ast::SubscriptExpr *node) {
  auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
  auto *i64Ty = llvm::Type::getInt64Ty(CG.LLVMCtx);

  // Emit the index (shared by both string and array paths).
  llvm::Value *idx = visit(node->getIndex());
  if (!idx) return nullptr;
  if (idx->getType()->isIntegerTy(1))
    idx = CG.Builder.CreateZExt(idx, i64Ty, kIRIdxExt);

  // Determine whether the receiver is a bare Str (not Str[]).
  // We check the scope type of the array sub-expression.
  bool receiverIsStr = false;
  if (auto *id = ast::dyn_cast<ast::Identifier>(node->getArray())) {
    if (CG.CurrentScope) {
      auto *t = CG.CurrentScope->lookupASTType(id->getName());
      receiverIsStr = (t == CG.ASTCtx.getStrTy());
    }
  }

  // String subscript: str[idx] -> Str  (PaykanString_at)
  if (receiverIsStr) {
    llvm::Value *strRaw = visit(node->getArray());
    if (!strRaw) return nullptr;
    // PaykanString_at returns a PaykanShared* — unwrap to raw PaykanString*.
    auto *atFnTy = llvm::FunctionType::get(ptrTy, {ptrTy, i64Ty}, false);
    llvm::Value *shared = CG.Builder.CreateCall(
        CG.declareFunction(kPaykanStringAt, atFnTy), {strRaw, idx}, "str.at");
    auto *getFnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    return CG.Builder.CreateCall(
        CG.declareFunction(kPaykanSharedGet, getFnTy), {shared}, "str.at.obj");
  }

  // Array subscript: arr[idx]
  // visitIdentifier already calls PaykanShared_get for owned array vars,
  // so visit() yields the raw PaykanArray* directly.
  llvm::Value *arrRaw = visit(node->getArray());
  if (!arrRaw) return nullptr;

  // Call PaykanArray_get(arr, idx) -> i64.
  auto *getFnTy = llvm::FunctionType::get(i64Ty, {ptrTy, i64Ty}, false);
  llvm::Value *raw = CG.Builder.CreateCall(
      CG.declareFunction(kPaykanArrayGet, getFnTy), {arrRaw, idx}, kIRElemRaw);

  // Reinterpret the 8-byte slot based on the element type.
  ast::Type *elemTy = node->getResolvedType();
  if (!elemTy)
    return raw; // unknown type: return as i64

  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(elemTy)) {
    switch (bt->getTypeKind()) {
    case ast::BuiltinType::Float:
      return CG.Builder.CreateBitCast(raw,
                                      llvm::Type::getDoubleTy(CG.LLVMCtx),
                                      kIRElemF64);
    case ast::BuiltinType::Bool:
      return CG.Builder.CreateTrunc(raw,
                                    llvm::Type::getInt1Ty(CG.LLVMCtx),
                                    kIRElemBool);
    case ast::BuiltinType::Int:
    default:
      return raw;
    }
  }
  // ClassType or ArrayType: slot stores a PaykanShared* as bits — convert back,
  // then unwrap to the raw PaykanObject* (same as visitIdentifier for owned vars).
  llvm::Value *shared = CG.Builder.CreateIntToPtr(raw, ptrTy, kIRElemShared);
  auto *unwrapFnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
  return CG.Builder.CreateCall(
      CG.declareFunction(kPaykanSharedGet, unwrapFnTy), {shared}, kIRElemObj);
}

llvm::Value *CodeGen::ExprEmitter::visitTernaryExpr(ast::TernaryExpr *node) {
  llvm::Value *condVal = visit(node->getCondition());
  if (!condVal)
    return nullptr;

  // When the ternary resolves to a class type, each branch must produce a
  // PaykanShared*.  Use emitAsShared to normalise string literals, owned vars,
  // None, and call results uniformly.
  bool isClassResult = node->getResolvedType() &&
                       ast::isa<ast::ClassType>(node->getResolvedType());

  auto *parentFn = CG.Builder.GetInsertBlock()->getParent();
  auto *thenBB  = llvm::BasicBlock::Create(CG.LLVMCtx, kIRTernThen, parentFn);
  auto *elseBB  = llvm::BasicBlock::Create(CG.LLVMCtx, kIRTernElse, parentFn);
  auto *mergeBB = llvm::BasicBlock::Create(CG.LLVMCtx, kIRTernEnd,  parentFn);

  CG.Builder.CreateCondBr(condVal, thenBB, elseBB);

  CG.Builder.SetInsertPoint(thenBB);
  llvm::Value *trueVal = isClassResult
      ? CG.emitAsShared(node->getTrueExpr())
      : visit(node->getTrueExpr());
  auto *thenEndBB = CG.Builder.GetInsertBlock();
  CG.Builder.CreateBr(mergeBB);

  CG.Builder.SetInsertPoint(elseBB);
  llvm::Value *falseVal = isClassResult
      ? CG.emitAsShared(node->getFalseExpr())
      : visit(node->getFalseExpr());
  auto *elseEndBB = CG.Builder.GetInsertBlock();
  CG.Builder.CreateBr(mergeBB);

  CG.Builder.SetInsertPoint(mergeBB);
  auto *phi = CG.Builder.CreatePHI(trueVal->getType(), 2, kIRTern);
  phi->addIncoming(trueVal, thenEndBB);
  phi->addIncoming(falseVal, elseEndBB);
  return phi;
}


llvm::Value *CodeGen::ExprEmitter::visitUnaryExpr(ast::UnaryExpr *node) {
  llvm::Value *operand = visit(node->getOperand());
  if (!operand)
    return nullptr;

  switch (node->getOpcode()) {
  case ast::UnaryOpcode::Neg:
    if (operand->getType()->isDoubleTy())
      return CG.Builder.CreateFNeg(operand, kIRFNeg);
    return CG.Builder.CreateNeg(operand, kIRNeg);

  case ast::UnaryOpcode::Not:
    return CG.Builder.CreateNot(operand, kIRNot);
  case ast::UnaryOpcode::Count: break;
  }
  llvm_unreachable("unknown UnaryOpcode");
}

llvm::Value *CodeGen::ExprEmitter::visitBinaryExpr(ast::BinaryExpr *node) {
  // Short-circuit logical operators: evaluate LHS first, conditionally evaluate RHS.
  if (node->getOpcode() == ast::BinaryOpcode::And) {
    auto *parentFn = CG.Builder.GetInsertBlock()->getParent();
    auto *rhsBB = llvm::BasicBlock::Create(CG.LLVMCtx, kIRAndRhs, parentFn);
    auto *mergeBB = llvm::BasicBlock::Create(CG.LLVMCtx, kIRAndEnd, parentFn);

    llvm::Value *lhs = visit(node->getLHS());
    auto *lhsBB = CG.Builder.GetInsertBlock();
    CG.Builder.CreateCondBr(lhs, rhsBB, mergeBB);

    CG.Builder.SetInsertPoint(rhsBB);
    llvm::Value *rhs = visit(node->getRHS());
    auto *rhsEndBB = CG.Builder.GetInsertBlock();
    CG.Builder.CreateBr(mergeBB);

    CG.Builder.SetInsertPoint(mergeBB);
    auto *phi = CG.Builder.CreatePHI(llvm::Type::getInt1Ty(CG.LLVMCtx), 2, kIRAnd);
    phi->addIncoming(llvm::ConstantInt::getFalse(CG.LLVMCtx), lhsBB);
    phi->addIncoming(rhs, rhsEndBB);
    return phi;
  }
  if (node->getOpcode() == ast::BinaryOpcode::Or) {
    auto *parentFn = CG.Builder.GetInsertBlock()->getParent();
    auto *rhsBB = llvm::BasicBlock::Create(CG.LLVMCtx, kIROrRhs, parentFn);
    auto *mergeBB = llvm::BasicBlock::Create(CG.LLVMCtx, kIROrEnd, parentFn);

    llvm::Value *lhs = visit(node->getLHS());
    auto *lhsBB = CG.Builder.GetInsertBlock();
    CG.Builder.CreateCondBr(lhs, mergeBB, rhsBB);

    CG.Builder.SetInsertPoint(rhsBB);
    llvm::Value *rhs = visit(node->getRHS());
    auto *rhsEndBB = CG.Builder.GetInsertBlock();
    CG.Builder.CreateBr(mergeBB);

    CG.Builder.SetInsertPoint(mergeBB);
    auto *phi = CG.Builder.CreatePHI(llvm::Type::getInt1Ty(CG.LLVMCtx), 2, kIROr);
    phi->addIncoming(llvm::ConstantInt::getTrue(CG.LLVMCtx), lhsBB);
    phi->addIncoming(rhs, rhsEndBB);
    return phi;
  }

  llvm::Value *lhs = visit(node->getLHS());
  llvm::Value *rhs = visit(node->getRHS());
  if (!lhs || !rhs)
    return nullptr;

  // Implicit int -> float promotion: if one side is double, promote the other.
  if (lhs->getType()->isDoubleTy() && rhs->getType()->isIntegerTy(64))
    rhs = CG.Builder.CreateSIToFP(rhs, llvm::Type::getDoubleTy(CG.LLVMCtx),
                                   kInt2FPName);
  else if (rhs->getType()->isDoubleTy() && lhs->getType()->isIntegerTy(64))
    lhs = CG.Builder.CreateSIToFP(lhs, llvm::Type::getDoubleTy(CG.LLVMCtx),
                                   kInt2FPName);

  bool isFloat = lhs->getType()->isDoubleTy();

  switch (node->getOpcode()) {
  // -- Arithmetic -----------------------------------------------------------
  case ast::BinaryOpcode::Add:
    if (lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy()) {
      // Wrap raw string literals into PaykanString* objects before concat.
      if (auto *sl = ast::dyn_cast<ast::StringLiteral>(node->getLHS()))
        lhs = CG.wrapStringLiteral(lhs, sl->getValue().size());
      if (auto *sl = ast::dyn_cast<ast::StringLiteral>(node->getRHS()))
        rhs = CG.wrapStringLiteral(rhs, sl->getValue().size());

      // Unwrap PaykanShared* -> raw PaykanObject* for any call/method/ternary
      // result used directly as a concat operand.
      auto *ptrTy2 = llvm::PointerType::getUnqual(CG.LLVMCtx);
      auto *getFnTy2 = llvm::FunctionType::get(ptrTy2, {ptrTy2}, false);
      if (CG.exprAlreadyShared(node->getLHS()))
        lhs = CG.Builder.CreateCall(CG.declareFunction(kPaykanSharedGet, getFnTy2), {lhs}, kIRLhsObj);
      if (CG.exprAlreadyShared(node->getRHS()))
        rhs = CG.Builder.CreateCall(CG.declareFunction(kPaykanSharedGet, getFnTy2), {rhs}, kIRRhsObj);

      // String concatenation: call PaykanString_concat(lhs, rhs) -> ptr
      auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
      auto *fnTy = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
      auto *callee = CG.declareFunction(kPaykanStringConcat, fnTy);
      return CG.Builder.CreateCall(callee, {lhs, rhs}, kIRConcat);
    }
    return isFloat ? CG.Builder.CreateFAdd(lhs, rhs, kIRFAdd)
                   : CG.Builder.CreateAdd(lhs, rhs, kIRAdd);
  case ast::BinaryOpcode::Sub:
    return isFloat ? CG.Builder.CreateFSub(lhs, rhs, kIRFSub)
                   : CG.Builder.CreateSub(lhs, rhs, kIRSub);
  case ast::BinaryOpcode::Mul:
    return isFloat ? CG.Builder.CreateFMul(lhs, rhs, kIRFMul)
                   : CG.Builder.CreateMul(lhs, rhs, kIRMul);
  case ast::BinaryOpcode::Div:
    return isFloat ? CG.Builder.CreateFDiv(lhs, rhs, kIRFDiv)
                   : CG.Builder.CreateSDiv(lhs, rhs, kIRSDiv);
  case ast::BinaryOpcode::Mod:
    return isFloat ? CG.Builder.CreateFRem(lhs, rhs, kIRFMod)
                   : CG.Builder.CreateSRem(lhs, rhs, kIRSRem);

  // -- Relational -----------------------------------------------------------
  case ast::BinaryOpcode::Lt:
    return isFloat ? CG.Builder.CreateFCmpOLT(lhs, rhs, kIRFLT)
                   : CG.Builder.CreateICmpSLT(lhs, rhs, kIRSLT);
  case ast::BinaryOpcode::Gt:
    return isFloat ? CG.Builder.CreateFCmpOGT(lhs, rhs, kIRFGT)
                   : CG.Builder.CreateICmpSGT(lhs, rhs, kIRSGT);
  case ast::BinaryOpcode::Le:
    return isFloat ? CG.Builder.CreateFCmpOLE(lhs, rhs, kIRFLE)
                   : CG.Builder.CreateICmpSLE(lhs, rhs, kIRSLE);
  case ast::BinaryOpcode::Ge:
    return isFloat ? CG.Builder.CreateFCmpOGE(lhs, rhs, kIRFGE)
                   : CG.Builder.CreateICmpSGE(lhs, rhs, kIRSGE);

  // -- Equality -------------------------------------------------------------
  case ast::BinaryOpcode::Eq:
    if (isFloat) return CG.Builder.CreateFCmpOEQ(lhs, rhs, kIRFEQ);
    return CG.Builder.CreateICmpEQ(lhs, rhs, kIREQ);
  case ast::BinaryOpcode::Ne:
    if (isFloat) return CG.Builder.CreateFCmpONE(lhs, rhs, kIRFNE);
    return CG.Builder.CreateICmpNE(lhs, rhs, kIRNE);
  case ast::BinaryOpcode::And:
  case ast::BinaryOpcode::Or:
    llvm_unreachable("And/Or handled above via short-circuit logic");
  case ast::BinaryOpcode::Count:
    break;
  }
  llvm_unreachable("unknown BinaryOpcode");
}

llvm::Value *CodeGen::ExprEmitter::emitIdentityCtor(ast::CallExpr *node) {
  llvm::Value *arg = visit(node->getArguments()[0]);
  if (!arg) return nullptr;
  if (auto *sl = ast::dyn_cast<ast::StringLiteral>(node->getArguments()[0]))
    arg = CG.wrapStringLiteral(arg, sl->getValue().size());
  return arg;
}

llvm::Value *CodeGen::ExprEmitter::emitBuiltinCall(ast::CallExpr *node) {
  auto it = CG.FunctionTable.find(node->getCalleeName());
  if (it == CG.FunctionTable.end())
    return nullptr;
  auto &info = it->second;

  llvm::Function *callee = CG.declareFunction(info.RuntimeName, info.FnTy);

  std::vector<llvm::Value *> args;
  if (info.IsVariadic) {
    auto *i64Ty = llvm::Type::getInt64Ty(CG.LLVMCtx);
    args.push_back(llvm::ConstantInt::get(i64Ty, node->getNumArguments()));
  }

  for (size_t i = 0; i < node->getNumArguments(); ++i) {
    auto *argExpr = node->getArguments()[i];
    llvm::Value *v = visit(argExpr);
    if (!v)
      return nullptr;
    // Wrap raw string literals into PaykanString* objects.
    if (auto *sl = ast::dyn_cast<ast::StringLiteral>(argExpr))
      v = CG.wrapStringLiteral(v, sl->getValue().size());
    // Unwrap PaykanShared* -> raw PaykanObject* for class-typed call/method-call/
    // ternary results passed directly to a builtin vararg (e.g. out()).
    // visitIdentifier already unwraps owned variables; this covers the case
    // where a Str-returning call is used as an argument without an intermediate
    // variable assignment.
    if (CG.exprAlreadyShared(argExpr) && v->getType()->isPointerTy()) {
      auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
      auto *getFnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
      v = CG.Builder.CreateCall(
          CG.declareFunction(kPaykanSharedGet, getFnTy), {v}, kIRUnboxed);
    }
    // Bool (i1) -> i64 coercion when the callee expects i64.
    if (v->getType()->isIntegerTy(1)) {
      auto *i64Ty = llvm::Type::getInt64Ty(CG.LLVMCtx);
      v = CG.Builder.CreateZExt(v, i64Ty, kIRBoolExt);
    }
    args.push_back(v);
  }

  if (info.FnTy->getReturnType()->isVoidTy()) {
    CG.Builder.CreateCall(callee, args);
    return nullptr;
  }
  return CG.Builder.CreateCall(callee, args, kIRCall);
}

llvm::Value *CodeGen::ExprEmitter::visitCallExpr(ast::CallExpr *node) {
  // -- Identity constructor (e.g. String(x)) — return the arg as-is ---------
  if (CG.IdentityCtors.count(node->getCalleeName()) &&
      node->getNumArguments() == 1)
    return emitIdentityCtor(node);

  // -- Concrete builtin function from FunctionTable -------------------------
  if (CG.FunctionTable.count(node->getCalleeName()))
    return emitBuiltinCall(node);

  // -- __super__(args): call parent class __init__ with self ----------------
  if (node->getCalleeName() == kMethodSuper) {
    if (!CG.Classes.CurrentMethodClassType)
      return nullptr;
    auto *superClass = CG.Classes.CurrentMethodClassType->getSuperClass();
    if (!superClass) return nullptr;
    std::string superInitName = superClass->getName() + "_" + names::kMethodInit;
    llvm::Function *superFn = CG.Module->getFunction(superInitName);
    if (!superFn) return nullptr;
    // `self` is the raw ptr stored in the unowned "self" alloca.
    auto *selfAlloca = CG.CurrentScope->lookup(kSelf);
    auto *ptrTy2 = llvm::PointerType::getUnqual(CG.LLVMCtx);
    auto *selfVal = CG.Builder.CreateLoad(ptrTy2, selfAlloca, kSelf);
    std::vector<llvm::Value *> initArgs = {selfVal};
    for (size_t i = 0; i < node->getNumArguments(); ++i) {
      auto *argExpr = node->getArguments()[i];
      auto *ptrTy3  = llvm::PointerType::getUnqual(CG.LLVMCtx);

      // Class-typed args must arrive as PaykanShared* (same as user-fn calls).
      // Check whether the arg is an owned identifier so we load the shared box
      // directly instead of going through visitIdentifier (which unwraps).
      bool passedAsShared = false;
      if (auto *id = ast::dyn_cast<ast::Identifier>(argExpr)) {
        if (CG.CurrentScope && CG.CurrentScope->isOwned(id->getName())) {
          auto *argAlloca = CG.CurrentScope->lookup(id->getName());
          llvm::Value *v = CG.Builder.CreateLoad(ptrTy3, argAlloca,
                                                  id->getName());
          CG.emitRetain(v);
          initArgs.push_back(v);
          passedAsShared = true;
        }
      }
      if (!passedAsShared) {
        llvm::Value *v = visit(argExpr);
        if (!v) return nullptr;
        if (auto *sl = ast::dyn_cast<ast::StringLiteral>(argExpr))
          v = CG.wrapStringLiteral(v, sl->getValue().size());
        initArgs.push_back(v);
      }
    }
    CG.Builder.CreateCall(superFn, initArgs);
    return nullptr;
  }

  // User-defined function — look up in the LLVM module.
  llvm::Function *callee = CG.Module->getFunction(node->getCalleeName());
  if (!callee)
    return nullptr;

  std::vector<llvm::Value *> args;
  for (size_t i = 0; i < node->getNumArguments(); ++i) {
    auto *arg = node->getArguments()[i];

    // Class-type args: retain the PaykanShared* and pass it directly.
    // The callee owns that reference (releases on scope exit).
    // Any mutation inside the callee via PaykanShared_set is visible to
    // the caller because they share the same box.
    llvm::Value *v = nullptr;
    bool isClassArg = i < callee->arg_size() &&
                      callee->getArg(i)->getType()->isPointerTy();
    if (isClassArg) {
      auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
      if (auto *id = ast::dyn_cast<ast::Identifier>(arg)) {
        auto *argAlloca = CG.CurrentScope->lookup(id->getName());
        if (argAlloca && CG.CurrentScope->isOwned(id->getName())) {
          // Load the existing shared box, retain, pass.
          v = CG.Builder.CreateLoad(ptrTy, argAlloca, id->getName());
          CG.emitRetain(v);
        }
      }
      if (!v) {
        // Temporary (literal, call result, etc.): produce a PaykanShared*.
        llvm::Value *raw = visit(arg);
        if (!raw) return nullptr;
        if (auto *sl = ast::dyn_cast<ast::StringLiteral>(arg))
          raw = CG.wrapStringLiteral(raw, sl->getValue().size());
        // If the arg already produced a PaykanShared* (e.g. Str-returning user
        // function), use it directly — wrapping again creates a double-box.
        if (CG.exprAlreadyShared(arg))
          v = raw;
        else
          v = CG.emitSharedNew(raw, kIRArgShared);
      }
      args.push_back(v);
      continue;
    }

    // Non-class arg: evaluate normally.
    v = visit(arg);
    if (!v) return nullptr;
    // Wrap raw string literals into PaykanString* objects.
    if (auto *sl = ast::dyn_cast<ast::StringLiteral>(arg))
      v = CG.wrapStringLiteral(v, sl->getValue().size());

    // Bool (i1) -> i64 coercion when the callee expects i64.
    if (v->getType()->isIntegerTy(1) &&
        i < callee->arg_size() &&
        callee->getArg(i)->getType()->isIntegerTy(64)) {
      auto *i64Ty = llvm::Type::getInt64Ty(CG.LLVMCtx);
      v = CG.Builder.CreateZExt(v, i64Ty, kIRBoolExt);
    }
    args.push_back(v);
  }

  if (callee->getReturnType()->isVoidTy()) {
    CG.Builder.CreateCall(callee, args);
    return nullptr;
  }
  return CG.Builder.CreateCall(callee, args, kIRCall);
}

llvm::Value *CodeGen::ExprEmitter::emitArrayPush(ast::MethodCallExpr *node,
                                                  llvm::Value *recv,
                                                  ast::Type *elemTy) {
  auto *ptrTy  = llvm::PointerType::getUnqual(CG.LLVMCtx);
  auto *i64Ty  = llvm::Type::getInt64Ty(CG.LLVMCtx);
  auto *voidTy = llvm::Type::getVoidTy(CG.LLVMCtx);

  if (node->getNumArguments() != 1) return nullptr;
  auto *argExpr = node->getArguments()[0];

  bool isObjElem = ast::isa<ast::ClassType>(elemTy) || ast::isa<ast::ArrayType>(elemTy);
  if (isObjElem) {
    // Use emitAsShared so that existing owned variables are retained rather than
    // double-wrapped (emitExpr on a ref-typed identifier unwraps to the raw pointer,
    // and a subsequent emitSharedNew would create a second owner of the same object).
    llvm::Value *argVal = CG.emitAsShared(argExpr);
    if (!argVal) return nullptr;
    auto *fnTy = llvm::FunctionType::get(voidTy, {ptrTy, ptrTy}, false);
    CG.Builder.CreateCall(CG.declareFunction(kPaykanArrayPushObj, fnTy), {recv, argVal});
  } else {
    llvm::Value *argVal = CG.emitExpr(argExpr);
    if (!argVal) return nullptr;
    if (argVal->getType()->isDoubleTy())
      argVal = CG.Builder.CreateBitCast(argVal, i64Ty, kIRF64Bits);
    else if (argVal->getType()->isIntegerTy(1))
      argVal = CG.Builder.CreateZExt(argVal, i64Ty, kIRBoolExt);
    auto *fnTy = llvm::FunctionType::get(voidTy, {ptrTy, i64Ty}, false);
    CG.Builder.CreateCall(CG.declareFunction(kPaykanArrayPush, fnTy), {recv, argVal});
  }
  return nullptr; // void
}

llvm::Value *CodeGen::ExprEmitter::emitArrayPop(llvm::Value *recv, ast::Type *elemTy) {
  auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
  auto *i64Ty = llvm::Type::getInt64Ty(CG.LLVMCtx);

  bool isObjElem = ast::isa<ast::ClassType>(elemTy) || ast::isa<ast::ArrayType>(elemTy);
  if (isObjElem) {
    auto *fnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    return CG.Builder.CreateCall(CG.declareFunction(kPaykanArrayPopObj, fnTy), {recv}, kIRMcall);
  }

  auto *fnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
  llvm::Value *rawPtr = CG.Builder.CreateCall(
      CG.declareFunction(kPaykanArrayPop, fnTy), {recv}, kIRMcall);
  // Runtime stores the 8-byte slot as a void*; reinterpret as the element type.
  llvm::Type *retLLTy = CG.toLLVMType(elemTy);
  if (!retLLTy || retLLTy == ptrTy) return rawPtr;
  llvm::Value *asI64 = CG.Builder.CreatePtrToInt(rawPtr, i64Ty, kIRMcall);
  return retLLTy->isIntegerTy(64) ? asI64
                                  : CG.Builder.CreateBitCast(asI64, retLLTy, kIRMcall);
}

llvm::Value *CodeGen::ExprEmitter::visitMethodCallExpr(ast::MethodCallExpr *node) {
  // Emit the receiver and resolve its ClassType.
  llvm::Value *recv = visit(node->getReceiver());
  if (!recv) return nullptr;

  // Wrap a raw string literal receiver (shouldn't happen in practice but be safe).
  if (auto *sl = ast::dyn_cast<ast::StringLiteral>(node->getReceiver()))
    recv = CG.wrapStringLiteral(recv, sl->getValue().size());

  // Array literal receivers arrive as PaykanShared* — unwrap before dispatch.
  if (ast::isa<ast::ArrayLiteralExpr>(node->getReceiver()) &&
      recv->getType()->isPointerTy()) {
    auto *ptrTy2 = llvm::PointerType::getUnqual(CG.LLVMCtx);
    auto *getFnTy2 = llvm::FunctionType::get(ptrTy2, {ptrTy2}, false);
    recv = CG.Builder.CreateCall(
        CG.declareFunction(kPaykanSharedGet, getFnTy2), {recv}, kIRRecvObj);
  }

  // If the receiver is already a PaykanShared* (e.g. result of another method call
  // or a Str-returning user-defined call), unwrap it to the raw PaykanObject*
  // before vtable dispatch.
  if (CG.exprAlreadyShared(node->getReceiver()) && recv->getType()->isPointerTy()) {
    auto *sharedPtrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
    auto *getFnTy = llvm::FunctionType::get(sharedPtrTy, {sharedPtrTy}, false);
    recv = CG.Builder.CreateCall(
        CG.declareFunction(kPaykanSharedGet, getFnTy), {recv}, kIRRecvObj);
  }

  // Determine the vtable slot index via the AST ClassType.
  // The receiver's Sema type is stored in the scope; we resolve it by visiting
  // the receiver through Sema's type map — but here in CodeGen we get it from
  // the Sema-annotated scope.  The simplest approach: look up the receiver's
  // class name through the ExprEmitter to get a ClassType*, then call
  // getVTableIndex.
  //
  // We use the LLVM IR approach: the object layout is { vtable*, fields... }.
  // vtable is a ptr-to-array-of-fn-ptrs.  Load vtable[slot](recv, args...).

  auto *ptrTy  = llvm::PointerType::getUnqual(CG.LLVMCtx);
  auto *i64Ty  = llvm::Type::getInt64Ty(CG.LLVMCtx);

  // -- Resolve the AST type of the receiver ----------------------------------
  // Returns the AST Type* (ArrayType, ClassType, …) for the receiver expression.
  // Handles identifiers, `self`, and member-access expressions.
  auto resolveReceiverASTType = [&]() -> ast::Type * {
    auto *recvExpr = node->getReceiver();
    if (auto *id = ast::dyn_cast<ast::Identifier>(recvExpr)) {
      // Named variable (or `self`).
      if (CG.CurrentScope) {
        if (auto *t = CG.CurrentScope->lookupASTType(id->getName()))
          return t;
      }
      if (id->getName() == kSelf && CG.Classes.CurrentMethodClassType)
        return CG.Classes.CurrentMethodClassType;
    }
    if (auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(recvExpr)) {
      if (auto *rid = ast::dyn_cast<ast::Identifier>(ma->getReceiver())) {
        ast::Type *ownerTy = CG.CurrentScope
                                 ? CG.CurrentScope->lookupASTType(rid->getName())
                                 : nullptr;
        if (!ownerTy && rid->getName() == kSelf && CG.Classes.CurrentMethodClassType)
          ownerTy = CG.Classes.CurrentMethodClassType;
        if (auto *ownerCt = ast::dyn_cast<ast::ClassType>(ownerTy))
          for (auto &[fn, ft] : ownerCt->getFields())
            if (fn == ma->getFieldName())
              return ft;
      }
    }
    return nullptr;
  };

  ast::Type *recvASTTy = resolveReceiverASTType();
  auto *arrTy = ast::dyn_cast<ast::ArrayType>(recvASTTy);

  // -- Direct-call dispatch for push / pop -----------------------------------
  // push/pop are NOT in the runtime vtable; they are emitted as direct calls.
  const std::string &mname = node->getMethodName();
  if (arrTy && mname == names::kPush)
    return emitArrayPush(node, recv, arrTy->getElementType());
  if (arrTy && mname == names::kPop)
    return emitArrayPop(recv, arrTy->getElementType());

  // -- Resolve the vtable ClassType ------------------------------------------
  ast::ClassType *ct = nullptr;
  if (arrTy)
    ct = CG.ASTCtx.getArrayTy();
  else
    ct = ast::dyn_cast<ast::ClassType>(recvASTTy);

  // Fallback paths for cases the scope walk above missed.
  if (!ct)
    ct = CG.Classes.getExprClassType(node->getReceiver());
  if (!ct) {
    for (auto *candidate : {CG.ASTCtx.getStrTy(), CG.ASTCtx.getFileTy(), CG.ASTCtx.getObjTy()}) {
      if (candidate && candidate->findMethod(node->getMethodName())) {
        ct = candidate;
        break;
      }
    }
  }

  int vtableIdx = ct ? ct->getVTableIndex(node->getMethodName()) : -1;
  assert(vtableIdx >= 0 && "Sema should have verified method exists");

  ast::MethodDecl *method = ct ? ct->findMethod(node->getMethodName()) : nullptr;

  // Build LLVM function type from the method signature.
  llvm::Type *retLLTy = method ? CG.toLLVMType(method->getReturnType()) : ptrTy;
  if (!retLLTy) retLLTy = ptrTy; // fallback for void/class return

  std::vector<llvm::Type *> paramLLTys = {ptrTy}; // receiver (self)
  if (method) {
    for (auto *pty : method->getParamTypes()) {
      auto *lty = CG.toLLVMType(pty);
      paramLLTys.push_back(lty ? lty : ptrTy);
    }
  }
  auto *fnTy = llvm::FunctionType::get(retLLTy, paramLLTys, false);

  // Emit user arguments.
  std::vector<llvm::Value *> args = {recv};
  bool isUserDefinedMethod = ct && ct != CG.ASTCtx.getObjTy() &&
                             ct != CG.ASTCtx.getStrTy() &&
                             ct != CG.ASTCtx.getFileTy();
  for (size_t i = 0; i < node->getNumArguments(); ++i) {
    auto *argExpr = node->getArguments()[i];
    // Determine expected param type from the resolved MethodDecl.
    ast::Type *paramASTTy = (method && i < method->getParamTypes().size())
                                ? method->getParamTypes()[i]
                                : nullptr;
    bool isClassParam = isUserDefinedMethod && paramASTTy &&
                        ast::isa<ast::ClassType>(paramASTTy);

    if (isClassParam) {
      // Class-type param: must arrive as PaykanShared* (callee releases on scope exit).
      llvm::Value *v = nullptr;
      if (auto *id = ast::dyn_cast<ast::Identifier>(argExpr)) {
        if (CG.CurrentScope && CG.CurrentScope->isOwned(id->getName())) {
          auto *argAlloca = CG.CurrentScope->lookup(id->getName());
          v = CG.Builder.CreateLoad(ptrTy, argAlloca, id->getName());
          CG.emitRetain(v);
        }
      }
      if (!v) {
        llvm::Value *raw = visit(argExpr);
        if (!raw) return nullptr;
        if (auto *sl = ast::dyn_cast<ast::StringLiteral>(argExpr))
          raw = CG.wrapStringLiteral(raw, sl->getValue().size());
        v = CG.exprAlreadyShared(argExpr) ? raw : CG.emitSharedNew(raw, kIRArgShared);
      }
      args.push_back(v);
      continue;
    }

    llvm::Value *v = visit(argExpr);
    if (!v) return nullptr;
    if (auto *sl = ast::dyn_cast<ast::StringLiteral>(argExpr))
      v = CG.wrapStringLiteral(v, sl->getValue().size());
    // Coerce bool i1 -> i64 if needed.
    if (v->getType()->isIntegerTy(1) && i + 1 < paramLLTys.size() &&
        paramLLTys[i + 1]->isIntegerTy(64))
      v = CG.Builder.CreateZExt(v, i64Ty, kIRBoolExt);
    args.push_back(v);
  }

  // Load vtable pointer and dispatch.
  // The pointer stored in the object points to methods[0] in the vtable struct
  // { i64 typeId, [N x ptr] methods }, so vtableIdx is correct as-is for both
  // user-defined and builtin classes.
  llvm::Value *vtablePtr = CG.Builder.CreateLoad(ptrTy, recv, kIRVtable);
  llvm::Value *slotPtr = CG.Builder.CreateGEP(
      ptrTy, vtablePtr,
      llvm::ConstantInt::get(i64Ty, vtableIdx), kIRVtSlot);
  llvm::Value *fnPtr = CG.Builder.CreateLoad(ptrTy, slotPtr, kIRVfn);

  if (retLLTy->isVoidTy()) {
    CG.Builder.CreateCall(fnTy, fnPtr, args);
    return nullptr;
  }
  auto *mcallResult = CG.Builder.CreateCall(fnTy, fnPtr, args, kIRMcall);
  // All vtable methods returning a ref type (class or array) return PaykanShared*.
  // This holds for both builtin runtime methods (normalized ABI) and user-defined
  // class methods (emitted via visitReturnStmt -> emitAsShared).
  bool returnsRef = method && method->getReturnType() &&
                    ast::isRefType(method->getReturnType());
  if (returnsRef)
    return mcallResult;
  return mcallResult;
}

llvm::Value *CodeGen::visitMethodDecl(ast::MethodDecl *) {
  // MethodDecl nodes are not produced by the parser yet.
  return nullptr;
}

llvm::Value *CodeGen::visitClassDecl(ast::ClassDecl *node) {
  return Classes.visitClassDecl(node);
}


llvm::Value *CodeGen::visitMemberAssignStmt(ast::MemberAssignStmt *node) {
  return Classes.visitMemberAssignStmt(node);
}

llvm::Value *CodeGen::visitSubscriptAssignStmt(ast::SubscriptAssignStmt *node) {
  auto *ptrTy  = llvm::PointerType::getUnqual(LLVMCtx);
  auto *i64Ty  = llvm::Type::getInt64Ty(LLVMCtx);
  auto *voidTy = llvm::Type::getVoidTy(LLVMCtx);

  // Get the raw PaykanArray*.
  llvm::Value *arrRaw = emitExpr(node->getArray());
  if (!arrRaw) return nullptr;

  // Emit the index.
  llvm::Value *idx = emitExpr(node->getIndex());
  if (!idx) return nullptr;
  if (idx->getType()->isIntegerTy(1))
    idx = Builder.CreateZExt(idx, i64Ty);

  // Determine element type from the array expression's AST type in scope.
  ast::Type *elemTy = nullptr;
  if (auto *id = ast::dyn_cast<ast::Identifier>(node->getArray())) {
    if (CurrentScope) {
      if (auto *at = ast::dyn_cast<ast::ArrayType>(
              CurrentScope->lookupASTType(id->getName())))
        elemTy = at->getElementType();
    }
  }

  bool isObjElem = elemTy && (ast::isa<ast::ClassType>(elemTy) ||
                               ast::isa<ast::ArrayType>(elemTy));

  if (isObjElem) {
    // Object element: use PaykanArray_set_obj (retains new, releases old).
    llvm::Value *val = emitAsShared(node->getValue());
    if (!val) return nullptr;
    auto *setFnTy = llvm::FunctionType::get(voidTy, {ptrTy, i64Ty, ptrTy}, false);
    Builder.CreateCall(declareFunction(kPaykanArraySetObj, setFnTy),
                       {arrRaw, idx, val});
  } else {
    // Primitive element: emit raw value, reinterpret to i64.
    llvm::Value *val = emitExpr(node->getValue());
    if (auto *sl = ast::dyn_cast<ast::StringLiteral>(node->getValue()))
      val = wrapStringLiteral(val, sl->getValue().size());
    if (!val) return nullptr;

    // Promote to i64 storage slot.
    if (val->getType()->isDoubleTy())
      val = Builder.CreateBitCast(val, i64Ty);
    else if (val->getType()->isIntegerTy(1))
      val = Builder.CreateZExt(val, i64Ty);

    // PaykanArray_set signature: (arr, idx, void* value) where value is the
    // 8-byte element passed as a pointer-width integer (memcpy'd by value).
    // Cast i64 -> ptr so the bits round-trip through &value in the C function.
    llvm::Value *valAsPtr = Builder.CreateIntToPtr(val, ptrTy);

    auto *setFnTy = llvm::FunctionType::get(voidTy, {ptrTy, i64Ty, ptrTy}, false);
    Builder.CreateCall(declareFunction(kPaykanArraySet, setFnTy),
                       {arrRaw, idx, valAsPtr});
  }
  return nullptr;
}

llvm::Value *CodeGen::visitMatchStmt(ast::MatchStmt *node) {
  auto *parentFn = Builder.GetInsertBlock()->getParent();
  auto *ptrTy    = llvm::PointerType::getUnqual(LLVMCtx);

  // 1. Emit the subject — visitIdentifier already unwraps owned class vars
  //    (PaykanShared* -> raw object pointer).  If the subject is a call/method-
  //    call/ternary result it may be a PaykanShared* — unwrap it here.
  llvm::Value *subjRaw = emitExpr(node->getSubject());
  if (!subjRaw)
    return nullptr;
  if (exprAlreadyShared(node->getSubject())) {
    auto *getFnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    subjRaw = Builder.CreateCall(declareFunction(kPaykanSharedGet, getFnTy),
                                 {subjRaw}, kIRSubjObj);
  }

  // 2. Build control-flow blocks.
  auto *endBB      = llvm::BasicBlock::Create(LLVMCtx, kIRMatchEnd, parentFn);
  auto *wildcardBB = [&]() -> llvm::BasicBlock * {
    for (const auto &arm : node->getArms())
      if (arm.isWildcard())
        return llvm::BasicBlock::Create(LLVMCtx, kIRMatchWildcard, parentFn);
    return nullptr;
  }();
  auto *defaultBB = wildcardBB ? wildcardBB : endBB;

  // 3. Collect type arms and pre-allocate body blocks.
  struct TypeArm {
    ast::ClassType    *CT;
    llvm::BasicBlock  *BodyBB;
    size_t             ArmIdx;
  };
  std::vector<TypeArm> typeArms;
  for (size_t i = 0; i < node->getArms().size(); ++i) {
    const auto &arm = node->getArms()[i];
    if (arm.isWildcard()) continue;
    auto *armCt = ast::dyn_cast<ast::ClassType>(arm.ArmType);
    assert(armCt && "Sema should have verified arm type exists");
    auto *bodyBB = llvm::BasicBlock::Create(
        LLVMCtx, kIRMatchArmPfx + std::to_string(i), parentFn);
    typeArms.push_back({armCt, bodyBB, i});
  }

  // 4. Emit an if-else chain comparing the object's vtable pointer against
  //    each arm's vtable global address.  Each class's vtable global has a
  //    unique address in memory — this is a stable, cross-module type identity
  //    that requires no integer ID scheme and works correctly after linking.
  if (typeArms.empty()) {
    Builder.CreateBr(defaultBB);
  } else {
    for (size_t i = 0; i < typeArms.size(); ++i) {
      llvm::Value *isMatch = Classes.emitIsExactType(subjRaw, typeArms[i].CT);
      auto *nextBB = (i + 1 < typeArms.size())
                         ? llvm::BasicBlock::Create(
                               LLVMCtx, kIRMatchCheckPfx + std::to_string(i + 1),
                               parentFn)
                         : defaultBB;
      Builder.CreateCondBr(isMatch, typeArms[i].BodyBB, nextBB);
      if (nextBB != defaultBB)
        Builder.SetInsertPoint(nextBB);
    }
  }

  // 5. Emit body blocks.
  for (const auto &ta : typeArms) {
    const auto &arm = node->getArms()[ta.ArmIdx];
    Builder.SetInsertPoint(ta.BodyBB);
    {
      ScopeGuard armGuard(*this);
      if (arm.hasBinding()) {
        auto *alloca = createEntryAlloca(parentFn, arm.Binding, ptrTy);
        Builder.CreateStore(subjRaw, alloca);
        // Unowned alias — the subject's scope owns the reference.
        CurrentScope->declareUnowned(arm.Binding, alloca, ta.CT);
      }
      for (auto *stmt : arm.Body->getStatements())
        visit(stmt);
    }
    if (!Builder.GetInsertBlock()->getTerminator())
      Builder.CreateBr(endBB);
  }

  // 6. Emit the wildcard arm body.
  if (wildcardBB) {
    Builder.SetInsertPoint(wildcardBB);
    for (const auto &arm : node->getArms()) {
      if (!arm.isWildcard()) continue;
      ScopeGuard armGuard(*this);
      for (auto *stmt : arm.Body->getStatements())
        visit(stmt);
      break;
    }
    if (!Builder.GetInsertBlock()->getTerminator())
      Builder.CreateBr(endBB);
  }

  Builder.SetInsertPoint(endBB);
  return nullptr;
}

// processImports and visitImportDecl are defined in CodeGenImport.cpp.

// ---------------------------------------------------------------------------

llvm::Value *CodeGen::visitFuncDecl(ast::FuncDecl *node) {
  llvm::Type *retTy = node->getReturnType()
                          ? toLLVMType(node->getReturnType())
                          : llvm::Type::getVoidTy(LLVMCtx);
  std::vector<llvm::Type *> paramTys;
  for (auto &p : node->getParams())
    paramTys.push_back(toLLVMType(p.ParamType));

  auto *fnTy = llvm::FunctionType::get(retTy, paramTys, false);
  auto *fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                    node->getName(), Module.get());

  // Name the parameters.
  size_t idx = 0;
  for (auto &arg : fn->args())
    arg.setName(node->getParams()[idx++].Name);

  // Save/restore current function return type.
  auto *savedRetASTTy = CurrentFuncReturnASTType;
  CurrentFuncReturnASTType = node->getReturnType();

  // Save current insert point.
  auto *savedBB = Builder.GetInsertBlock();
  auto savedIP = Builder.GetInsertPoint();

  // Create entry block and emit body.
  auto *entry = llvm::BasicBlock::Create(LLVMCtx, kIREntry, fn);
  Builder.SetInsertPoint(entry);

  {
    ScopeGuard guard(*this);
    // Create allocas for each parameter.
    for (size_t i = 0; i < fn->arg_size(); ++i) {
      auto &arg = *std::next(fn->arg_begin(), i);
      auto *alloca = createEntryAlloca(fn, arg.getName(), arg.getType());
      Builder.CreateStore(&arg, alloca);
      // Class-type params arrive as PaykanShared* (caller retained) — declare as
      // owned so scope cleanup releases them. Non-class params get nullptr AST type.
      auto *astTy = node->getParams()[i].ParamType;
      if (astTy) {
        if (auto *ct = ast::dyn_cast<ast::ClassType>(astTy))
          if (auto *canonical = ASTCtx.lookupClassType(ct->getName()))
            astTy = canonical;
      }
      if (astTy && ast::isRefType(astTy))
        CurrentScope->declare(std::string(arg.getName()), alloca, astTy);
      else
        CurrentScope->declare(std::string(arg.getName()), alloca, nullptr);
    }

    // Emit the body statements.
    for (auto *stmt : node->getBody()->getStatements())
      visit(stmt);
  }

  // If no terminator, add implicit return.
  if (!Builder.GetInsertBlock()->getTerminator()) {
    if (retTy->isVoidTy())
      Builder.CreateRetVoid();
    else
      Builder.CreateRet(llvm::Constant::getNullValue(retTy));
  }

  CurrentFuncReturnASTType = savedRetASTTy;

  // Restore insert point to the caller.
  if (savedBB)
    Builder.SetInsertPoint(savedBB, savedIP);

  return fn;
}

} // namespace codegen
} // namespace paykan
