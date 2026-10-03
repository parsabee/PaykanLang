// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Semantic analysis pass for the Paykan language

#pragma once

#include "ASTContext.h"
#include "ASTVisitor.h"
#include "DiagEngine.h"

#include "StringMap.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
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
  StringMap<std::shared_ptr<SemaContext>> ImportedContexts;

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
//   DiagEngine diag(std::cerr);
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
    StringMap<ast::Type *> Locals;
    /// Names moved-out of this scope via `mov`.  A moved name may not be read
    /// again until it is re-assigned (which revives it).
    StringSet Moved;

    explicit Scope(Scope *parent = nullptr);

    /// Look up a name, walking the scope chain.
    ast::Type *lookup(std::string_view name) const;

    /// Declare a name in *this* scope (does not check parent scopes).
    /// Returns false if the name already exists in this scope.
    bool declare(std::string_view name, ast::Type *ty);

    /// Insert or update a binding in this scope.
    void set(std::string_view name, ast::Type *ty);

    /// Returns true if the name exists in *this* scope (not parents).
    bool contains(std::string_view name) const;

    /// Find the innermost scope that contains this name, or nullptr.
    Scope *findOwner(std::string_view name);

    /// Move tracking (operate on the owning scope, walking the chain).
    /// markMoved / clearMoved are no-ops if the name is unknown.
    void markMoved(std::string_view name);
    void clearMoved(std::string_view name);
    bool isMoved(std::string_view name) const;
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
  using MovedState = std::vector<std::pair<Scope *, StringSet>>;

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
    /// True for entries not defined by this module: the compiler builtins
    /// registered in run() (print, Str<int>, open, ...) and the qualified
    /// `mod::fn` entries injected by imports.  Such entries are never
    /// re-exported, and a user declaration may never replace one.  Because a
    /// user-declared name never contains the module qualifier, a builtin entry
    /// found under a plain declared name is always a compiler builtin.
    bool IsBuiltin = false;
  };

  /// Maps function names to their signatures.
  StringMap<FunctionSig> FunctionTable;

  /// Register a function signature.
  void declareFunction(std::string_view name, ast::Type *retTy,
                       std::vector<ast::Type *> paramTys,
                       bool isBuiltin = false);

  /// Look up a function signature, or nullptr if unknown.
  const FunctionSig *lookupFunction(std::string_view name) const;

  /// The kind of top-level entity being declared, for checkDeclNameAvailable.
  enum class DeclKind { Function, Class, Enum };

  /// Top-level declarations — functions, classes (whose constructors are
  /// called by class name), and enums — share one namespace with the compiler
  /// builtins: a name identifies exactly one entity.  Returns true if @p name
  /// is free; otherwise emits a diagnostic at @p loc naming the existing
  /// entity (builtin function / builtin class / class / enum / function) and
  /// returns false.
  bool checkDeclNameAvailable(const std::string &name, ast::SourceLocation loc,
                              DeclKind kind);

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

  // Type equality.  Resolved types are canonical (including ArrayTypes, which
  // ASTContext interns per element type), so this is pointer identity with a
  // structural fallback for any unresolved parser ArrayType node.
  static bool typesEqual(ast::Type *a, ast::Type *b);

  // Returns true if a value of type `src` can be assigned to a location of
  // type `dst`.  This includes exact match, int->float promotion, ClassType
  // subtyping, and the optional-type rules (`T` -> `T?`, `S?` -> `T?` when
  // `S` -> `T`, `T?` -> `Obj`; never `T?` -> `T`).
  bool isAssignable(ast::Type *dst, ast::Type *src) const;

  // Expression-level assignability: isAssignable(dst, srcTy) plus the two
  // optional-type rules that depend on the expression itself, both of which
  // are RECORDED on the AST for CodeGen:
  //   * the `None` literal (statically `Obj`) is assignable to every `T?`;
  //     its resolved type is rewritten to that `T?` so CodeGen emits a null
  //     box instead of the boxed `None` singleton;
  //   * a `T?` value flowing into an `Obj` slot is marked (Expr::CoercedType)
  //     so CodeGen materialises the `None` singleton for a null box;
  //   * a primitive flowing into an optional primitive slot (`int` ->
  //     `int?`) is marked with the optional so the lowering boxes it.
  bool checkAssignable(ast::Type *dst, ast::Type *srcTy, ast::Expr *src);

  // Record the destination's array type on an empty array literal `[]` (and
  // on empty literals nested in a non-empty one) so CodeGen can choose an
  // object-element array where the elements need releasing.  Called from
  // checkAssignable, so every typed sink -- declaration, assignment, field
  // store, call/method/push argument, return, subscript store -- is covered.
  void adoptArrayLiteralType(ast::Type *dst, ast::Expr *src);

  // An array or tuple literal flowing into a slot whose element type is an
  // optional primitive (`[1, 2]` into `int?[]`, `(1, "a")` into
  // `(int?, Str)`): retype the literal to hold the optional at those
  // positions and mark each such element for boxing (Expr::CoercedType).
  // Returns the literal's (possibly new) type; @p srcTy otherwise.
  ast::Type *adoptBoxedLiteralElements(ast::Type *dst, ast::Type *srcTy,
                                       ast::Expr *src);

  // If `srcTy` is an optional and `dst` is not (the `T?` -> `T` narrowing
  // that needs a `match`), emit the "cannot use optional ... without
  // unwrapping" error at @p loc and return true; otherwise return false so the
  // caller emits its own generic mismatch diagnostic.
  bool diagnoseOptionalNarrowing(ast::SourceLocation loc, ast::Type *dst,
                                 ast::Type *srcTy);

  // Emit the unwrap diagnostic for an operation that is not defined on an
  // optional value (member access, method call, subscript, arithmetic, …).
  void errorOptionalUnwrap(ast::SourceLocation loc, ast::OptionalType *optTy);

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
  ast::Type *checkIdentLive(std::string_view name, ast::SourceLocation loc);

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

    /// Dispatch to the visitXxx overloads and record the resulting type on
    /// the node (Expr::ResolvedType).  Every expression therefore carries its
    /// static type after Sema — CodeGen relies on this for identifiers and
    /// literals too, e.g. to tell a `T?`-typed operand from a `T` one.
    ast::Type *visit(ast::Expr *e) {
      ast::Type *ty = ast::ExprVisitor<ExprChecker, ast::Type *>::visit(e);
      if (ty)
        e->setResolvedType(ty);
      return ty;
    }

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

  // Optional-mode match checking: the subject is `T?`.  A type arm naming `T`
  // itself matches every non-None value (and binds it as `T`); a type arm
  // naming a strict subclass of `T` matches on exact runtime type as in class
  // mode; a `None` literal arm or `_` covers the absent case.
  bool checkOptionalMatch(ast::MatchStmt *node, ast::OptionalType *subjectTy);

  // -- Import resolution ----------------------------------------------------

  /// The directory of the file currently being analyzed.
  std::string ProjectRoot;

  /// The frontend imported modules are parsed with ("" = the default).
  std::string FrontendName;

  /// Files currently being imported (for cycle detection).
  StringSet *ImportStack = nullptr;

  /// Accumulated SemaContexts for each directly-imported module, keyed by
  /// resolved path. Built by processImport; moved into the SemaContext
  /// returned by run().
  StringMap<std::shared_ptr<SemaContext>> AccumulatedImportContexts;

  /// Defining module (resolved file path) of every class/enum this Sema has
  /// reconstructed from an import, keyed by canonical type name.  Type names
  /// are global across the import graph, so reconstructing a type whose name
  /// is already bound to a type from a different module is an error rather
  /// than a silent merge of two unrelated types.
  StringMap<std::string> ImportedTypeOrigins;

  /// Every module qualifier this file's imports bind (each import's alias or
  /// last path segment, and its full module path) -> the module it names.
  /// One qualifier cannot name two modules: `util::f` would be ambiguous.
  struct ImportQualifier {
    std::string Resolved;   ///< resolved file path
    std::string ModulePath; ///< as written, e.g. `a::util`
  };
  StringMap<ImportQualifier> ImportQualifiers;

