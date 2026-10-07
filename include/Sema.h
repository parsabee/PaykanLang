// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Semantic analysis pass for the Paykan language

#pragma once

#include "ASTContext.h"
#include "ASTVisitor.h"
#include "DiagEngine.h"

#include "StringMap.h"
#include "paykan/pkm/Interface.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace paykan {
namespace parser {
class ParserDriver;
}
namespace modules {
class ModuleResolver;
}
namespace sema {

/// One specialization of a builtin conversion target (Sema.cpp).
struct ConversionPair;

/// The display name of a type in diagnostics (ASTContext.cpp).
using ast::typeName;

/// Result object returned by Sema::run(). Carries the populated ASTContext
/// and (for imported modules) the owning ParserDriver plus pre-computed child
/// SemaContexts so the lowering can consume them without re-running Sema.
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
  /// True when the module failed (also) because it imports a module whose
  /// failure was already reported elsewhere in this compilation (#132): such
  /// a module can fail with no error of its own.
  bool ImportsFailedModule = false;
  std::vector<Diagnostic> Diagnostics;
  /// Pre-computed SemaContexts for the modules this Sema built from source,
  /// keyed by canonical module name (ModuleName.h). Populated by Sema::run()
  /// so the lowering can reuse them without re-running the Sema pass.
  StringMap<std::shared_ptr<SemaContext>> ImportedContexts;

  explicit operator bool() const { return Ok; }
};

// Semantic analysis: one walk over a parsed translation unit that resolves
// and checks every name, type, call, class and import, instantiates the
// generics it uses, and records on the AST (resolved types, rewritten callee
// names, canonical annotations) what the lowering needs.  Statement and
// declaration visitors return true on success; every error goes through
// the DiagEngine.
//
//   DiagEngine diag(std::cerr);
//   diag.setSourceInfo("foo.pkn", &lines);
//   Sema S(ctx, diag);
//   bool ok = S.run(translationUnit);
class Sema : public ast::ASTVisitor<Sema, bool> {
  DiagEngine &Diags;
  ast::ASTContext &Ctx;

  // -- Scoped symbol table

  /// A single lexical scope. Each scope has its own local bindings and a
  /// pointer to its enclosing (parent) scope.
  struct Scope {
    Scope *Parent = nullptr;
    StringMap<ast::Type *> Locals;

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
  };

  Scope *CurrentScope = nullptr;

  /// The expected return type of the current function (nullptr = top-level /
  /// void).
  ast::Type *CurrentReturnType = nullptr;

  /// Loop nesting depth (0 = not inside a loop).
  unsigned LoopDepth = 0;

  // -- Class analysis context
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

  // -- Function signature table

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

  /// RAII helper to push/pop a scope.  The scope lives on the heap so that
  /// CurrentScope never points into a stack frame (GCC's -Wdangling-pointer
  /// cannot see that the destructor restores CurrentScope).
  struct ScopeGuard {
    Sema &S;
    std::unique_ptr<Scope> ScopeObj;
    ScopeGuard(Sema &s);
    ~ScopeGuard();
  };

  // -- Internal helpers

  void error(ast::SourceLocation loc, const std::string &msg);
  void warning(ast::SourceLocation loc, const std::string &msg);
  void note(ast::SourceLocation loc, const std::string &msg);

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
  // are RECORDED on the AST for the lowering:
  //   * the `None` literal (statically `Obj`) is assignable to every `T?`;
  //     its resolved type is rewritten to that `T?` so the lowering emits a
  //     null box instead of the boxed `None` singleton;
  //   * a `T?` value flowing into an `Obj` slot is marked (Expr::CoercedType)
  //     so the lowering materialises the `None` singleton for a null box;
  //   * a primitive flowing into an optional primitive slot (`int` ->
  //     `int?`) is marked with the optional so the lowering boxes it.
  bool checkAssignable(ast::Type *dst, ast::Type *srcTy, ast::Expr *src);

  // Record the destination's array type on an empty array literal `[]` (and
  // on empty literals nested in a non-empty one) so the lowering can choose an
  // object-element array where the elements need releasing.  Called from
  // checkAssignable, so every typed sink -- declaration, assignment, field
  // store, call/method/push argument, return, subscript store -- is covered.
  void adoptArrayLiteralType(ast::Type *dst, ast::Expr *src);

