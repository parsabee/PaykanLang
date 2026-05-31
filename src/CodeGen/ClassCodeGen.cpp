// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// ClassCodeGen — LLVM IR code generation for user-defined Paykan classes.
//
// This file implements ClassCodeGen, a helper owned by CodeGen that encapsulates
// all class-specific lowering:
//
//   • LLVM struct types: { vtable_ptr, field0, field1, ... }
//   • Vtable globals: constant array of function pointers in vtable order
//   • Method functions: ClassName_methodName(ptr self, params...)
//   • Constructor functions: ClassName(initParams...) -> PaykanShared*
//   • Member field access (load) and assignment (store + retain/release)

#include "ClassCodeGen.h"
#include "CodeGen.h"
#include "CodeGenNames.h"
#include "Names.h"
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>

namespace paykan {
namespace codegen {

using namespace names;

namespace {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Collect all instance fields in layout order: ancestor fields first,
/// then the class's own fields — mirrors C struct inheritance by composition.
std::vector<std::pair<std::string, ast::Type *>>
getAllFieldsInOrder(ast::ClassType *ct) {
  if (!ct) return {};
  auto fields = getAllFieldsInOrder(ct->getSuperClass());
  for (auto &[name, ty] : ct->getFields())
    fields.emplace_back(name, ty);
  return fields;
}

} // namespace

// ---------------------------------------------------------------------------
// Struct-type helpers
// ---------------------------------------------------------------------------

llvm::StructType *
ClassCodeGen::getOrCreateClassStructType(ast::ClassType *ct) {
  auto it = ClassStructTypes.find(ct);
  if (it != ClassStructTypes.end())
    return it->second;

  auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
  std::vector<llvm::Type *> elems;
  elems.push_back(ptrTy); // index 0: vtable pointer
  for (auto &[name, ty] : getAllFieldsInOrder(ct)) {
    auto *llvmTy = CG.toLLVMType(ty);
    elems.push_back(llvmTy ? llvmTy : ptrTy);
  }
  auto *structTy = llvm::StructType::create(CG.LLVMCtx, elems,
                                             ct->getName() + kStructSuffix);
  ClassStructTypes[ct] = structTy;
  return structTy;
}

int ClassCodeGen::getFieldIndex(ast::ClassType *ct,
                                const std::string &name) const {
  int idx = 1; // index 0 is the vtable pointer
  for (auto &[fname, fty] : getAllFieldsInOrder(ct)) {
    if (fname == name)
      return idx;
    ++idx;
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Type helpers
// ---------------------------------------------------------------------------

llvm::Value *ClassCodeGen::emitIsExactType(llvm::Value *rawObjPtr,
                                           ast::ClassType *ct) {
  auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);

  // Load the vtable pointer stored at slot 0 of the object struct.
  auto *structTy = getOrCreateClassStructType(ct);
  auto *vtableSlotPtr = CG.Builder.CreateStructGEP(structTy, rawObjPtr, 0,
                                                    kIRVtableSlot);
  auto *loadedVtable = CG.Builder.CreateLoad(ptrTy, vtableSlotPtr,
                                              kIRVtablePrefix + ct->getName());

  // Obtain the vtable global for this class.  It is created by visitClassDecl
  // before any function bodies are emitted, so it must be present.
  // Fall back to a module lookup to handle imported classes whose ClassDecl
  // was codegen'd in a separate module and whose pointer was linked in.
  llvm::GlobalVariable *vtableGlobal = ClassVTableGlobals.lookup(ct);
  llvm::Value *expectedVtable = nullptr;
  if (vtableGlobal) {
    // vtable global is [N x ptr]. The value stored in the object is &vtable[0];
    // compute the same GEP so the pointer comparison is equal at runtime.
    auto *arrTy = llvm::cast<llvm::ArrayType>(vtableGlobal->getValueType());
    expectedVtable = CG.Builder.CreateConstInBoundsGEP2_64(
        arrTy, vtableGlobal, 0, 0, kIRVtableExpected + ct->getName());
  } else {
    // Imported class: reference the global by name as an external symbol.
    // Use [0 x ptr] as a placeholder — GEP(0,0) has zero offset so the
    // address equals the global base regardless of the true array size.
    std::string globalName = ct->getName() + kVTableSuffix;
    auto *arrTy = llvm::ArrayType::get(ptrTy, 0);
    auto *extGlobal = CG.Module->getOrInsertGlobal(globalName, arrTy);
    expectedVtable = CG.Builder.CreateConstInBoundsGEP2_64(
        arrTy, extGlobal, 0, 0, kIRVtableExpected + ct->getName());
  }

  return CG.Builder.CreateICmpEQ(loadedVtable, expectedVtable,
                                  kIRIsPrefix + ct->getName());
}

ast::ClassType *ClassCodeGen::getExprClassType(ast::Expr *expr) const {
  if (auto *id = ast::dyn_cast<ast::Identifier>(expr)) {
    if (CG.CurrentScope) {
      auto *astTy = CG.CurrentScope->lookupASTType(id->getName());
      if (auto *ct = ast::dyn_cast<ast::ClassType>(astTy)) {
        // Canonicalize: parser may have stored a stub ClassType.
        if (auto *canonical = CG.ASTCtx.lookupClassType(ct->getName()))
          return canonical;
        return ct;
      }
    }
    // self inside a method body.
    if (id->getName() == kSelf && CurrentMethodClassType)
      return CurrentMethodClassType;
  }
  if (auto *mae = ast::dyn_cast<ast::MemberAccessExpr>(expr))
    return ast::dyn_cast<ast::ClassType>(mae->getResolvedType());
  if (auto *mce = ast::dyn_cast<ast::MethodCallExpr>(expr))
    return ast::dyn_cast<ast::ClassType>(mce->getResolvedType());
  if (auto *ce = ast::dyn_cast<ast::CallExpr>(expr))
    return ast::dyn_cast<ast::ClassType>(ce->getResolvedType());
  if (auto *se = ast::dyn_cast<ast::SubscriptExpr>(expr))
    return ast::dyn_cast<ast::ClassType>(se->getResolvedType());
  return nullptr;
}

// ---------------------------------------------------------------------------
// Method name helpers
// ---------------------------------------------------------------------------

std::string ClassCodeGen::findConcreteMethodFuncName(ast::ClassType *ct,
                                                      const std::string &name) {
  if (!ct) return "";

  // Builtin Obj.
  if (ct == CG.ASTCtx.getObjTy()) {
    if (name == kMethodDestroy)  return kPaykanObjectDestroy;
    if (name == kMethodToString) return kPaykanObjectToString;
    if (name == kMethodEquals)   return kPaykanObjectEquals;
    return ""; // e.g. __init__ — no runtime symbol
  }
  // Builtin Str.
  if (ct == CG.ASTCtx.getStrTy()) {
    if (name == kMethodDestroy)  return kPaykanStringDestroy;
    if (name == kMethodToString) return kPaykanStringToString;
    if (name == kMethodEquals)   return kPaykanStringEquals;
    if (name == kMethodLength)   return kPaykanStringLength;
    if (name == kMethodConcat)   return kPaykanStringConcat;
    return "";
  }
  // Builtin File.
  if (ct == CG.ASTCtx.getFileTy()) {
    if (name == kMethodDestroy)  return kPaykanFileDestroy;
    if (name == kMethodToString) return kPaykanFileToString;
    if (name == kMethodEquals)   return kPaykanFileEquals;
    if (name == kMethodWrite)    return kPaykanFileWrite;
    if (name == kMethodReadln)   return kPaykanFileReadln;
    return "";
  }
  // User-defined class: prefer the most-derived concrete function.
  std::string mangled = ct->getName() + kNameSep + name;
  if (CG.Module->getFunction(mangled))
    return mangled;
  // If this is an imported class, try the qualified name (e.g. "helper::Adder_get").
  auto qualIt = ImportedClassQualifiers.find(ct);
  if (qualIt != ImportedClassQualifiers.end()) {
    std::string qualMangled = qualIt->second + kQualSep + mangled;
    if (CG.Module->getFunction(qualMangled))
      return qualMangled;
  }
  // Walk up the inheritance chain.
  return findConcreteMethodFuncName(ct->getSuperClass(), name);
}

// ---------------------------------------------------------------------------
// visitClassDecl
// ---------------------------------------------------------------------------

llvm::Value *ClassCodeGen::visitClassDecl(ast::ClassDecl *node) {
  auto *ct = CG.ASTCtx.lookupClassType(node->getName());
  assert(ct && "ClassType must have been registered by Sema");

  auto *ptrTy  = llvm::PointerType::getUnqual(CG.LLVMCtx);
  auto *i64Ty  = llvm::Type::getInt64Ty(CG.LLVMCtx);
  auto *voidTy = llvm::Type::getVoidTy(CG.LLVMCtx);

  // Ensure the struct type exists before emitting any method bodies
  // (methods may refer to fields of their own class).
  getOrCreateClassStructType(ct);

  // -------------------------------------------------------------------------
  // 1. Emit a concrete LLVM function for each method declared in this class.
  //    Signature: retTy  ClassName_methodName(ptr self, params...)
  //    __init__ returns void (the constructor wrapper handles the allocation).
  // -------------------------------------------------------------------------
  for (auto *funcDecl : node->getMethods()) {
    // Get the Sema-resolved MethodDecl (has resolved types).
    auto *md = ct->findMethod(funcDecl->getName());
    if (!md) continue;

    llvm::Type *retTy = md->getReturnType()
                            ? CG.toLLVMType(md->getReturnType())
                            : voidTy;
    if (!retTy) retTy = voidTy;

    std::vector<llvm::Type *> paramTys;
    paramTys.push_back(ptrTy); // self (raw PaykanObject*)
    for (auto *pty : md->getParamTypes()) {
      auto *lty = CG.toLLVMType(pty);
      paramTys.push_back(lty ? lty : ptrTy);
    }
    auto *fnTy  = llvm::FunctionType::get(retTy, paramTys, false);
    auto  fnName = node->getName() + kNameSep + funcDecl->getName();
    auto *fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                      fnName, CG.Module.get());

    // Name the parameters.
    fn->arg_begin()->setName(kSelf);
    for (size_t i = 0; i < funcDecl->getParams().size(); ++i)
      std::next(fn->arg_begin(), static_cast<int>(i + 1))
          ->setName(funcDecl->getParams()[i].Name);

    // Save outer codegen state.
    auto *savedBB            = CG.Builder.GetInsertBlock();
    auto  savedIP            = CG.Builder.GetInsertPoint();
    auto *savedRetASTTy      = CG.CurrentFuncReturnASTType;
    auto *savedMethodClassTy = CurrentMethodClassType;

    CG.CurrentFuncReturnASTType = md->getReturnType();
    CurrentMethodClassType      = ct;

    auto *entry = llvm::BasicBlock::Create(CG.LLVMCtx, kIREntry, fn);
    CG.Builder.SetInsertPoint(entry);

    {
      CodeGen::ScopeGuard guard(CG);

      // 'self' is a raw pointer — not ref-counted by the method body.
      auto *selfAlloca = CG.createEntryAlloca(fn, kSelf, ptrTy);
      CG.Builder.CreateStore(fn->getArg(0), selfAlloca);
      CG.CurrentScope->declareUnowned(kSelf, selfAlloca, ct);

      // Bind parameters.
      for (size_t i = 0; i < funcDecl->getParams().size(); ++i) {
        auto &p    = funcDecl->getParams()[i];
        auto *arg  = fn->getArg(static_cast<unsigned>(i + 1));
        auto *pty  = md->getParamTypes()[i];
        auto *alloca = CG.createEntryAlloca(fn, p.Name, arg->getType());
        CG.Builder.CreateStore(arg, alloca);
        if (ast::isRefType(pty))
          CG.CurrentScope->declare(p.Name, alloca, pty);
        else
          CG.CurrentScope->declare(p.Name, alloca, nullptr);
      }

      // Emit the method body.
      for (auto *stmt : funcDecl->getBody()->getStatements())
        CG.visit(stmt);
    }

    // Add implicit return if the block has no terminator.
    if (!CG.Builder.GetInsertBlock()->getTerminator()) {
      if (retTy->isVoidTy())
        CG.Builder.CreateRetVoid();
      else
        CG.Builder.CreateRet(llvm::Constant::getNullValue(retTy));
    }

    // Restore outer state.
    CG.CurrentFuncReturnASTType = savedRetASTTy;
    CurrentMethodClassType      = savedMethodClassTy;
    if (savedBB)
      CG.Builder.SetInsertPoint(savedBB, savedIP);
  }

  // -------------------------------------------------------------------------
  // 2. Build the vtable global constant.
  //    An array of function pointers in vtable-slot order.
  //    Inherited slots that are not overridden in this class resolve to the
  //    nearest ancestor's concrete function.
  // -------------------------------------------------------------------------
  const auto &vtable = ct->getVTable();
  std::vector<llvm::Constant *> vtableEntries;
  for (auto *md : vtable) {
    std::string concreteName = findConcreteMethodFuncName(ct, md->getName());

    llvm::Constant *fnPtr = nullptr;
    if (!concreteName.empty()) {
      llvm::Type *slotRetTy = md->getReturnType()
                                   ? CG.toLLVMType(md->getReturnType())
                                   : voidTy;
      if (!slotRetTy) slotRetTy = voidTy;
      std::vector<llvm::Type *> slotParams = {ptrTy};
      for (auto *pty : md->getParamTypes()) {
        auto *lty = CG.toLLVMType(pty);
        slotParams.push_back(lty ? lty : ptrTy);
      }
      auto *slotFnTy = llvm::FunctionType::get(slotRetTy, slotParams, false);
      fnPtr = llvm::cast<llvm::Constant>(
          CG.Module->getOrInsertFunction(concreteName, slotFnTy).getCallee());
    } else {
      fnPtr = llvm::ConstantPointerNull::get(ptrTy);
    }
    vtableEntries.push_back(fnPtr);
  }

  // Build [N x ptr] vtable array.
  // The vtable pointer stored in each object points to vtable[0].
  // Each class's vtable global has a unique address in memory — this address
  // IS the type identity, used for match dispatch via pointer comparison.
  // This is stable across modules because the linker resolves addresses.
  auto *methodArrTy    = llvm::ArrayType::get(ptrTy, vtableEntries.size());
  auto *methodArrConst = llvm::ConstantArray::get(methodArrTy, vtableEntries);
  std::string vtableGlobalName = node->getName() + kVTableSuffix;
  auto *vtableGlobal = new llvm::GlobalVariable(
      *CG.Module, methodArrTy, /*isConstant=*/true,
      llvm::GlobalValue::ExternalLinkage, methodArrConst, vtableGlobalName);
  ClassVTableGlobals[ct] = vtableGlobal;

  // -------------------------------------------------------------------------
  // 3. Emit the constructor function.
  //
  //    Signature: ptr  ClassName(initParam0_ty, ...)
  //    Returns a PaykanShared* (the caller owns the first reference).
  //
  //    Steps:
  //      a. malloc(sizeof(ClassName_struct))
  //      b. Store vtable pointer at struct[0]
  //      c. Zero-initialise all field slots
  //      d. Call ClassName___init__(rawPtr, initParams) if __init__ exists
  //      e. Wrap rawPtr in a PaykanShared box and return it
  // -------------------------------------------------------------------------
  auto *initMd = ct->findMethod(names::kMethodInit);
  std::vector<llvm::Type *> ctorParamTys;
  if (initMd) {
    for (auto *pty : initMd->getParamTypes()) {
      auto *lty = CG.toLLVMType(pty);
      ctorParamTys.push_back(lty ? lty : ptrTy);
    }
  }

  auto *ctorFnTy = llvm::FunctionType::get(ptrTy, ctorParamTys, false);
  auto *ctorFn   = llvm::Function::Create(ctorFnTy,
                                           llvm::Function::ExternalLinkage,
                                           node->getName(), CG.Module.get());

  // Name constructor parameters to match __init__.
  if (initMd) {
    const std::vector<ast::Param> *initParams = nullptr;
    for (auto *m : node->getMethods())
      if (m->getName() == names::kMethodInit) { initParams = &m->getParams(); break; }
    if (initParams) {
      size_t ai = 0;
      for (auto &arg : ctorFn->args())
        if (ai < initParams->size())
          arg.setName((*initParams)[ai++].Name);
    }
  }

  auto *savedBB2 = CG.Builder.GetInsertBlock();
  auto  savedIP2 = CG.Builder.GetInsertPoint();

  auto *ctorEntry = llvm::BasicBlock::Create(CG.LLVMCtx, kIREntry, ctorFn);
  CG.Builder.SetInsertPoint(ctorEntry);

  {
    auto *structTy = getOrCreateClassStructType(ct);

    // a. Allocate raw memory.
    auto *mallocFnTy = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
    auto *mallocFn   = CG.declareFunction(kMalloc, mallocFnTy);
    llvm::DataLayout dl(CG.Module.get());
    auto  structSize = dl.getTypeAllocSize(structTy);
    auto *sizeVal    = llvm::ConstantInt::get(i64Ty, structSize);
    auto *rawPtr     = CG.Builder.CreateCall(mallocFn, {sizeVal}, kIRObj);

    // b. Store the vtable pointer (&vtable[0]) at struct slot 0.
    auto *vtableSlotPtr = CG.Builder.CreateStructGEP(structTy, rawPtr, 0,
                                                      kIRVtableSlot);
    // vtable global is [N x ptr]. Store &vtable[0] so that dispatch works as
    // vtablePtr[slotIdx], and the global's unique address is the type identity.
    auto *arrTy = llvm::cast<llvm::ArrayType>(vtableGlobal->getValueType());
    auto *vtableGEP = CG.Builder.CreateConstInBoundsGEP2_64(
        arrTy, vtableGlobal, 0, 0, kIRVtablePtr);
    CG.Builder.CreateStore(vtableGEP, vtableSlotPtr);

    // c. Zero-initialise field slots.
    auto allFields = getAllFieldsInOrder(ct);
    for (size_t fi = 0; fi < allFields.size(); ++fi) {
      auto *fieldSlot = CG.Builder.CreateStructGEP(
          structTy, rawPtr, static_cast<unsigned>(fi + 1),
          kIRFieldPrefix + allFields[fi].first);
      auto *fty = CG.toLLVMType(allFields[fi].second);
      if (!fty) fty = ptrTy;
      CG.Builder.CreateStore(llvm::Constant::getNullValue(fty), fieldSlot);
    }

    // d. Call __init__ if present.
    if (initMd) {
      std::string initFnName = node->getName() + kNameSep + kMethodInit;
      if (auto *initFn = CG.Module->getFunction(initFnName)) {
        std::vector<llvm::Value *> initArgs = {rawPtr};
        for (auto &arg : ctorFn->args())
          initArgs.push_back(&arg);
        CG.Builder.CreateCall(initFn, initArgs);
      }
    }

    // e. Wrap in PaykanShared and return.
    llvm::Value *shared = CG.emitSharedNew(rawPtr, node->getName() + ".shared");
    CG.Builder.CreateRet(shared);
  }

  if (savedBB2)
    CG.Builder.SetInsertPoint(savedBB2, savedIP2);

  return nullptr;
}

// ---------------------------------------------------------------------------
// visitMemberAssignStmt
// ---------------------------------------------------------------------------

llvm::Value *
ClassCodeGen::visitMemberAssignStmt(ast::MemberAssignStmt *node) {
  auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);

