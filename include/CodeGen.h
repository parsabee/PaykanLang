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

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/Passes/OptimizationLevel.h>

#include <memory>
#include <string>
#include <vector>

namespace paykan {
namespace codegen {

/// Ownership classification for a code-generated expression value (#32).
///
/// Every produced `llvm::Value*` is either:
///
///   • Borrowed — nothing to free (primitives, borrowed field/box loads, raw
///                C-string globals).  The consumer must NOT release it.
///   • Owned    — the consumer owns the value and must tear it down.  The exact
///                teardown depends on the underlying runtime object: a raw
///                `PaykanString*` temporary is freed with
///                `PaykanString_destroy` (tracked in `OwnedStringTemps`), while
///                a freshly produced `PaykanShared*` box (+1 refcount) is freed
///                with `Paykan_release`.  That distinction is a property of the
///                value, not of the ownership decision, so it is resolved at
///                teardown time (releaseIfOwned) rather than encoded in a
///                separate kind.
///
/// Previously each consumer re-derived ownership from the AST (via
/// `exprAlreadyShared` / `exprProducesFreshBox`) and from `OwnedStringTemps`
/// membership, in an ad-hoc way scattered across many call sites.  `ExprValue`
/// bundles the value with a single ownership bit so the decision is made once,
/// at production, and acted on consistently at consumption.
struct ExprValue {
  enum class Ownership {
    Borrowed, ///< Caller must not release; nothing to free.
    Owned,    ///< Caller owns it and must tear it down (string temp or box).
  };

  llvm::Value *Val = nullptr;
  Ownership Own = Ownership::Borrowed;

  ExprValue() = default;
  ExprValue(llvm::Value *v, Ownership own) : Val(v), Own(own) {}

  static ExprValue borrowed(llvm::Value *v) { return {v, Ownership::Borrowed}; }
  static ExprValue owned(llvm::Value *v) { return {v, Ownership::Owned}; }

  bool isOwned() const { return Own == Ownership::Owned; }
  llvm::Value *value() const { return Val; }
  explicit operator bool() const { return Val != nullptr; }
};

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
  /// per-import SemaContexts that processImports reuses instead of re-running
  /// Sema.
  sema::SemaContext SemaCtx;
  std::unique_ptr<llvm::Module> Module;
  llvm::IRBuilder<> Builder;

  // -- Scoped symbol table (name -> alloca) --------------------------------

  struct Scope {
    Scope *Parent = nullptr;
    llvm::StringMap<llvm::AllocaInst *> Locals;
    llvm::StringMap<ast::Type *> ASTTypeMap;
    /// Variables whose alloca stores a PaykanShared* (owned, ref-counted).
    llvm::StringSet<> SharedVars;

    /// Metadata for variables that need cleanup at scope exit.
    struct VarMeta {
      llvm::AllocaInst *Alloca;
      ast::Type *ASTType; // for choosing the right delete function
    };
    /// Variables declared in this scope, in declaration order.
    std::vector<VarMeta> DeclOrder;
    /// Extra owned PaykanShared* boxes (not bound to a named variable) that
    /// must be released when this scope exits — e.g. a match subject whose
    /// arm binding is only an unowned alias.  Released LIFO at scope cleanup.
    std::vector<llvm::Value *> PendingReleases;
    /// For unowned arm bindings: the PaykanShared* that backs the raw-ptr
    /// alias.
    llvm::StringMap<llvm::Value *> BackingShared;

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
    /// Like declareUnowned but also records the PaykanShared* that backs this
    /// binding so that call sites can retain+pass the original box instead of
    /// wrapping the raw pointer in a fresh PaykanShared_new (which would give
    /// the callee sole ownership, destroying the object prematurely).
    void declareUnownedWithBacking(llvm::StringRef name,
                                   llvm::AllocaInst *alloca,
                                   llvm::Value *shared, ast::Type *astTy);
    /// Walk the scope chain looking for a backing PaykanShared* for name.
    llvm::Value *lookupBackingShared(llvm::StringRef name) const;
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
    Scope *EnclosingScope;    ///< Scope active when the loop was entered.
  };
  std::vector<LoopContext> LoopStack;

  // -- Function table -------------------------------------------------------

  struct FunctionInfo {
    llvm::StringRef RuntimeName; ///< C symbol name in the runtime.
    llvm::FunctionType *FnTy = nullptr;
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