  // An array or tuple literal flowing into a slot whose element types it
  // must take: `[1, 2]` into `int?[]` and `(1, "a")` into `(int?, Str)` box
  // their primitives (Expr::CoercedType), a `None` element takes the slot's
  // `T?` (`(None, 1)` into `(Node?, int)`), and nested literals are adopted
  // recursively (`[("a", 1), ("b", n)]` into `(Str, int?)[]`).  The literal
  // is retyped to hold the destination's types at those positions.  Returns
  // the literal's (possibly new) type; @p srcTy otherwise.
  ast::Type *adoptLiteralElements(ast::Type *dst, ast::Type *srcTy,
                                  ast::Expr *src);

  // If `srcTy` is an optional and `dst` is not (the `T?` -> `T` narrowing
  // that needs a `match`), emit the "cannot use optional ... without
  // unwrapping" error at @p loc and return true; otherwise return false so the
  // caller emits its own generic mismatch diagnostic.
  bool diagnoseOptionalNarrowing(ast::SourceLocation loc, ast::Type *dst,
                                 ast::Type *srcTy);

  // If @p srcTy is an empty array literal's type (`[]`, no element type of
  // its own) and @p dst (nullptr: none) cannot supply one -- it is not an
  // array or an optional array -- report it at @p loc and return true.
  bool diagnoseEmptyArrayLiteral(ast::SourceLocation loc, ast::Type *dst,
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

  // -- Poisoned binders (#89)
  //
  // A binder whose declaration failed (a bad initializer, an unresolvable type
  // annotation, a failed destructuring, a match arm whose type is in error) is
  // still bound -- to ASTContext::getPoisonTy() -- so that its uses are not
  // reported as "use of undeclared variable".  A use of a poisoned name yields
  // no type and counts a suppressed follow-on (see SuppressedFollowOns), so
  // every enclosing check stays quiet: only the original error is reported.
  // A later valid assignment re-binds the name with the value's type.

  /// Bind @p name in @p scope to the poison type (replacing any binding there).
  /// Only called after the failure was reported.
  void declarePoisoned(Scope *scope, std::string_view name);

  /// Number of poisoned binders; a successful run must have none (run()
  /// guards that no poison type escapes Sema).
  unsigned PoisonedBindings = 0;

  /// True if @p name is a type name or the reserved `Stdin`, which no
  /// variable may be bound to.
  bool isTypeNameForVariable(const std::string &name) const;

  /// The guard every binder applies to the name it binds (#126): an inferred
  /// or typed declaration, a destructuring target, a function or method
  /// parameter, a match binding.  Reports, at @p loc, a name that cannot
  /// be a variable (see isTypeNameForVariable) and returns false.
  bool checkBinderName(const std::string &name, ast::SourceLocation loc);

  /// checkBinderName for a match arm's binding; a rejected binding is bound
  /// poisoned in the arm's scope.
  bool bindArmName(ast::MatchArm *arm);

  // Check that a variable is declared. Returns its type,
  // or nullptr (with error emitted) on failure.  A poisoned variable yields
  // nullptr without an error (a suppressed follow-on).
  ast::Type *checkIdentLive(std::string_view name, ast::SourceLocation loc);

  // -- Expression type-checker
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
    /// static type after Sema; the lowering relies on this for identifiers and
    /// literals too, e.g. to tell a `T?`-typed operand from a `T` one.
    ast::Type *visit(ast::Expr *e) {
      ast::Type *ty = ast::ExprVisitor<ExprChecker, ast::Type *>::visit(e);
      if (ty)
        e->setResolvedType(ty);
      return ty;
    }

    /// visit(@p e) where the value flows into a slot of type @p expected
    /// (nullptr: unknown).  Only an array or tuple literal uses it: its
    /// elements are visited against the slot's element types, and an array
    /// literal whose elements disagree (`[("a", 1), ("b", n)]` with `n:
    /// int?`) takes the slot's element type when every element fits it.
    ast::Type *visitExpecting(ast::Expr *e, ast::Type *expected) {
      // `mov` of a literal forwards it: the literal still sees the slot.
      ast::Expr *lit = e;
      if (auto *mv = ast::dyn_cast<ast::MovExpr>(e))
        lit = mv->getOperand();
      if (expected && (ast::isa<ast::ArrayLiteralExpr>(lit) ||
                       ast::isa<ast::TupleLiteralExpr>(lit)))
        S.LiteralExpectation = expected;
      return visit(e);
    }

#define EXPR_VISIT(Kind, Name, Cast) ast::Type *visit##Name(ast::Cast *node);
    PAYKAN_EXPR_NODES(EXPR_VISIT)
#undef EXPR_VISIT
  };

