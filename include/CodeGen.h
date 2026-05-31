// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// LLVM IR code generation for the Paykan language

#pragma once

#include "ASTContext.h"
#include "ASTVisitor.h"
#include "ClassCodeGen.h"
#include "Sema.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/Passes/OptimizationLevel.h>

#include <memory>
#include <string>
#include <vector>

namespace paykan {
namespace codegen {

/// LLVM IR code generator.
///
/// Walks the (already Sema-checked) AST and emits LLVM IR.  The entire
/// translation unit is lowered into a single `main()` function that
/// returns i32 0.
///
/// Expression emission is delegated to a nested ExprEmitter
/// (an ExprVisitor<ExprEmitter, llvm::Value*>).
///
class CodeGen : public ast::ASTVisitor<CodeGen, llvm::Value *> {
  ast::ASTContext &ASTCtx;
  llvm::LLVMContext &LLVMCtx;
  /// Copy of the SemaContext passed at construction; holds the pre-computed
  /// per-import SemaContexts that processImports reuses instead of re-running Sema.
  sema::SemaContext SemaCtx;
  std::unique_ptr<llvm::Module> Module;
  llvm::IRBuilder<> Builder;

  // -- Scoped symbol table (name → alloca) --------------------------------

  struct Scope {
    Scope *Parent = nullptr;
    llvm::StringMap<llvm::AllocaInst *> Locals;
    llvm::StringMap<ast::Type *> ASTTypeMap;
    /// Variables whose alloca stores a PaykanShared* (owned, ref-counted).
    llvm::StringSet<> SharedVars;

    /// Metadata for variables that need cleanup at scope exit.
    struct VarMeta {
      llvm::AllocaInst *Alloca;
      ast::Type *ASTType;  // for choosing the right delete function
    };
    /// Variables declared in this scope, in declaration order.
    std::vector<VarMeta> DeclOrder;

    explicit Scope(Scope *parent = nullptr);

    llvm::AllocaInst *lookup(llvm::StringRef name) const;
    bool isOwned(llvm::StringRef name) const;
    ast::Type *lookupASTType(llvm::StringRef name) const;
    void updateASTType(llvm::StringRef name, ast::Type *newTy);
    void set(llvm::StringRef name, llvm::AllocaInst *alloca);
    void declare(llvm::StringRef name, llvm::AllocaInst *alloca,
                 ast::Type *astTy);
    void declareUnowned(llvm::StringRef name, llvm::AllocaInst *alloca,
                        ast::Type *astTy);
    /// Promote a previously-unowned class variable to owned (SharedVars).
    void promoteToOwned(llvm::StringRef name, ast::Type *astTy);
    Scope *findOwner(llvm::StringRef name);
  };

  Scope *CurrentScope = nullptr;

  /// AST return type of the currently-emitting function (nullptr at top level).
  ast::Type *CurrentFuncReturnASTType = nullptr;

  // -- Loop context (for break / continue) ----------------------------------

  struct LoopContext {
    llvm::BasicBlock *CondBB; ///< Loop condition (target of continue)
    llvm::BasicBlock *EndBB;  ///< Loop exit (target of break)
  };
  std::vector<LoopContext> LoopStack;

  // -- Function table -------------------------------------------------------

  struct FunctionInfo {
    llvm::StringRef RuntimeName; ///< C symbol name in the runtime.
    llvm::FunctionType *FnTy = nullptr;
    bool IsVariadic = false;
  };

  /// Maps Paykan-level function names to their codegen info.
  llvm::StringMap<FunctionInfo> FunctionTable;

  /// Names of identity constructors (e.g. "Str") that simply
  /// return their single argument, wrapping string literals as needed.
  llvm::StringSet<> IdentityCtors;

  /// Register builtin functions in the function table.
  void bootstrapBuiltins();

  // -- Class codegen --------------------------------------------------------

  /// Handles all class-specific lowering: struct types, vtables,
  /// method bodies, constructors, and member access / assignment.
  ClassCodeGen Classes;

  friend class ClassCodeGen;

  /// Project root directory for import resolution.
  std::string ProjectRoot;

  /// LLVM modules generated for imported files.
  std::vector<std::unique_ptr<llvm::Module>> ImportedModules;

  /// Set of already-codegen'd import file paths (avoids duplicates).
  llvm::StringSet<> CodeGenedImports;

  /// Process imports: codegen each imported module.
  void processImports(ast::TranslationUnit *tu);

  /// Wrap a raw C string pointer into a PaykanString* via PaykanString_new.
  llvm::Value *wrapStringLiteral(llvm::Value *rawStr, size_t len);

