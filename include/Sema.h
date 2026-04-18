// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Semantic analysis pass for the Paykan language

#pragma once

#include "ASTContext.h"
#include "ASTVisitor.h"

#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/Support/raw_ostream.h>
#include <string>
#include <vector>

namespace paykan {
namespace sema {

// A single diagnostic emitted during semantic analysis.
struct Diagnostic {
  enum Severity { Error, Warning, Note };

  Severity Level;
  ast::SourceLocation Loc;
  std::string Message;
};

// Semantic analysis visitor.
//
// Walks the AST once and checks:
//   - variable use before declaration
//   - duplicate variable declarations
//   - type compatibility in assignments and initializers
//   - operand types for arithmetic, relational, and unary operators
//
// Statement and declaration visitors return true on success, false on failure.
//
// Usage:
//   Sema S(llvm::errs());
//   bool ok = S.run(translationUnit);
//   // S.getDiagnostics() contains all collected errors/warnings.
//
class Sema : public ast::ASTVisitor<Sema, bool> {
  llvm::raw_ostream &OS;
  ast::ASTContext &Ctx;

  // -- Scoped symbol table --------------------------------------------------

  /// A single lexical scope. Each scope has its own local bindings and a
  /// pointer to its enclosing (parent) scope.
  struct Scope {
    Scope *Parent = nullptr;
    llvm::StringMap<ast::Type *> Locals;
    llvm::StringMap<ast::Ownership> OwnershipMap;
    llvm::StringSet<> MovedSet;  // unique variables that have been moved
    llvm::StringSet<> ConstSet;  // variables declared as const

    explicit Scope(Scope *parent = nullptr);

    /// Look up a name, walking the scope chain.
    ast::Type *lookup(llvm::StringRef name) const;

    /// Look up ownership of a name, walking the scope chain.
    ast::Ownership lookupOwnership(llvm::StringRef name) const;

    /// Check if a variable has been moved (walks scope chain).
    bool isMoved(llvm::StringRef name) const;

    /// Mark a variable as moved in the scope that owns it.
    void markMoved(llvm::StringRef name);

    /// Declare a name in *this* scope (does not check parent scopes).
    /// Returns false if the name already exists in this scope.
    bool declare(llvm::StringRef name, ast::Type *ty);

    /// Declare with ownership info.
    bool declare(llvm::StringRef name, ast::Type *ty, ast::Ownership ownership,
                 bool isConst = false);

    /// Check if a variable is const (walks scope chain).
    bool isConst(llvm::StringRef name) const;

    /// Insert or update a binding in this scope.
    void set(llvm::StringRef name, ast::Type *ty);

    /// Returns true if the name exists in *this* scope (not parents).
    bool contains(llvm::StringRef name) const;

    /// Find the innermost scope that contains this name, or nullptr.
    Scope *findOwner(llvm::StringRef name);
  };

  Scope *CurrentScope = nullptr;

  // -- Function signature table ---------------------------------------------

  /// Describes a known function's type signature.
  struct FunctionSig {
    ast::Type *ReturnType;
    std::vector<ast::Type *> ParamTypes;
    bool IsVariadic = false;
  };

  /// Maps function names to their signatures.
  llvm::StringMap<FunctionSig> FunctionTable;

  /// Register a function signature.
  void declareFunction(llvm::StringRef name, ast::Type *retTy,
                       std::vector<ast::Type *> paramTys,
                       bool isVariadic = false);

  /// Look up a function signature, or nullptr if unknown.
  const FunctionSig *lookupFunction(llvm::StringRef name) const;

  /// RAII helper to push/pop a scope.
  struct ScopeGuard {
    Sema &S;
    Scope ScopeObj;
    ScopeGuard(Sema &s);
    ~ScopeGuard();
  };

  // Collected diagnostics.
  std::vector<Diagnostic> Diagnostics;

  // Count of errors (not warnings) emitted so far.
  unsigned ErrorCount = 0;

  // -- Internal helpers -----------------------------------------------------

  void diag(Diagnostic::Severity level, ast::SourceLocation loc,
            const std::string &msg);
  void error(ast::SourceLocation loc, const std::string &msg);
  void warning(ast::SourceLocation loc, const std::string &msg);

  static const char *typeName(ast::Type *ty);

  // Returns true if the type is a numeric builtin (int or float).
  static bool isNumeric(ast::Type *ty);

  // Returns true if a value of type `src` can be assigned to a location of
  // type `dst`.  This includes exact match, int→float promotion, and
  // ClassType subtyping.
  bool isAssignable(ast::Type *dst, ast::Type *src) const;

  // Resolve a declared AST Type* to its canonical equivalent from
  // ASTContext (e.g. a BuiltinType(Int) node → Ctx.getIntTy(), a
  // ClassType("String") → Ctx.getStringTy()).  Returns nullptr and
  // emits an error on failure.
  ast::Type *resolveType(ast::Type *ty, ast::SourceLocation loc,
                         const std::string &context);

  // -- Expression type-checker ----------------------------------------------
  //
  // A separate ExprVisitor that walks expression trees and returns the
  // resolved Type* (nullptr on error).  It has access to Sema's symbol table,
  // ASTContext, and diagnostic helpers through a back-reference.
  //
  class ExprChecker : public ast::ExprVisitor<ExprChecker, ast::Type *> {
    Sema &S;

  public:
    explicit ExprChecker(Sema &sema) : S(sema) {}

#define EXPR_VISIT(Kind, Name, Cast) \
    ast::Type *visit##Name(ast::Cast *node);
    PAYKAN_EXPR_NODES(EXPR_VISIT)
#undef EXPR_VISIT
  };

  ExprChecker EC{*this};

  // Visit an expression and return its resolved type (nullptr on error).
  ast::Type *resolveExprType(ast::Expr *expr);

public:
  explicit Sema(ast::ASTContext &ctx, llvm::raw_ostream &os = llvm::errs());

  // Entry point -- run semantic analysis on a TranslationUnit.
  bool run(ast::TranslationUnit *tu);

  // Access diagnostics after analysis.
  const std::vector<Diagnostic> &getDiagnostics() const { return Diagnostics; }
  unsigned getErrorCount() const { return ErrorCount; }
  bool hasErrors() const { return ErrorCount > 0; }

  // -- Visitor overrides ----------------------------------------------------

#define SEMA_VISIT(Kind, Name, Cast) \
  bool visit##Name(ast::Cast *node);
  PAYKAN_STMT_NODES(SEMA_VISIT)
  PAYKAN_DECL_NODES(SEMA_VISIT)
  PAYKAN_TOPLEVEL_NODES(SEMA_VISIT)
#undef SEMA_VISIT
};

} // namespace sema
} // namespace paykan
