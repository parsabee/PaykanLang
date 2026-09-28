// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Semantic analysis pass for the Paykan language

#pragma once

#include "ASTContext.h"
#include "ASTVisitor.h"
#include "DiagEngine.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringSet.h>
#include <memory>
#include <string>
#include <vector>

namespace paykan {
namespace parser {
class ParserDriver;
}
namespace sema {

/// Result object returned by Sema::run(). Carries the populated ASTContext
/// and (for imported modules) the owning ParserDriver plus pre-computed child
/// SemaContexts so CodeGen can consume them without re-running Sema.
struct SemaContext {
  /// For imported modules: owns the ParserDriver (and thus its ASTContext).
  /// nullptr for top-level contexts where the caller owns the driver.
  std::shared_ptr<parser::ParserDriver> OwnedDriver;
  /// Non-owning pointer into the relevant ASTContext.
  ast::ASTContext *ASTCtx = nullptr;
  /// Root TU node — set when OwnedDriver is non-null.
  ast::TranslationUnit *Root = nullptr;
  bool Ok = false;
  unsigned ErrorCount = 0;
  std::vector<Diagnostic> Diagnostics;
  /// Pre-computed SemaContexts for directly-imported modules, keyed by
  /// resolved file path. Populated by Sema::run() so CodeGen can reuse
  /// them without re-running the Sema pass.
  llvm::StringMap<std::shared_ptr<SemaContext>> ImportedContexts;

  explicit operator bool() const { return Ok; }
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
//   DiagEngine diag(llvm::errs());
//   diag.setSourceInfo("foo.pkn", &lines);
//   Sema S(ctx, diag);
//   bool ok = S.run(translationUnit);
//   // diag.getDiagnostics() contains all collected errors/warnings.
//
class Sema : public ast::ASTVisitor<Sema, bool> {
  DiagEngine &Diags;
  ast::ASTContext &Ctx;

  // -- Scoped symbol table --------------------------------------------------

  /// A single lexical scope. Each scope has its own local bindings and a
  /// pointer to its enclosing (parent) scope.
  struct Scope {
    Scope *Parent = nullptr;
    llvm::StringMap<ast::Type *> Locals;
    /// Names moved-out of this scope via `mov`.  A moved name may not be read
    /// again until it is re-assigned (which revives it).
    llvm::StringSet<> Moved;

    explicit Scope(Scope *parent = nullptr);

    /// Look up a name, walking the scope chain.
    ast::Type *lookup(llvm::StringRef name) const;

    /// Declare a name in *this* scope (does not check parent scopes).
    /// Returns false if the name already exists in this scope.
    bool declare(llvm::StringRef name, ast::Type *ty);

    /// Insert or update a binding in this scope.
    void set(llvm::StringRef name, ast::Type *ty);

    /// Returns true if the name exists in *this* scope (not parents).
    bool contains(llvm::StringRef name) const;

    /// Find the innermost scope that contains this name, or nullptr.
    Scope *findOwner(llvm::StringRef name);

    /// Move tracking (operate on the owning scope, walking the chain).
    /// markMoved / clearMoved are no-ops if the name is unknown.
    void markMoved(llvm::StringRef name);
    void clearMoved(llvm::StringRef name);
    bool isMoved(llvm::StringRef name) const;
  };

  Scope *CurrentScope = nullptr;

  // -- Flow-sensitive move tracking -------------------------------------------
  //
  // The per-scope `Moved` sets above track the *current* moved-state along the
  // straight-line path Sema is walking.  Control-flow constructs make that
  // state path-dependent, so they snapshot and merge it explicitly:
  //
  //   * Branches (if/else, match arms): each branch is checked against the
  //     moved-state at ENTRY to the construct (a `mov` in one branch must not
  //     poison a sibling).  Afterwards the state is the UNION over all branch
  //     exit states — a name moved on ANY path counts as moved, unless every
  //     path (including the implicit skip path of an `if` without `else` or a
  //     match without a wildcard arm) re-assigned it.
  //
  //   * Loops (while): a name owned by a scope OUTSIDE the loop that is still
  //     moved at the loop back edge would be read-after-consume on the next
  //     iteration, so it is a compile error unless the body definitely
  //     re-assigned it before the back edge.  (Codegen nulls the slot on mov;
  //     iteration 2 would otherwise crash or silently misbehave.)
  //
  // A MovedState snapshots the Moved set of every scope on the current chain,
  // innermost first.  Scopes created inside a branch die with the branch, so
  // the chain at a construct's entry and at each of its branch exits is
  // identical and entries correspond positionally.
  using MovedState =
      llvm::SmallVector<std::pair<Scope *, llvm::StringSet<>>, 8>;

  /// Snapshot the Moved set of every scope on the current chain.
  MovedState saveMovedState() const;
  /// Reset the chain's Moved sets to a previously saved snapshot.
  void restoreMovedState(const MovedState &st);
  /// dst |= src, scope by scope (both must snapshot the same chain).
  static void unionMovedState(MovedState &dst, const MovedState &src);

