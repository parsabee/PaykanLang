// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Internal declarations of the AST -> PIR lowering.  The structure mirrors
// the former LLVM CodeGen (src/CodeGen) deliberately: the ownership rules it
// encoded are ported here function by function, and the names are kept so the
// two can be compared.  See docs/pir.md §7 for the rules themselves.

#pragma once

#include "ASTContext.h"
#include "ASTVisitor.h"
#include "Sema.h"
#include "paykan/pir/Builder.h"
#include "paykan/pir/PIR.h"

#include <deque>
#include <functional>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace paykan::lowering {

using pir::Val;

/// Ownership classification of an emitted expression value (see the former
/// codegen::ExprValue): Borrowed values are left alone by the consumer, Owned
/// ones are torn down by it (a tracked raw string temporary with
/// PaykanString_destroy, a fresh +1 box with Paykan_release).
struct ExprValue {
  enum class Ownership { Borrowed, Owned };
  Val V;
  Ownership Own = Ownership::Borrowed;

  ExprValue() = default;
  ExprValue(Val v, Ownership own) : V(std::move(v)), Own(own) {}
  static ExprValue borrowed(Val v) {
    return {std::move(v), Ownership::Borrowed};
  }
  static ExprValue owned(Val v) { return {std::move(v), Ownership::Owned}; }
  bool isOwned() const { return Own == Ownership::Owned; }
};

/// Program-wide lowering state shared by every ModuleLowering: the modules
/// lowered so far (imports are lowered before their importer), where each
/// class is defined, and the SemaContext tree for import resolution.
struct ProgramLowering {
  const sema::SemaContext &Top;
  std::string ProjectRoot;
  std::ostream &Errs;
  /// Lowered modules in completion order (imports first); stable addresses.
  std::deque<pir::Module> Modules;
  /// The lowered imports by resolved file path (Sema's identity of a module)
  /// and by canonical module name (the PIR's).
  std::unordered_map<std::string, pir::Module *> ByPath;
  std::unordered_map<std::string, pir::Module *> ByName;
  /// Every module name handed out (the main module's included).
  std::unordered_set<std::string> UsedNames;
  /// Class name -> name of the module whose ClassDecl defines it.
  std::unordered_map<std::string, std::string> ClassOrigins;
  /// Every SemaContext reachable from Top, keyed by resolved file path.
  std::unordered_map<std::string, const sema::SemaContext *> Contexts;
  bool Indexed = false;

  ProgramLowering(const sema::SemaContext &top, std::string projectRoot,
                  std::ostream &errs)
      : Top(top), ProjectRoot(std::move(projectRoot)), Errs(errs) {}

  const sema::SemaContext *lookupImportContext(const std::string &resolved);
  /// Reserve @p name for a module, or `name.2`, `name.3`, ... when another
  /// module already has it.
  std::string claimName(const std::string &name);
  /// Lower the module at @p resolved (once), naming it @p name (its
  /// canonical module name), and return it.
  pir::Module *lowerImport(const std::string &resolved,
                           const std::string &name);
};

/// Lowers ONE module (translation unit).  Imported modules get their own
/// instance (see LoweringProgram.cpp); the SymbolOrigins and the set of
/// already-lowered modules are shared across the program.
class ModuleLowering : public ast::ASTVisitor<ModuleLowering, Val> {
public:
  ModuleLowering(ProgramLowering &program, const sema::SemaContext &semaCtx,
                 std::string moduleName);

  /// Lower @p tu into Module.  Returns false after an internal error.
  bool run(ast::TranslationUnit *tu);

  pir::Module takeModule();

  // -- Visitor overrides
  // --------------------------------------------------------
#define LW_VISIT(Kind, Name, Cast) Val visit##Name(ast::Cast *node);
  PAYKAN_STMT_NODES(LW_VISIT)
  PAYKAN_DECL_NODES(LW_VISIT)
  PAYKAN_TOPLEVEL_NODES(LW_VISIT)
#undef LW_VISIT

private:
  ProgramLowering &PL;
  ast::ASTContext &ASTCtx;
  const sema::SemaContext &SemaCtx;
  std::ostream &Errs;

  /// Functions reachable through this module's imports: the qualified name
  /// used at call sites (`helper::add`, `path::to::helper::add`) -> the
  /// defining module and the plain symbol there.
  struct ImportedFn {
    std::string Module;
    std::string Plain;
  };
  std::unordered_map<std::string, ImportedFn> ImportedFunctions;
  void processImports(ast::TranslationUnit *tu);

