// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// ClassCodeGen — LLVM IR code generation for user-defined Paykan classes.
//
// This file implements ClassCodeGen, a helper owned by CodeGen that
// encapsulates all class-specific lowering:
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

/// Number of pointer slots in the object header shared by every Paykan heap
/// object (runtime builtins and generated classes alike):
///   [0] vtable pointer   [1] PaykanShared* unique-box backpointer
/// Must mirror the header of `struct PaykanObject` in src/Runtime/Runtime.h.
constexpr unsigned kNumHeaderSlots = 2;

/// Collect all instance fields in layout order: ancestor fields first,
/// then the class's own fields — mirrors C struct inheritance by composition.
std::vector<std::pair<std::string, ast::Type *>>
getAllFieldsInOrder(ast::ClassType *ct) {
  if (!ct)
    return {};
  auto fields = getAllFieldsInOrder(ct->getSuperClass());
  for (auto &[name, ty] : ct->getFields())
    fields.emplace_back(name, ty);
  return fields;
}

} // namespace

// ---------------------------------------------------------------------------
// Struct-type helpers
// ---------------------------------------------------------------------------

llvm::StructType *ClassCodeGen::getOrCreateClassStructType(ast::ClassType *ct) {
  auto it = ClassStructTypes.find(ct);
  if (it != ClassStructTypes.end())
    return it->second;

  auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
  std::vector<llvm::Type *> elems;
  // Object header — must match the runtime's PaykanObject layout exactly,
  // since raw class pointers are handed to runtime functions:
  elems.push_back(ptrTy); // index 0: vtable pointer
  elems.push_back(ptrTy); // index 1: PaykanShared* backpointer (unique box)
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
  // Fields start after the two-slot object header (vtable, shared).
  int idx = kNumHeaderSlots;
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
  auto *vtableSlotPtr =
      CG.Builder.CreateStructGEP(structTy, rawObjPtr, 0, kIRVtableSlot);
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
  } else if (auto *elemTy = CG.ASTCtx.getSpecializedArrayElemType(ct)) {
    // Specialized array type (e.g. Array<Str>): use the runtime vtable.
    // Value-element arrays (int/float/bool) use PaykanArray_vtable;
    // object-element arrays (Str[], Point[], ...) use PaykanArray_obj_vtable.
    bool isObjElem =
        ast::isa<ast::ClassType>(elemTy) || ast::isa<ast::ArrayType>(elemTy);
    const char *vtName =
        isObjElem ? names::kPaykanArrayObjVtable : names::kPaykanArrayVtable;
    // Use [0 x ptr] as placeholder — GEP(0,0) is offset zero so the address
    // equals the global base.
    auto *arrTy = llvm::ArrayType::get(ptrTy, 0);
    auto *extGlobal = CG.Module->getOrInsertGlobal(vtName, arrTy);
    expectedVtable = CG.Builder.CreateConstInBoundsGEP2_64(
        arrTy, extGlobal, 0, 0, kIRVtableExpected + ct->getName());
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
  if (auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(expr))
    return ast::dyn_cast<ast::ClassType>(ti->getResolvedType());
  return nullptr;
}

// ---------------------------------------------------------------------------
// Method name helpers
// ---------------------------------------------------------------------------

