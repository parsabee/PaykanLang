// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "CodeGen.h"

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

static constexpr llvm::StringLiteral kPaykanStringNew    = "PaykanString_new";
static constexpr llvm::StringLiteral kPaykanStringConcat = "PaykanString_concat";
static constexpr llvm::StringLiteral kPaykanOut          = "Paykan_out";
static constexpr llvm::StringLiteral kPaykanErr          = "Paykan_err";
static constexpr llvm::StringLiteral kMainFnName         = "main";
static constexpr llvm::StringLiteral kEntryBBName        = "entry";
static constexpr llvm::StringLiteral kStrGlobalName      = ".str";
static constexpr llvm::StringLiteral kInt2FPName         = "int2fp";

/// Map Paykan-level builtin function names to their C runtime symbols.
static const llvm::StringMap<llvm::StringLiteral> kBuiltinNames = {
    {"out", kPaykanOut},
    {"err", kPaykanErr},
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

void CodeGen::Scope::set(llvm::StringRef name, llvm::AllocaInst *alloca) {
  Locals[name] = alloca;
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

CodeGen::ScopeGuard::~ScopeGuard() { CG.CurrentScope = ScopeObj.Parent; }

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
}

// -- Top-level ---------------------------------------------------------------

llvm::Value *CodeGen::visitTranslationUnit(ast::TranslationUnit *node) {
  // Create main(): i32 main()
  auto *mainTy = llvm::FunctionType::get(llvm::Type::getInt32Ty(LLVMCtx),
                                         /*isVarArg=*/false);
  auto *mainFn = llvm::Function::Create(mainTy, llvm::Function::ExternalLinkage,
                                        kMainFnName, Module.get());

  auto *entry = llvm::BasicBlock::Create(LLVMCtx, kEntryBBName, mainFn);
  Builder.SetInsertPoint(entry);

  visitCompoundStmt(node->getBody());

  // Terminate with `return 0`.
  Builder.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(LLVMCtx), 0));

  return mainFn;
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

    // Implicit int → float promotion.
    if (allocaTy->isDoubleTy() && val->getType()->isIntegerTy(64))
      val = Builder.CreateSIToFP(val, llvm::Type::getDoubleTy(LLVMCtx),
                                 kInt2FPName);

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
    if (auto *sl = ast::dyn_cast<ast::StringLiteral>(node->getInitExpr()))
      initVal = wrapStringLiteral(initVal, sl->getValue().size());

    // If no explicit type, infer from the initializer value.
    if (!llvmTy)
      llvmTy = initVal->getType();

    // Implicit int → float promotion.
    if (llvmTy->isDoubleTy() && initVal->getType()->isIntegerTy(64))
      initVal = Builder.CreateSIToFP(initVal, llvm::Type::getDoubleTy(LLVMCtx),
                                     kInt2FPName);
  }

  // If we still have no type (shouldn't happen after Sema), default to i64.
  if (!llvmTy)
    llvmTy = llvm::Type::getInt64Ty(LLVMCtx);

  auto *alloca = createEntryAlloca(fn, node->getName(), llvmTy);

  if (initVal)
    Builder.CreateStore(initVal, alloca);
  else
    Builder.CreateStore(llvm::Constant::getNullValue(llvmTy), alloca);

  CurrentScope->set(node->getName(), alloca);
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
  return CG.Builder.CreateLoad(alloca->getAllocatedType(), alloca,
                               node->getName());
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

  std::vector<llvm::Value *> args;
  for (auto *arg : node->getArguments()) {
    llvm::Value *v = visit(arg);
    if (!v)
      return nullptr;
    args.push_back(v);
  }

  return CG.Builder.CreateCall(callee, args, "call");
}

llvm::Value *CodeGen::visitMethodDecl(ast::MethodDecl *) {
  // MethodDecl nodes are not produced by the parser yet.
  return nullptr;
}

} // namespace codegen
} // namespace paykan