  pir::Module Mod;
  /// Functions in declaration order with stable addresses (the module vector
  /// is filled from this when the module is taken).
  std::deque<pir::Function> Funcs;
  /// Every function by its PIR name in this module (definitions, runtime
  /// and module externs).
  std::unordered_map<std::string, pir::Function *> FuncByName;
  /// Module externs by origin, "<defining module>\n<symbol there>".
  std::unordered_map<std::string, pir::Function *> ExternByOrigin;
  std::unordered_map<std::string, pir::Class *> ClassByName;
  std::deque<pir::Class> Classes_;
  std::unordered_set<std::string> ExternGlobals;

  pir::Builder B;

  // -- Scoped symbol table (name -> local slot)
  // ---------------------------------

  struct Scope {
    Scope *Parent = nullptr;
    std::unordered_map<std::string, pir::LocalId> Locals;
    std::unordered_map<std::string, ast::Type *> ASTTypeMap;
    /// Variables whose slot stores an owned PaykanShared* box.
    std::unordered_set<std::string> SharedVars;
    struct VarMeta {
      pir::LocalId Local;
      ast::Type *ASTType;
    };
    /// Owned variables declared in this scope, in declaration order.
    std::vector<VarMeta> DeclOrder;
    /// Extra owned boxes released at scope exit (e.g. a match subject).
    std::vector<Val> PendingReleases;

    explicit Scope(Scope *parent = nullptr) : Parent(parent) {}

    bool hasLocal(const std::string &name) const;
    pir::LocalId lookup(const std::string &name) const; // asserts found
    bool isOwned(const std::string &name) const;
    ast::Type *lookupASTType(const std::string &name) const;
    void updateASTType(const std::string &name, ast::Type *newTy);
    void declare(const std::string &name, pir::LocalId local, ast::Type *astTy);
    /// A borrowed, never-released raw-object binding.  Only `self` is
    /// declared this way: the caller owns the object for the whole call, and
    /// Sema rejects assignments to `self`.  Match-arm bindings are ordinary
    /// owned variables (see emitTypeArmBody in LoweringMatch.cpp).
    void declareUnowned(const std::string &name, pir::LocalId local,
                        ast::Type *astTy);
    Scope *findOwner(const std::string &name);
  };

  Scope *CurrentScope = nullptr;

  /// AST return type of the function being emitted (nullptr at top level).
  ast::Type *CurrentFuncReturnASTType = nullptr;

  /// ClassType of the method being emitted (nullptr otherwise).
  ast::ClassType *CurrentMethodClassType = nullptr;

  struct LoopContext {
    Scope *EnclosingScope;
  };
  std::vector<LoopContext> LoopStack;

  // -- Builtin function table (Paykan name -> runtime symbol)
  // --------------------

  struct FunctionInfo {
    const char *RuntimeName;
    pir::Signature Sig;
  };
  std::unordered_map<std::string, FunctionInfo> FunctionTable;
  std::unordered_set<std::string> IdentityCtors;
  void bootstrapBuiltins();

  bool HadInternalError = false;
  void reportInternalError(const std::string &msg);

  // -- Module-level interning
  // -------------------------------------------------------

  std::unordered_map<std::string, std::string>
      InternedStrings; // content -> @sym
  std::unordered_map<std::string, std::string> InternedArrayData;
  std::unordered_map<std::string, std::string> InternedTupleKinds;

  /// Address (ptr) of the interned C-string global holding @p content.
  Val internString(const std::string &content);

  // -- Declarations
  // ---------------------------------------------------------------

  /// Declare a runtime function by its C symbol (signature from the ABI
  /// table) and return its signature.
  const pir::Signature &declareRuntime(const std::string &name);
  /// Declare an extern object/vtable global and return a Val naming it.
  Val externObject(const std::string &name);
  Val externVTable(const std::string &name);
  /// Get-or-create a module function (body filled in later).
  pir::Function *getOrCreateFunction(const std::string &name,
                                     const pir::Signature &sig);
  /// Find a callable function: defined here, or declared extern from the
  /// module that defines it (through ImportedFunctions).  nullptr if unknown.
  pir::Function *lookupFunction(const std::string &name);
  /// Like lookupFunction for a class's generated function (`C`, `C.m`,
  /// `C.destroy`): found locally, or declared extern from the module that
  /// defines class @p ct (ProgramLowering::ClassOrigins).
  pir::Function *lookupClassFunction(ast::ClassType *ct,
                                     const std::string &symbol);
  /// Declare @p fn (defined in module @p modulePath) as extern here, named
  /// @p localName in this module (or a variant of it, if that is taken).
  pir::Function *declareExternFrom(const pir::Function &fn,
                                   const std::string &modulePath,
                                   const std::string &localName);

  /// Call a runtime function.
  Val callRuntime(const std::string &name, const std::vector<Val> &args,
                  std::string resultName = "");

  // -- RAII helpers
  // ------------------------------------------------------------------

  struct ScopeGuard {
    ModuleLowering &L;
    Scope ScopeObj;
    explicit ScopeGuard(ModuleLowering &l);
    ~ScopeGuard();
  };