  /// Registry of already-codegen'd imports: resolved file path -> the LLVM
  /// module that DEFINES that import's functions (the target for qualifier
  /// aliases).  Backing store for the top-level CodeGen; nested import
  /// CodeGens share the top-level's registry via @ref ImportRegistry so a
  /// module reachable through multiple import paths (a diamond) is generated
  /// exactly once.
  llvm::StringMap<llvm::Module *> CodeGenedImports;

  /// Points at the registry shared across the whole import graph.  Defaults to
  /// this instance's own @ref CodeGenedImports for the top-level CodeGen.
  llvm::StringMap<llvm::Module *> *ImportRegistry = nullptr;

  /// Wire up one import site: for every function defined in @p defMod, declare
  /// the qualified name in the current module and add the matching alias to
  /// @p defMod (idempotent).  Used both when a module is freshly generated and
  /// when it was already generated via another import path.
  void addImportAliases(llvm::Module *defMod, const std::string &qualifier,
                        const std::string &modulePath);

  // -- Global interning ----------------------------------------------------

  /// Intern table for raw C-string globals (keyed by string content).
  /// Avoids emitting duplicate `.str` globals for identical string literals.
  llvm::StringMap<llvm::GlobalVariable *> InternedStrings;

  /// Intern table for primitive array data globals (keyed by a byte-level
  /// fingerprint of the element values).  Avoids duplicate `.arr.data`
  /// globals for identical array literals.
  llvm::StringMap<llvm::GlobalVariable *> InternedArrayData;

  /// Process imports: codegen each imported module.
  void processImports(ast::TranslationUnit *tu);

  /// Wrap a raw C string pointer into a PaykanString* via PaykanString_new.
  llvm::Value *wrapStringLiteral(llvm::Value *rawStr, size_t len);

  /// Value-mode match: emit an equality if-else chain over literal-pattern
  /// arms.
  llvm::Value *emitValueMatch(ast::MatchStmt *node, llvm::Value *subjRaw,
                              llvm::Function *parentFn);

  /// Enum-mode match: emit an icmp-eq if-else chain comparing the subject's
  /// i64 value against each bare-variant arm's constant.
  llvm::Value *emitEnumMatch(ast::MatchStmt *node, llvm::Value *subjRaw,
                             llvm::Function *parentFn);

  // -- Match-lowering scaffolding shared by all three modes ------------------

  /// Create the wildcard body block if the match has a wildcard arm, else
  /// return nullptr (the check chain then falls through to match.end).
  llvm::BasicBlock *createMatchWildcardBlock(ast::MatchStmt *node,
                                             llvm::Function *parentFn);

  /// Emit one arm's body statements into `bodyBB` inside a fresh scope, then
  /// branch to `endBB` unless the body already terminated the block.
  void emitMatchArmBody(ast::MatchArm *arm, llvm::BasicBlock *bodyBB,
                        llvm::BasicBlock *endBB);

  /// Emit the wildcard arm's body into `wildcardBB` (no-op when nullptr).
  void emitMatchWildcardBody(ast::MatchStmt *node, llvm::BasicBlock *wildcardBB,
                             llvm::BasicBlock *endBB);

  /// Emit cleanup (delete / release) for all variables in the given scope.
  void emitScopeCleanup(Scope &scope);

  /// Emit cleanup for all active scopes (used by return statements).
  void emitAllScopesCleanup();

  /// Emit cleanup for scopes from the current scope up to (but not including)
  /// the innermost loop's enclosing scope.  Used by break / continue so that
  /// owned variables and pending box releases inside the loop body are not
  /// leaked when control jumps out of / restarts the loop.
  void emitLoopScopesCleanup();

  /// Emit expr as a PaykanShared* — wraps raw pointers, retains owned vars,
  /// passes through already-shared call/ternary results.  Applies the
  /// optional-type representation rules (see the helpers below): a `None`
  /// destined for a `T?` slot yields the null box, and a `T?` value destined
  /// for an `Obj` slot has its null box replaced by the boxed None singleton.
  llvm::Value *emitAsShared(ast::Expr *expr);
  /// emitAsShared without the optional-type rules (the body of the old
  /// emitAsShared; kept separate so every caller gets the rules uniformly).
  llvm::Value *emitAsSharedRaw(ast::Expr *expr);