std::string ClassCodeGen::findConcreteMethodFuncName(ast::ClassType *ct,
                                                     const std::string &name) {
  if (!ct)
    return "";

  // Builtin Obj.
  if (ct == CG.ASTCtx.getObjTy()) {
    if (name == kMethodDestroy)
      return kPaykanObjectDestroy;
    if (name == kMethodToString)
      return kPaykanObjectToString;
    if (name == kMethodEquals)
      return kPaykanObjectEquals;
    return ""; // e.g. __init__ — no runtime symbol
  }
  // Builtin Str.
  if (ct == CG.ASTCtx.getStrTy()) {
    if (name == kMethodDestroy)
      return kPaykanStringDestroy;
    if (name == kMethodToString)
      return kPaykanStringToString;
    if (name == kMethodEquals)
      return kPaykanStringEquals;
    if (name == kMethodLength)
      return kPaykanStringLength;
    // NOTE: no mapping for `concat`.  Str is final, so no user vtable can
    // inherit its slots and this lookup is unreachable for concat; the Str
    // vtable itself (String.c) holds PaykanString_concat_inplace, whose
    // void-return convention differs from PaykanString_concat — mapping the
    // name here would wire the wrong function type into a vtable slot.
    return "";
  }
  // Builtin File.
  if (ct == CG.ASTCtx.getFileTy()) {
    if (name == kMethodDestroy)
      return kPaykanFileDestroy;
    if (name == kMethodToString)
      return kPaykanFileToString;
    if (name == kMethodEquals)
      return kPaykanFileEquals;
    if (name == kMethodWrite)
      return kPaykanFileWrite;
    if (name == kMethodReadln)
      return kPaykanFileReadln;
    return "";
  }
  // User-defined class: prefer the most-derived concrete function.
  std::string mangled = ct->getName() + kNameSep + name;
  if (CG.Module->getFunction(mangled))
    return mangled;
  // If this is an imported class, try the qualified name (e.g.
  // "helper::Adder_get").
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

  auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
  auto *i64Ty = llvm::Type::getInt64Ty(CG.LLVMCtx);
  auto *voidTy = llvm::Type::getVoidTy(CG.LLVMCtx);

  // Ensure the struct type exists before emitting any method bodies
  // (methods may refer to fields of their own class).
  getOrCreateClassStructType(ct);

  // Create the vtable global up-front — as a declaration (no initializer) —
  // before any method body is emitted.  A `match` on this class inside one of
  // its own methods takes the address of this global for its type check; if the
  // global did not yet exist it would synthesize an external placeholder
  // (`<Class>_vtable`) that later collides with the real definition (renamed
  // `<Class>_vtable.1`), leaving the placeholder unresolved at link time.  The
  // vtable size is known from the Sema-built vtable, so the type is fixed here;
  // the initializer (the concrete function pointers) is filled in at step 3.
  auto *vtableArrTy = llvm::ArrayType::get(ptrTy, ct->getVTableSize());
  auto *vtableGlobal = new llvm::GlobalVariable(
      *CG.Module, vtableArrTy, /*isConstant=*/true,
      llvm::GlobalValue::ExternalLinkage, /*Initializer=*/nullptr,
      node->getName() + kVTableSuffix);
  ClassVTableGlobals[ct] = vtableGlobal;

  // -------------------------------------------------------------------------
  // 1. Emit a concrete LLVM function for each method declared in this class.
  //    Signature: retTy  ClassName_methodName(ptr self, params...)
  //    __init__ returns void (the constructor wrapper handles the allocation).
  // -------------------------------------------------------------------------
  for (auto *funcDecl : node->getMethods()) {
    // The `destroy` slot is emitted as a synthetic destructor (see
    // emitDestructor) that also releases owned fields, so skip it here to
    // avoid defining the destructor twice.
    if (funcDecl->getName() == kMethodDestroy)
      continue;

    // Get the Sema-resolved MethodDecl (has resolved types).
    auto *md = ct->findMethod(funcDecl->getName());
    if (!md)
      continue;

    llvm::Type *retTy =
        md->getReturnType() ? CG.toLLVMType(md->getReturnType()) : voidTy;
    if (!retTy)
      retTy = voidTy;

    std::vector<llvm::Type *> paramTys;
    paramTys.push_back(ptrTy); // self (raw PaykanObject*)
    for (auto *pty : md->getParamTypes()) {
      auto *lty = CG.toLLVMType(pty);
      paramTys.push_back(lty ? lty : ptrTy);
    }
    auto *fnTy = llvm::FunctionType::get(retTy, paramTys, false);
    auto fnName = node->getName() + kNameSep + funcDecl->getName();
    auto *fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                      fnName, CG.Module.get());

    // Name the parameters.
    fn->arg_begin()->setName(kSelf);
    for (size_t i = 0; i < funcDecl->getParams().size(); ++i)
      std::next(fn->arg_begin(), static_cast<int>(i + 1))
          ->setName(funcDecl->getParams()[i].getName());

    // Save/restore all per-function codegen state (#34): return type, insertion
    // point, string-temp tracking, and method-class context.
    CodeGen::FunctionStateGuard fnState(CG, md->getReturnType(), ct);

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
        auto &p = funcDecl->getParams()[i];
        auto *arg = fn->getArg(static_cast<unsigned>(i + 1));
        auto *pty = md->getParamTypes()[i];
        auto *alloca = CG.createEntryAlloca(fn, p.getName(), arg->getType());
        CG.Builder.CreateStore(arg, alloca);
        if (ast::isRefType(pty))
          CG.CurrentScope->declare(p.getName(), alloca, pty);
        else
          CG.CurrentScope->declare(p.getName(), alloca, nullptr);
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
    // fnState's destructor restores per-function state and the caller's
    // insertion point at end of scope.
  }

  // -------------------------------------------------------------------------
  // 2. Emit the synthetic destructor before building the vtable so that the
  //    destroy slot resolves to ClassName_destroy (which releases owned
  //    fields) rather than the default PaykanObject_destroy.
  // -------------------------------------------------------------------------
  emitDestructor(node, ct);

  // -------------------------------------------------------------------------
  // 3. Build the vtable global constant.
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
      llvm::Type *slotRetTy =
          md->getReturnType() ? CG.toLLVMType(md->getReturnType()) : voidTy;
      if (!slotRetTy)
        slotRetTy = voidTy;
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
  auto *methodArrTy = llvm::ArrayType::get(ptrTy, vtableEntries.size());
  auto *methodArrConst = llvm::ConstantArray::get(methodArrTy, vtableEntries);
  // The global was created (as a declaration) before the method bodies; fill in
  // its initializer now that the concrete function pointers are known.  The
  // array type matches (both [getVTableSize() x ptr]).
  assert(vtableGlobal->getValueType() == methodArrTy &&
         "vtable global type must match its initializer");
  vtableGlobal->setInitializer(methodArrConst);

  // -------------------------------------------------------------------------
  // 4. Emit the constructor function.
  //
  //    Signature: ptr  ClassName(initParam0_ty, ...)
  //    Returns a PaykanShared* (the caller owns the first reference).
  //
  //    Steps:
  //      a. malloc(sizeof(ClassName_struct))
  //      b. Store vtable pointer at struct[0], null the shared backpointer
  //         at struct[1]
  //      c. Zero-initialise all field slots
  //      d. Wrap rawPtr in its unique PaykanShared box (installs the
  //         backpointer) BEFORE running __init__, so that `self` used as a
  //         value inside __init__ recovers the caller's box instead of
  //         creating — and prematurely releasing — a second one
  //      e. Call ClassName___init__(rawPtr, initParams) if __init__ exists
  //      f. Return the box (the caller owns the first reference)
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
  auto *ctorFn =
      llvm::Function::Create(ctorFnTy, llvm::Function::ExternalLinkage,
                             node->getName(), CG.Module.get());

  // Name constructor parameters to match __init__.
  if (initMd) {
    const std::vector<ast::Param> *initParams = nullptr;
    for (auto *m : node->getMethods())
      if (m->getName() == names::kMethodInit) {
        initParams = &m->getParams();
        break;
      }
    if (initParams) {
      size_t ai = 0;
      for (auto &arg : ctorFn->args())
        if (ai < initParams->size())
          arg.setName((*initParams)[ai++].getName());
    }
  }

  // Per-function state guard (#34).  The constructor body emits only fixed IR
  // (no user statements), so it touches none of the return-type / string-temp /
  // method-class state, but routing it through the same guard keeps a single
  // "emitting a function body" pattern and restores the caller's insert point.
  CodeGen::FunctionStateGuard fnState(CG, /*retASTType=*/nullptr,
                                      /*methodClassTy=*/nullptr);

  auto *ctorEntry = llvm::BasicBlock::Create(CG.LLVMCtx, kIREntry, ctorFn);
  CG.Builder.SetInsertPoint(ctorEntry);

  {
    auto *structTy = getOrCreateClassStructType(ct);

    // a. Allocate raw memory.
    auto *mallocFnTy = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
    auto *mallocFn = CG.declareFunction(kPaykanMalloc, mallocFnTy);
    llvm::DataLayout dl(CG.Module.get());
    auto structSize = dl.getTypeAllocSize(structTy);
    auto *sizeVal = llvm::ConstantInt::get(i64Ty, structSize);
    auto *rawPtr = CG.Builder.CreateCall(mallocFn, {sizeVal}, kIRObj);

    // b. Store the vtable pointer (&vtable[0]) at struct slot 0.
    auto *vtableSlotPtr =
        CG.Builder.CreateStructGEP(structTy, rawPtr, 0, kIRVtableSlot);
    // vtable global is [N x ptr]. Store &vtable[0] so that dispatch works as
    // vtablePtr[slotIdx], and the global's unique address is the type identity.
    auto *arrTy = llvm::cast<llvm::ArrayType>(vtableGlobal->getValueType());
    auto *vtableGEP = CG.Builder.CreateConstInBoundsGEP2_64(arrTy, vtableGlobal,
                                                            0, 0, kIRVtablePtr);
    CG.Builder.CreateStore(vtableGEP, vtableSlotPtr);

    // Null the shared backpointer (header slot 1) — malloc'd memory is
    // garbage, and PaykanShared_new reads this slot to decide between
    // creating a fresh box and acquiring an existing one.
    auto *sharedSlotPtr =
        CG.Builder.CreateStructGEP(structTy, rawPtr, 1, kIRSharedSlot);
    CG.Builder.CreateStore(llvm::ConstantPointerNull::get(ptrTy),
                           sharedSlotPtr);

    // c. Zero-initialise field slots (fields follow the two header slots).
    auto allFields = getAllFieldsInOrder(ct);
    for (size_t fi = 0; fi < allFields.size(); ++fi) {
      auto *fieldSlot = CG.Builder.CreateStructGEP(
          structTy, rawPtr, static_cast<unsigned>(fi + kNumHeaderSlots),
          kIRFieldPrefix + allFields[fi].first);
      auto *fty = CG.toLLVMType(allFields[fi].second);
      if (!fty)
        fty = ptrTy;
      CG.Builder.CreateStore(llvm::Constant::getNullValue(fty), fieldSlot);
    }

    // d. Wrap in PaykanShared BEFORE __init__ runs.  This installs the
    // object's unique-box backpointer, so ownership-taking uses of `self`
    // inside __init__ (f(self), field aliasing, …) retain THIS box rather
    // than boxing the half-constructed object separately and destroying it
    // when that stray box drops to zero.
    llvm::Value *shared = CG.emitSharedNew(rawPtr, node->getName() + ".shared");

    // e. Call __init__ if present.
    if (initMd) {
      std::string initFnName = node->getName() + kNameSep + kMethodInit;
      if (auto *initFn = CG.Module->getFunction(initFnName)) {
        std::vector<llvm::Value *> initArgs = {rawPtr};
        for (auto &arg : ctorFn->args())
          initArgs.push_back(&arg);
        CG.Builder.CreateCall(initFn, initArgs);
      }
    }

    // f. Return the box — the caller owns the first reference.
    CG.Builder.CreateRet(shared);
  }

  // fnState's destructor restores the caller's insertion point at end of scope.
  return nullptr;
}

