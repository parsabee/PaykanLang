// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "CodeGen.h"
#include "Names.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>

#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>

namespace paykan {
namespace codegen {

using namespace names;

/// Map Paykan-level builtin function names to their C runtime symbols.
static const llvm::StringMap<const char *> kBuiltinNames = {
    {kOut, kPaykanOut},
    {kErr, kPaykanErr},
};

// -- Constructor -------------------------------------------------------------

CodeGen::CodeGen(ast::ASTContext &astCtx, llvm::LLVMContext &llvmCtx,
                 llvm::StringRef moduleName)
    : ASTCtx(astCtx), LLVMCtx(llvmCtx),
      Module(std::make_unique<llvm::Module>(moduleName, llvmCtx)),
      Builder(llvmCtx) {
  (void)ASTCtx; // reserved for future use
}

// -- Scope / ScopeGuard ------------------------------------------------------

CodeGen::Scope::Scope(Scope *parent) : Parent(parent) {}

llvm::AllocaInst *CodeGen::Scope::lookup(llvm::StringRef name) const {
  auto it = Locals.find(name);
  if (it != Locals.end())
    return it->second;
  return Parent ? Parent->lookup(name) : nullptr;
}

llvm::AllocaInst *CodeGen::Scope::lookupRefTarget(llvm::StringRef name) const {
  auto it = RefTargets.find(name);
  if (it != RefTargets.end())
    return it->second;
  return Parent ? Parent->lookupRefTarget(name) : nullptr;
}

void CodeGen::Scope::set(llvm::StringRef name, llvm::AllocaInst *alloca) {
  Locals[name] = alloca;
}

void CodeGen::Scope::declare(llvm::StringRef name, llvm::AllocaInst *alloca,
                             ast::Ownership own, ast::Type *astTy) {
  Locals[name] = alloca;
  OwnershipMap[name] = own;
  if (astTy)
    ASTTypeMap[name] = astTy;
  DeclOrder.push_back({alloca, own, astTy});
}

ast::Type *CodeGen::Scope::lookupASTType(llvm::StringRef name) const {
  auto it = ASTTypeMap.find(name);
  if (it != ASTTypeMap.end())
    return it->second;
  return Parent ? Parent->lookupASTType(name) : nullptr;
}

ast::Ownership CodeGen::Scope::lookupOwnership(llvm::StringRef name) const {
  auto it = OwnershipMap.find(name);
  if (it != OwnershipMap.end())
    return it->second;
  return Parent ? Parent->lookupOwnership(name) : ast::Ownership::Unique;
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
  if (ast::isa<ast::ClassType>(ty)) {
    // All class types are represented as opaque pointers (ptr) for now.
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
  return Builder.CreateCall(callee, {rawStr, lenVal}, "str");
}

llvm::StringRef CodeGen::getDeleteFnName(ast::Type *ty) {
  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty)) {
    if (ct->getName() == kString)
      return kPaykanStringDelete;
  }
  return kPaykanObjectDelete;
}

void CodeGen::emitScopeCleanup(Scope &scope) {
  auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);
  auto *voidTy = llvm::Type::getVoidTy(LLVMCtx);
  auto *deleteFnTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);

  // Walk in reverse declaration order for proper LIFO cleanup.
  for (auto it = scope.DeclOrder.rbegin(); it != scope.DeclOrder.rend(); ++it) {
    auto &meta = *it;
    if (!meta.ASTType || !ast::isa<ast::ClassType>(meta.ASTType))
      continue; // builtins don't need cleanup
    if (meta.Ownership == ast::Ownership::Reference)
      continue; // references don't own the object

    llvm::Value *ptr = Builder.CreateLoad(ptrTy, meta.Alloca);

    switch (meta.Ownership) {
    case ast::Ownership::Unique: {
      // Guard: skip delete if the pointer is null (variable was moved).
      auto *parentFn = Builder.GetInsertBlock()->getParent();
      auto *deleteBB = llvm::BasicBlock::Create(LLVMCtx, "cleanup.del", parentFn);
      auto *skipBB = llvm::BasicBlock::Create(LLVMCtx, "cleanup.skip", parentFn);
      auto *isNull = Builder.CreateICmpEQ(
          ptr, llvm::ConstantPointerNull::get(
                   llvm::cast<llvm::PointerType>(ptrTy)),
          "isnull");
      Builder.CreateCondBr(isNull, skipBB, deleteBB);

      Builder.SetInsertPoint(deleteBB);
      auto *delFn = declareFunction(getDeleteFnName(meta.ASTType), deleteFnTy);
      Builder.CreateCall(delFn, {ptr});
      Builder.CreateBr(skipBB);

      Builder.SetInsertPoint(skipBB);
      break;
    }
    case ast::Ownership::Shared: {
      auto *releaseFn = declareFunction(kPaykanRelease, deleteFnTy);
      Builder.CreateCall(releaseFn, {ptr});
      break;
    }
    case ast::Ownership::Reference:
      break; // unreachable due to continue above
    }
  }
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
  visit(tu);
  return !llvm::verifyModule(*Module, &llvm::errs());
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

  FunctionTable[kStringInt]   = {kPaykanStringFromInt,   ptrFromI64, false};
  FunctionTable[kStringFloat] = {kPaykanStringFromFloat, ptrFromDbl, false};
  FunctionTable[kStringBool]  = {kPaykanStringFromBool,  ptrFromI64, false};

  // Identity constructors — just return the argument as-is.
  IdentityCtors.insert(kString);
}