  /// Saves and restores all per-function emission state around one body.
  struct FunctionStateGuard {
    ModuleLowering &L;
    pir::Builder::State SavedBuilder;
    ast::Type *SavedRetASTType;
    ast::ClassType *SavedMethodClassType;
    std::unordered_set<pir::ValueId> SavedStringTemps;
    std::vector<LoopContext> SavedLoops;
    FunctionStateGuard(ModuleLowering &l, pir::Function *fn,
                       ast::Type *retASTType, ast::ClassType *methodClassTy);
    ~FunctionStateGuard();
    FunctionStateGuard(const FunctionStateGuard &) = delete;
    FunctionStateGuard &operator=(const FunctionStateGuard &) = delete;
  };

  // -- Type helpers
  // ---------------------------------------------------------------------

  pir::Type toPIRType(ast::Type *ty);
  bool isObjectElementType(ast::Type *elemTy) const;
  ast::Type *canonicalizeDeclType(ast::Type *ty);
  ast::ClassType *resolveExprClassType(ast::Expr *expr);
  pir::Signature functionSignature(ast::FuncDecl *node);
  pir::Function *declareFunctionPrototype(ast::FuncDecl *node);

  // -- Ownership predicates (ported verbatim)
  // ---------------------------------------------

  bool exprAlreadyShared(ast::Expr *expr) const;
  bool exprProducesFreshBox(ast::Expr *expr) const;
  ExprValue classifyExpr(ast::Expr *expr, const Val &val) const;
  void releaseIfOwned(const ExprValue &ev);
  Val takeSharedOwnership(ast::Expr *expr, const Val &val);
  Val promoteIntToFloat(const Val &v, ast::Type *targetTy);
  /// bool -> i64 when the destination is i64 (runtime ABI, user i64 params).
  Val coerceBoolToI64(const Val &v, pir::Type dest);

  void emitRetain(const Val &shared) { B.retain(shared); }
  void emitRelease(const Val &shared) { B.release(shared); }
  Val emitSharedNew(const Val &raw, std::string name = "shared");
  Val emitSharedGet(const Val &shared, std::string name = "obj");
  Val emitUnwrappedRef(ast::Expr *expr, std::string name = "ref.obj");
  Val emitAsShared(ast::Expr *expr);
  Val emitAsSharedRaw(ast::Expr *expr);
  /// An object array element read that borrows the array's reference
  /// (`arr[i]` of ref type on a live array): a new owner retains the stored
  /// box, which emitAsShared does while evaluating the array and index once.
  bool isBorrowedObjectElement(ast::Expr *expr) const;

  // -- String temporaries
  // ----------------------------------------------------------------

  std::unordered_set<pir::ValueId> OwnedStringTemps;
  void trackStringTemp(const Val &v);
  void untrackStringTemp(const Val &v);
  void destroyStringTempIfOwned(const Val &v);
  bool isTrackedStringTemp(const Val &v) const;
  Val wrapStringLiteral(const Val &rawStr, size_t len);

  // -- Conversions (#64, #88), by the spelled callee Sema rebinds them to
  // -------------------------------------------------------------------------

  /// A parse of a Str (`int<Str>`, `Int<Str>`, `bool<Str>`, ...): returns
  /// a fresh +1 box, or None.
  static bool isParseConversion(const std::string &callee);
  /// A `Str<...>` conversion: returns an owned PaykanString temporary.
  static bool isStrConversion(const std::string &callee);
  /// For a boxed source (`Str<Int>` & co.), the accessor that unboxes the
  /// argument before the primitive formatting; nullptr otherwise.
  static const char *conversionUnboxer(const std::string &callee);

  // -- Optionals
  // ----------------------------------------------------------------------------

  static bool isNoneForOptional(ast::Expr *expr);
  /// True when Sema marked @p expr (a primitive) for boxing into the
  /// optional primitive slot it flows into (`int` -> `int?`).
  static bool isPrimitiveBoxing(const ast::Expr *expr);
  /// Box the primitive value @p v as the present value of @p optTy (`int?`,
  /// `float?`, `bool?`, `char?`): a fresh +1 box of the runtime's Int /
  /// Float / Bool / Char object.
  Val emitPrimitiveBox(const Val &v, ast::Type *optTy);
  /// The primitive value held by the boxed Int / Float / Bool / Char object
  /// @p rawObj (a present `int?` & co.), as a value of type @p innerTy.
  Val emitPrimitiveUnbox(const Val &rawObj, ast::Type *innerTy,
                         const std::string &name);
  Val emitOptionalToObj(ast::Expr *expr, const Val &box);
  Val emitOptionalToObjRaw(ast::Expr *expr, const Val &raw);
  Val emitOptionalEquality(ast::BinaryExpr *node);

  // -- Statements
  // -------------------------------------------------------------------------------