  /// Implements the branch-merge rule above for any multi-branch construct.
  /// Usage: construct one merger; wrap each branch in
  /// beginBranch()/endBranch(); call finish(coversAllPaths) once, where
  /// coversAllPaths is true iff some branch is guaranteed to run (if/else
  /// present, match has a wildcard arm).
  class MovedBranchMerger {
    Sema &S;
    MovedState Entry;  // moved-state at construct entry (shared branch input)
    MovedState Merged; // union of branch exit states accumulated so far
    bool AnyBranch = false;

  public:
    explicit MovedBranchMerger(Sema &s);
    void beginBranch();
    void endBranch();
    void finish(bool coversAllPaths);
  };

  /// The expected return type of the current function (nullptr = top-level /
  /// void).
  ast::Type *CurrentReturnType = nullptr;

  /// Loop nesting depth (0 = not inside a loop).
  unsigned LoopDepth = 0;

  // -- Class analysis context ----------------------------------------------
  //
  // Populated while visitClassDecl is running; nullptr outside of a class.
  //
  struct ClassContext {
    ast::ClassType *ClassType; // the type being checked
    std::string MethodName;    // method currently being checked (empty = none)
    bool SuperInitRequired = false; // __init__ must call __super__
    bool SuperInitCalled = false;   // __super__ has been called
  };

  ClassContext *CurrentClassCtx = nullptr;

  // -- Function signature table ---------------------------------------------

  /// Describes a known function's type signature.
  struct FunctionSig {
    ast::Type *ReturnType = nullptr;
    std::vector<ast::Type *> ParamTypes;
    bool IsBuiltin = false;
  };

  /// Maps function names to their signatures.
  llvm::StringMap<FunctionSig> FunctionTable;

  /// Register a function signature.
  void declareFunction(llvm::StringRef name, ast::Type *retTy,
                       std::vector<ast::Type *> paramTys,
                       bool isBuiltin = false);

  /// Look up a function signature, or nullptr if unknown.
  const FunctionSig *lookupFunction(llvm::StringRef name) const;

  /// RAII helper to push/pop a scope.
  struct ScopeGuard {
    Sema &S;
    Scope ScopeObj;
    ScopeGuard(Sema &s);
    ~ScopeGuard();
  };

  // -- Internal helpers -----------------------------------------------------

  void error(ast::SourceLocation loc, const std::string &msg);
  void warning(ast::SourceLocation loc, const std::string &msg);
  void note(ast::SourceLocation loc, const std::string &msg);

  static std::string typeName(ast::Type *ty);

  // Render a method signature for diagnostics, e.g. "Animal.describe() -> int".
  static std::string signatureString(const ast::ClassType *owner,
                                     const std::string &methodName,
                                     ast::Type *retTy,
                                     const std::vector<ast::Type *> &paramTys);

  // Structural type equality (pointer equality is insufficient for ArrayType
  // nodes because each make<ArrayType>() call yields a fresh allocation).
  static bool typesEqual(ast::Type *a, ast::Type *b);

  // Returns true if a value of type `src` can be assigned to a location of
  // type `dst`.  This includes exact match, int->float promotion, and
  // ClassType subtyping.
  bool isAssignable(ast::Type *dst, ast::Type *src) const;

  // Returns the lowest common ancestor in the class hierarchy of two class
  // types, or nullptr if they share no common ancestor.
  static ast::ClassType *findLowestCommonAncestor(ast::ClassType *a,
                                                  ast::ClassType *b);

  // Resolve a declared AST Type* to its canonical equivalent from
  // ASTContext (e.g. a BuiltinType(Int) node -> Ctx.getIntTy(), a
  // ClassType("Str") -> Ctx.getStrTy()).  Returns nullptr and
  // emits an error on failure.
  ast::Type *resolveType(ast::Type *ty, ast::SourceLocation loc,
                         const std::string &context);

  // Check that a variable is declared. Returns its type,
  // or nullptr (with error emitted) on failure.
  ast::Type *checkIdentLive(llvm::StringRef name, ast::SourceLocation loc);

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

#define EXPR_VISIT(Kind, Name, Cast) ast::Type *visit##Name(ast::Cast *node);
    PAYKAN_EXPR_NODES(EXPR_VISIT)
#undef EXPR_VISIT
  };

  ExprChecker EC{*this};

  // Visit an expression and return its resolved type (nullptr on error).
  ast::Type *resolveExprType(ast::Expr *expr);

  // Value-mode match checking (primitive / Str subject with literal arms).
  bool checkValueMatch(ast::MatchStmt *node, ast::Type *subjectTy);

