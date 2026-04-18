// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// LLVM IR code generation for the Paykan language

#pragma once

#include "ASTContext.h"
#include "ASTVisitor.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

#include <llvm/ADT/StringMap.h>
#include <llvm/Passes/OptimizationLevel.h>

#include <memory>
#include <string>

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
  std::unique_ptr<llvm::Module> Module;
  llvm::IRBuilder<> Builder;

  // -- Scoped symbol table (name → alloca) --------------------------------

  struct Scope {
    Scope *Parent = nullptr;
    llvm::StringMap<llvm::AllocaInst *> Locals;

    explicit Scope(Scope *parent = nullptr);

    llvm::AllocaInst *lookup(llvm::StringRef name) const;
    void set(llvm::StringRef name, llvm::AllocaInst *alloca);
    Scope *findOwner(llvm::StringRef name);
  };

  Scope *CurrentScope = nullptr;

  // -- Function table -------------------------------------------------------

  struct FunctionInfo {
    llvm::StringRef RuntimeName; ///< C symbol name in the runtime.
    llvm::FunctionType *FnTy = nullptr;
    bool IsVariadic = false;
  };

  /// Maps Paykan-level function names to their codegen info.
  llvm::StringMap<FunctionInfo> FunctionTable;

  /// Register builtin functions in the function table.
  void bootstrapBuiltins();

  /// Wrap a raw C string pointer into a PaykanString* via PaykanString_new.
  llvm::Value *wrapStringLiteral(llvm::Value *rawStr, size_t len);

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

  // -- Expression emitter ---------------------------------------------------

  class ExprEmitter : public ast::ExprVisitor<ExprEmitter, llvm::Value *> {
    CodeGen &CG;

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
  CodeGen(ast::ASTContext &astCtx, llvm::LLVMContext &llvmCtx,
          llvm::StringRef moduleName);

  /// Run code generation on the translation unit.
  /// Returns true on success.
  bool run(ast::TranslationUnit *tu);

  /// Run optimization passes at the given level (O0 = none, O1 = basic, …).
  void optimize(llvm::OptimizationLevel level);

  /// Release ownership of the generated LLVM module.
  std::unique_ptr<llvm::Module> takeModule() { return std::move(Module); }

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
