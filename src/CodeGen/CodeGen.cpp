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
    {kPrint, kPaykanPrint},
    {kPrintln, kPaykanPrintln},
    {kErrPrint, kPaykanErrPrint},
    {kErrPrintln, kPaykanErrPrintln},
};

// -- Constructor -------------------------------------------------------------

CodeGen::CodeGen(const sema::SemaContext &semaCtx, llvm::LLVMContext &llvmCtx,
                 llvm::StringRef moduleName, const std::string &projectRoot,
                 llvm::StringMap<llvm::Module *> *importRegistry)
    : ASTCtx(*semaCtx.ASTCtx), LLVMCtx(llvmCtx), SemaCtx(semaCtx),
      Module(std::make_unique<llvm::Module>(moduleName, llvmCtx)),
      Builder(llvmCtx), Classes(*this), ProjectRoot(projectRoot),
      ImportRegistry(importRegistry ? importRegistry : &CodeGenedImports) {}

// -- Scope / ScopeGuard ------------------------------------------------------

CodeGen::Scope::Scope(Scope *parent) : Parent(parent) {}

llvm::AllocaInst *CodeGen::Scope::lookup(llvm::StringRef name) const {
  auto it = Locals.find(name);
  if (it != Locals.end())
    return it->second;
  return Parent ? Parent->lookup(name) : nullptr;
}

bool CodeGen::Scope::isOwned(llvm::StringRef name) const {
  if (SharedVars.count(name))
    return true;
  // If the name is declared in THIS scope (but not in SharedVars), it is
  // explicitly NOT owned — stop here and do not walk up to a parent scope
  // that may have an owned variable with the same name.
  if (Locals.count(name))
    return false;
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

void CodeGen::Scope::declareUnownedWithBacking(llvm::StringRef name,
                                               llvm::AllocaInst *alloca,
                                               llvm::Value *shared,
                                               ast::Type *astTy) {
  declareUnowned(name, alloca, astTy);
  if (shared)
    BackingShared[name] = shared;
}

llvm::Value *CodeGen::Scope::lookupBackingShared(llvm::StringRef name) const {
  auto it = BackingShared.find(name);
  if (it != BackingShared.end())
    return it->second;
  return Parent ? Parent->lookupBackingShared(name) : nullptr;
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

// -- FunctionStateGuard (#34) ------------------------------------------------

CodeGen::FunctionStateGuard::FunctionStateGuard(CodeGen &cg,
                                                ast::Type *retASTType,
                                                ast::ClassType *methodClassTy)
    : CG(cg), SavedBB(cg.Builder.GetInsertBlock()),
      SavedIP(SavedBB ? cg.Builder.GetInsertPoint()
                      : llvm::BasicBlock::iterator()),
      SavedRetASTType(cg.CurrentFuncReturnASTType),
      SavedMethodClassType(cg.Classes.CurrentMethodClassType) {
  CG.CurrentFuncReturnASTType = retASTType;
  CG.Classes.CurrentMethodClassType = methodClassTy;
  // Isolate string-temp tracking: stale entries from a sibling function must
  // not affect this body's ownership decisions.  Swap the live set out so the
  // body starts with an empty tracker and the caller's set is restored intact.
  std::swap(SavedStringTemps, CG.OwnedStringTemps);
}

CodeGen::FunctionStateGuard::~FunctionStateGuard() {
  std::swap(CG.OwnedStringTemps, SavedStringTemps);
  CG.CurrentFuncReturnASTType = SavedRetASTType;
  CG.Classes.CurrentMethodClassType = SavedMethodClassType;
  if (SavedBB)
    CG.Builder.SetInsertPoint(SavedBB, SavedIP);
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
    case ast::BuiltinType::Char:
      return llvm::Type::getInt8Ty(LLVMCtx);
    case ast::BuiltinType::Void:
      return llvm::Type::getVoidTy(LLVMCtx);
    }
  }
  // The parser emits a ClassType stub for any unknown type name, which may in
  // fact be an enum (resolved by Sema).  Canonicalize such stubs to the enum
  // type so they lower to i64 rather than an opaque pointer.
  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty))
    if (ASTCtx.lookupEnumType(ct->getName()))
      return llvm::Type::getInt64Ty(LLVMCtx);
  if (ast::isa<ast::EnumType>(ty)) {
    // Enums lower to a 64-bit unsigned integer (the variant's index).
    return llvm::Type::getInt64Ty(LLVMCtx);
  }
  if (ast::isRefType(ty)) {
    // All class/array types are represented as opaque pointers (ptr) for now.
    return llvm::PointerType::getUnqual(LLVMCtx);
  }
  return nullptr;
}

bool CodeGen::isObjectElementType(ast::Type *elemTy) const {
  if (!elemTy)
    return false;
  if (ast::isa<ast::EnumType>(elemTy))
    return false;
  // The parser emits a ClassType stub for any named type; one naming an enum is
  // an i64 primitive, not an object slot.
  if (auto *ct = ast::dyn_cast<ast::ClassType>(elemTy))
    return ASTCtx.lookupEnumType(ct->getName()) == nullptr;
  return ast::isa<ast::ArrayType>(elemTy);
}

ast::Type *CodeGen::canonicalizeDeclType(ast::Type *ty) {
  if (!ty)
    return ty;
  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty)) {
    if (auto *et = ASTCtx.lookupEnumType(ct->getName()))
      return et;
    if (auto *canonical = ASTCtx.lookupClassType(ct->getName()))
      return canonical;
  }
  return ty;
}

ast::ClassType *CodeGen::resolveExprClassType(ast::Expr *expr) {
  ast::Type *resolved = nullptr;
  if (auto *ce = ast::dyn_cast<ast::CallExpr>(expr))
    resolved = ce->getResolvedType();
  else if (auto *mce = ast::dyn_cast<ast::MethodCallExpr>(expr))
    resolved = mce->getResolvedType();
  else if (auto *mae = ast::dyn_cast<ast::MemberAccessExpr>(expr))
    resolved = mae->getResolvedType();
  else if (auto *se = ast::dyn_cast<ast::SubscriptExpr>(expr))
    resolved = se->getResolvedType(); // object array element: y = arr[i]
  // `mov expr` transfers the operand's value unchanged, so it resolves to the
  // operand's type.
  else if (auto *mv = ast::dyn_cast<ast::MovExpr>(expr))
    resolved = mv->getResolvedType();
  else if (auto *id = ast::dyn_cast<ast::Identifier>(expr))
    resolved =
        CurrentScope ? CurrentScope->lookupASTType(id->getName()) : nullptr;

  auto *ct = ast::dyn_cast<ast::ClassType>(resolved); // null-safe
  if (!ct)
    return nullptr;
  // Canonicalize: parser stubs have no fields/methods.
  if (auto *canonical = ASTCtx.lookupClassType(ct->getName()))
    return canonical;
  return ct;
}

llvm::Function *CodeGen::declareFunctionPrototype(ast::FuncDecl *node) {
  if (auto *existing = Module->getFunction(node->getName()))
    return existing;

  ast::Type *retAstTy = canonicalizeDeclType(node->getReturnType());
  llvm::Type *retTy =
      retAstTy ? toLLVMType(retAstTy) : llvm::Type::getVoidTy(LLVMCtx);
  std::vector<llvm::Type *> paramTys;
  for (auto &p : node->getParams())
    paramTys.push_back(toLLVMType(p.ParamType));

  auto *fnTy = llvm::FunctionType::get(retTy, paramTys, false);
  auto *fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                    node->getName(), Module.get());
  size_t idx = 0;
  for (auto &arg : fn->args())
    arg.setName(node->getParams()[idx++].getName());
  return fn;
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
  auto *fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, name,
                                    Module.get());
  fn->addFnAttr(llvm::Attribute::NoUnwind);

  // Mark pointer-returning allocator functions as nonnull — all of them call
  // Paykan_malloc which aborts on failure, so they never return null.  This
  // lets the optimiser elide null-checks and improves alias analysis.
  if (fnTy->getReturnType()->isPointerTy()) {
    // (PaykanObject_new / PaykanBool_new are test-only constructors that
    // CodeGen never declares, so they carry no entry here.)
    static const llvm::StringSet<> kNonNullReturn = {
        kPaykanSharedNew,        kPaykanStringNew,      kPaykanStringFromInt,
        kPaykanStringFromFloat,  kPaykanStringFromBool, kPaykanStringFromChar,
        kPaykanStringConcat,     kPaykanArrayNew,       kPaykanArrayNewObj,
        kPaykanArrayNewFromData, kPaykanMalloc,         kPaykanRealloc,
        kPaykanFileNew,          kPaykanFileOpen,       kPaykanErrorNew,
        kPaykanIntNew,           kPaykanFloatNew,
    };
    if (kNonNullReturn.count(name))
      fn->addRetAttr(llvm::Attribute::NonNull);
  }
  return fn;
}

llvm::Value *CodeGen::wrapStringLiteral(llvm::Value *rawStr, size_t len) {
  auto *lenVal = llvm::ConstantInt::get(llvm::Type::getInt64Ty(LLVMCtx), len);
  auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);
  auto *i64Ty = llvm::Type::getInt64Ty(LLVMCtx);
  auto *fnTy = llvm::FunctionType::get(ptrTy, {ptrTy, i64Ty}, false);
  auto *callee = declareFunction(kPaykanStringNew, fnTy);
  auto *str = Builder.CreateCall(callee, {rawStr, lenVal}, kIRStr);
  // The freshly-built PaykanString* is an owned temporary until it is boxed
  // or explicitly consumed/destroyed.
  trackStringTemp(str);
  return str;
}

void CodeGen::emitScopeCleanup(Scope &scope) {
  auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);

  // Walk in reverse declaration order for proper LIFO cleanup.
  for (auto it = scope.DeclOrder.rbegin(); it != scope.DeclOrder.rend(); ++it) {
    assert(it->ASTType && ast::isRefType(it->ASTType) &&
           "DeclOrder must only contain ref-typed variables");
    emitRelease(Builder.CreateLoad(ptrTy, it->Alloca));
  }

  // Release any extra owned boxes (e.g. match subjects) in LIFO order.
  for (auto it = scope.PendingReleases.rbegin();
       it != scope.PendingReleases.rend(); ++it)
    emitRelease(*it);
}

// Emit cleanup for all active scopes up through the function body.
// Called by visitReturnStmt before emitting the ret instruction so that
// every owned variable declared in the function (across nested scopes) is
// released regardless of where the return appears.
void CodeGen::emitAllScopesCleanup() {
  for (Scope *s = CurrentScope; s != nullptr; s = s->Parent)
    emitScopeCleanup(*s);
}

// Emit cleanup for scopes inside the innermost loop body.  break / continue
// jump out of / restart the loop without running the normal ScopeGuard
// fall-through cleanup, so we must explicitly release owned variables and
// pending box releases declared between the current scope and the loop's
// enclosing scope (exclusive).
void CodeGen::emitLoopScopesCleanup() {
  if (LoopStack.empty())
    return;
  Scope *stop = LoopStack.back().EnclosingScope;
  for (Scope *s = CurrentScope; s != nullptr && s != stop; s = s->Parent)
    emitScopeCleanup(*s);
}