  /// Emit cleanup (delete / release) for all variables in the given scope.
  void emitScopeCleanup(Scope &scope);

  /// Emit cleanup for all active scopes (used by return statements).
  void emitAllScopesCleanup();

  /// Emit expr as a PaykanShared* — wraps raw pointers, retains owned vars,
  /// passes through already-shared call/ternary results.
  llvm::Value *emitAsShared(ast::Expr *expr);

  /// RAII helper to push/pop a scope.
  struct ScopeGuard {
    CodeGen &CG;
    Scope ScopeObj;
    ScopeGuard(CodeGen &cg);
    ~ScopeGuard();
  };

  // -- Helpers --------------------------------------------------------------

  /// Map an AST Type* to the corresponding LLVM type.
  llvm::Type *toLLVMType(ast::Type *ty);

  /// Create an alloca in the entry block of the current function.
  llvm::AllocaInst *createEntryAlloca(llvm::Function *fn,
                                      llvm::StringRef name,
                                      llvm::Type *ty);

  /// Declare a runtime function in the LLVM module, or return the existing
  /// declaration if already present.
  llvm::Function *declareFunction(llvm::StringRef name,
                                  llvm::FunctionType *fnTy);

  /// Convenience wrappers for the three most common runtime calls.
  /// Returns true when expr already produces a PaykanShared* — i.e. it is a
  /// user-defined call/method-call/ternary whose resolved type is a class.
  /// Builtin calls (StringInt, StringFloat, etc.) return raw pointers and are
  /// excluded.
  bool exprAlreadyShared(ast::Expr *expr) const;
  void emitRetain(llvm::Value *shared);
  void emitRelease(llvm::Value *shared);
  llvm::Value *emitSharedNew(llvm::Value *raw, llvm::StringRef name = "shared");

  /// Handle the first-assignment (implicit declaration) path of visitAssignStmt:
  /// allocates an alloca, optionally wraps the value in a PaykanShared box,
  /// and registers the variable in the current scope.
  llvm::Value *emitImplicitVarDecl(llvm::StringRef name, ast::Expr *rhsExpr,
                                   llvm::Value *val);

  /// Rebind an existing class-type variable to a new shared box:
  /// retains the RHS box (if it's an owned var) or wraps a fresh value,
  /// releases the old box, and stores the new one into the alloca.
  void emitClassVarRebind(llvm::AllocaInst *alloca, ast::Expr *rhsExpr,
                          llvm::Value *val);

  // -- Expression emitter ---------------------------------------------------

  class ExprEmitter : public ast::ExprVisitor<ExprEmitter, llvm::Value *> {
    CodeGen &CG;

    llvm::Value *emitIdentityCtor(ast::CallExpr *node);
    llvm::Value *emitBuiltinCall(ast::CallExpr *node);

  public:
    explicit ExprEmitter(CodeGen &cg) : CG(cg) {}

#define EXPR_EMIT(Kind, Name, Cast) \
    llvm::Value *visit##Name(ast::Cast *node);
    PAYKAN_EXPR_NODES(EXPR_EMIT)
#undef EXPR_EMIT
  };

  ExprEmitter Emitter{*this};

  /// Emit an expression, returning its llvm::Value*.
  llvm::Value *emitExpr(ast::Expr *expr);

public:
  CodeGen(const sema::SemaContext &semaCtx, llvm::LLVMContext &llvmCtx,
          llvm::StringRef moduleName,
          const std::string &projectRoot = "");

  /// Run code generation on the translation unit.
  /// Returns true on success.
  bool run(ast::TranslationUnit *tu);

  /// Run optimization passes at the given level (O0 = none, O1 = basic, …).
  void optimize(llvm::OptimizationLevel level);

  /// Release ownership of the generated LLVM module.
  std::unique_ptr<llvm::Module> takeModule() { return std::move(Module); }

  /// Release ownership of all imported LLVM modules.
  std::vector<std::unique_ptr<llvm::Module>> takeImportedModules() {
    return std::move(ImportedModules);
  }

  /// Access the module (non-owning).
  llvm::Module &getModule() const { return *Module; }

  // -- Visitor overrides ----------------------------------------------------

#define CG_VISIT(Kind, Name, Cast) \
  llvm::Value *visit##Name(ast::Cast *node);
  PAYKAN_STMT_NODES(CG_VISIT)
  PAYKAN_DECL_NODES(CG_VISIT)
  PAYKAN_TOPLEVEL_NODES(CG_VISIT)
#undef CG_VISIT
};

} // namespace codegen
} // namespace paykan