// -- Top-level ---------------------------------------------------------------

llvm::Value *CodeGen::visitTranslationUnit(ast::TranslationUnit *node) {
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
  return emitExpr(node->getExpr());
}

llvm::Value *CodeGen::visitAssignStmt(ast::AssignStmt *node) {
  llvm::Value *val = emitExpr(node->getValue());
  if (!val)
    return nullptr;

  // Wrap raw string literal into a PaykanString* object.
  if (auto *sl = ast::dyn_cast<ast::StringLiteral>(node->getValue()))
    val = wrapStringLiteral(val, sl->getValue().size());

  auto *owner = CurrentScope->findOwner(node->getVarName());
  if (owner) {
    // Variable exists -- store into its alloca.
    auto *alloca = owner->lookup(node->getVarName());
    llvm::Type *allocaTy = alloca->getAllocatedType();

    // Assignment through a reference: store to the referent's alloca.
    auto own = CurrentScope->lookupOwnership(node->getVarName());
    if (own == ast::Ownership::Reference) {
      auto *refTarget = CurrentScope->lookupRefTarget(node->getVarName());
      if (refTarget) {
        Builder.CreateStore(val, refTarget);
        // Also update the reference's own alloca so subsequent reads see the new value.
        Builder.CreateStore(val, alloca);
        return val;
      }
    }
    // Implicit int → float promotion.
    if (allocaTy->isDoubleTy() && val->getType()->isIntegerTy(64))
      val = Builder.CreateSIToFP(val, llvm::Type::getDoubleTy(LLVMCtx),
                                 kInt2FPName);

    // Shared reassignment: retain new, release old, then store.
    if (own == ast::Ownership::Shared) {
      llvm::Value *sharedVal = nullptr;

      // If the RHS is an identifier referring to a shared var, reload from its alloca
      // to get the PaykanShared* wrapper (not the unwrapped object).
      if (auto *ident = ast::dyn_cast<ast::Identifier>(node->getValue())) {
        auto rhsOwn = CurrentScope->lookupOwnership(ident->getName());
        if (rhsOwn == ast::Ownership::Shared) {
          auto *rhsAlloca = CurrentScope->lookup(ident->getName());
          auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);
          sharedVal = Builder.CreateLoad(ptrTy, rhsAlloca, ident->getName() + ".shared");
        }
      }

      auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);
      auto *voidTy = llvm::Type::getVoidTy(LLVMCtx);
      auto *fnTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
      auto *retainFn = declareFunction(kPaykanRetain, fnTy);
      auto *releaseFn = declareFunction(kPaykanRelease, fnTy);

      if (!sharedVal) {
        // RHS is a new value (literal, constructor, etc.) — wrap in PaykanShared_new.
        auto *newFnTy = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
        auto *sharedNewFn = declareFunction(kPaykanSharedNew, newFnTy);
        auto *deleteFn = Module->getOrInsertFunction(
            getDeleteFnName(CurrentScope->lookupASTType(node->getVarName())),
            llvm::FunctionType::get(voidTy, {ptrTy}, false)).getCallee();
        sharedVal = Builder.CreateCall(sharedNewFn, {val, deleteFn}, "shared");
      }

      Builder.CreateCall(retainFn, {sharedVal});
      auto *oldVal = Builder.CreateLoad(ptrTy, alloca, "old.shared");
      Builder.CreateCall(releaseFn, {oldVal});
      Builder.CreateStore(sharedVal, alloca);
      return val;
    }

    Builder.CreateStore(val, alloca);
    return val;
  }

  // First assignment — declaration by assignment.
  auto *fn = Builder.GetInsertBlock()->getParent();
  llvm::Type *llvmTy = val->getType();
  auto *alloca = createEntryAlloca(fn, node->getVarName(), llvmTy);
  Builder.CreateStore(val, alloca);
  CurrentScope->set(node->getVarName(), alloca);
  return val;
}