// ---------------------------------------------------------------------------
// emitDestructor
// ---------------------------------------------------------------------------
//
// Generates `void ClassName_destroy(ptr self)`:
//
//   1. (optional) runs the user-defined `destroy` body with `self` bound;
//   2. releases every reference-counted field so the objects they own are
//      freed (Paykan_release is null-safe, so unset fields cost nothing);
//   3. frees the object struct itself.
//
// This function becomes the class's vtable destroy slot, so the final
// Paykan_release of an instance tears down the whole object graph.
void ClassCodeGen::emitDestructor(ast::ClassDecl *node, ast::ClassType *ct) {
  auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);
  auto *voidTy = llvm::Type::getVoidTy(CG.LLVMCtx);

  auto *structTy = getOrCreateClassStructType(ct);

  // Create the destructor function: void ClassName_destroy(ptr self).
  auto fnName = node->getName() + kNameSep + kMethodDestroy;
  auto *fnTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
  auto *fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                    fnName, CG.Module.get());
  fn->arg_begin()->setName(kSelf);

  // Locate a user-defined `destroy` body to run before field teardown.
  ast::FuncDecl *userDestroy = nullptr;
  for (auto *m : node->getMethods())
    if (m->getName() == kMethodDestroy) {
      userDestroy = m;
      break;
    }

  // Save/restore all per-function codegen state (#34): return type (void here),
  // insertion point, string-temp tracking, and method-class context.
  CodeGen::FunctionStateGuard fnState(CG, /*retASTType=*/nullptr, ct);

  auto *entry = llvm::BasicBlock::Create(CG.LLVMCtx, kIREntry, fn);
  CG.Builder.SetInsertPoint(entry);

  // Run the user-defined destroy body (if any) inside its own scope.
  if (userDestroy && userDestroy->getBody()) {
    // Create the cleanup block that always runs field teardown, even when the
    // user body terminates early with an explicit `return`.
    auto *cleanupBB = llvm::BasicBlock::Create(CG.LLVMCtx, "dtor.cleanup", fn);

    {
      CodeGen::ScopeGuard guard(CG);

      // 'self' is a raw pointer — not ref-counted inside the destructor body.
      auto *selfAlloca = CG.createEntryAlloca(fn, kSelf, ptrTy);
      CG.Builder.CreateStore(fn->getArg(0), selfAlloca);
      CG.CurrentScope->declareUnowned(kSelf, selfAlloca, ct);

      for (auto *stmt : userDestroy->getBody()->getStatements())
        CG.visit(stmt);
    }

    // Redirect the user-body exit to the cleanup block, replacing any `ret
    // void` that an early `return` statement may have emitted.
    auto *exitBB = CG.Builder.GetInsertBlock();
    if (exitBB->getTerminator())
      exitBB->getTerminator()->eraseFromParent();
    CG.Builder.CreateBr(cleanupBB);
    CG.Builder.SetInsertPoint(cleanupBB);
  }

  // Always release ref-counted fields and free the struct.
  {
    llvm::Value *self = fn->getArg(0);

    // Release every reference-counted field (Str, Obj, class, array).
    // Fields follow the two-slot object header (vtable, shared).
    auto allFields = getAllFieldsInOrder(ct);
    for (size_t fi = 0; fi < allFields.size(); ++fi) {
      if (!ast::isRefType(allFields[fi].second))
        continue;
      auto *fieldSlot = CG.Builder.CreateStructGEP(
          structTy, self, static_cast<unsigned>(fi + kNumHeaderSlots),
          kIRFieldPrefix + allFields[fi].first);
      auto *box = CG.Builder.CreateLoad(ptrTy, fieldSlot, allFields[fi].first);
      CG.emitRelease(box); // Paykan_release tolerates null boxes.
    }

    // Free the object struct.
    auto *freeFnTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
    auto *freeFn = CG.declareFunction(kPaykanFree, freeFnTy);
    CG.Builder.CreateCall(freeFn, {self});
    CG.Builder.CreateRetVoid();
  }

  // fnState's destructor restores per-function state and the caller's insertion
  // point at end of scope.
}