  // Enum-mode match checking: each non-wildcard arm names a bare variant of the
  // subject enum (parsed as a type-name arm).  No bindings are allowed.
  bool checkEnumMatch(ast::MatchStmt *node, ast::EnumType *subjectTy);

  // -- Import resolution ----------------------------------------------------

  /// The directory of the file currently being analyzed.
  std::string ProjectRoot;

  /// Files currently being imported (for cycle detection).
  llvm::StringSet<> *ImportStack = nullptr;

  /// Accumulated SemaContexts for each directly-imported module, keyed by
  /// resolved path. Built by processImport; moved into the SemaContext
  /// returned by run().
  llvm::StringMap<std::shared_ptr<SemaContext>> AccumulatedImportContexts;

  /// Info about an already-analyzed module.
  struct ModuleInfo {
    // Functions are stored as serialised name-strings so the cache entry never
    // holds raw Type* pointers into a foreign (potentially destroyed)
    // ASTContext.
    struct FunctionInfo {
      std::string Name;
      std::string ReturnTypeName;
      std::vector<std::string> ParamTypeNames;
    };
    std::vector<FunctionInfo> ExportedFunctions;

    // Serialised class-type descriptions so they can be reconstructed in
    // any importing ASTContext without holding raw pointers into a foreign
    // arena.
    struct ClassInfo {
      std::string Name;
      std::string SuperClassName; // "" -> implicit Object root
      struct FieldInfo {
        std::string FieldName;
        std::string TypeName;
      };
      struct MethodInfo {
        std::string Name;
        std::string ReturnTypeName;
        std::vector<std::string> ParamTypeNames;
        uint8_t Flags; // ast::MethodDecl::Private bit
      };
      std::vector<FieldInfo> Fields;
      std::vector<MethodInfo> Methods;
    };
    std::vector<ClassInfo> ExportedClasses;

    // Serialised enum-type descriptions: the enum name plus its variant names
    // in declaration order (the index is the variant's underlying value).
    struct EnumInfo {
      std::string Name;
      std::vector<std::string> Variants;
    };
    std::vector<EnumInfo> ExportedEnums;
  };

  /// Global cache of already-analyzed modules (keyed by resolved file path).
  static llvm::StringMap<ModuleInfo> ModuleCache;

  /// Resolve a module path to an absolute file path.
  std::string resolveModulePath(const std::string &modulePath, bool isSystem,
                                ast::SourceLocation loc);

  /// Process a single import declaration.
  bool processImport(ast::ImportDecl *node);

  /// Register classes: names, hierarchy, field types, method signatures, and
  /// constructors (phases 1–4b).  Method bodies are deferred to
  /// checkClassBodies() so that free functions can be forward-declared in
  /// between.  Stores the topological class order in SortedClasses.
  bool checkClassDecls(const std::vector<ast::ClassDecl *> &classDecls);

  /// Type-check class method bodies (phase 5) over SortedClasses.  Run after
  /// checkClassDecls and after free-function signatures are registered.
  bool checkClassBodies();

  /// Definite-assignment check for a constructor body: verify every own field
  /// of `ct` is assigned (self.field = …) on every control-flow path out of
  /// `body` (each return and the final fall-through).  Emits an error per
  /// field left possibly-unassigned.  Returns true if all fields are
  /// definitely assigned.
  bool checkInitFieldsAssigned(ast::ClassType *ct, ast::CompoundStmt *body,
                               ast::SourceLocation initLoc,
                               const std::string &className);

  /// Topologically-sorted class decls (superclass before subclass), populated
  /// by checkClassDecls and consumed by checkClassBodies.
  std::vector<ast::ClassDecl *> SortedClasses;

  /// Resolve a free function's signature (return + parameter types) and
  /// register it in the function table.  Run as a forward-declaration pass
  /// before any function or method body is checked, so calls resolve regardless
  /// of the order declarations appear in the module.
  bool declareFunctionSignature(ast::FuncDecl *node);

public:
  explicit Sema(ast::ASTContext &ctx, DiagEngine &diags,
                const std::string &projectRoot = "");

  // Entry point -- run semantic analysis on a TranslationUnit.
  // Returns a SemaContext whose bool operator is true on success.
  SemaContext run(ast::TranslationUnit *tu);

  // Access diagnostics after analysis.
  const std::vector<Diagnostic> &getDiagnostics() const {
    return Diags.getDiagnostics();
  }
  unsigned getErrorCount() const { return Diags.getErrorCount(); }
  bool hasErrors() const { return Diags.hasErrors(); }

  // -- Visitor overrides ----------------------------------------------------

#define SEMA_VISIT(Kind, Name, Cast) bool visit##Name(ast::Cast *node);
  PAYKAN_STMT_NODES(SEMA_VISIT)
  PAYKAN_DECL_NODES(SEMA_VISIT)
  PAYKAN_TOPLEVEL_NODES(SEMA_VISIT)
#undef SEMA_VISIT
};

} // namespace sema
} // namespace paykan