llvm::Value *CodeGen::visitReturnStmt(ast::ReturnStmt *node) {
  if (node->getReturnValue()) {
    llvm::Value *val = emitExpr(node->getReturnValue());
    return Builder.CreateRet(val);
  }
  return Builder.CreateRetVoid();
}

llvm::Value *CodeGen::visitIfStmt(ast::IfStmt *node) {
  llvm::Value *condVal = emitExpr(node->getCondition());
  if (!condVal)
    return nullptr;

  auto *parentFn = Builder.GetInsertBlock()->getParent();
  auto *thenBB = llvm::BasicBlock::Create(LLVMCtx, "if.then", parentFn);
  auto *elseBB = node->hasElse()
                     ? llvm::BasicBlock::Create(LLVMCtx, "if.else", parentFn)
                     : nullptr;
  auto *mergeBB = llvm::BasicBlock::Create(LLVMCtx, "if.end", parentFn);

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
  auto *condBB = llvm::BasicBlock::Create(LLVMCtx, "while.cond", parentFn);
  auto *bodyBB = llvm::BasicBlock::Create(LLVMCtx, "while.body", parentFn);
  auto *endBB  = llvm::BasicBlock::Create(LLVMCtx, "while.end", parentFn);

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
      LLVMCtx, "break.dead", Builder.GetInsertBlock()->getParent());
  Builder.SetInsertPoint(deadBB);
  return nullptr;
}

llvm::Value *CodeGen::visitContinueStmt(ast::ContinueStmt *) {
  assert(!LoopStack.empty() && "continue outside loop");
  Builder.CreateBr(LoopStack.back().CondBB);
  // Create an unreachable block for any code after continue.
  auto *deadBB = llvm::BasicBlock::Create(
      LLVMCtx, "cont.dead", Builder.GetInsertBlock()->getParent());
  Builder.SetInsertPoint(deadBB);
  return nullptr;
}

// -- Declarations ------------------------------------------------------------