public:
  /// Info about an already-analyzed module.  Public (with ModuleCache) so
  /// tests can seed a cache entry and exercise the error paths of export
  /// reconstruction; production code only touches it in SemaImport.cpp.
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
      /// True when the class is declared in the module itself.  False for a
      /// class the module merely reached through its own imports: such a
      /// class is reconstructed (so the module's signatures that mention it
      /// resolve, with type identity preserved) but is NOT given the
      /// importer's qualifier — names are never re-exported transitively.
      bool IsLocal = true;
      /// Resolved path of the module that declares the class ("" for
      /// compiler builtins).  Used to detect two modules exporting different
      /// classes under one name.
      std::string OriginPath;
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
      bool IsLocal = true;    // see ClassInfo::IsLocal
      std::string OriginPath; // see ClassInfo::OriginPath
    };
    std::vector<EnumInfo> ExportedEnums;
  };

  /// Global cache of already-analyzed modules (keyed by resolved file path).
  static StringMap<ModuleInfo> ModuleCache;

private:
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

  /// Phase 4 for one class: resolve field types and method signatures into
  /// the pre-registered ClassType (superclass must already be populated).
  bool populateClassType(ast::ClassDecl *cd, ast::ClassType *ct);

  /// Phase 4b for one class: register the constructor function `Name(...)`.
  void declareConstructor(ast::ClassDecl *cd, ast::ClassType *ct);

  // -- Generics (prototype) --------------------------------------------------
  //
  // Generic declarations are templates: they are registered by name here and
  // never type-checked as such.  Every use with a distinct tuple of canonical
  // type arguments instantiates the template once (cached by the canonical
  // name, e.g. `Box<int>`, `Pair<Str, int>`) into an ordinary ClassDecl /
  // FuncDecl clone with the type parameters substituted (ast::ASTCloner),
  // which is then registered, checked, and emitted like hand-written code.
  // Bodies of instantiations are checked from a worklist after all
  // hand-written bodies (they may trigger further instantiations).

  /// Class and function templates declared by this module, by name.
  StringMap<ast::ClassDecl *> ClassTemplates;
  StringMap<ast::FuncDecl *> FuncTemplates;

  /// Instantiation caches keyed by canonical instantiation name.  A null
  /// ClassType* / false records an instantiation that failed, so a repeated
  /// use neither re-instantiates nor re-reports.
  StringMap<ast::ClassType *> ClassInstantiations;
  StringMap<bool> FuncInstantiations;

  /// What an instantiated ClassType was made from (for inference through
  /// `Box<T>` parameters and for diagnostics).
  struct InstantiationInfo {
    std::string TemplateName;
    std::vector<ast::Type *> Args;
  };
  std::unordered_map<ast::ClassType *, InstantiationInfo>
      ClassInstantiationInfo;

  /// Instantiations whose bodies still have to be checked.
  struct PendingInstantiation {
    ast::ClassDecl *Class = nullptr; // exactly one of Class / Func is set
    ast::FuncDecl *Func = nullptr;
    std::string Name;               // canonical instantiation name
    ast::SourceLocation RequestLoc; // where the instantiation was requested
    StringSet TypeParams;           // the template's type parameter names
  };
  std::vector<PendingInstantiation> PendingInstantiations;

  /// Instantiations in creation order; injected into the TranslationUnit at
  /// the end of run() so CodeGen emits them.
  std::vector<ast::ClassDecl *> InstantiatedClassDecls;
  std::vector<ast::FuncDecl *> InstantiatedFuncDecls;

  /// Active instantiation contexts, outermost first.  Every error reported
  /// while this is non-empty is followed by a note per frame ("in
  /// instantiation of 'Box<int>' requested here").
  struct InstantiationFrame {
    std::string Name;
    ast::SourceLocation RequestLoc;
  };
  std::vector<InstantiationFrame> InstantiationStack;
  static constexpr size_t kMaxInstantiationDepth = 16;

  /// Type parameter names of the instantiation whose body is being checked
  /// (nullptr outside one).  Used to diagnose a type parameter used as a value.
  const StringSet *CurrentTypeParams = nullptr;

  /// Classes constructed inside each class's method bodies (by name).  Used to
  /// order instantiated classes for CodeGen: a constructor must be emitted
  /// before a method that calls it.
  StringMap<StringSet> ConstructsEdges;

  /// Register every generic declaration of the module as a template, checking
  /// its name and type parameter list.
  bool registerGenericTemplates(ast::TranslationUnit *tu);

  /// Canonical name of an instantiation: `Name<arg1, arg2>`.
  static std::string instantiationName(const std::string &templateName,
                                       const std::vector<ast::Type *> &args);

  /// Instantiate class template @p name with canonical @p args, or return the
  /// cached instantiation.  Registers the class (stub, fields, methods,
  /// constructor) immediately and queues its bodies.  Returns nullptr after
  /// reporting an error.
  ast::ClassType *instantiateClass(const std::string &name,
                                   const std::vector<ast::Type *> &args,
                                   ast::SourceLocation loc);

  /// Instantiate function template @p name with canonical @p args, or return
  /// the cached instantiation.  Returns the instantiation's function name, or
  /// "" after reporting an error.
  std::string instantiateFunction(const std::string &name,
                                  const std::vector<ast::Type *> &args,
                                  ast::SourceLocation loc);

  /// Names of the module's own concrete classes (set by
  /// registerGenericTemplates) and of every class whose fields and method
  /// signatures have been populated so far.  A generic class whose superclass
  /// is a local class may only be instantiated once that superclass is
  /// populated (its vtable prefix must be complete).
  StringSet LocalClassNames;
  StringSet PopulatedClasses;

  /// A failed unification: type parameter @p Param was deduced as both
  /// @p First and @p Second.
  struct InferenceConflict {
    std::string Param;
    ast::Type *First = nullptr;
    ast::Type *Second = nullptr;
  };

  /// Structural unification of a template parameter type @p pattern (which
  /// may mention type parameters as ClassType stubs) against a canonical
  /// argument type @p actual, extending @p bindings.  Returns false, filling
  /// @p conflict, on a conflicting binding (reported by the caller).  Shapes
  /// that do not match (e.g. an array pattern against a class argument) are
  /// not an inference failure: they are left to the ordinary argument check.
  bool unifyTypes(ast::Type *pattern, ast::Type *actual,
                  const StringSet &typeParams, StringMap<ast::Type *> &bindings,
                  InferenceConflict &conflict);

  /// Infer the type arguments of a generic call from its argument types.
  /// @p paramTypes are the template's declared parameter types; on success
  /// @p out holds one canonical type per type parameter, in order.
  bool inferTypeArgs(const std::string &templateName,
                     const std::vector<const std::string *> &typeParams,
                     const std::vector<ast::Type *> &paramTypes,
                     const std::vector<ast::Type *> &argTypes,
                     ast::SourceLocation loc, std::vector<ast::Type *> &out);

  /// Resolve a call whose callee is a template (or carries explicit type
  /// arguments): instantiate and rewrite the callee name.  Returns false after
  /// reporting an error.
  bool resolveGenericCall(ast::CallExpr *node,
                          const std::vector<ast::Type *> &argTypes);

  /// True when @p name is a conversion target (`Str`, `int`, `float`,
  /// `bool`, `char`), i.e. `name<Source>(value)` is a conversion, or the
  /// spelled form (`int<float>`) checkConversion rebinds such a call to.
  static bool isConversionTarget(std::string_view name);

  /// Check a conversion constructor `Target<Source>(value)` (#64): the pair
  /// must be one of the supported conversions and the argument must have
  /// exactly the Source type.  Rebinds the callee to the spelled conversion
  /// (`names::kConvIntFloat`, ...) for the lowering and returns the result
  /// type, or nullptr after reporting an error.
  ast::Type *checkConversion(ast::CallExpr *node,
                             const std::vector<ast::Type *> &argTypes);

  /// Check the bodies of every pending instantiation (transitively).
  bool checkPendingInstantiations();

  /// Append the instantiated declarations to the TranslationUnit.
  void injectInstantiations(ast::TranslationUnit *tu);

public:
  /// @p frontendName names the frontend used to parse imported modules; ""
  /// selects the build's default.  The driver passes the frontend it parsed
  /// the main file with, so one program is parsed by one frontend.
  explicit Sema(ast::ASTContext &ctx, DiagEngine &diags,
                const std::string &projectRoot = "",
                const std::string &frontendName = "");

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