llvm::Value *CodeGen::emitExpr(ast::Expr *expr) { return Emitter.visit(expr); }

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

  llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(level);
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
  // `mov x`: an owned ref-typed variable transfers its existing +1 box, so the
  // result is already a PaykanShared*.  Any other operand is a pass-through
  // that inherits the operand's ownership.
  if (auto *mv = ast::dyn_cast<ast::MovExpr>(expr)) {
    ast::Expr *op = mv->getOperand();
    if (auto *id = ast::dyn_cast<ast::Identifier>(op))
      if (CurrentScope && CurrentScope->isOwned(id->getName()))
        return true;
    return exprAlreadyShared(op);
  }
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
    // Builtins in FunctionTable or IdentityCtors return raw pointers — not
    // shared. Exception: open() was changed to return PaykanShared* so match
    // works on the result.
    if (FunctionTable.count(ce->getCalleeName()) ||
        IdentityCtors.count(ce->getCalleeName())) {
      if (ce->getCalleeName() == names::kOpen ||
          ce->getCalleeName() == names::kIntStr ||
          ce->getCalleeName() == names::kFloatStr)
        return true;
      return false;
    }
    return true;
  }
  return false;
}

bool CodeGen::exprProducesFreshBox(ast::Expr *expr) const {
  // `mov x` of an owned ref variable hands its +1 box to the consumer (who now
  // owns it); any other operand inherits the operand's freshness.
  if (auto *mv = ast::dyn_cast<ast::MovExpr>(expr)) {
    ast::Expr *op = mv->getOperand();
    if (auto *id = ast::dyn_cast<ast::Identifier>(op))
      if (CurrentScope && CurrentScope->isOwned(id->getName()))
        return true;
    return exprProducesFreshBox(op);
  }
  // A borrowed box (member-access field load, owned-variable read) must NOT be
  // released by a transient consumer — only freshly-produced +1 boxes are
  // owned by the consumer.  A plain field read returns the field's box without
  // retaining, so it is borrowed even though exprAlreadyShared is true — EXCEPT
  // when the receiver chain is itself a fresh temporary (e.g. `makeH().a`):
  // visitMemberAccessExpr then retains the field box before tearing the
  // temporary receiver down, handing the consumer an owned +1 box.
  if (auto *mae = ast::dyn_cast<ast::MemberAccessExpr>(expr))
    return mae->getResolvedType() && ast::isRefType(mae->getResolvedType()) &&
           exprProducesFreshBox(mae->getReceiver());
  if (auto *mce = ast::dyn_cast<ast::MethodCallExpr>(expr))
    return mce->getResolvedType() && ast::isRefType(mce->getResolvedType());
  if (auto *te = ast::dyn_cast<ast::TernaryExpr>(expr))
    return te->getResolvedType() && ast::isRefType(te->getResolvedType());
  if (ast::isa<ast::ArrayLiteralExpr>(expr))
    return true;
  // CallExpr: fresh only for user functions / open() (per exprAlreadyShared).
  if (ast::isa<ast::CallExpr>(expr))
    return exprAlreadyShared(expr);
  return false;
}

void CodeGen::emitRetain(llvm::Value *shared) {
  auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);
  auto *voidTy = llvm::Type::getVoidTy(LLVMCtx);
  auto *fnTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
  Builder.CreateCall(declareFunction(kPaykanRetain, fnTy), {shared});
}

void CodeGen::emitRelease(llvm::Value *shared) {
  auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);
  auto *voidTy = llvm::Type::getVoidTy(LLVMCtx);
  auto *fnTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
  Builder.CreateCall(declareFunction(kPaykanRelease, fnTy), {shared});
}

llvm::Value *CodeGen::takeSharedOwnership(ast::Expr *expr, llvm::Value *val) {
  // See CodeGen.h: fresh +1 boxes are owned as-is; a borrowed box (plain
  // ref-typed field read) must be retained because its field slot keeps its
  // own reference.
  if (!exprProducesFreshBox(expr))
    emitRetain(val);
  return val;
}

llvm::Value *CodeGen::emitSharedNew(llvm::Value *raw, llvm::StringRef name) {
  auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);
  auto *fnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
  // Boxing transfers ownership of a raw string temporary to the PaykanShared.
  untrackStringTemp(raw);
  // PaykanShared_new has create-OR-acquire semantics (unique-box invariant,
  // see Runtime.h): a freshly constructed object gets a new box, while a raw
  // pointer that merely aliases an already-boxed object (`self`, a match-arm
  // binding, an array element) yields a +1 on the object's existing box.
  // Either way the caller receives an owned (+1) PaykanShared*, so boxing a
  // raw value is always safe — no call site needs to know which case it is.
  return Builder.CreateCall(declareFunction(kPaykanSharedNew, fnTy), {raw},
                            name);
}

llvm::Value *CodeGen::emitUnwrappedRef(ast::Expr *expr, llvm::StringRef name) {
  llvm::Value *val = emitExpr(expr);
  if (!val)
    return nullptr;
  // A member access, user call, ternary or array literal yields a
  // PaykanShared* box; unwrap it to the raw pointer.  Identifiers and
  // object-element subscripts are already raw (their emitters unwrap), and are
  // not flagged by exprAlreadyShared, so they pass through unchanged.
  if (exprAlreadyShared(expr)) {
    auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);
    auto *getFnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    val = Builder.CreateCall(declareFunction(kPaykanSharedGet, getFnTy), {val},
                             name);
  }
  return val;
}

void CodeGen::trackStringTemp(llvm::Value *v) {
  if (v)
    OwnedStringTemps.insert(v);
}

void CodeGen::untrackStringTemp(llvm::Value *v) {
  if (v)
    OwnedStringTemps.erase(v);
}

void CodeGen::destroyStringTempIfOwned(llvm::Value *v) {
  if (!v || !OwnedStringTemps.count(v))
    return;
  OwnedStringTemps.erase(v);
  if (Builder.GetInsertBlock()->getTerminator())
    return;
  auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);
  auto *voidTy = llvm::Type::getVoidTy(LLVMCtx);
  auto *fnTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
  Builder.CreateCall(declareFunction(kPaykanStringDestroy, fnTy), {v});
}

// -- Unified ownership classification / cleanup (#32) ------------------------

ExprValue CodeGen::classifyExpr(ast::Expr *expr, llvm::Value *val) const {
  if (!val)
    return ExprValue();

  // A raw PaykanString* temporary we are tracking (string literal wrapped via
  // wrapStringLiteral, a Str-returning builtin result, or a concat result) is
  // owned by the consumer until torn down or boxed.
  if (OwnedStringTemps.count(val))
    return ExprValue::owned(val);

  // A PaykanShared* box.  Only a *freshly* produced +1 box is owned by the
  // consumer; a borrowed field/member box (exprAlreadyShared but not
  // exprProducesFreshBox) must not be released.
  if (exprAlreadyShared(expr) && val->getType()->isPointerTy() &&
      exprProducesFreshBox(expr))
    return ExprValue::owned(val);

  return ExprValue::borrowed(val);
}

void CodeGen::releaseIfOwned(const ExprValue &ev) {
  if (!ev.isOwned() || !ev.Val)
    return;
  // The teardown depends on what the value actually is: a tracked raw
  // PaykanString* temporary is destroyed in place, while a PaykanShared* box is
  // reference-counted down.  destroyStringTempIfOwned() no-ops for values that
  // are not tracked string temps, so a non-string owned value falls through to
  // the box release path.
  if (OwnedStringTemps.count(ev.Val)) {
    destroyStringTempIfOwned(ev.Val);
    return;
  }
  emitRelease(ev.Val);
}

void CodeGen::bootstrapBuiltins() {
  auto *i64Ty = llvm::Type::getInt64Ty(LLVMCtx);
  auto *voidTy = llvm::Type::getVoidTy(LLVMCtx);
  auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);

  // The print family takes a single object pointer (printed via toString).
  auto *printFnTy =
      llvm::FunctionType::get(voidTy, {ptrTy}, /*isVarArg=*/false);
  for (auto &[paykanName, runtimeName] : kBuiltinNames)
    FunctionTable[paykanName] = {runtimeName, printFnTy};

  // Register type-conversion builtins.
  auto *dblTy = llvm::Type::getDoubleTy(LLVMCtx);

  auto *ptrFromI64 = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
  auto *ptrFromDbl = llvm::FunctionType::get(ptrTy, {dblTy}, false);

  auto *i8Ty = llvm::Type::getInt8Ty(LLVMCtx);
  auto *ptrFromI8 = llvm::FunctionType::get(ptrTy, {i8Ty}, false);

  FunctionTable[kStrInt] = {kPaykanStringFromInt, ptrFromI64};
  FunctionTable[kStrFloat] = {kPaykanStringFromFloat, ptrFromDbl};
  FunctionTable[kStrBool] = {kPaykanStringFromBool, ptrFromI64};
  FunctionTable[kStrChar] = {kPaykanStringFromChar, ptrFromI8};

  // open(path: Str, mode: Str) -> Obj  (PaykanShared*)
  auto *ptrFromPtrPtr = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
  FunctionTable[kOpen] = {kPaykanFileOpen, ptrFromPtrPtr};

  // IntStr(s: Str) -> Obj  /  FloatStr(s: Str) -> Obj  (PaykanShared*)
  auto *ptrFromPtr = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
  FunctionTable[kIntStr] = {kPaykanIntFromStr, ptrFromPtr};
  FunctionTable[kFloatStr] = {kPaykanFloatFromStr, ptrFromPtr};

  // Identity constructors — just return the argument as-is.
  IdentityCtors.insert(kString);
}

// -- Top-level ---------------------------------------------------------------

llvm::Value *CodeGen::visitTranslationUnit(ast::TranslationUnit *node) {
  // Forward-declare every free-function prototype before emitting any body, so
  // a call inside a class method (emitted below) resolves even when the callee
  // is defined later in the module.
  for (auto *fn : node->getFuncDecls())
    declareFunctionPrototype(fn);
  for (auto *cls : node->getClassDecls())
    visitClassDecl(cls);
  for (auto *fn : node->getFuncDecls())
    visitFuncDecl(fn);
  return nullptr;
}

llvm::Value *CodeGen::visitEnumDecl(ast::EnumDecl *) {
  // Enum declarations emit no code — they are pure type definitions (like a
  // typedef).  Variant values are materialized at use sites via EnumValueExpr.
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
  // If the expression result is owned but bound to nothing (e.g. `A();` or a
  // bare string temporary), tear it down immediately to avoid a leak.  The
  // unified classification handles both fresh PaykanShared* boxes and tracked
  // raw PaykanString* temporaries.
  releaseIfOwned(classifyExpr(node->getExpr(), val));
  return val;
}