llvm::Value *CodeGen::visitVarDecl(ast::VarDecl *node) {
  auto *fn = Builder.GetInsertBlock()->getParent();

  // Resolve LLVM type from the explicit annotation if present,
  // otherwise fall back to the initializer's type.
  llvm::Type *llvmTy = nullptr;
  if (node->getType())
    llvmTy = toLLVMType(node->getType());

  llvm::Value *initVal = nullptr;
  if (node->getInitExpr()) {
    initVal = emitExpr(node->getInitExpr());

    // Wrap raw string literal into a PaykanString* object.
    // References don't allocate — they just alias the raw pointer.
    if (auto *sl = ast::dyn_cast<ast::StringLiteral>(node->getInitExpr()))
      if (node->getOwnership() != ast::Ownership::Reference)
        initVal = wrapStringLiteral(initVal, sl->getValue().size());

    // If no explicit type, infer from the initializer value.
    if (!llvmTy)
      llvmTy = initVal->getType();

    // Implicit int → float promotion.
    if (llvmTy->isDoubleTy() && initVal->getType()->isIntegerTy(64))
      initVal = Builder.CreateSIToFP(initVal, llvm::Type::getDoubleTy(LLVMCtx),
                                     kInt2FPName);

    // Wrap in a PaykanShared box for shared ownership.
    if (node->getOwnership() == ast::Ownership::Shared &&
        initVal->getType()->isPointerTy()) {
      auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);
      auto *sharedNewTy = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
      auto *sharedNewFn = declareFunction(kPaykanSharedNew, sharedNewTy);
      // Pass the appropriate destroy function as the second argument.
      auto *voidTy = llvm::Type::getVoidTy(LLVMCtx);
      auto *destroyTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
      auto *destroyFn = declareFunction(getDeleteFnName(node->getType()), destroyTy);
      initVal = Builder.CreateCall(sharedNewFn, {initVal, destroyFn}, "shared");
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

  // For references, record the referent's alloca so assignment-through-ref works.
  if (node->getOwnership() == ast::Ownership::Reference) {
    if (auto *ident = ast::dyn_cast<ast::Identifier>(node->getInitExpr())) {
      if (auto *refTarget = CurrentScope->lookup(ident->getName()))
        CurrentScope->RefTargets[node->getName()] = refTarget;
    }
  }

  CurrentScope->declare(node->getName(), alloca,
                        node->getOwnership(),
                        node->getType());
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

llvm::Value *CodeGen::ExprEmitter::visitStringLiteral(ast::StringLiteral *node) {
  // Emit the raw C string as a global constant.
  // The string only gets wrapped into a PaykanString* at usage sites
  // (assignment, variable declaration, concatenation).
  return CG.Builder.CreateGlobalStringPtr(node->getValue(), kStrGlobalName);
}

llvm::Value *CodeGen::ExprEmitter::visitIdentifier(ast::Identifier *node) {
  auto *alloca = CG.CurrentScope->lookup(node->getName());
  assert(alloca && "Sema should have caught undeclared variable");
  llvm::Value *val = CG.Builder.CreateLoad(alloca->getAllocatedType(), alloca,
                                           node->getName());

  // Shared variables store a PaykanShared* — unwrap to get the underlying object.
  auto own = CG.CurrentScope->lookupOwnership(node->getName());
  if (own == ast::Ownership::Shared && val->getType()->isPointerTy()) {
    auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
    auto *getFnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    auto *getFn = CG.declareFunction(kPaykanSharedGet, getFnTy);
    val = CG.Builder.CreateCall(getFn, {val}, node->getName() + ".obj");
  }

  return val;
}

llvm::Value *CodeGen::ExprEmitter::visitMovExpr(ast::MovExpr *node) {
  // Evaluate the operand (loads the pointer).
  llvm::Value *val = visit(node->getOperand());
  // Null-out the source alloca so scope cleanup skips it.
  auto *alloca = CG.CurrentScope->lookup(node->getOperand()->getName());
  if (alloca) {
    auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
    CG.Builder.CreateStore(llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(ptrTy)), alloca);
  }
  return val;
}

llvm::Value *CodeGen::ExprEmitter::visitRefExpr(ast::RefExpr *node) {
  // A reference expression (&x) just loads the value — it's a borrow.
  return visit(node->getOperand());
}

llvm::Value *CodeGen::ExprEmitter::visitTernaryExpr(ast::TernaryExpr *node) {
  llvm::Value *condVal = visit(node->getCondition());
  if (!condVal)
    return nullptr;

  auto *parentFn = CG.Builder.GetInsertBlock()->getParent();
  auto *thenBB = llvm::BasicBlock::Create(CG.LLVMCtx, "tern.then", parentFn);
  auto *elseBB = llvm::BasicBlock::Create(CG.LLVMCtx, "tern.else", parentFn);
  auto *mergeBB = llvm::BasicBlock::Create(CG.LLVMCtx, "tern.end", parentFn);

  CG.Builder.CreateCondBr(condVal, thenBB, elseBB);

  CG.Builder.SetInsertPoint(thenBB);
  llvm::Value *trueVal = visit(node->getTrueExpr());
  auto *thenEndBB = CG.Builder.GetInsertBlock();
  CG.Builder.CreateBr(mergeBB);

  CG.Builder.SetInsertPoint(elseBB);
  llvm::Value *falseVal = visit(node->getFalseExpr());
  auto *elseEndBB = CG.Builder.GetInsertBlock();
  CG.Builder.CreateBr(mergeBB);

  CG.Builder.SetInsertPoint(mergeBB);
  auto *phi = CG.Builder.CreatePHI(trueVal->getType(), 2, "tern");
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
      return CG.Builder.CreateFNeg(operand, "fneg");
    return CG.Builder.CreateNeg(operand, "neg");

  case ast::UnaryOpcode::Not:
    return CG.Builder.CreateNot(operand, "not");
  }
  return nullptr;
}