// ---------------------------------------------------------------------------
// visitMemberAssignStmt
// ---------------------------------------------------------------------------

llvm::Value *ClassCodeGen::visitMemberAssignStmt(ast::MemberAssignStmt *node) {
  auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);

  // Evaluate the receiver and classify what it produced (a call-rooted
  // receiver is a fresh +1 box this statement must tear down after the
  // store), then unwrap to the RAW object pointer.  Identifiers (already
  // unwrapped by visitIdentifier) and `self` pass through untouched; a nested
  // ref-typed member access (h.inner.field = v) or call result is unwrapped
  // from its PaykanShared* box — GEPing into the box itself would silently
  // overwrite its refCount/object words.
  llvm::Value *recv = CG.emitExpr(node->getReceiver());
  if (!recv)
    return nullptr;
  ExprValue recvOwned = CG.classifyExpr(node->getReceiver(), recv);
  llvm::Value *objPtr = recv;
  if (CG.exprAlreadyShared(node->getReceiver()) &&
      recv->getType()->isPointerTy()) {
    auto *getFnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    objPtr = CG.Builder.CreateCall(
        CG.declareFunction(kPaykanSharedGet, getFnTy), {recv}, kIRObj);
  }

  // Determine the ClassType of the receiver.
  ast::ClassType *ct = getExprClassType(node->getReceiver());
  if (!ct)
    return nullptr;

  auto *structTy = getOrCreateClassStructType(ct);
  int fieldIdx = getFieldIndex(ct, node->getFieldName());
  assert(fieldIdx > 0 && "Sema should have verified field exists");

  // Determine the field's AST type to handle class-typed fields correctly.
  ast::Type *fieldASTTy = nullptr;
  for (auto &[fname, fty] : getAllFieldsInOrder(ct))
    if (fname == node->getFieldName()) {
      fieldASTTy = fty;
      break;
    }

  llvm::Value *fieldSlot = CG.Builder.CreateStructGEP(
      structTy, objPtr, static_cast<unsigned>(fieldIdx),
      kIRFieldPrefix + node->getFieldName());

  if (fieldASTTy && ast::isRefType(fieldASTTy)) {
    // Ref-typed field (class OR array): the slot owns a PaykanShared* box —
    // retain/steal the new box, conditionally release the old one.  Array
    // fields must take this path too (A4): the destructor and the read path
    // treat every ref-typed slot as a box, so a plain store of the raw
    // PaykanArray* here would be read back as a box and double-freed.
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
      if (!rhs)
        return nullptr;
      // Wrap raw string literals into PaykanString* before boxing.
      if (auto *sl = ast::dyn_cast<ast::StringLiteral>(node->getValue()))
        rhs = CG.wrapStringLiteral(rhs, sl->getValue().size());
      // A raw pointer here may be a fresh temporary (string literal, concat)
      // OR an alias of an already-boxed object (match-arm binding, `self`,
      // array element).  PaykanShared_new's acquire semantics (unique-box
      // invariant, Runtime.h) make both correct: fresh objects get a new box,
      // aliases yield a +1 on the object's existing box.  An already-shared
      // RHS goes through takeSharedOwnership, which retains a borrowed
      // field-read box instead of stealing it.
      newShared = CG.exprAlreadyShared(node->getValue())
                      ? CG.takeSharedOwnership(node->getValue(), rhs)
                      : CG.emitSharedNew(rhs, kIRFieldShared);
    }

    // Release old value only if it's non-null.
    llvm::Value *old = CG.Builder.CreateLoad(ptrTy, fieldSlot, kIROldField);
    auto *isNull = CG.Builder.CreateICmpEQ(
        old,
        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy)),
        kIRIsNull);
    auto *parentFn = CG.Builder.GetInsertBlock()->getParent();
    auto *releaseBB =
        llvm::BasicBlock::Create(CG.LLVMCtx, kIRFieldRel, parentFn);
    auto *afterBB =
        llvm::BasicBlock::Create(CG.LLVMCtx, kIRFieldAfter, parentFn);
    CG.Builder.CreateCondBr(isNull, afterBB, releaseBB);
    CG.Builder.SetInsertPoint(releaseBB);
    CG.emitRelease(old);
    CG.Builder.CreateBr(afterBB);
    CG.Builder.SetInsertPoint(afterBB);

    CG.Builder.CreateStore(newShared, fieldSlot);
  } else {
    llvm::Value *rhs = CG.emitExpr(node->getValue());
    if (!rhs)
      return nullptr;
    CG.Builder.CreateStore(rhs, fieldSlot);
  }
  // Tear down an owned (call-rooted) receiver temporary now that the store
  // is complete.
  CG.releaseIfOwned(recvOwned);
  return nullptr;
}