llvm::Value *CodeGen::emitImplicitVarDecl(llvm::StringRef name,
                                          ast::Expr *rhsExpr,
                                          llvm::Value *val) {
  auto *fn = Builder.GetInsertBlock()->getParent();
  bool isNoneRHS = ast::isa<ast::NoneLiteral>(rhsExpr);

  // Determine the AST class type for the RHS (for isOwned / method dispatch).
  ast::ClassType *rhsAstTy = resolveExprClassType(rhsExpr);
  if (!rhsAstTy && val->getType()->isPointerTy())
    rhsAstTy = ASTCtx.getObjTy(); // conservative fallback

  if (!isNoneRHS && val->getType()->isPointerTy() && rhsAstTy) {
    auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);
    if (exprAlreadyShared(rhsExpr)) {
      // val is already a PaykanShared* (from a call/method-call/ternary or a
      // ref-typed field read) — take ownership without re-emitting the
      // expression (emitAsShared would call the function a second time).  A
      // borrowed field-read box is retained inside takeSharedOwnership.
      val = takeSharedOwnership(rhsExpr, val);
    } else if (auto *id = ast::dyn_cast<ast::Identifier>(rhsExpr);
               id && CurrentScope->isOwned(id->getName())) {
      // Owned identifier: visitIdentifier already unwrapped to raw; load the
      // PaykanShared* box directly from the alloca and retain it.
      val = Builder.CreateLoad(ptrTy, CurrentScope->lookup(id->getName()),
                               id->getName());
      emitRetain(val);
    } else {
      // Raw value — box it.  For a fresh temporary (string literal, concat
      // result) this creates the object's first box; for an unowned alias
      // (`self`, a match-arm binding) emitSharedNew's acquire semantics
      // retain the object's existing unique box instead.
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
      // Already a PaykanShared* — take ownership of it.  A fresh box
      // (call/method-call/ternary result, refcount already = 1) is used
      // directly; a borrowed ref-typed field read is retained first (its
      // field slot keeps its own reference).
      newBox = takeSharedOwnership(rhsExpr, val);
    else if (ast::isa<ast::SubscriptExpr>(rhsExpr))
      // Borrowed array element: emitAsShared retains the stored box rather
      // than wrapping the borrowed object in a fresh one.  (A ref-typed
      // member access is always exprAlreadyShared and handled above.)
      newBox = emitAsShared(rhsExpr);
    else
      // Any freshly-produced raw value already computed as `val` — a string
      // literal (pre-wrapped to a PaykanString* by visitAssignStmt) or a
      // concatenation result. Box it directly. Re-emitting via emitAsShared
      // would evaluate the RHS a second time and leak the first result (e.g.
      // `s = s + x` in a loop).
      newBox = emitSharedNew(val, kIRNewBox);
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
        newBox = takeSharedOwnership(node->getValue(), val);
      else
        newBox = emitSharedNew(val, kIRNewBox);
      Builder.CreateStore(newBox, alloca);
    } else {
      emitClassVarRebind(alloca, node->getValue(), val);
    }
    // Narrow the scope type to the concrete RHS type (enables vtable dispatch
    // through base-type variables, e.g. `obj: Obj = MyObj(...)`).
    ast::ClassType *rhsCT = resolveExprClassType(node->getValue());
    if (rhsCT)
      CurrentScope->updateASTType(node->getVarName(), rhsCT);
    if (wasUnowned)
      CurrentScope->promoteToOwned(
          node->getVarName(), rhsCT ? static_cast<ast::Type *>(rhsCT) : astTy);
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
    // Unowned arm-binding backed by a recorded PaykanShared*: retain + return
    // the original box directly.  (The emitSharedNew fallback below would now
    // recover the same box via the object's backpointer — this branch just
    // keeps the recorded-backing fast path.)
    if (alloca) {
      if (auto *backing = CurrentScope->lookupBackingShared(id->getName())) {
        emitRetain(backing);
        return backing;
      }
    }
  }

  // Object array element (arr[i]) of ref type: the array stores PaykanShared*
  // boxes and owns one reference each.  Acquiring the element for an owner
  // (e.g. `s: Str = a[i]`) must RETAIN the stored box rather than wrap the raw
  // object in a fresh box — otherwise both the array and the new owner would
  // free the same object.
  if (auto *se = ast::dyn_cast<ast::SubscriptExpr>(expr)) {
    bool isStrReceiver = false;
    if (auto *id = ast::dyn_cast<ast::Identifier>(se->getArray()))
      isStrReceiver = CurrentScope && CurrentScope->lookupASTType(
                                          id->getName()) == ASTCtx.getStrTy();
    if (!isStrReceiver && se->getResolvedType() &&
        ast::isRefType(se->getResolvedType())) {
      auto *i64Ty = llvm::Type::getInt64Ty(LLVMCtx);
      llvm::Value *idx = emitExpr(se->getIndex());
      if (idx->getType()->isIntegerTy(1))
        idx = Builder.CreateZExt(idx, i64Ty);
      llvm::Value *arrRaw = emitUnwrappedRef(se->getArray());
      auto *getFnTy = llvm::FunctionType::get(i64Ty, {ptrTy, i64Ty}, false);
      llvm::Value *bits = Builder.CreateCall(
          declareFunction(kPaykanArrayGet, getFnTy), {arrRaw, idx}, kIRElemRaw);
      llvm::Value *box = Builder.CreateIntToPtr(bits, ptrTy, kIRElemShared);
      emitRetain(box);
      return box;
    }
  }

  // Ref-typed member-access field (obj.field): the field slot owns its box, so
  // acquiring it for a new owner must retain rather than alias the borrow.
  // (takeSharedOwnership skips the retain when the read is call-rooted, e.g.
  // `makeH().a`, and the loaded box is already a fresh +1 for us.)
  if (auto *mae = ast::dyn_cast<ast::MemberAccessExpr>(expr)) {
    if (mae->getResolvedType() && ast::isRefType(mae->getResolvedType())) {
      llvm::Value *box = emitExpr(expr); // loads the field's PaykanShared* box
      return takeSharedOwnership(expr, box);
    }
  }

  // Call/ternary that already returns PaykanShared* — pass through.
  if (exprAlreadyShared(expr))
    return emitExpr(expr);

  // Everything else: emit raw, wrap string literals, then box.  The raw value
  // may alias an already-boxed object — `self` inside a method, a match-arm
  // binding whose subject was a plain owned variable — and emitSharedNew's
  // acquire semantics then retain that unique box instead of creating a
  // doomed second one (double free).
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
  auto *endBB = llvm::BasicBlock::Create(LLVMCtx, kIRWhileEnd, parentFn);

  // Push loop context for break/continue.
  LoopStack.push_back({condBB, endBB, CurrentScope});

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
  emitLoopScopesCleanup();
  Builder.CreateBr(LoopStack.back().EndBB);
  // Dead block for any code after break — must be terminated to satisfy the
  // verifier.
  auto *deadBB = llvm::BasicBlock::Create(
      LLVMCtx, kIRBreakDead, Builder.GetInsertBlock()->getParent());
  Builder.SetInsertPoint(deadBB);
  Builder.CreateUnreachable();
  return nullptr;
}

llvm::Value *CodeGen::visitContinueStmt(ast::ContinueStmt *) {
  assert(!LoopStack.empty() && "continue outside loop");
  emitLoopScopesCleanup();
  Builder.CreateBr(LoopStack.back().CondBB);
  // Dead block for any code after continue — must be terminated to satisfy the
  // verifier.
  auto *deadBB = llvm::BasicBlock::Create(
      LLVMCtx, kIRContDead, Builder.GetInsertBlock()->getParent());
  Builder.SetInsertPoint(deadBB);
  Builder.CreateUnreachable();
  return nullptr;
}

// -- Declarations ------------------------------------------------------------