llvm::Value *CodeGen::ExprEmitter::visitBinaryExpr(ast::BinaryExpr *node) {
  // Short-circuit logical operators: evaluate LHS first, conditionally evaluate RHS.
  if (node->getOpcode() == ast::BinaryOpcode::And) {
    auto *parentFn = CG.Builder.GetInsertBlock()->getParent();
    auto *rhsBB = llvm::BasicBlock::Create(CG.LLVMCtx, "and.rhs", parentFn);
    auto *mergeBB = llvm::BasicBlock::Create(CG.LLVMCtx, "and.end", parentFn);

    llvm::Value *lhs = visit(node->getLHS());
    auto *lhsBB = CG.Builder.GetInsertBlock();
    CG.Builder.CreateCondBr(lhs, rhsBB, mergeBB);

    CG.Builder.SetInsertPoint(rhsBB);
    llvm::Value *rhs = visit(node->getRHS());
    auto *rhsEndBB = CG.Builder.GetInsertBlock();
    CG.Builder.CreateBr(mergeBB);

    CG.Builder.SetInsertPoint(mergeBB);
    auto *phi = CG.Builder.CreatePHI(llvm::Type::getInt1Ty(CG.LLVMCtx), 2, "and");
    phi->addIncoming(llvm::ConstantInt::getFalse(CG.LLVMCtx), lhsBB);
    phi->addIncoming(rhs, rhsEndBB);
    return phi;
  }
  if (node->getOpcode() == ast::BinaryOpcode::Or) {
    auto *parentFn = CG.Builder.GetInsertBlock()->getParent();
    auto *rhsBB = llvm::BasicBlock::Create(CG.LLVMCtx, "or.rhs", parentFn);
    auto *mergeBB = llvm::BasicBlock::Create(CG.LLVMCtx, "or.end", parentFn);

    llvm::Value *lhs = visit(node->getLHS());
    auto *lhsBB = CG.Builder.GetInsertBlock();
    CG.Builder.CreateCondBr(lhs, mergeBB, rhsBB);

    CG.Builder.SetInsertPoint(rhsBB);
    llvm::Value *rhs = visit(node->getRHS());
    auto *rhsEndBB = CG.Builder.GetInsertBlock();
    CG.Builder.CreateBr(mergeBB);

    CG.Builder.SetInsertPoint(mergeBB);
    auto *phi = CG.Builder.CreatePHI(llvm::Type::getInt1Ty(CG.LLVMCtx), 2, "or");
    phi->addIncoming(llvm::ConstantInt::getTrue(CG.LLVMCtx), lhsBB);
    phi->addIncoming(rhs, rhsEndBB);
    return phi;
  }

  llvm::Value *lhs = visit(node->getLHS());
  llvm::Value *rhs = visit(node->getRHS());
  if (!lhs || !rhs)
    return nullptr;

  // Implicit int → float promotion: if one side is double, promote the other.
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

      // String concatenation: call PaykanString_concat(lhs, rhs) -> ptr
      auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
      auto *fnTy = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
      auto *callee = CG.declareFunction(kPaykanStringConcat, fnTy);
      return CG.Builder.CreateCall(callee, {lhs, rhs}, "concat");
    }
    return isFloat ? CG.Builder.CreateFAdd(lhs, rhs, "fadd")
                   : CG.Builder.CreateAdd(lhs, rhs, "add");
  case ast::BinaryOpcode::Sub:
    return isFloat ? CG.Builder.CreateFSub(lhs, rhs, "fsub")
                   : CG.Builder.CreateSub(lhs, rhs, "sub");
  case ast::BinaryOpcode::Mul:
    return isFloat ? CG.Builder.CreateFMul(lhs, rhs, "fmul")
                   : CG.Builder.CreateMul(lhs, rhs, "mul");
  case ast::BinaryOpcode::Div:
    return isFloat ? CG.Builder.CreateFDiv(lhs, rhs, "fdiv")
                   : CG.Builder.CreateSDiv(lhs, rhs, "sdiv");
  case ast::BinaryOpcode::Mod:
    return isFloat ? CG.Builder.CreateFRem(lhs, rhs, "fmod")
                   : CG.Builder.CreateSRem(lhs, rhs, "srem");

  // -- Relational -----------------------------------------------------------
  case ast::BinaryOpcode::Lt:
    return isFloat ? CG.Builder.CreateFCmpOLT(lhs, rhs, "flt")
                   : CG.Builder.CreateICmpSLT(lhs, rhs, "slt");
  case ast::BinaryOpcode::Gt:
    return isFloat ? CG.Builder.CreateFCmpOGT(lhs, rhs, "fgt")
                   : CG.Builder.CreateICmpSGT(lhs, rhs, "sgt");
  case ast::BinaryOpcode::Le:
    return isFloat ? CG.Builder.CreateFCmpOLE(lhs, rhs, "fle")
                   : CG.Builder.CreateICmpSLE(lhs, rhs, "sle");
  case ast::BinaryOpcode::Ge:
    return isFloat ? CG.Builder.CreateFCmpOGE(lhs, rhs, "fge")
                   : CG.Builder.CreateICmpSGE(lhs, rhs, "sge");

  // -- Equality -------------------------------------------------------------
  case ast::BinaryOpcode::Eq:
    if (isFloat) return CG.Builder.CreateFCmpOEQ(lhs, rhs, "feq");
    return CG.Builder.CreateICmpEQ(lhs, rhs, "eq");
  case ast::BinaryOpcode::Ne:
    if (isFloat) return CG.Builder.CreateFCmpONE(lhs, rhs, "fne");
    return CG.Builder.CreateICmpNE(lhs, rhs, "ne");
  }
  return nullptr;
}