  ExprChecker EC{*this};

  // Visit an expression and return its resolved type (nullptr on error).
  ast::Type *resolveExprType(ast::Expr *expr);
  // The same for a value flowing into a slot of type @p expected (see
  // ExprChecker::visitExpecting).
  ast::Type *resolveExprType(ast::Expr *expr, ast::Type *expected);

  /// The slot type the array / tuple literal about to be visited flows into
  /// (set by visitExpecting, taken by the literal's visitor).
  ast::Type *LiteralExpectation = nullptr;

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

  // -- Import resolution

  /// The directory of the file currently being analyzed.
  std::string ProjectRoot;

  /// The frontend imported modules are parsed with ("" = the default).
  std::string FrontendName;

  /// Canonical names of the modules currently being imported (for cycle
  /// detection).
  StringSet *ImportStack = nullptr;

  /// Canonical names (ModuleName.h) of the modules that failed to load in
  /// this compilation -- not found, not a source file, or with errors of
  /// their own.  Shared, like ImportStack, by every Sema of one compilation,
  /// so each failure is reported once however many import paths reach the
  /// module; a later import of it adds only a note (#132).
  StringSet *FailedModules = nullptr;

  /// Imports of modules in FailedModules: they fail this module without an
  /// error of its own (see SemaContext::ImportsFailedModule).
  unsigned RepeatedImportFailures = 0;

  /// Accumulated SemaContexts for each module built from source by this
  /// Sema, keyed by canonical name. Built by processImport; moved into the
  /// SemaContext returned by run().
  StringMap<std::shared_ptr<SemaContext>> AccumulatedImportContexts;

  /// Defining module (canonical name, ModuleName.h) of every class/enum this
  /// Sema has reconstructed from an import, keyed by canonical type name.
  /// Type names are global across the import graph, so reconstructing a type
  /// whose name is already bound to a type from a different module is an
  /// error rather than a silent merge of two unrelated types.
  StringMap<std::string> ImportedTypeOrigins;

  /// Every module qualifier this file's imports bind (each import's alias or
  /// last path segment, and its full module path) -> the module it names.
  /// One qualifier cannot name two modules: `util::f` would be ambiguous.
  struct ImportQualifier {
    std::string Module;     ///< canonical module name
    std::string ModulePath; ///< as written, e.g. `a::util`
  };
  StringMap<ImportQualifier> ImportQualifiers;

  /// Qualifiers (alias or last path segment, and the full module path) of the
  /// imports that failed -- a module that was not found, did not parse, had
  /// errors of its own or closes an import cycle.  The failure was reported
  /// once, at the import; a qualified name under one of these qualifiers
  /// (`m::f`, `m::T`) that does not resolve is a poisoned use (#119): it is
  /// absorbed silently as a suppressed follow-on (see SuppressedFollowOns).
  StringSet FailedImportQualifiers;

  /// True (counting a suppressed follow-on) if @p name is a qualified name
  /// under a failed import's qualifier: its use must not be reported.
  bool isFailedImportUse(std::string_view name);

  /// Errors reported inside imported modules, through their own DiagEngine
  /// (with their own file name and source lines) and so not counted by Diags.
  /// They fail this module too: run() adds them to its error count.
  unsigned ImportedModuleErrors = 0;

  /// @p path for a diagnostic: relative to ProjectRoot (the source root) when
  /// it lies under it, as given otherwise.
  std::string displayPath(const std::filesystem::path &path) const;