  // Evaluate the receiver.
  // Note: visitIdentifier already unwraps owned class variables (PaykanShared*
  // -> raw object pointer), so no additional unwrapping is needed here.
  llvm::Value *objPtr = CG.emitExpr(node->getReceiver());
  if (!objPtr) return nullptr;

  // Determine the ClassType of the receiver.
  ast::ClassType *ct = getExprClassType(node->getReceiver());
  if (!ct) return nullptr;

  auto *structTy = getOrCreateClassStructType(ct);
  int   fieldIdx = getFieldIndex(ct, node->getFieldName());
  assert(fieldIdx > 0 && "Sema should have verified field exists");

  // Determine the field's AST type to handle class-typed fields correctly.
  ast::Type *fieldASTTy = nullptr;
  for (auto &[fname, fty] : getAllFieldsInOrder(ct))
    if (fname == node->getFieldName()) { fieldASTTy = fty; break; }

  llvm::Value *fieldSlot = CG.Builder.CreateStructGEP(
      structTy, objPtr, static_cast<unsigned>(fieldIdx),
      kIRFieldPrefix + node->getFieldName());

  if (fieldASTTy && ast::isa<ast::ClassType>(fieldASTTy)) {
    // Class-typed field: retain the new shared box, conditionally release old.
    llvm::Value *newShared = nullptr;

    // If RHS is an owned identifier, load its shared box and retain — do NOT
    // call emitExpr which would unwrap it to a raw pointer via visitIdentifier.
    if (auto *rhsId = ast::dyn_cast<ast::Identifier>(node->getValue())) {
      if (CG.CurrentScope && CG.CurrentScope->isOwned(rhsId->getName())) {
        auto *rhsAlloca = CG.CurrentScope->lookup(rhsId->getName());
        newShared = CG.Builder.CreateLoad(ptrTy, rhsAlloca, rhsId->getName());
        CG.emitRetain(newShared);
      }
    }
    if (!newShared) {
      // Evaluate the RHS normally (string literal, call, etc.)
      llvm::Value *rhs = CG.emitExpr(node->getValue());
      if (!rhs) return nullptr;
      // Wrap raw string literals into PaykanString* before boxing.
      if (auto *sl = ast::dyn_cast<ast::StringLiteral>(node->getValue()))
        rhs = CG.wrapStringLiteral(rhs, sl->getValue().size());
      newShared = CG.exprAlreadyShared(node->getValue())
                      ? rhs
                      : CG.emitSharedNew(rhs, kIRFieldShared);
    }

    // Release old value only if it's non-null.
    llvm::Value *old    = CG.Builder.CreateLoad(ptrTy, fieldSlot, kIROldField);
    auto        *isNull = CG.Builder.CreateICmpEQ(
        old,
        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy)),
        kIRIsNull);
    auto *parentFn  = CG.Builder.GetInsertBlock()->getParent();
    auto *releaseBB = llvm::BasicBlock::Create(CG.LLVMCtx, kIRFieldRel,   parentFn);
    auto *afterBB   = llvm::BasicBlock::Create(CG.LLVMCtx, kIRFieldAfter, parentFn);
    CG.Builder.CreateCondBr(isNull, afterBB, releaseBB);
    CG.Builder.SetInsertPoint(releaseBB);
    CG.emitRelease(old);
    CG.Builder.CreateBr(afterBB);
    CG.Builder.SetInsertPoint(afterBB);