llvm::Value *CodeGen::ExprEmitter::visitCallExpr(ast::CallExpr *node) {
  // -- Identity constructor (e.g. String(x)) — return the arg as-is ---------
  if (CG.IdentityCtors.count(node->getCalleeName()) &&
      node->getNumArguments() == 1) {
    llvm::Value *arg = visit(node->getArguments()[0]);
    if (!arg) return nullptr;
    if (auto *sl = ast::dyn_cast<ast::StringLiteral>(node->getArguments()[0]))
      arg = CG.wrapStringLiteral(arg, sl->getValue().size());
    return arg;
  }

  // -- Builtin function from FunctionTable ----------------------------------
  auto it = CG.FunctionTable.find(node->getCalleeName());
  if (it != CG.FunctionTable.end()) {
    auto &info = it->second;

    // Declare the runtime function if not already present.
    llvm::Function *callee = CG.declareFunction(info.RuntimeName, info.FnTy);

    // Build argument list.
    std::vector<llvm::Value *> args;
    if (info.IsVariadic) {
      // Prepend the argument count for variadic C functions.
      auto *i64Ty = llvm::Type::getInt64Ty(CG.LLVMCtx);
      args.push_back(llvm::ConstantInt::get(i64Ty, node->getNumArguments()));
    }

    for (size_t i = 0; i < node->getNumArguments(); ++i) {
      llvm::Value *v = visit(node->getArguments()[i]);
      if (!v)
        return nullptr;
      // Wrap raw string literals into PaykanString* objects.
      if (auto *sl = ast::dyn_cast<ast::StringLiteral>(node->getArguments()[i]))
        v = CG.wrapStringLiteral(v, sl->getValue().size());
      // Bool (i1) → i64 coercion when the callee expects i64.
      if (v->getType()->isIntegerTy(1)) {
        auto *i64Ty = llvm::Type::getInt64Ty(CG.LLVMCtx);
        v = CG.Builder.CreateZExt(v, i64Ty, "boolext");
      }
      args.push_back(v);
    }

    // Void functions don't produce a value.
    if (info.FnTy->getReturnType()->isVoidTy()) {
      CG.Builder.CreateCall(callee, args);
      return nullptr;
    }
    return CG.Builder.CreateCall(callee, args, "call");
  }

  // User-defined function — look up in the LLVM module.
  llvm::Function *callee = CG.Module->getFunction(node->getCalleeName());
  if (!callee)
    return nullptr;

  // Look up parameter ownerships for this function (if known).
  const std::vector<ast::Ownership> *paramOwns = nullptr;
  auto ownsIt = CG.UserFuncParamOwns.find(node->getCalleeName());
  if (ownsIt != CG.UserFuncParamOwns.end())
    paramOwns = &ownsIt->second;

  std::vector<llvm::Value *> args;
  for (size_t i = 0; i < node->getNumArguments(); ++i) {
    auto *arg = node->getArguments()[i];

    // If the param is shared and the arg is a shared identifier, load the
    // shared pointer directly — don't unwrap via PaykanShared_get.
    bool needRawShared = paramOwns && i < paramOwns->size() &&
                         (*paramOwns)[i] == ast::Ownership::Shared;
    llvm::Value *v = nullptr;
    if (needRawShared) {
      if (auto *id = ast::dyn_cast<ast::Identifier>(arg)) {
        auto *alloca = CG.CurrentScope->lookup(id->getName());
        if (alloca)
          v = CG.Builder.CreateLoad(alloca->getAllocatedType(), alloca,
                                    id->getName());
      }
    }
    if (!v)
      v = visit(arg);
    if (!v)
      return nullptr;
    // Wrap raw string literals into PaykanString* objects.
    if (auto *sl = ast::dyn_cast<ast::StringLiteral>(arg))
      v = CG.wrapStringLiteral(v, sl->getValue().size());
    // Bool (i1) → i64 coercion when the callee expects i64.
    if (v->getType()->isIntegerTy(1) &&
        i < callee->arg_size() &&
        callee->getArg(i)->getType()->isIntegerTy(64)) {
      auto *i64Ty = llvm::Type::getInt64Ty(CG.LLVMCtx);
      v = CG.Builder.CreateZExt(v, i64Ty, "boolext");
    }
    args.push_back(v);
  }

  if (callee->getReturnType()->isVoidTy()) {
    CG.Builder.CreateCall(callee, args);
    return nullptr;
  }
  return CG.Builder.CreateCall(callee, args, "call");
}