  /// Where imports come from when the driver set one (setResolver): source
  /// files, cache entries or prebuilt `.pkm` files.  Without it every import
  /// is parsed from its source file.
  modules::ModuleResolver *Resolver = nullptr;

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
      bool operator==(const FunctionInfo &) const = default;
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
      /// Canonical name (ModuleName.h) of the module that declares the
      /// class ("" for compiler builtins): the module's identity, used to
      /// detect two modules exporting different classes under one name, and
      /// what diagnostics show.
      std::string OriginModule;
      struct FieldInfo {
        std::string FieldName;
        std::string TypeName;
        bool operator==(const FieldInfo &) const = default;
      };
      struct MethodInfo {
        std::string Name;
        std::string ReturnTypeName;
        std::vector<std::string> ParamTypeNames;
        uint8_t Flags = 0; // ast::MethodDecl::Private bit
        bool operator==(const MethodInfo &) const = default;
      };
      std::vector<FieldInfo> Fields;
      std::vector<MethodInfo> Methods;
      bool operator==(const ClassInfo &) const = default;
    };
    std::vector<ClassInfo> ExportedClasses;

    // Serialised enum-type descriptions: the enum name plus its variant names
    // in declaration order (the index is the variant's underlying value).
    struct EnumInfo {
      std::string Name;
      std::vector<std::string> Variants;
      bool IsLocal = true;      // see ClassInfo::IsLocal
      std::string OriginModule; // see ClassInfo::OriginModule
      bool operator==(const EnumInfo &) const = default;
    };
    std::vector<EnumInfo> ExportedEnums;
    bool operator==(const ModuleInfo &) const = default;
  };

  /// Global cache of already-analyzed modules, keyed by the resolved path
  /// of the file they came from (a source, or a .pkm).
  static StringMap<ModuleInfo> ModuleCache;

  /// The exports of the module this Sema analysed (after run()): what an
  /// importer sees, in the module's own terms.  @p moduleName is its
  /// canonical name.
  ModuleInfo exportModuleInfo(ast::TranslationUnit *tu,
                              const std::string &moduleName) const;

  /// ModuleInfo as the IFACE records of a .pkm (pkm::Interface), and back.
  /// The two are the same declarations field for field; the Interface's
  /// module name, dependencies and display file are the resolver's to fill.
  /// fromInterface orders the declarations as exportModuleInfo does, so a
  /// module loaded from a file is injected exactly like one built in
  /// process.
  static pkm::Interface toInterface(const ModuleInfo &info);
  static ModuleInfo fromInterface(const pkm::Interface &iface);

  /// Set the resolver imports go through (the driver's).  Child Semas
  /// inherit it.
  void setResolver(modules::ModuleResolver *resolver) { Resolver = resolver; }