    CG.Builder.CreateStore(newShared, fieldSlot);
  } else {
    llvm::Value *rhs = CG.emitExpr(node->getValue());
    if (!rhs) return nullptr;
    CG.Builder.CreateStore(rhs, fieldSlot);
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// visitMemberAccessExpr
// ---------------------------------------------------------------------------

llvm::Value *
ClassCodeGen::visitMemberAccessExpr(ast::MemberAccessExpr *node) {
  auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);

  // Evaluate the receiver.
  llvm::Value *objPtr = CG.emitExpr(node->getReceiver());
  if (!objPtr) return nullptr;

  // Determine the ClassType of the receiver.
  // Note: visitIdentifier already unwraps owned class variables (PaykanShared*
  // -> raw object pointer), so objPtr is already the raw struct pointer.
  ast::ClassType *ct = getExprClassType(node->getReceiver());
  if (!ct) return nullptr;

  auto *structTy = getOrCreateClassStructType(ct);
  int   fieldIdx = getFieldIndex(ct, node->getFieldName());
  assert(fieldIdx > 0 && "Sema should have verified field exists");

  ast::Type    *fieldASTTy = node->getResolvedType();
  llvm::Type   *fieldLLTy  = fieldASTTy ? CG.toLLVMType(fieldASTTy) : ptrTy;
  if (!fieldLLTy) fieldLLTy = ptrTy;

  llvm::Value *fieldSlot = CG.Builder.CreateStructGEP(
      structTy, objPtr, static_cast<unsigned>(fieldIdx),
      kIRFieldPrefix + node->getFieldName());
  return CG.Builder.CreateLoad(fieldLLTy, fieldSlot, node->getFieldName());
}

} // namespace codegen
} // namespace paykan