  // -- Optional types (prototype, issue #5) ----------------------------------
  //
  // A `T?` is the same PaykanShared* box as a `T`, with NULL meaning None.
  // Sema records the two implicit conversions on the AST (see
  // Sema::checkAssignable) and CodeGen applies them here:
  //
  //   * `None` flowing into a `T?` slot: the literal's resolved type is that
  //     `T?` (isNoneForOptional) and the emitted value is the null box —
  //     never the boxed `None` singleton, which is how a *present* `Obj`
  //     spells None.
  //   * a `T?` flowing into an `Obj` slot (Expr::CoercedType): the value is
  //     coerced so an `Obj` never holds a NULL box — a null box becomes a +1
  //     box of the None singleton (emitOptionalToObj), or, on the raw-pointer
  //     builtin-call path, the singleton's address (emitOptionalToObjRaw).

  /// True when `expr` is the `None` literal in an optional-typed position.
  static bool isNoneForOptional(ast::Expr *expr);
  /// If Sema marked `expr` as a `T?` -> `Obj` conversion, return a box that
  /// is never NULL (phi of `box` and a fresh box of the None singleton);
  /// otherwise return `box` unchanged.
  llvm::Value *emitOptionalToObj(ast::Expr *expr, llvm::Value *box);
  /// Raw-pointer variant of emitOptionalToObj: `raw` is an unboxed
  /// PaykanObject* that is NULL for None; substitutes the None singleton.
  llvm::Value *emitOptionalToObjRaw(ast::Expr *expr, llvm::Value *raw);
  /// Lower `==` / `!=` when at least one operand is optional: a null check
  /// against the `None` literal, or the two-optional protocol (both None ->
  /// equal; one None -> not equal; otherwise the virtual `equals`).  Returns
  /// the i1 "equal" result (the caller negates for `!=`).
  llvm::Value *emitOptionalEquality(ast::BinaryExpr *node);

  /// RAII helper to push/pop a scope.
  struct ScopeGuard {
    CodeGen &CG;
    Scope ScopeObj;
    ScopeGuard(CodeGen &cg);
    ~ScopeGuard();
  };

  /// RAII helper that saves and restores ALL per-function codegen state around
  /// the emission of one function/method/destructor body (#34).
  ///
  /// This consolidates the previously-scattered manual save/restore at the
  /// three body-emission sites (visitFuncDecl, ClassCodeGen method emission,
  /// emitDestructor) into a single named unit.  It is the seam at which a
  /// standalone `CodeGenFunction` can later be split out: everything this guard
  /// snapshots is exactly the mutable state that must NOT leak between sibling
  /// functions, and which a future parallel per-function compiler would have to
  /// own privately rather than share on `CodeGen`.
  ///
  /// Saved/restored state:
  ///   • Builder insertion point (block + iterator)
  ///   • CurrentFuncReturnASTType
  ///   • OwnedStringTemps (swapped out to an empty set for the body)
  ///   • ClassCodeGen::CurrentMethodClassType
  struct FunctionStateGuard {
    CodeGen &CG;
    llvm::BasicBlock *SavedBB;
    llvm::BasicBlock::iterator SavedIP;
    ast::Type *SavedRetASTType;
    ast::ClassType *SavedMethodClassType;
    llvm::SmallPtrSet<llvm::Value *, 16> SavedStringTemps;

    /// @param retASTType    AST return type for the body being emitted.
    /// @param methodClassTy ClassType when emitting a method/destructor body,
    ///                      or nullptr for a free function.
    FunctionStateGuard(CodeGen &cg, ast::Type *retASTType,
                       ast::ClassType *methodClassTy);
    ~FunctionStateGuard();

    FunctionStateGuard(const FunctionStateGuard &) = delete;
    FunctionStateGuard &operator=(const FunctionStateGuard &) = delete;
  };

  // -- Helpers --------------------------------------------------------------

  /// Map an AST Type* to the corresponding LLVM type.
  llvm::Type *toLLVMType(ast::Type *ty);

  /// True when an array element of this type occupies a PaykanShared* slot (a
  /// real class or nested array), as opposed to a raw primitive slot.  Enums
  /// lower to i64 primitives, so an EnumType — or a ClassType stub that
  /// actually names an enum — is NOT an object element.
  bool isObjectElementType(ast::Type *elemTy) const;