private:
  /// Resolve a module path to an absolute file path.
  std::string resolveModulePath(const std::string &modulePath, bool isSystem,
                                ast::SourceLocation loc);
  /// The resolved path of module @p module's file @p file ("" with an error,
  /// which shows the file as @p shown, if it does not exist or is not a
  /// regular file).  @p kind is "module" or "system module".
  std::string checkModuleFile(const std::filesystem::path &file,
                              const std::string &shown,
                              const std::string &module, const char *kind,
                              ast::SourceLocation loc);

  /// Where a module was found: its source file, or the interface loaded
  /// from a `.pkm` by the resolver.
  struct LocatedModule {
    std::string Path;                      ///< source file, or the .pkm
    const pkm::Interface *Iface = nullptr; ///< set when loaded from a .pkm
  };
  /// Locate module @p canonical (an import of @p modulePath): through the
  /// resolver when there is one (resolving the dependencies a cache entry
  /// needs first), from the source tree otherwise.  Reports a module that
  /// cannot be found or used and returns false.
  bool locateModule(const std::string &canonical, bool isSystem,
                    const std::string &modulePath, ast::SourceLocation loc,
                    LocatedModule &out);
  /// The exports of module @p canonical: from ModuleCache, from the
  /// interface @p located holds, or by parsing and analysing its source.
  /// Returns nullptr after reporting the failure.
  const ModuleInfo *loadModule(const std::string &canonical,
                               const LocatedModule &located,
                               ast::SourceLocation loc);
  /// Make a module's exports visible under @p qualifier (and
  /// @p fullModulePath): reconstruct its types in Ctx and declare its
  /// functions.  Returns false after reporting a failure.
  bool injectModule(const ModuleInfo &info, const std::string &qualifier,
                    const std::string &fullModulePath,
                    const std::string &moduleName, ast::SourceLocation loc);

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

  /// Error recovery for rejected class declarations.  A class whose name is
  /// taken (by a builtin, an import, an enum, or an earlier class) is dropped,
  /// and its name recorded in ErroneousNames.  A class whose superclass or
  /// members could not be resolved is still registered (on Obj, if its
  /// superclass is unusable), so the rest of the module can name it, and
  /// recorded in ErroneousClasses; its method bodies are not checked.  The
  /// declaration error is the one reported: calls by an erroneous name and
  /// member lookups on an erroneous class (or a subclass) report nothing.
  StringSet ErroneousNames;
  StringSet ErroneousClasses;
  /// Errors left unreported as follow-ons of the above, so that a check that
  /// reports only "if nothing inside was reported" stays quiet for them too.
  unsigned SuppressedFollowOns = 0;

  /// True if @p ct or one of its superclasses is in ErroneousClasses.
  bool isErroneousClass(const ast::ClassType *ct) const;

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

  // -- Generics
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
  /// the end of run() so they are lowered.
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
  /// order instantiated classes for the lowering: a constructor must be
  /// emitted before a method that calls it.
  StringMap<StringSet> ConstructsEdges;

  /// Report that qualified name @p name (`shapes::Box`, `g::first`) names a
  /// template of another module, which cannot be imported yet.  The rejection
  /// does not depend on the type arguments, but a use inside a generic body is
  /// resolved again for every instantiation (clones keep the template's source
  /// locations), so it is reported once per use: a repeat at the same location
  /// only counts a suppressed follow-on (#112).
  void errorImportedTemplate(ast::SourceLocation loc, const std::string &name,
                             const std::string &msg);
  /// The uses errorImportedTemplate has reported ("line:col:name").
  StringSet ReportedImportedTemplateUses;

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
  /// `bool`, `char`, `Int`, `Float`, `Bool`), i.e. `name<Source>(value)` is
  /// a conversion, or the spelled form (`int<float>`) checkConversion rebinds
  /// such a call to.
  static bool isConversionTarget(std::string_view name);

  /// True when @p node is a call of the `Str` constructor rather than a
  /// conversion: `Str(s)` with a `Str` (or `Str?`) argument, or a `Str(...)`
  /// call whose argument count or argument is already wrong.
  bool isStrConstruction(ast::CallExpr *node,
                         const std::vector<ast::Type *> &argTypes) const;

  /// Check a conversion constructor without its type argument, `Target(value)`
  /// (#88): it picks the specialization whose source is exactly the
  /// argument's type (`Str(n)` with an `int` is `Str<int>(n)`), or reports
  /// that there is none.  Rebinds the callee like checkConversion.
  ast::Type *inferConversion(ast::CallExpr *node,
                             const std::vector<ast::Type *> &argTypes);

  /// Rebind a checked conversion @p node to @p pair's spelled form and
  /// return its result type (the optional target for a parse).
  ast::Type *applyConversion(ast::CallExpr *node, const ConversionPair *pair);

  /// Check a conversion constructor `Target<Source>(value)` (#64, #88): the
  /// pair must be one of the target's closed set of specializations and the
  /// argument must have exactly the Source type.  Rebinds the callee to the
  /// spelled conversion (`names::kConvIntFloat`, ...) for the lowering and
  /// returns the result type, or nullptr after reporting an error.
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

  /// How run() checks the program's entry point, `main` (#132).  A module
  /// that is imported, or analysed on its own, needs none; a program that is
  /// built or run needs `fn main() -> int` or `fn main(args: Str[]) -> int`.
  enum class EntryPoint {
    None,       ///< no check (the default; imported modules)
    IfDeclared, ///< a declared `main` must have an entry-point signature
    Required,   ///< ... and the program must declare one
  };
  void setEntryPointCheck(EntryPoint check) { EntryPointCheck = check; }

private:
  EntryPoint EntryPointCheck = EntryPoint::None;
  /// The check EntryPointCheck selects, over the main file's declarations.
  bool checkEntryPoint(ast::TranslationUnit *tu,
                       const std::vector<ast::FuncDecl *> &declaredFns);

public:
  // Entry point -- run semantic analysis on a TranslationUnit.
  // Returns a SemaContext whose bool operator is true on success.
  SemaContext run(ast::TranslationUnit *tu);

  // Access diagnostics after analysis.
  const std::vector<Diagnostic> &getDiagnostics() const {
    return Diags.getDiagnostics();
  }
  unsigned getErrorCount() const {
    return Diags.getErrorCount() + ImportedModuleErrors;
  }
  bool hasErrors() const { return getErrorCount() > 0; }

  // -- Visitor overrides

#define SEMA_VISIT(Kind, Name, Cast) bool visit##Name(ast::Cast *node);
  PAYKAN_STMT_NODES(SEMA_VISIT)
  PAYKAN_DECL_NODES(SEMA_VISIT)
  PAYKAN_TOPLEVEL_NODES(SEMA_VISIT)
#undef SEMA_VISIT
};

} // namespace sema
} // namespace paykan