llvm::Value *CodeGen::visitVarDecl(ast::VarDecl *node) {
  auto *fn = Builder.GetInsertBlock()->getParent();

  // Resolve LLVM type from the explicit annotation if present,
  // otherwise fall back to the initializer's type.
  llvm::Type *llvmTy = nullptr;
  if (node->getType()) {
    // Canonicalize: the parser may have created a ClassType stub (possibly
    // naming an enum) before Sema populated the registries.
    llvmTy = toLLVMType(canonicalizeDeclType(node->getType()));
  }

  llvm::Value *initVal = nullptr;
  if (node->getInitExpr()) {
    bool isNoneInit = ast::isa<ast::NoneLiteral>(node->getInitExpr());

    // For class-typed (non-None) VarDecls, use emitAsShared to get a properly
    // owned PaykanShared* — handles owned-identifier retain, already-shared
    // call pass-through, string literal wrapping, and raw->shared boxing.
    // Canonicalize the annotation so an enum stub (a ClassType stub at this
    // point) resolves to its EnumType and is not mistaken for a ref type.
    ast::Type *declTy = canonicalizeDeclType(node->getType());
    bool isClassDecl = !isNoneInit && declTy && ast::isRefType(declTy);
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
        initVal = Builder.CreateSIToFP(
            initVal, llvm::Type::getDoubleTy(LLVMCtx), kInt2FPName);
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
  bool isNoneVar =
      node->getInitExpr() && ast::isa<ast::NoneLiteral>(node->getInitExpr());

  // Canonicalize ClassType to the registry entry (the parser may have created
  // a stub with no fields before Sema populated the canonical ClassType).
  ast::Type *scopeTy = canonicalizeDeclType(node->getType());
  // Narrow the scope type to the concrete RHS type when the declared type is a
  // base class (e.g. `obj: Obj = MyObj(...)`). This ensures method dispatch
  // uses the concrete vtable convention, not the builtin Obj/Str convention.
  if (node->getInitExpr() && scopeTy && ast::isa<ast::ClassType>(scopeTy)) {
    ast::ClassType *rhsCT = resolveExprClassType(node->getInitExpr());
    if (rhsCT && rhsCT != ASTCtx.getObjTy() && rhsCT != ASTCtx.getStrTy())
      scopeTy = rhsCT;
  }

  if (isNoneVar)
    CurrentScope->declareUnowned(node->getName(), alloca, scopeTy);
  else
    CurrentScope->declare(node->getName(), alloca, scopeTy);
  return alloca;
}

// -- Expression emitter ------------------------------------------------------

llvm::Value *
CodeGen::ExprEmitter::visitIntegerLiteral(ast::IntegerLiteral *node) {
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

llvm::Value *CodeGen::ExprEmitter::visitCharLiteral(ast::CharLiteral *node) {
  return llvm::ConstantInt::get(llvm::Type::getInt8Ty(CG.LLVMCtx),
                                static_cast<uint8_t>(node->getValue()));
}

llvm::Value *CodeGen::ExprEmitter::visitNoneLiteral(ast::NoneLiteral *) {
  // None is a global singleton PaykanObject with its own vtable.
  // We declare it as an external global and return its address directly
  // (as a raw PaykanObject* — no PaykanShared wrapper, it's immortal).
  auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
  auto *noneGlobal = CG.Module->getOrInsertGlobal(kPaykanObjectNone, ptrTy);
  return noneGlobal;
}

llvm::Value *
CodeGen::ExprEmitter::visitStringLiteral(ast::StringLiteral *node) {
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

llvm::Value *
CodeGen::ExprEmitter::visitEnumValueExpr(ast::EnumValueExpr *node) {
  // Enums lower to a 64-bit integer constant (the variant's index).  Sema has
  // already resolved and validated the variant value.  No ARC: enum values are
  // plain primitives, not heap objects.
  return llvm::ConstantInt::get(llvm::Type::getInt64Ty(CG.LLVMCtx),
                                static_cast<uint64_t>(node->getValue()));
}

llvm::Value *CodeGen::ExprEmitter::visitIdentifier(ast::Identifier *node) {
  if (node->getName() == names::kStdin) {
    auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
    return CG.Module->getOrInsertGlobal(names::kPaykanFileStdin, ptrTy);
  }
  auto *alloca = CG.CurrentScope->lookup(node->getName());
  assert(alloca && "Sema should have caught undeclared variable");

  llvm::Value *val = CG.Builder.CreateLoad(alloca->getAllocatedType(), alloca,
                                           node->getName());

  // Owned class/array vars store a PaykanShared* — unwrap to get the underlying
  // object.
  auto *astTy = CG.CurrentScope->lookupASTType(node->getName());
  if (ast::isRefType(astTy) && CG.CurrentScope->isOwned(node->getName())) {
    auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
    auto *getFnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    auto *getFn = CG.declareFunction(kPaykanSharedGet, getFnTy);
    val = CG.Builder.CreateCall(getFn, {val}, node->getName() + ".obj");
  }

  return val;
}

llvm::Value *CodeGen::ExprEmitter::visitMovExpr(ast::MovExpr *node) {
  ast::Expr *op = node->getOperand();

  // Owned ref-typed variable: transfer its PaykanShared* box.  Load the box,
  // null the source alloca so scope cleanup releases nothing for it, and hand
  // the existing +1 reference to the consumer WITHOUT an extra retain.  The
  // runtime's release/retain/get are all null-safe, so the nulled slot is
  // harmless on every control-flow path (including the not-taken branch of an
  // `if` that performed the move).
  if (auto *id = ast::dyn_cast<ast::Identifier>(op)) {
    if (CG.CurrentScope->isOwned(id->getName())) {
      auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
      auto *alloca = CG.CurrentScope->lookup(id->getName());
      llvm::Value *box = CG.Builder.CreateLoad(ptrTy, alloca, id->getName());
      CG.Builder.CreateStore(llvm::ConstantPointerNull::get(ptrTy), alloca);
      return box;
    }
  }

  // Primitive variable or any temporary: `mov` is a transparent forward — the
  // value is already owned by (or a plain register of) the producing context.
  return CG.emitExpr(op);
}

llvm::Value *
CodeGen::ExprEmitter::visitMemberAccessExpr(ast::MemberAccessExpr *node) {
  return CG.Classes.visitMemberAccessExpr(node);
}

llvm::Value *
CodeGen::ExprEmitter::visitArrayLiteralExpr(ast::ArrayLiteralExpr *node) {
  auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
  auto *i64Ty = llvm::Type::getInt64Ty(CG.LLVMCtx);
  auto *voidTy = llvm::Type::getVoidTy(CG.LLVMCtx);

  // Determine element type from the ArrayType resolved by Sema.
  ast::Type *elemTy = nullptr;
  if (auto *at = ast::dyn_cast<ast::ArrayType>(node->getResolvedType()))
    elemTy = at->getElementType();

  // An object array holds PaykanShared* slots; a primitive array holds raw
  // values.  Enum elements are i64 primitives (see isObjectElementType).
  bool isObjectArray = CG.isObjectElementType(elemTy);

  size_t len = node->getNumElements();
  auto *lenVal = llvm::ConstantInt::get(i64Ty, (uint64_t)len);

  // Allocate the backing array.
  // For primitive literals we may switch to PaykanArray_new_from_data below;
  // for object arrays and dynamic primitives we allocate here.
  auto *newFnTy = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
  const char *newFnName = isObjectArray ? kPaykanArrayNewObj : kPaykanArrayNew;
  // arr is reassigned below for constant-primitive arrays.
  llvm::Value *arr =
      (!isObjectArray && len > 0)
          ? nullptr // defer: may use new_from_data
          : CG.Builder.CreateCall(CG.declareFunction(newFnName, newFnTy),
                                  {lenVal}, kIRArr);

  if (isObjectArray) {
    // Object array: each element needs retain semantics — emit per-element
    // set_obj.
    for (size_t i = 0; i < len; ++i) {
      auto *elemExpr = node->getElements()[i];
      auto *idxVal = llvm::ConstantInt::get(i64Ty, (uint64_t)i);
      llvm::Value *v = CG.emitAsShared(elemExpr);
      if (!v)
        return nullptr;
      auto *setFnTy =
          llvm::FunctionType::get(voidTy, {ptrTy, i64Ty, ptrTy}, false);
      CG.Builder.CreateCall(CG.declareFunction(kPaykanArraySetObj, setFnTy),
                            {arr, idxVal, v});
      // set_obj retains the stored box; release the +1 temporary from
      // emitAsShared so the literal's elements are not leaked.
      CG.emitRelease(v);
    }
  } else if (len > 0) {
    arr = emitPrimitiveArrayLiteral(node, len, lenVal, newFnTy);
    if (!arr)
      return nullptr;
  }

  // Box the raw PaykanArray* in a PaykanShared*.
  return CG.emitSharedNew(arr, kIRArrShared);
}

llvm::Value *
CodeGen::ExprEmitter::emitPrimitiveArrayLiteral(ast::ArrayLiteralExpr *node,
                                                size_t len, llvm::Value *lenVal,
                                                llvm::FunctionType *newFnTy) {
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
    if (!v)
      return nullptr;
    if (auto *sl = ast::dyn_cast<ast::StringLiteral>(elemExpr))
      v = CG.wrapStringLiteral(v, sl->getValue().size());
    if (v->getType()->isDoubleTy()) {
      // For a ConstantFP, extract the bit pattern directly so we get a
      // ConstantInt rather than a ConstantExpr(BitCast) — the latter would
      // crash the cast<ConstantInt> in the key-generation loop below.
      if (auto *cf = llvm::dyn_cast<llvm::ConstantFP>(v)) {
        uint64_t bits = cf->getValueAPF().bitcastToAPInt().getZExtValue();
        v = llvm::ConstantInt::get(i64Ty, bits);
      } else {
        v = CG.Builder.CreateBitCast(v, i64Ty, kIRF64Bits);
      }
    } else if (v->getType()->isIntegerTy(1))
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
      auto *arrTy = llvm::ArrayType::get(i64Ty, len);
      auto *initData = llvm::ConstantArray::get(arrTy, elems);
      global = new llvm::GlobalVariable(*CG.Module, arrTy, /*isConstant=*/true,
                                        llvm::GlobalValue::PrivateLinkage,
                                        initData, kIRArrData);
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
    auto *idxVal = llvm::ConstantInt::get(i64Ty, (uint64_t)i);
    llvm::Value *v = visit(elemExpr);
    if (!v)
      return nullptr;
    if (auto *sl = ast::dyn_cast<ast::StringLiteral>(elemExpr))
      v = CG.wrapStringLiteral(v, sl->getValue().size());
    if (v->getType()->isDoubleTy())
      v = CG.Builder.CreateBitCast(v, i64Ty, kIRF64Bits);
    else if (v->getType()->isIntegerTy(1))
      v = CG.Builder.CreateZExt(v, i64Ty, kIRBoolExtArr);
    auto *setFnTy =
        llvm::FunctionType::get(voidTy, {ptrTy, i64Ty, i64Ty}, false);
    CG.Builder.CreateCall(CG.declareFunction(kPaykanArraySet, setFnTy),
                          {arr, idxVal, v});
  }
  return arr;
}

llvm::Value *
CodeGen::ExprEmitter::visitSubscriptExpr(ast::SubscriptExpr *node) {
  auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
  auto *i64Ty = llvm::Type::getInt64Ty(CG.LLVMCtx);

  // Emit the index (shared by both string and array paths).
  llvm::Value *idx = visit(node->getIndex());
  if (!idx)
    return nullptr;
  if (idx->getType()->isIntegerTy(1))
    idx = CG.Builder.CreateZExt(idx, i64Ty, kIRIdxExt);

  // Determine whether the receiver is a Str (not Str[]).
  // Sema sets the SubscriptExpr's resolved type to CharTy for Str subscripts,
  // so this covers both bare identifiers and member-access expressions like
  // self.field[i].
  bool receiverIsStr = (node->getResolvedType() == CG.ASTCtx.getCharTy());

  // Emit the receiver, classify its ownership (a call-rooted receiver like
  // `makeArr()[0]` hands us a fresh +1 box to tear down once the element is
  // copied out — mirrors visitMemberAccessExpr), then unwrap to the raw
  // object pointer.
  llvm::Value *recv = CG.emitExpr(node->getArray());
  if (!recv)
    return nullptr;
  ExprValue recvOwned = CG.classifyExpr(node->getArray(), recv);
  llvm::Value *recvRaw = recv;
  if (CG.exprAlreadyShared(node->getArray()) &&
      recv->getType()->isPointerTy()) {
    auto *unwrapTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    recvRaw = CG.Builder.CreateCall(
        CG.declareFunction(kPaykanSharedGet, unwrapTy), {recv}, "recv.obj");
  }

  // String subscript: str[idx] -> char  (PaykanString_char_at).
  if (receiverIsStr) {
    auto *i8Ty = llvm::Type::getInt8Ty(CG.LLVMCtx);
    auto *charAtFnTy = llvm::FunctionType::get(i8Ty, {ptrTy, i64Ty}, false);
    llvm::Value *ch = CG.Builder.CreateCall(
        CG.declareFunction(kPaykanStringCharAt, charAtFnTy), {recvRaw, idx},
        "str.char_at");
    CG.releaseIfOwned(recvOwned); // char copied out — receiver may die now
    return ch;
  }

  // Array subscript: arr[idx] -> PaykanArray_get(arr, idx) -> i64.
  auto *getFnTy = llvm::FunctionType::get(i64Ty, {ptrTy, i64Ty}, false);
  llvm::Value *raw = CG.Builder.CreateCall(
      CG.declareFunction(kPaykanArrayGet, getFnTy), {recvRaw, idx}, kIRElemRaw);

  // Reinterpret the 8-byte slot based on the element type.  Primitive/enum
  // elements are copied out of the slot, so a fresh receiver temporary can be
  // released as soon as the load is done.  An OBJECT element, however, is
  // returned as a raw alias whose box is owned by the array — releasing a
  // fresh array here would free the element under the caller, so the fresh
  // array box is intentionally kept alive (leaked) in that case; a proper fix
  // needs the element to carry its own ownership (tracked follow-up).
  ast::Type *elemTy = node->getResolvedType();
  if (!elemTy || !CG.isObjectElementType(elemTy)) {
    CG.releaseIfOwned(recvOwned);
    if (!elemTy)
      return raw; // unknown type: return as i64
  }

  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(elemTy)) {
    switch (bt->getTypeKind()) {
    case ast::BuiltinType::Float:
      return CG.Builder.CreateBitCast(raw, llvm::Type::getDoubleTy(CG.LLVMCtx),
                                      kIRElemF64);
    case ast::BuiltinType::Bool:
      return CG.Builder.CreateTrunc(raw, llvm::Type::getInt1Ty(CG.LLVMCtx),
                                    kIRElemBool);
    case ast::BuiltinType::Int:
    default:
      return raw;
    }
  }
  // Enum elements are i64 primitives stored raw — not boxed.
  if (!CG.isObjectElementType(elemTy))
    return raw;
  // ClassType or ArrayType: slot stores a PaykanShared* as bits — convert back,
  // then unwrap to the raw PaykanObject* (same as visitIdentifier for owned
  // vars).
  llvm::Value *shared = CG.Builder.CreateIntToPtr(raw, ptrTy, kIRElemShared);
  auto *unwrapFnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
  return CG.Builder.CreateCall(CG.declareFunction(kPaykanSharedGet, unwrapFnTy),
                               {shared}, kIRElemObj);
}

llvm::Value *CodeGen::ExprEmitter::visitTernaryExpr(ast::TernaryExpr *node) {
  llvm::Value *condVal = visit(node->getCondition());
  if (!condVal)
    return nullptr;

  // When the ternary resolves to a ref type (class or array), each branch must
  // produce a PaykanShared*: exprAlreadyShared/exprProducesFreshBox classify a
  // ref-typed ternary as a fresh +1 box, so both incoming phi values must be
  // boxed.  Use emitAsShared to normalise string literals, owned vars, None,
  // array literals, and call results uniformly.
  bool isClassResult =
      node->getResolvedType() && ast::isRefType(node->getResolvedType());

  auto *parentFn = CG.Builder.GetInsertBlock()->getParent();
  auto *thenBB = llvm::BasicBlock::Create(CG.LLVMCtx, kIRTernThen, parentFn);
  auto *elseBB = llvm::BasicBlock::Create(CG.LLVMCtx, kIRTernElse, parentFn);
  auto *mergeBB = llvm::BasicBlock::Create(CG.LLVMCtx, kIRTernEnd, parentFn);

  CG.Builder.CreateCondBr(condVal, thenBB, elseBB);

  CG.Builder.SetInsertPoint(thenBB);
  llvm::Value *trueVal = isClassResult ? CG.emitAsShared(node->getTrueExpr())
                                       : visit(node->getTrueExpr());
  auto *thenEndBB = CG.Builder.GetInsertBlock();
  CG.Builder.CreateBr(mergeBB);

  CG.Builder.SetInsertPoint(elseBB);
  llvm::Value *falseVal = isClassResult ? CG.emitAsShared(node->getFalseExpr())
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
  case ast::UnaryOpcode::Count:
    break;
  }
  llvm_unreachable("unknown UnaryOpcode");
}