  /// Create (or return the existing) LLVM prototype for a free function,
  /// without emitting its body.  Run for every function before any body so a
  /// call — including one inside a class method — resolves regardless of source
  /// order.
  llvm::Function *declareFunctionPrototype(ast::FuncDecl *node);

  /// Canonicalize a declared type annotation: a parser ClassType stub that
  /// names an enum becomes the EnumType (so it lowers to i64, not a boxed
  /// pointer); a stub naming a class becomes the canonical ClassType.  Returns
  /// the input for anything else (including nullptr).
  ast::Type *canonicalizeDeclType(ast::Type *ty);

  /// Resolve the concrete ClassType an RHS expression produces, canonicalized
  /// against the registry (parser stubs carry no fields/methods), or nullptr
  /// when the expression has no class-typed resolution.  Shared by the
  /// variable-binding sites (visitVarDecl / visitAssignStmt /
  /// emitImplicitVarDecl) that narrow a variable's scope type to the concrete
  /// RHS class so later method dispatch uses the concrete vtable convention.
  ast::ClassType *resolveExprClassType(ast::Expr *expr);

  /// Create an alloca in the entry block of the current function.
  llvm::AllocaInst *createEntryAlloca(llvm::Function *fn, llvm::StringRef name,
                                      llvm::Type *ty);

  /// Declare a runtime function in the LLVM module, or return the existing
  /// declaration if already present.
  llvm::Function *declareFunction(llvm::StringRef name,
                                  llvm::FunctionType *fnTy);

  /// Convenience wrappers for the three most common runtime calls.
  /// Returns true when expr already produces a PaykanShared* — i.e. it is a
  /// user-defined call/method-call/ternary whose resolved type is a class.
  /// Builtin calls (StrInt, StrFloat, etc.) return raw pointers and are
  /// excluded.
  bool exprAlreadyShared(ast::Expr *expr) const;
  /// Returns true when `expr` yields a *freshly owned* PaykanShared* (+1 ref)
  /// that the consumer must release — as opposed to a borrowed box such as a
  /// member-access field load or an owned-variable read.  Used to decide
  /// whether a box unwrapped for a borrow (e.g. println(call())) must be
  /// released afterwards.
  bool exprProducesFreshBox(ast::Expr *expr) const;
  void emitRetain(llvm::Value *shared);
  void emitRelease(llvm::Value *shared);
  llvm::Value *emitSharedNew(llvm::Value *raw, llvm::StringRef name = "shared");

  /// Take ownership of `val`, the PaykanShared* just emitted for `expr`
  /// (exprAlreadyShared(expr) must hold): a freshly produced +1 box (call /
  /// method call / ternary / array literal / `mov` / call-rooted field read)
  /// is owned as-is, while a borrowed box (a plain ref-typed field read) is
  /// retained first — the field slot keeps its own reference.  Stealing the
  /// borrow instead used to over-release: the new owner's release and the
  /// field's release together freed the object one time too many.
  llvm::Value *takeSharedOwnership(ast::Expr *expr, llvm::Value *val);

  /// Emit a reference-typed expression and return the *raw* underlying pointer
  /// (PaykanArray* / PaykanString* / PaykanObject*), unwrapping the
  /// PaykanShared* box when the expression produces one.  Identifiers and
  /// object-element subscripts already yield raw pointers; member accesses,
  /// user calls, ternaries and array literals yield a box that is unwrapped
  /// here.  This is the single point that turns "any ref expression" into the
  /// raw receiver a runtime call (PaykanArray_get, PaykanString_char_at, …)
  /// expects, so no call site has to re-implement the box/raw distinction.
  llvm::Value *emitUnwrappedRef(ast::Expr *expr,
                                llvm::StringRef name = "ref.obj");

  // -- Unified ownership classification / cleanup (#32) ----------------------
  //
  // classifyExpr() pairs the just-emitted llvm::Value* for `expr` with its
  // ownership bit (Borrowed vs Owned), derived once from the same predicates
  // the scattered call sites used to consult (exprAlreadyShared /
  // exprProducesFreshBox / OwnedStringTemps).  releaseIfOwned() acts on that
  // classification, emitting the teardown that matches the underlying value: a
  // tracked raw PaykanString* temp is freed with PaykanString_destroy, while a
  // fresh PaykanShared* box is freed with Paykan_release; Borrowed values are
  // left untouched.  Together they let a consumer manage an arbitrary
  // expression result without re-inspecting the AST.
  ExprValue classifyExpr(ast::Expr *expr, llvm::Value *val) const;
  void releaseIfOwned(const ExprValue &ev);