// ---------------------------------------------------------------------------
// visitMemberAccessExpr
// ---------------------------------------------------------------------------

llvm::Value *ClassCodeGen::visitMemberAccessExpr(ast::MemberAccessExpr *node) {
  auto *ptrTy = llvm::PointerType::getUnqual(CG.LLVMCtx);

  // Evaluate the receiver and classify the ownership of what it produced
  // BEFORE unwrapping: a call-rooted receiver (e.g. `makeH().a`) hands this
  // expression a fresh +1 box that must be torn down after the field is read
  // — mirroring the receiver teardown in visitMethodCallExpr — while a plain
  // variable / `self` / borrowed field is left untouched.
  llvm::Value *recv = CG.emitExpr(node->getReceiver());
  if (!recv)
    return nullptr;
  ExprValue recvOwned = CG.classifyExpr(node->getReceiver(), recv);

  // Unwrap to the RAW object pointer (see visitMemberAssignStmt: identifiers
  // and `self` are already raw, a nested ref-typed member access or call
  // result is unwrapped from its PaykanShared* box — GEPing into the box
  // would return its refCount/object words instead).
  llvm::Value *objPtr = recv;
  if (CG.exprAlreadyShared(node->getReceiver()) &&
      recv->getType()->isPointerTy()) {
    auto *getFnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    objPtr = CG.Builder.CreateCall(
        CG.declareFunction(kPaykanSharedGet, getFnTy), {recv}, kIRObj);
  }

  // Determine the ClassType of the receiver.
  ast::ClassType *ct = getExprClassType(node->getReceiver());
  if (!ct)
    return nullptr;

  auto *structTy = getOrCreateClassStructType(ct);
  int fieldIdx = getFieldIndex(ct, node->getFieldName());
  assert(fieldIdx > 0 && "Sema should have verified field exists");

  ast::Type *fieldASTTy = node->getResolvedType();
  llvm::Type *fieldLLTy = fieldASTTy ? CG.toLLVMType(fieldASTTy) : ptrTy;
  if (!fieldLLTy)
    fieldLLTy = ptrTy;

  llvm::Value *fieldSlot = CG.Builder.CreateStructGEP(
      structTy, objPtr, static_cast<unsigned>(fieldIdx),
      kIRFieldPrefix + node->getFieldName());
  llvm::Value *fieldVal =
      CG.Builder.CreateLoad(fieldLLTy, fieldSlot, node->getFieldName());

  // Tear down an owned receiver temporary.  The receiver object dies with its
  // box, so a ref-typed field value must be RETAINED first to survive the
  // teardown; exprProducesFreshBox then reports this expression's result as
  // an owned +1 box, so the consumer releases it exactly once.
  if (recvOwned.isOwned()) {
    if (fieldASTTy && ast::isRefType(fieldASTTy))
      CG.emitRetain(fieldVal);
    CG.releaseIfOwned(recvOwned);
  }
  return fieldVal;
}

} // namespace codegen
} // namespace paykan