llvm::Value *CodeGen::ExprEmitter::visitBinaryExpr(ast::BinaryExpr *node) {
  // Short-circuit logical operators: evaluate LHS first, conditionally evaluate
  // RHS.
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
    auto *phi =
        CG.Builder.CreatePHI(llvm::Type::getInt1Ty(CG.LLVMCtx), 2, kIRAnd);
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
    auto *phi =
        CG.Builder.CreatePHI(llvm::Type::getInt1Ty(CG.LLVMCtx), 2, kIROr);
    phi->addIncoming(llvm::ConstantInt::getTrue(CG.LLVMCtx), lhsBB);
    phi->addIncoming(rhs, rhsEndBB);
    return phi;
  }

  // Reference-typed equality lowers to the virtual `equals` method:
  //   a == b  ==>  a.equals(b)
  //   a != b  ==>  !a.equals(b)
  // Dispatched through the object's vtable, so a user `equals` override is
  // honoured and the runtime default (identity) applies otherwise.  We
  // synthesize a MethodCallExpr and emit it, which reuses the method-call ABI
  // (receiver-temporary teardown + the callee-consumes `Obj` argument) and
  // evaluates each operand exactly once — avoiding the eager double-evaluation
  // below.  Operand types were recorded by Sema (visitBinaryExpr).
  if (node->getOpcode() == ast::BinaryOpcode::Eq ||
      node->getOpcode() == ast::BinaryOpcode::Ne) {
    ast::Type *lt = node->getLHS()->getResolvedType();
    ast::Type *rt = node->getRHS()->getResolvedType();
    if (lt && rt && ast::isRefType(lt) && ast::isRefType(rt)) {
      auto *call = CG.ASTCtx.make<ast::MethodCallExpr>(
          node->getLocation(), node->getLHS(),
          CG.ASTCtx.intern(names::kMethodEquals),
          std::vector<ast::Expr *>{node->getRHS()});
      call->setResolvedType(CG.ASTCtx.getBoolTy());
      llvm::Value *eq = visit(call);
      if (!eq)
        return nullptr;
      if (node->getOpcode() == ast::BinaryOpcode::Ne)
        eq = CG.Builder.CreateNot(eq, kIRNE);
      return eq;
    }
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

  // Integer divide/modulo by zero is undefined behaviour for LLVM's sdiv/srem.
  // Emit a guard that aborts via the runtime, mirroring array bounds checking.
  // (Float division by zero is well-defined IEEE-754 and is left untouched.)
  auto emitIntDivByZeroGuard = [&](llvm::Value *divisor) {
    auto *zero = llvm::ConstantInt::get(divisor->getType(), 0);
    auto *isZero = CG.Builder.CreateICmpEQ(divisor, zero, kIRDivZeroChk);
    auto *fn = CG.Builder.GetInsertBlock()->getParent();
    auto *panicBB = llvm::BasicBlock::Create(CG.LLVMCtx, kIRDivZeroPanic, fn);
    auto *contBB = llvm::BasicBlock::Create(CG.LLVMCtx, kIRDivZeroCont, fn);
    CG.Builder.CreateCondBr(isZero, panicBB, contBB);
    CG.Builder.SetInsertPoint(panicBB);
    auto *panicTy =
        llvm::FunctionType::get(llvm::Type::getVoidTy(CG.LLVMCtx), false);
    CG.Builder.CreateCall(CG.declareFunction(kPaykanPanicDivByZero, panicTy),
                          {});
    CG.Builder.CreateUnreachable();
    CG.Builder.SetInsertPoint(contBB);
  };

  switch (node->getOpcode()) {
  // -- Arithmetic -----------------------------------------------------------
  case ast::BinaryOpcode::Add:
    if (lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy()) {
      // Wrap raw string literals into PaykanString* objects before concat.
      if (auto *sl = ast::dyn_cast<ast::StringLiteral>(node->getLHS()))
        lhs = CG.wrapStringLiteral(lhs, sl->getValue().size());
      if (auto *sl = ast::dyn_cast<ast::StringLiteral>(node->getRHS()))
        rhs = CG.wrapStringLiteral(rhs, sl->getValue().size());

      // Classify each operand's ownership once (#32): an owned raw string temp
      // or a fresh PaykanShared* box must be torn down after the concat, which
      // borrows its operands and returns a brand-new owned string.
      ExprValue lhsEV = CG.classifyExpr(node->getLHS(), lhs);
      ExprValue rhsEV = CG.classifyExpr(node->getRHS(), rhs);

      // Unwrap PaykanShared* -> raw PaykanObject* for any call/method/ternary
      // result used directly as a concat operand.  The captured ExprValue still
      // refers to the box so it can be released after the concat.
      auto *ptrTy2 = llvm::PointerType::getUnqual(CG.LLVMCtx);
      auto *getFnTy2 = llvm::FunctionType::get(ptrTy2, {ptrTy2}, false);
      if (CG.exprAlreadyShared(node->getLHS()))
        lhs = CG.Builder.CreateCall(
            CG.declareFunction(kPaykanSharedGet, getFnTy2), {lhs}, kIRLhsObj);
      if (CG.exprAlreadyShared(node->getRHS()))
        rhs = CG.Builder.CreateCall(
            CG.declareFunction(kPaykanSharedGet, getFnTy2), {rhs}, kIRRhsObj);

      // String concatenation: call PaykanString_concat(lhs, rhs) -> ptr
      auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
      auto *fnTy = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
      auto *callee = CG.declareFunction(kPaykanStringConcat, fnTy);
      auto *result = CG.Builder.CreateCall(callee, {lhs, rhs}, kIRConcat);

      // Tear down owned operands, then track the result as a new owned
      // temporary for the consumer to manage.
      CG.releaseIfOwned(lhsEV);
      CG.releaseIfOwned(rhsEV);
      CG.trackStringTemp(result);
      return result;
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
    if (isFloat)
      return CG.Builder.CreateFDiv(lhs, rhs, kIRFDiv);
    emitIntDivByZeroGuard(rhs);
    return CG.Builder.CreateSDiv(lhs, rhs, kIRSDiv);
  case ast::BinaryOpcode::Mod:
    if (isFloat)
      return CG.Builder.CreateFRem(lhs, rhs, kIRFMod);
    emitIntDivByZeroGuard(rhs);
    return CG.Builder.CreateSRem(lhs, rhs, kIRSRem);

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
  // Reference-typed equality is handled earlier (lowered to `equals`); by here
  // the operands are primitives (int/bool/char/enum) or floats.
  case ast::BinaryOpcode::Eq:
    if (isFloat)
      return CG.Builder.CreateFCmpOEQ(lhs, rhs, kIRFEQ);
    return CG.Builder.CreateICmpEQ(lhs, rhs, kIREQ);
  case ast::BinaryOpcode::Ne:
    if (isFloat)
      return CG.Builder.CreateFCmpONE(lhs, rhs, kIRFNE);
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
  if (!arg)
    return nullptr;
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

  // Owned argument temporaries the builtin borrows: raw PaykanString* temps
  // (string literals, StrInt/StrFloat/StrBool results, concatenations) and
  // fresh PaykanShared* boxes produced by Str-returning call/method/ternary
  // arguments (e.g. println(p.toString())).  Classified once per argument and
  // torn down after the call via the unified ownership path (#32).
  std::vector<ExprValue> ownedArgs;

  for (size_t i = 0; i < node->getNumArguments(); ++i) {
    auto *argExpr = node->getArguments()[i];
    llvm::Value *v = visit(argExpr);
    if (!v)
      return nullptr;
    // Wrap raw string literals into PaykanString* objects.
    if (auto *sl = ast::dyn_cast<ast::StringLiteral>(argExpr))
      v = CG.wrapStringLiteral(v, sl->getValue().size());
    // Classify ownership of the value as produced, before any unboxing below.
    ExprValue ev = CG.classifyExpr(argExpr, v);
    if (ev.isOwned())
      ownedArgs.push_back(ev);
    // Unwrap PaykanShared* -> raw PaykanObject* for class-typed
    // call/method-call/ ternary results passed directly to a builtin vararg
    // (e.g. out()). visitIdentifier already unwraps owned variables; this
    // covers the case where a Str-returning call is used as an argument without
    // an intermediate variable assignment.
    if (CG.exprAlreadyShared(argExpr) && v->getType()->isPointerTy()) {
      auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
      auto *getFnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
      v = CG.Builder.CreateCall(CG.declareFunction(kPaykanSharedGet, getFnTy),
                                {v}, kIRUnboxed);
    }
    // Bool (i1) -> i64 coercion when the callee expects i64.
    if (v->getType()->isIntegerTy(1)) {
      auto *i64Ty = llvm::Type::getInt64Ty(CG.LLVMCtx);
      v = CG.Builder.CreateZExt(v, i64Ty, kIRBoolExt);
    }
    args.push_back(v);
  }

  // Emit the call, then tear down the argument temporaries we own.
  auto cleanupArgs = [&]() {
    for (const auto &ev : ownedArgs)
      CG.releaseIfOwned(ev);
  };

  // Str-returning builtins (StrInt/StrFloat/StrBool) hand back a fresh, owned
  // PaykanString* — track it as a temporary for the consumer to manage.
  bool returnsOwnedString =
      node->getCalleeName() == kStrInt || node->getCalleeName() == kStrFloat ||
      node->getCalleeName() == kStrBool || node->getCalleeName() == kStrChar;

  if (info.FnTy->getReturnType()->isVoidTy()) {
    CG.Builder.CreateCall(callee, args);
    cleanupArgs();
    return nullptr;
  }
  auto *result = CG.Builder.CreateCall(callee, args, kIRCall);
  cleanupArgs();
  if (returnsOwnedString)
    CG.trackStringTemp(result);
  return result;
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
    if (!superClass)
      return nullptr;
    std::string superInitName =
        superClass->getName() + "_" + names::kMethodInit;
    llvm::Function *superFn = CG.Module->getFunction(superInitName);
    if (!superFn)
      return nullptr;
    // `self` is the raw ptr stored in the unowned "self" alloca.
    auto *selfAlloca = CG.CurrentScope->lookup(kSelf);
    auto *ptrTy2 = llvm::PointerType::getUnqual(CG.LLVMCtx);
    auto *selfVal = CG.Builder.CreateLoad(ptrTy2, selfAlloca, kSelf);
    std::vector<llvm::Value *> initArgs = {selfVal};
    for (size_t i = 0; i < node->getNumArguments(); ++i) {
      auto *argExpr = node->getArguments()[i];
      auto *ptrTy3 = llvm::PointerType::getUnqual(CG.LLVMCtx);

      // Class-typed args must arrive as PaykanShared* (same as user-fn calls).
      // Check whether the arg is an owned identifier so we load the shared box
      // directly instead of going through visitIdentifier (which unwraps).
      bool passedAsShared = false;
      if (auto *id = ast::dyn_cast<ast::Identifier>(argExpr)) {
        if (CG.CurrentScope && CG.CurrentScope->isOwned(id->getName())) {
          auto *argAlloca = CG.CurrentScope->lookup(id->getName());
          llvm::Value *v =
              CG.Builder.CreateLoad(ptrTy3, argAlloca, id->getName());
          CG.emitRetain(v);
          initArgs.push_back(v);
          passedAsShared = true;
        }
      }
      if (!passedAsShared) {
        llvm::Value *v = visit(argExpr);
        if (!v)
          return nullptr;
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
    bool isClassArg =
        i < callee->arg_size() && callee->getArg(i)->getType()->isPointerTy();
    if (isClassArg) {
      // Class/Str argument: emitAsShared yields a +1 PaykanShared* box for the
      // callee to consume — retaining the existing box for owned variables,
      // match bindings (via backing), array elements, and object fields, and
      // wrapping fresh values otherwise. Wrapping a borrowed element/field in a
      // new box would double-free it (e.g. `f(arr[i])`).
      v = CG.emitAsShared(arg);
      args.push_back(v);
      continue;
    }

    // Non-class arg: evaluate normally.
    v = visit(arg);
    if (!v)
      return nullptr;
    // Wrap raw string literals into PaykanString* objects.
    if (auto *sl = ast::dyn_cast<ast::StringLiteral>(arg))
      v = CG.wrapStringLiteral(v, sl->getValue().size());

    // Bool (i1) -> i64 coercion when the callee expects i64.
    if (v->getType()->isIntegerTy(1) && i < callee->arg_size() &&
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
  auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
  auto *i64Ty = llvm::Type::getInt64Ty(CG.LLVMCtx);
  auto *voidTy = llvm::Type::getVoidTy(CG.LLVMCtx);

  if (node->getNumArguments() != 1)
    return nullptr;
  auto *argExpr = node->getArguments()[0];

  bool isObjElem = CG.isObjectElementType(elemTy);
  if (isObjElem) {
    // Use emitAsShared so that existing owned variables are retained rather
    // than double-wrapped (emitExpr on a ref-typed identifier unwraps to the
    // raw pointer, and a subsequent emitSharedNew would create a second owner
    // of the same object).
    llvm::Value *argVal = CG.emitAsShared(argExpr);
    if (!argVal)
      return nullptr;
    auto *fnTy = llvm::FunctionType::get(voidTy, {ptrTy, ptrTy}, false);
    CG.Builder.CreateCall(CG.declareFunction(kPaykanArrayPushObj, fnTy),
                          {recv, argVal});
    // push_obj takes its own retain on the stored box; emitAsShared handed us a
    // +1 temporary, so release it here to avoid leaking the pushed element.
    CG.emitRelease(argVal);
  } else {
    llvm::Value *argVal = CG.emitExpr(argExpr);
    if (!argVal)
      return nullptr;
    if (argVal->getType()->isDoubleTy())
      argVal = CG.Builder.CreateBitCast(argVal, i64Ty, kIRF64Bits);
    else if (argVal->getType()->isIntegerTy(1))
      argVal = CG.Builder.CreateZExt(argVal, i64Ty, kIRBoolExt);
    auto *fnTy = llvm::FunctionType::get(voidTy, {ptrTy, i64Ty}, false);
    CG.Builder.CreateCall(CG.declareFunction(kPaykanArrayPush, fnTy),
                          {recv, argVal});
  }
  return nullptr; // void
}

llvm::Value *CodeGen::ExprEmitter::emitArrayPop(llvm::Value *recv,
                                                ast::Type *elemTy) {
  auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
  auto *i64Ty = llvm::Type::getInt64Ty(CG.LLVMCtx);

  bool isObjElem = CG.isObjectElementType(elemTy);
  if (isObjElem) {
    auto *fnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    return CG.Builder.CreateCall(CG.declareFunction(kPaykanArrayPopObj, fnTy),
                                 {recv}, kIRMcall);
  }

  // Declare PaykanArray_pop as i64(ptr) — consistent with how PaykanArray_get
  // is declared, and avoids a PtrToInt cast.  On LP64 the ABI is identical to
  // the C declaration (void*(PaykanArray*)) since both return an 8-byte value.
  auto *fnTy = llvm::FunctionType::get(i64Ty, {ptrTy}, false);
  llvm::Value *raw = CG.Builder.CreateCall(
      CG.declareFunction(kPaykanArrayPop, fnTy), {recv}, kIRMcall);
  // Reinterpret the 8-byte slot based on the element type.
  llvm::Type *retLLTy = CG.toLLVMType(elemTy);
  if (!retLLTy || retLLTy->isIntegerTy(64))
    return raw;
  return CG.Builder.CreateBitCast(raw, retLLTy, kIRMcall);
}

llvm::Value *
CodeGen::ExprEmitter::visitMethodCallExpr(ast::MethodCallExpr *node) {
  // Emit the receiver and resolve its ClassType.
  llvm::Value *recv = visit(node->getReceiver());
  if (!recv)
    return nullptr;

  // Wrap a raw string literal receiver (shouldn't happen in practice but be
  // safe).
  if (auto *sl = ast::dyn_cast<ast::StringLiteral>(node->getReceiver()))
    recv = CG.wrapStringLiteral(recv, sl->getValue().size());

  // Classify the receiver for teardown BEFORE it is unwrapped to a raw object
  // below.  A receiver that is a freshly-owned temporary — a user
  // call/ternary/array-literal/`mov` PaykanShared* box, or a raw PaykanString*
  // temp from a literal/concatenation — is owned by this expression and must be
  // released after the call (the method borrows it).  A borrowed receiver (a
  // plain variable, `self`, or an object field) is left untouched.  This
  // mirrors the owned-argument teardown below and fixes receiver leaks such as
  // `make().len()`, `("a" + "b").len()`, and `(mov s).len()`.
  ExprValue recvOwned = CG.classifyExpr(node->getReceiver(), recv);

  // If the receiver is a PaykanShared* (an array literal, the result of
  // another method call, or a Str-returning user-defined call), unwrap it to
  // the raw PaykanObject* before vtable dispatch.  exprAlreadyShared covers
  // ArrayLiteralExpr, so this is the SINGLE unwrap point — a second,
  // literal-specific unwrap here used to strip the box twice and then
  // dispatch through the raw array reinterpreted as a box (SIGSEGV on
  // `[1, 2] == [1, 2]` and any other literal-receiver method call).
  if (CG.exprAlreadyShared(node->getReceiver()) &&
      recv->getType()->isPointerTy()) {
    auto *sharedPtrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
    auto *getFnTy = llvm::FunctionType::get(sharedPtrTy, {sharedPtrTy}, false);
    recv = CG.Builder.CreateCall(CG.declareFunction(kPaykanSharedGet, getFnTy),
                                 {recv}, kIRRecvObj);
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

  auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
  auto *i64Ty = llvm::Type::getInt64Ty(CG.LLVMCtx);

  // -- Resolve the AST type of the receiver ----------------------------------
  // Returns the AST Type* (ArrayType, ClassType, …) for the receiver
  // expression. Handles identifiers, `self`, and member-access expressions.
  auto resolveReceiverASTType = [&]() -> ast::Type * {
    auto *recvExpr = node->getReceiver();
    if (auto *id = ast::dyn_cast<ast::Identifier>(recvExpr)) {
      // Named variable (or `self`).
      if (id->getName() == names::kStdin)
        return CG.ASTCtx.getFileTy();
      if (CG.CurrentScope) {
        if (auto *t = CG.CurrentScope->lookupASTType(id->getName()))
          return t;
      }
      if (id->getName() == kSelf && CG.Classes.CurrentMethodClassType)
        return CG.Classes.CurrentMethodClassType;
    }
    if (auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(recvExpr)) {
      if (auto *rid = ast::dyn_cast<ast::Identifier>(ma->getReceiver())) {
        ast::Type *ownerTy =
            CG.CurrentScope ? CG.CurrentScope->lookupASTType(rid->getName())
                            : nullptr;
        if (!ownerTy && rid->getName() == kSelf &&
            CG.Classes.CurrentMethodClassType)
          ownerTy = CG.Classes.CurrentMethodClassType;
        if (auto *ownerCt = ast::dyn_cast<ast::ClassType>(ownerTy))
          for (auto &[fn, ft] : ownerCt->getFields())
            if (fn == ma->getFieldName())
              return ft;
      }
    }
    // Fallback: any other receiver form — a call result (makeArr().push(x)),
    // a subscript (m[0].push(x)), a nested member access, an array literal —
    // carries its Sema-resolved type on the expression itself.  Without this,
    // array receivers that are not bare identifiers / self.field missed the
    // push/pop direct dispatch and the Array vtable entirely, tripping the
    // vtableIdx assert (or dispatching through a coincidental Str slot).
    // Canonicalize so a parser ClassType stub resolves to the registry entry.
    if (auto *resolved = recvExpr->getResolvedType())
      return CG.canonicalizeDeclType(resolved);
    return nullptr;
  };

  ast::Type *recvASTTy = resolveReceiverASTType();
  auto *arrTy = ast::dyn_cast<ast::ArrayType>(recvASTTy);

  // -- Direct-call dispatch for push / pop -----------------------------------
  // push/pop are NOT in the runtime vtable; they are emitted as direct calls.
  // This path returns early, so it must tear down an owned receiver
  // temporary itself (e.g. the fresh array box from `makeArr().push(3)`) —
  // the vtable path's destroyTemps below never runs for it.
  const std::string &mname = node->getMethodName();
  if (arrTy && (mname == names::kPush || mname == names::kPop)) {
    llvm::Value *result =
        mname == names::kPush
            ? emitArrayPush(node, recv, arrTy->getElementType())
            : emitArrayPop(recv, arrTy->getElementType());
    CG.releaseIfOwned(recvOwned);
    return result;
  }

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
    for (auto *candidate :
         {CG.ASTCtx.getStrTy(), CG.ASTCtx.getFileTy(), CG.ASTCtx.getObjTy()}) {
      if (candidate && candidate->findMethod(node->getMethodName())) {
        ct = candidate;
        break;
      }
    }
  }

  int vtableIdx = ct ? ct->getVTableIndex(node->getMethodName()) : -1;
  assert(vtableIdx >= 0 && "Sema should have verified method exists");

  ast::MethodDecl *method =
      ct ? ct->findMethod(node->getMethodName()) : nullptr;

  // Build LLVM function type from the method signature.
  llvm::Type *retLLTy = method ? CG.toLLVMType(method->getReturnType()) : ptrTy;
  if (!retLLTy)
    retLLTy = ptrTy; // fallback for void/class return

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
  // Owned argument temporaries (literals, StrInt/concat results) passed to a
  // builtin method (e.g. File.write("...")).  The callee borrows them, so we
  // tear them down after the call via the unified ownership path (#32).
  std::vector<ExprValue> ownedArgs;
  bool isUserDefinedMethod =
      ct && ct != CG.ASTCtx.getObjTy() && ct != CG.ASTCtx.getStrTy() &&
      ct != CG.ASTCtx.getFileTy() && ct != CG.ASTCtx.getArrayTy() &&
      ct != CG.ASTCtx.getErrorTy() && ct != CG.ASTCtx.getIntBoxTy() &&
      ct != CG.ASTCtx.getFloatBoxTy() && ct != CG.ASTCtx.getBoolBoxTy();
  for (size_t i = 0; i < node->getNumArguments(); ++i) {
    auto *argExpr = node->getArguments()[i];
    // Determine expected param type from the resolved MethodDecl.
    ast::Type *paramASTTy = (method && i < method->getParamTypes().size())
                                ? method->getParamTypes()[i]
                                : nullptr;
    // Class-typed arguments are passed as PaykanShared boxes (callee consumes)
    // for user-defined methods. The builtin `equals` is virtual and may be
    // overridden by a user class, so its `other` argument must use the same
    // boxed-and-consumed ABI regardless of the static receiver type; the
    // runtime `*_equals` implementations unbox and release it to match.
    bool isClassParam =
        paramASTTy && ast::isa<ast::ClassType>(paramASTTy) &&
        (isUserDefinedMethod || node->getMethodName() == names::kMethodEquals);

    if (isClassParam) {
      // Class-type param uses the callee-consumes ABI: the argument must
      // arrive as an owned (+1) PaykanShared* that the callee releases on
      // scope exit.  emitAsShared produces exactly that for every expression
      // form — retaining borrowed member-access / array-element / owned-
      // identifier boxes, passing fresh call/ternary results through, and
      // boxing raw values / string literals.  (Previously a borrowed member
      // access was passed without retaining, so the callee's release and the
      // owner's release together freed the object one time too many.)
      llvm::Value *v = CG.emitAsShared(argExpr);
      if (!v)
        return nullptr;
      args.push_back(v);
      continue;
    }

    llvm::Value *v = visit(argExpr);
    if (!v)
      return nullptr;
    if (auto *sl = ast::dyn_cast<ast::StringLiteral>(argExpr))
      v = CG.wrapStringLiteral(v, sl->getValue().size());
    if (ExprValue ev = CG.classifyExpr(argExpr, v); ev.isOwned())
      ownedArgs.push_back(ev);
    // Coerce bool i1 -> i64 if needed.
    if (v->getType()->isIntegerTy(1) && i + 1 < paramLLTys.size() &&
        paramLLTys[i + 1]->isIntegerTy(64))
      v = CG.Builder.CreateZExt(v, i64Ty, kIRBoolExt);
    args.push_back(v);
  }

  // Load vtable pointer and dispatch.
  // The pointer stored in the object points to methods[0] in the vtable array
  // [N x ptr]. GEP through the array type so LLVM's type system can verify
  // the index — avoids the ambiguity of GEP-ing a raw ptr by element count.
  auto *vtableLoad = CG.Builder.CreateLoad(ptrTy, recv, kIRVtable);
  // Vtable pointer is immutable after construction — mark as invariant.
  vtableLoad->setMetadata(llvm::LLVMContext::MD_invariant_load,
                          llvm::MDNode::get(CG.LLVMCtx, {}));
  llvm::Value *vtablePtr = vtableLoad;
  auto *vtableTy = llvm::ArrayType::get(ptrTy, 0);
  llvm::Value *gep_indices[] = {llvm::ConstantInt::get(i64Ty, 0),
                                llvm::ConstantInt::get(i64Ty, vtableIdx)};
  llvm::Value *slotPtr =
      CG.Builder.CreateInBoundsGEP(vtableTy, vtablePtr, gep_indices, kIRVtSlot);
  llvm::Value *fnPtr = CG.Builder.CreateLoad(ptrTy, slotPtr, kIRVfn);

  // Tear down owned argument temporaries and, if the receiver was itself a
  // freshly-owned temporary, the receiver too.
  auto destroyTemps = [&]() {
    for (const auto &ev : ownedArgs)
      CG.releaseIfOwned(ev);
    CG.releaseIfOwned(recvOwned);
  };

  if (retLLTy->isVoidTy()) {
    CG.Builder.CreateCall(fnTy, fnPtr, args);
    destroyTemps();
    return nullptr;
  }
  auto *mcallResult = CG.Builder.CreateCall(fnTy, fnPtr, args, kIRMcall);
  destroyTemps();
  // A vtable method returning a ref type (class or array) hands back a
  // PaykanShared* — for both builtin runtime methods (normalized ABI) and
  // user-defined class methods (emitted via visitReturnStmt -> emitAsShared);
  // primitive returns come back raw.  Either way the value is returned as-is.
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
  auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);
  auto *i64Ty = llvm::Type::getInt64Ty(LLVMCtx);
  auto *voidTy = llvm::Type::getVoidTy(LLVMCtx);

  // Get the raw PaykanArray* (unwrapping the box for member-access fields,
  // call results, etc. — not just bare identifiers).
  llvm::Value *arrRaw = emitUnwrappedRef(node->getArray());
  if (!arrRaw)
    return nullptr;

  // Emit the index.
  llvm::Value *idx = emitExpr(node->getIndex());
  if (!idx)
    return nullptr;
  if (idx->getType()->isIntegerTy(1))
    idx = Builder.CreateZExt(idx, i64Ty);

  // Determine the element type from the array expression's resolved type, so
  // this works for any array-valued receiver (identifier, self.field, arr[i],
  // call()), not only bare identifiers in scope.
  ast::Type *elemTy = nullptr;
  if (ast::Type *arrTy = node->getArray()->getResolvedType()) {
    if (auto *at = ast::dyn_cast<ast::ArrayType>(arrTy))
      elemTy = at->getElementType();
  }
  if (!elemTy) {
    if (auto *id = ast::dyn_cast<ast::Identifier>(node->getArray())) {
      if (CurrentScope) {
        if (auto *st = CurrentScope->lookupASTType(id->getName()))
          if (auto *at = ast::dyn_cast<ast::ArrayType>(st))
            elemTy = at->getElementType();
      }
    }
  }

  bool isObjElem = isObjectElementType(elemTy);

  if (isObjElem) {
    // Object element: use PaykanArray_set_obj (retains new, releases old).
    llvm::Value *val = emitAsShared(node->getValue());
    if (!val)
      return nullptr;
    auto *setFnTy =
        llvm::FunctionType::get(voidTy, {ptrTy, i64Ty, ptrTy}, false);
    Builder.CreateCall(declareFunction(kPaykanArraySetObj, setFnTy),
                       {arrRaw, idx, val});
    // set_obj retains the stored box; release the +1 temporary from
    // emitAsShared so the assigned value is not leaked.
    emitRelease(val);
  } else {
    // Primitive element: emit raw value, reinterpret to i64.
    llvm::Value *val = emitExpr(node->getValue());
    if (auto *sl = ast::dyn_cast<ast::StringLiteral>(node->getValue()))
      val = wrapStringLiteral(val, sl->getValue().size());
    if (!val)
      return nullptr;

    // Promote to i64 storage slot.
    if (val->getType()->isDoubleTy())
      val = Builder.CreateBitCast(val, i64Ty);
    else if (val->getType()->isIntegerTy(1))
      val = Builder.CreateZExt(val, i64Ty);

    // PaykanArray_set(arr, idx, i64 value): declare as void(ptr, i64, i64)
    // so the type is consistent with how emitPrimitiveArrayLiteral declares
    // the same function — the C ABI passes an 8-byte value either way.
    auto *setFnTy =
        llvm::FunctionType::get(voidTy, {ptrTy, i64Ty, i64Ty}, false);
    Builder.CreateCall(declareFunction(kPaykanArraySet, setFnTy),
                       {arrRaw, idx, val});
  }
  return nullptr;
}

// -- Match-lowering scaffolding shared by all three modes --------------------

llvm::BasicBlock *CodeGen::createMatchWildcardBlock(ast::MatchStmt *node,
                                                    llvm::Function *parentFn) {
  for (ast::MatchArm *arm : node->getArms())
    if (arm->isWildcard())
      return llvm::BasicBlock::Create(LLVMCtx, kIRMatchWildcard, parentFn);
  return nullptr;
}

void CodeGen::emitMatchArmBody(ast::MatchArm *arm, llvm::BasicBlock *bodyBB,
                               llvm::BasicBlock *endBB) {
  Builder.SetInsertPoint(bodyBB);
  {
    ScopeGuard armGuard(*this);
    for (auto *stmt : arm->getBody()->getStatements())
      visit(stmt);
  }
  if (!Builder.GetInsertBlock()->getTerminator())
    Builder.CreateBr(endBB);
}

void CodeGen::emitMatchWildcardBody(ast::MatchStmt *node,
                                    llvm::BasicBlock *wildcardBB,
                                    llvm::BasicBlock *endBB) {
  if (!wildcardBB)
    return;
  for (ast::MatchArm *arm : node->getArms())
    if (arm->isWildcard())
      return emitMatchArmBody(arm, wildcardBB, endBB);
}

llvm::Value *CodeGen::visitMatchStmt(ast::MatchStmt *node) {
  auto *parentFn = Builder.GetInsertBlock()->getParent();
  auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);

  // 1. Emit the subject — visitIdentifier already unwraps owned class vars
  //    (PaykanShared* -> raw object pointer).  If the subject is a call/method-
  //    call/ternary result it may be a PaykanShared* — unwrap it here.
  //    We keep a pointer to the shared wrapper so we can release it at
  //    match.end (the arm binding is an unowned raw-pointer alias, so the
  //    wrapper's refcount never drops otherwise — causing a resource leak, e.g.
  //    fclose never called on a File returned by open()).
  llvm::Value *subjRaw = emitExpr(node->getSubject());
  if (!subjRaw)
    return nullptr;
  llvm::Value *sharedSubj = nullptr; // non-null iff we must release at end
  if (exprAlreadyShared(node->getSubject())) {
    // Take ownership of the subject box for the duration of the match: a
    // fresh +1 box (call result) is ours as-is, while a borrowed box (a
    // ref-typed field read, `match h.a`) must be retained — releasing the
    // field slot's own reference at match.end would free the field under it.
    sharedSubj = takeSharedOwnership(node->getSubject(), subjRaw);
    auto *getFnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    subjRaw = Builder.CreateCall(declareFunction(kPaykanSharedGet, getFnTy),
                                 {sharedSubj}, kIRSubjObj);
  }

  // Open a scope spanning the whole match so the subject box is released on
  // every exit path: normal fall-through (matchGuard at match.end), early
  // `return` (emitAllScopesCleanup walks this scope), and `break` / `continue`
  // (emitLoopScopesCleanup walks this scope).  Registering it as a pending
  // release here \u2014 rather than a single emitRelease at match.end \u2014
  // prevents a leak when an arm jumps out of the match before reaching
  // match.end.
  ScopeGuard matchGuard(*this);
  if (sharedSubj)
    CurrentScope->PendingReleases.push_back(sharedSubj);

  // Enum-mode: the subject resolves to an EnumType.  Each non-wildcard arm
  // names a bare variant (parsed as a type-name arm).  Emit an icmp-eq if-else
  // chain comparing the subject i64 against each variant's constant.
  if (auto *subjTy = node->getSubject()->getResolvedType())
    if (ast::isa<ast::EnumType>(subjTy))
      return emitEnumMatch(node, subjRaw, parentFn);

  // Value-mode: any literal arm means Sema verified this is a primitive/Str
  // match.  Emit an equality if-else chain instead of vtable dispatch.
  bool valueMode = false;
  for (ast::MatchArm *arm : node->getArms())
    if (arm->isLiteral()) {
      valueMode = true;
      break;
    }
  if (valueMode)
    return emitValueMatch(node, subjRaw, parentFn);

  // 2. Build control-flow blocks.
  auto *endBB = llvm::BasicBlock::Create(LLVMCtx, kIRMatchEnd, parentFn);
  auto *wildcardBB = createMatchWildcardBlock(node, parentFn);
  auto *defaultBB = wildcardBB ? wildcardBB : endBB;

  // 3. Collect type arms and pre-allocate body blocks.
  struct TypeArm {
    ast::ClassType *CT;
    llvm::BasicBlock *BodyBB;
    size_t ArmIdx;
  };
  std::vector<TypeArm> typeArms;
  for (size_t i = 0; i < node->getArms().size(); ++i) {
    ast::MatchArm *arm = node->getArms()[i];
    if (arm->isWildcard())
      continue;
    ast::ClassType *armCt = ast::dyn_cast<ast::ClassType>(arm->getArmType());
    // Array-type arms (e.g. int[]) have an ArrayType, not a ClassType.
    // Resolve to the ASTContext's specialized array ClassType so that vtable
    // identity comparison works the same way as class-type arms.
    if (!armCt) {
      if (auto *at = ast::dyn_cast<ast::ArrayType>(arm->getArmType()))
        armCt = ASTCtx.getOrCreateSpecializedArrayType(at->getElementType());
    }
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
      auto *nextBB =
          (i + 1 < typeArms.size())
              ? llvm::BasicBlock::Create(
                    LLVMCtx, kIRMatchCheckPfx + std::to_string(i + 1), parentFn)
              : defaultBB;
      Builder.CreateCondBr(isMatch, typeArms[i].BodyBB, nextBB);
      if (nextBB != defaultBB)
        Builder.SetInsertPoint(nextBB);
    }
  }

  // 5. Emit body blocks.
  for (const auto &ta : typeArms) {
    ast::MatchArm *arm = node->getArms()[ta.ArmIdx];
    Builder.SetInsertPoint(ta.BodyBB);
    {
      ScopeGuard armGuard(*this);
      if (arm->hasBinding()) {
        auto *alloca = createEntryAlloca(parentFn, arm->getBinding(), ptrTy);
        Builder.CreateStore(subjRaw, alloca);
        // Unowned alias — the subject's scope owns the reference.
        // Record the backing PaykanShared* (if any) so that call sites can
        // retain+pass the original box rather than wrapping the raw pointer.
        // When the subject is a plain owned variable there is no box value in
        // hand here; that is safe because every ownership-taking use of the
        // binding boxes the raw alias through emitSharedNew, whose acquire
        // semantics (unique-box invariant, Runtime.h) recover the subject's
        // existing box via the object-header backpointer.
        if (sharedSubj)
          CurrentScope->declareUnownedWithBacking(arm->getBinding(), alloca,
                                                  sharedSubj, ta.CT);
        else
          CurrentScope->declareUnowned(arm->getBinding(), alloca, ta.CT);
      }
      for (auto *stmt : arm->getBody()->getStatements())
        visit(stmt);
    }
    if (!Builder.GetInsertBlock()->getTerminator())
      Builder.CreateBr(endBB);
  }

  // 6. Emit the wildcard arm body.
  emitMatchWildcardBody(node, wildcardBB, endBB);

  Builder.SetInsertPoint(endBB);
  // The subject's PaykanShared* wrapper is released by matchGuard's cleanup
  // (registered as a PendingRelease above) on whichever path reaches here, as
  // well as on early return / break / continue paths.
  return nullptr;
}

// Value-mode match: emit an equality if-else chain.  `subjRaw` is the subject
// value already materialized by visitMatchStmt (a raw scalar for primitives, a
// raw PaykanString* for Str subjects).  The enclosing matchGuard scope still
// owns the subject-box release (if any); we only add the control flow here.
llvm::Value *CodeGen::emitValueMatch(ast::MatchStmt *node, llvm::Value *subjRaw,
                                     llvm::Function *parentFn) {
  auto *ptrTy = llvm::PointerType::getUnqual(LLVMCtx);
  auto *i64Ty = llvm::Type::getInt64Ty(LLVMCtx);

  auto *endBB = llvm::BasicBlock::Create(LLVMCtx, kIRMatchEnd, parentFn);
  auto *wildcardBB = createMatchWildcardBlock(node, parentFn);
  auto *defaultBB = wildcardBB ? wildcardBB : endBB;

  // Pre-allocate a body block for each literal arm.
  struct LitArm {
    ast::MatchArm *Arm;
    llvm::BasicBlock *BodyBB;
  };
  std::vector<LitArm> litArms;
  for (size_t i = 0; i < node->getArms().size(); ++i) {
    ast::MatchArm *arm = node->getArms()[i];
    if (!arm->isLiteral())
      continue;
    litArms.push_back(
        {arm, llvm::BasicBlock::Create(
                  LLVMCtx, kIRMatchArmPfx + std::to_string(i), parentFn)});
  }

  // Emit the comparison chain.  Each check evaluates the arm's literal,
  // compares it against the subject, and branches to the body block on
  // equality.
  if (litArms.empty()) {
    Builder.CreateBr(defaultBB);
  } else {
    for (size_t i = 0; i < litArms.size(); ++i) {
      ast::Expr *lit = litArms[i].Arm->getLiteralPattern();
      llvm::Value *isEq = nullptr;
      if (auto *sl = ast::dyn_cast<ast::StringLiteral>(lit)) {
        // Build a temporary PaykanString* for the literal and compare contents.
        // PaykanString_equals consumes its `other` argument as a PaykanShared
        // box (the vtable-equals ABI), so box the literal and let the call
        // release it — this transfers ownership of the temp into the box.
        llvm::Value *litStr =
            wrapStringLiteral(emitExpr(sl), sl->getValue().size());
        llvm::Value *litBox = emitSharedNew(litStr, kIRMatchEq);
        auto *eqFnTy = llvm::FunctionType::get(i64Ty, {ptrTy, ptrTy}, false);
        llvm::Value *eq =
            Builder.CreateCall(declareFunction(kPaykanStringEquals, eqFnTy),
                               {subjRaw, litBox}, kIRMatchEq);
        isEq = Builder.CreateICmpNE(eq, llvm::ConstantInt::get(i64Ty, 0),
                                    kIRMatchEq);
      } else if (ast::isa<ast::FloatLiteral>(lit)) {
        isEq = Builder.CreateFCmpOEQ(subjRaw, emitExpr(lit), kIRMatchEq);
      } else {
        // int / bool / char are all integer-typed scalars.
        isEq = Builder.CreateICmpEQ(subjRaw, emitExpr(lit), kIRMatchEq);
      }
      auto *nextBB =
          (i + 1 < litArms.size())
              ? llvm::BasicBlock::Create(
                    LLVMCtx, kIRMatchCheckPfx + std::to_string(i + 1), parentFn)
              : defaultBB;
      Builder.CreateCondBr(isEq, litArms[i].BodyBB, nextBB);
      if (nextBB != defaultBB)
        Builder.SetInsertPoint(nextBB);
    }
  }

  // Emit literal-arm body blocks, then the wildcard arm body.
  for (const auto &la : litArms)
    emitMatchArmBody(la.Arm, la.BodyBB, endBB);
  emitMatchWildcardBody(node, wildcardBB, endBB);

  Builder.SetInsertPoint(endBB);
  return nullptr;
}

// Enum-mode match: emit an icmp-eq if-else chain.  `subjRaw` is the subject's
// i64 value.  Each non-wildcard arm names a bare variant (carried by the arm's
// type-name stub); we compare against the variant's constant index.
llvm::Value *CodeGen::emitEnumMatch(ast::MatchStmt *node, llvm::Value *subjRaw,
                                    llvm::Function *parentFn) {
  auto *i64Ty = llvm::Type::getInt64Ty(LLVMCtx);
  auto *enumTy =
      ast::cast<ast::EnumType>(node->getSubject()->getResolvedType());

  auto *endBB = llvm::BasicBlock::Create(LLVMCtx, kIRMatchEnd, parentFn);
  auto *wildcardBB = createMatchWildcardBlock(node, parentFn);
  auto *defaultBB = wildcardBB ? wildcardBB : endBB;

  // Pre-allocate a body block for each variant arm, recording its value.
  struct VariantArm {
    ast::MatchArm *Arm;
    llvm::BasicBlock *BodyBB;
    int64_t Value;
  };
  std::vector<VariantArm> variantArms;
  for (size_t i = 0; i < node->getArms().size(); ++i) {
    ast::MatchArm *arm = node->getArms()[i];
    if (arm->isWildcard())
      continue;
    std::string variant;
    if (auto *stub = ast::dyn_cast<ast::ClassType>(arm->getArmType()))
      variant = stub->getName();
    else if (auto *et = ast::dyn_cast<ast::EnumType>(arm->getArmType()))
      variant = et->getName();
    int64_t val = enumTy->findVariant(variant);
    assert(val >= 0 && "Sema should have verified the variant exists");
    variantArms.push_back(
        {arm,
         llvm::BasicBlock::Create(LLVMCtx, kIRMatchArmPfx + std::to_string(i),
                                  parentFn),
         val});
  }

  // Emit the comparison chain.
  if (variantArms.empty()) {
    Builder.CreateBr(defaultBB);
  } else {
    for (size_t i = 0; i < variantArms.size(); ++i) {
      llvm::Value *isEq = Builder.CreateICmpEQ(
          subjRaw,
          llvm::ConstantInt::get(i64Ty,
                                 static_cast<uint64_t>(variantArms[i].Value)),
          kIRMatchEq);
      auto *nextBB =
          (i + 1 < variantArms.size())
              ? llvm::BasicBlock::Create(
                    LLVMCtx, kIRMatchCheckPfx + std::to_string(i + 1), parentFn)
              : defaultBB;
      Builder.CreateCondBr(isEq, variantArms[i].BodyBB, nextBB);
      if (nextBB != defaultBB)
        Builder.SetInsertPoint(nextBB);
    }
  }

  // Emit variant-arm body blocks, then the wildcard arm body.
  for (const auto &va : variantArms)
    emitMatchArmBody(va.Arm, va.BodyBB, endBB);
  emitMatchWildcardBody(node, wildcardBB, endBB);

  Builder.SetInsertPoint(endBB);
  return nullptr;
}

// processImports and visitImportDecl are defined in CodeGenImport.cpp.

// ---------------------------------------------------------------------------

llvm::Value *CodeGen::visitFuncDecl(ast::FuncDecl *node) {
  // The prototype was created by the forward-declaration pass in
  // visitTranslationUnit; fetch it (the enum/class return-type canonicalization
  // lives in declareFunctionPrototype) and emit the body here.
  llvm::Function *fn = declareFunctionPrototype(node);
  ast::Type *retAstTy = canonicalizeDeclType(node->getReturnType());

  // Save/restore all per-function codegen state (#34): return type, insertion
  // point, string-temp tracking, and method-class context.
  FunctionStateGuard fnState(*this, retAstTy, /*methodClassTy=*/nullptr);

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
      // Class-type params arrive as PaykanShared* (caller retained) — declare
      // as owned so scope cleanup releases them. Non-class params get nullptr
      // AST type.
      // Canonicalize the parser's stub: an enum-typed param resolves to its
      // EnumType — a plain i64 value, not a ref type — and is declared as a
      // non-owned primitive below.
      auto *astTy = canonicalizeDeclType(node->getParams()[i].ParamType);
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
    llvm::Type *retTy = fn->getReturnType();
    if (retTy->isVoidTy())
      Builder.CreateRetVoid();
    else
      Builder.CreateRet(llvm::Constant::getNullValue(retTy));
  }

  // fnState's destructor restores per-function state (return type, string-temp
  // tracking, method-class context) and the caller's insertion point.
  return fn;
}

} // namespace codegen
} // namespace paykan