  // -- Transient string-temporary tracking ----------------------------------
  //
  // A raw PaykanString* produced by a string literal, a Str-returning builtin
  // (StrInt/StrFloat/StrBool), or string concatenation is heap-allocated and
  // owned by the code that produced it.  When such a value is consumed without
  // being boxed in a PaykanShared (e.g. passed directly to println, used as a
  // concat operand, or used as the receiver of a method call) nothing would
  // otherwise free it.  These helpers track those temporaries so the consuming
  // site can destroy them in the same basic block where they were produced
  // (keeping SSA dominance valid).  Boxing a temporary via emitSharedNew
  // transfers ownership to the box and untracks it.
  llvm::SmallPtrSet<llvm::Value *, 16> OwnedStringTemps;
  void trackStringTemp(llvm::Value *v);
  void untrackStringTemp(llvm::Value *v);
  /// If `v` is a tracked owned string temporary, emit PaykanString_destroy(v)
  /// at the current insertion point and stop tracking it.
  void destroyStringTempIfOwned(llvm::Value *v);

  /// Handle the first-assignment (implicit declaration) path of
  /// visitAssignStmt: allocates an alloca, optionally wraps the value in a
  /// PaykanShared box, and registers the variable in the current scope.
  llvm::Value *emitImplicitVarDecl(llvm::StringRef name, ast::Expr *rhsExpr,
                                   llvm::Value *val);

  /// Rebind an existing class-type variable to a new shared box:
  /// retains the RHS box (if it's an owned var) or wraps a fresh value,
  /// releases the old box, and stores the new one into the alloca.
  void emitClassVarRebind(llvm::AllocaInst *alloca, ast::Expr *rhsExpr,
                          llvm::Value *val);

  // -- Expression emitter ---------------------------------------------------

  class ExprEmitter : public ast::ExprVisitor<ExprEmitter, llvm::Value *> {
  public:
    CodeGen &CG;

    llvm::Value *emitIdentityCtor(ast::CallExpr *node);
    llvm::Value *emitBuiltinCall(ast::CallExpr *node);
    /// Emit a primitive (non-object) array literal, returning the raw
    /// PaykanArray*. Tries the constant-interning fast path first; falls back
    /// to dynamic per-element stores.
    llvm::Value *emitPrimitiveArrayLiteral(ast::ArrayLiteralExpr *node,
                                           size_t len, llvm::Value *lenVal,
                                           llvm::FunctionType *newFnTy);
    /// Emit a direct call to PaykanArray_push / PaykanArray_push_obj.
    /// @p recv   raw PaykanArray*; @p elemTy  Paykan element type.
    llvm::Value *emitArrayPush(ast::MethodCallExpr *node, llvm::Value *recv,
                               ast::Type *elemTy);
    /// Emit a direct call to PaykanArray_pop / PaykanArray_pop_obj.
    /// Returns the popped element value (cast to the appropriate LLVM type).
    llvm::Value *emitArrayPop(llvm::Value *recv, ast::Type *elemTy);

    explicit ExprEmitter(CodeGen &cg) : CG(cg) {}

#define EXPR_EMIT(Kind, Name, Cast) llvm::Value *visit##Name(ast::Cast *node);
    PAYKAN_EXPR_NODES(EXPR_EMIT)
#undef EXPR_EMIT
  };

  ExprEmitter Emitter{*this};

  /// Emit an expression, returning its llvm::Value*.
  llvm::Value *emitExpr(ast::Expr *expr);

public:
  CodeGen(const sema::SemaContext &semaCtx, llvm::LLVMContext &llvmCtx,
          llvm::StringRef moduleName, const std::string &projectRoot = "",
          llvm::StringMap<llvm::Module *> *importRegistry = nullptr);

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

#define CG_VISIT(Kind, Name, Cast) llvm::Value *visit##Name(ast::Cast *node);
  PAYKAN_STMT_NODES(CG_VISIT)
  PAYKAN_DECL_NODES(CG_VISIT)
  PAYKAN_TOPLEVEL_NODES(CG_VISIT)
#undef CG_VISIT
};

} // namespace codegen
} // namespace paykan