  void emitScopeCleanup(Scope &scope);
  void emitAllScopesCleanup();
  void emitLoopScopesCleanup();
  Val emitImplicitVarDecl(const std::string &name, ast::Expr *rhsExpr,
                          const Val &val);
  void emitClassVarRebind(pir::LocalId local, ast::Expr *rhsExpr,
                          const Val &val);
  void emitBody(ast::CompoundStmt *body);
  void emitImplicitReturn(const pir::Signature &sig);
  Val emitValueMatch(ast::MatchStmt *node, const Val &subjRaw);
  Val emitEnumMatch(ast::MatchStmt *node, const Val &subjRaw);
  /// Emit `if (check0) body0 else if (check1) body1 ... else default`.
  void emitMatchChain(size_t n, const std::function<Val(size_t)> &emitCheck,
                      const std::function<void(size_t)> &emitBody,
                      const std::function<void()> &emitDefault);
  void emitMatchArmBody(ast::MatchArm *arm);
  ast::MatchArm *findWildcardArm(ast::MatchStmt *node);

  // -- Tuples
  // -----------------------------------------------------------------------------------

  unsigned char tupleElementKind(ast::Type *elemTy) const;
  Val emitTupleKindsGlobal(ast::TupleType *tt);
  Val toSlotBits(const Val &v);
  Val fromSlotBits(const Val &bits, ast::Type *elemTy);

  // -- Expressions
  // ----------------------------------------------------------------------------------

  class ExprEmitter : public ast::ExprVisitor<ExprEmitter, Val> {
  public:
    ModuleLowering &L;
    explicit ExprEmitter(ModuleLowering &l) : L(l) {}

    /// Every expression is emitted through here (it hides the visitor's own
    /// dispatch), so a primitive that Sema marked for boxing comes out as
    /// the +1 box its optional primitive slot expects, whichever site
    /// consumes it.
    Val visit(ast::Expr *node);

    Val emitIdentityCtor(ast::CallExpr *node);
    Val emitConversion(ast::CallExpr *node);
    Val emitBuiltinCall(ast::CallExpr *node);
    Val emitPrimitiveArrayLiteral(ast::ArrayLiteralExpr *node, size_t len);
    Val emitArrayPush(ast::MethodCallExpr *node, const Val &recv,
                      ast::Type *elemTy);
    Val emitArrayPop(const Val &recv, ast::Type *elemTy);
    Val emitShortCircuit(ast::BinaryExpr *node, bool isAnd);
    Val emitIntDivGuards(const Val &lhs, const Val &rhs, bool isDiv);

#define LW_EXPR(Kind, Name, Cast) Val visit##Name(ast::Cast *node);
    PAYKAN_EXPR_NODES(LW_EXPR)
#undef LW_EXPR
  };
  ExprEmitter Emitter{*this};
  Val emitExpr(ast::Expr *expr);

  // -- Classes (LoweringClass.cpp)
  // ----------------------------------------------------------------

  /// The pir::Class for @p ct (declared locally or as extern), creating it.
  pir::Class *getOrCreateClass(ast::ClassType *ct);
  std::vector<std::pair<std::string, ast::Type *>>
  allFieldsInOrder(ast::ClassType *ct) const;
  ast::Type *fieldASTType(ast::ClassType *ct, const std::string &name) const;
  ast::ClassType *getExprClassType(ast::Expr *expr) const;
  std::string findConcreteMethodFuncName(ast::ClassType *ct,
                                         const std::string &name);
  pir::Function *lookupOwnMethodFunction(ast::ClassType *ct,
                                         const std::string &methodName);
  pir::Signature methodSignature(ast::MethodDecl *md);
  /// The signature of @p md's vtable slot in class @p ct (runtime methods
  /// borrow ref arguments as raw objects, user methods consume boxes).
  pir::Signature slotSignature(ast::ClassType *ct, ast::MethodDecl *md);
  pir::Signature constructorSignature(ast::ClassType *ct);
  Val emitIsExactType(const Val &rawObjPtr, ast::ClassType *ct);
  void declareClass(ast::ClassDecl *node);
  Val lowerClassDecl(ast::ClassDecl *node);
  void emitDestructor(ast::ClassDecl *node, ast::ClassType *ct);
  Val lowerMemberAssignStmt(ast::MemberAssignStmt *node);
  Val lowerMemberAccessExpr(ast::MemberAccessExpr *node);
  /// Runtime symbol implementing @p name for the builtin class @p ct, or "".
  const char *builtinMethodSymbol(ast::ClassType *ct, const std::string &name);
};

/// Resolve an import to the canonical path of its source file (mirrors
/// Sema::resolveModulePath).  Empty if it does not exist.
std::string resolveImportFile(const std::string &projectRoot, bool isSystem,
                              const std::string &modulePath);

} // namespace paykan::lowering