llvm::Value *CodeGen::visitMethodDecl(ast::MethodDecl *) {
  // MethodDecl nodes are not produced by the parser yet.
  return nullptr;
}

llvm::Value *CodeGen::visitFuncDecl(ast::FuncDecl *node) {
  // Build the LLVM function type.
  llvm::Type *retTy = node->getReturnType()
                          ? toLLVMType(node->getReturnType())
                          : llvm::Type::getVoidTy(LLVMCtx);
  std::vector<llvm::Type *> paramTys;
  for (auto &p : node->getParams())
    paramTys.push_back(toLLVMType(p.ParamType));

  auto *fnTy = llvm::FunctionType::get(retTy, paramTys, false);
  auto *fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                    node->getName(), Module.get());

  // Record parameter ownerships for call-site codegen.
  {
    std::vector<ast::Ownership> owns;
    for (auto &p : node->getParams())
      owns.push_back(p.Own);
    UserFuncParamOwns[node->getName()] = std::move(owns);
  }

  // Name the parameters.
  size_t idx = 0;
  for (auto &arg : fn->args())
    arg.setName(node->getParams()[idx++].Name);

  // Save current insert point.
  auto *savedBB = Builder.GetInsertBlock();
  auto savedIP = Builder.GetInsertPoint();

  // Create entry block and emit body.
  auto *entry = llvm::BasicBlock::Create(LLVMCtx, "entry", fn);
  Builder.SetInsertPoint(entry);

  {
    ScopeGuard guard(*this);
    // Create allocas for each parameter.
    for (size_t i = 0; i < fn->arg_size(); ++i) {
      auto &arg = *std::next(fn->arg_begin(), i);
      auto *alloca = createEntryAlloca(fn, arg.getName(), arg.getType());
      Builder.CreateStore(&arg, alloca);
      CurrentScope->declare(std::string(arg.getName()), alloca,
                            node->getParams()[i].Own, nullptr);
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

  // Restore insert point to the caller.
  if (savedBB)
    Builder.SetInsertPoint(savedBB, savedIP);

  return fn;
}

} // namespace codegen
} // namespace paykan
