// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Internal declarations of the AST -> PIR lowering, the single home of the
// ownership rules (docs/pir.md §7): what every expression's value is
// (borrowed or owned), where retains, releases and string-temporary
// teardowns go, and how scopes are cleaned up.

#pragma once

#include "ASTContext.h"
#include "ASTVisitor.h"
#include "Sema.h"
#include "paykan/modules/ModuleResolver.h"
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

/// Ownership classification of an emitted expression value: Borrowed values
/// are left alone by the consumer, Owned ones are torn down by it (a tracked
/// raw string temporary with PaykanString_destroy, a fresh +1 box with
/// Paykan_release).
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
  /// The driver's resolver, or nullptr: a module it loaded from a `.pkm`
  /// is decoded, not lowered.
  modules::ModuleResolver *Resolver;
  /// Lowered modules in completion order (imports first); stable addresses.
  std::deque<pir::Module> Modules;
  /// The imports by canonical module name (Sema's identity of a module;
  /// nullptr while one is being lowered) and by the name the PIR gives them
  /// (claimName).
  std::unordered_map<std::string, pir::Module *> ByCanonical;
  std::unordered_map<std::string, pir::Module *> ByName;
  /// Every module name handed out (the main module's included).
  std::unordered_set<std::string> UsedNames;
  /// Class name -> name of the module whose ClassDecl defines it.
  std::unordered_map<std::string, std::string> ClassOrigins;
  /// An instantiation of another module's generic function
  /// (`$<module>::f<int>`) -> name of the module that emits it.
  std::unordered_map<std::string, std::string> FuncOrigins;
  /// The instantiations of other modules' templates (classes and
  /// functions), each emitted by the first module lowered that needs it.
  std::unordered_set<std::string> ForeignInstantiations;
  /// Every SemaContext reachable from Top, keyed by canonical module name.
  std::unordered_map<std::string, const sema::SemaContext *> Contexts;
  bool Indexed = false;

  ProgramLowering(const sema::SemaContext &top, std::string projectRoot,
                  std::ostream &errs, modules::ModuleResolver *resolver)
      : Top(top), ProjectRoot(std::move(projectRoot)), Errs(errs),
        Resolver(resolver) {}

  const sema::SemaContext *lookupImportContext(const std::string &canonical);
  /// Reserve @p name for a module, or `name.2`, `name.3`, ... when another
  /// module already has it.
  std::string claimName(const std::string &name);
  /// Lower (or, when the resolver loaded it from a `.pkm`, decode) the
  /// module of canonical name @p name once, and return it.
  pir::Module *lowerImport(const std::string &name);
};

/// Lowers ONE module (translation unit).  Imported modules get their own
/// instance (see LoweringProgram.cpp); the SymbolOrigins and the set of
/// already-lowered modules are shared across the program.
class ModuleLowering : public ast::ASTVisitor<ModuleLowering, Val> {
public:
  /// @p canonical is the module's canonical name, empty for the main
  /// module.
  ModuleLowering(ProgramLowering &program, const sema::SemaContext &semaCtx,
                 std::string moduleName, std::string canonical = "");

  /// Lower @p tu into Module.  Returns false after an internal error.
  bool run(ast::TranslationUnit *tu);

  pir::Module takeModule();

  // -- Visitor overrides
#define LW_VISIT(Kind, Name, Cast) Val visit##Name(ast::Cast *node);
  PAYKAN_STMT_NODES(LW_VISIT)
  PAYKAN_DECL_NODES(LW_VISIT)
  PAYKAN_TOPLEVEL_NODES(LW_VISIT)
#undef LW_VISIT

private:
  ProgramLowering &PL;
  std::string Canonical;
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
  /// The instantiations of other modules' templates this module emits:
  /// those of SemaContext::ForeignClasses / ForeignFunctions that no module
  /// lowered before it emits (each is emitted once per program).
  std::vector<ast::ClassDecl *> ForeignClasses;
  std::vector<ast::FuncDecl *> ForeignFuncs;
  std::vector<ast::StructDecl *> ForeignStructs;

  pir::Module Mod;
  /// Functions in declaration order with stable addresses (the module vector
  /// is filled from this when the module is taken).
  std::deque<pir::Function> Funcs;
  /// Every program function by its PIR name in this module (definitions and
  /// module externs).  Runtime externs are not in it (RuntimeFuncs).
  std::unordered_map<std::string, pir::Function *> FuncByName;
  /// This module's native functions (instances included), by name.
  std::unordered_map<std::string, const ast::FuncDecl *> NativeDecls;
  /// Runtime extern functions declared in this module, by C symbol (their
  /// PIR name is pir::runtimeName(symbol)).
  std::unordered_map<std::string, pir::Function *> RuntimeFuncs;
  /// Module externs by origin, "<defining module>\n<symbol there>".
  std::unordered_map<std::string, pir::Function *> ExternByOrigin;
  std::unordered_map<std::string, pir::Class *> ClassByName;
  std::deque<pir::Class> Classes_;
  std::unordered_set<std::string> ExternGlobals;

  pir::Builder B;

  // -- Scoped symbol table (name -> local slot)

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
    /// Values held in memory, holding references, declared in this scope:
    /// their references are released at its exit.
    std::vector<VarMeta> MemVars;
    /// `inout` parameters declared in this scope: their local holds the
    /// address of the caller's storage, of this type.
    std::unordered_map<std::string, pir::Type> InoutVars;
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
    /// An `inout` parameter of type @p astTy, whose local holds the address
    /// of a @p pointee: the caller's storage, never owned here.
    void declareInout(const std::string &name, pir::LocalId local,
                      pir::Type pointee, ast::Type *astTy);
    /// The type an `inout` parameter @p name points to; Void for any other
    /// variable.
    pir::Type inoutType(const std::string &name) const;
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

  std::unordered_map<std::string, std::string>
      InternedStrings; // content -> @sym
  std::unordered_map<std::string, std::string> InternedTupleLayouts;
  std::unordered_map<std::string, std::string> InternedTypeDescriptors;

  /// Address (ptr) of the interned C-string global holding @p content.
  Val internString(const std::string &content);

  // -- Declarations

  /// Declare a runtime function by its C symbol (signature from the ABI
  /// table) and return the declaration, named `$rt.<symbol>` in PIR.
  const pir::Function &declareRuntime(const std::string &symbol);
  /// Declare a runtime extern object/vtable global by its C symbol and
  /// return a Val naming it (`@$rt.<symbol>`).
  Val externGlobal(const std::string &symbol, pir::ExternGlobal::Kind kind);
  Val externObject(const std::string &symbol);
  Val externVTable(const std::string &symbol);
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
  /// The same for the class or struct named @p typeName (a struct's
  /// functions are `S`, its constructor, and `S.m`, its methods).
  pir::Function *lookupTypeFunction(const std::string &typeName,
                                    const std::string &symbol);
  /// Declare @p fn (defined in module @p modulePath) as extern here, named
  /// @p localName in this module (or a variant of it, if that is taken).
  pir::Function *declareExternFrom(const pir::Function &fn,
                                   const std::string &modulePath,
                                   const std::string &localName);
  /// This module uses @p name, declared extern: when it is an instantiation
  /// of another module's template, the module that emits it is this
  /// program's choice, so this module is not cached (docs/design/pkm.md
  /// §4.8).
  void noteExternUse(const std::string &name);

  /// Call a runtime function by its C symbol.
  Val callRuntime(const std::string &symbol, const std::vector<Val> &args,
                  std::string resultName = "");

  // -- RAII helpers

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

  pir::Type toPIRType(ast::Type *ty);
  bool isObjectElementType(ast::Type *elemTy) const;
  ast::Type *canonicalizeDeclType(ast::Type *ty);
  ast::ClassType *resolveExprClassType(ast::Expr *expr);
  pir::Signature functionSignature(ast::FuncDecl *node);
  pir::Function *declareFunctionPrototype(ast::FuncDecl *node);

  // -- Ownership predicates

  bool exprAlreadyShared(ast::Expr *expr) const;
  bool exprProducesFreshBox(ast::Expr *expr) const;
  /// A lambda, or a function used as a value: a fresh closure box.
  static bool isFunctionValueExpr(const ast::Expr *expr);
  ExprValue classifyExpr(ast::Expr *expr, const Val &val) const;
  void releaseIfOwned(const ExprValue &ev);
  Val takeSharedOwnership(ast::Expr *expr, const Val &val);
  Val promoteIntToFloat(const Val &v, ast::Type *targetTy);
  /// bool -> i64 when the destination is i64 (runtime ABI, user i64 params).
  Val coerceBoolToI64(const Val &v, pir::Type dest);
  /// @p v as the PIR type @p dest of the slot it flows into: int -> float
  /// promotion and bool -> i64, the conversions Sema's assignability allows.
  Val coerceTo(const Val &v, pir::Type dest);

  /// The receiver of a method call, field access, subscript or tuple index:
  /// its emitted value with its ownership (a call-rooted receiver is a fresh
  /// +1 box to tear down after the use) and the raw object it unwraps to.
  struct Receiver {
    ExprValue Owned;
    Val Raw;
    explicit operator bool() const { return static_cast<bool>(Raw); }
  };
  /// Emit @p expr as a receiver; the unwrapped object is named @p name.
  /// Raw is void when the expression failed.
  Receiver emitReceiver(ast::Expr *expr, const std::string &name = "recv.obj");

  void emitRetain(const Val &shared) { B.retain(shared); }
  void emitRelease(const Val &shared) { B.release(shared); }
  Val emitSharedNew(const Val &raw, std::string name = "shared");
  Val emitSharedGet(const Val &shared, std::string name = "obj");
  Val emitUnwrappedRef(ast::Expr *expr, std::string name = "ref.obj");
  Val emitAsShared(ast::Expr *expr);
  /// @p arg's box for a native function to borrow: a variable's or a
  /// field's as it is, anything else a +1 box added to @p keep, which the
  /// caller releases after the call.
  Val emitBorrowedBox(ast::Expr *arg, std::vector<Val> &keep);
  Val emitAsSharedRaw(ast::Expr *expr);

  // -- String temporaries

  std::unordered_set<pir::ValueId> OwnedStringTemps;
  void trackStringTemp(const Val &v);
  void untrackStringTemp(const Val &v);
  void destroyStringTempIfOwned(const Val &v);
  bool isTrackedStringTemp(const Val &v) const;
  Val wrapStringLiteral(const Val &rawStr, size_t len);

  // -- Conversions (#64, #88), by the spelled callee Sema rebinds them to

  /// A `Str<...>` conversion: returns an owned PaykanString temporary.
  static bool isStrConversion(const std::string &callee);

  // -- Optionals

  static bool isNoneForOptional(ast::Expr *expr);
  /// True when Sema marked @p expr (a primitive) to become the present
  /// value of the optional primitive slot it flows into (`int` -> `int?`).
  static bool isOptionalWrap(const ast::Expr *expr);
  /// The optional type of @p expr's value when that is an optional of a
  /// value type (`int?` & co.): its own type, or the one it is wrapped into.
  /// nullptr otherwise.
  static ast::Type *valueOptionalType(ast::Expr *expr);
  /// The present `int?` & co. (@p optTy) of the primitive @p v.
  Val emitSome(const Val &v, ast::Type *optTy);
  /// Two optionals of one value type, equal when both are None or both are
  /// present with `==` payloads.
  Val emitValueOptionalEquals(const Val &a, const Val &b);
  /// The text of the optional value @p opt, "None" or its value's (what
  /// `Str<int>` & co. give): an owned string temporary.
  Val emitOptionalToString(const Val &opt);
  /// The raw object @p raw of an optional reference printed by a print
  /// builtin, with the None object for a null one.
  Val emitNoneObject(const Val &raw);
  Val emitOptionalEquality(ast::BinaryExpr *node);

  // -- Statements

  void emitScopeCleanup(Scope &scope);
  void emitAllScopesCleanup();
  void emitLoopScopesCleanup();
  Val emitImplicitVarDecl(const std::string &name, ast::Expr *rhsExpr,
                          const Val &val);
  void emitClassVarRebind(const std::string &name, ast::Expr *rhsExpr,
                          const Val &val);
  void emitBody(ast::CompoundStmt *body);
  void emitImplicitReturn(const pir::Signature &sig);
  void emitNativeBody(ast::FuncDecl *node, const pir::Function &fn);
  /// Call @p node's C symbol with @p args, one per parameter: a reference
  /// as a borrowed box, an `inout` argument as its address, a value as is.
  /// A value held in memory crosses by address: its argument in @p args is
  /// one, and a result of one is written to @p memResult.
  Val emitNativeCall(const ast::FuncDecl &node, const std::vector<Val> &args,
                     const Val &memResult = Val());
  /// The native function of this module named @p name, or nullptr.
  const ast::FuncDecl *localNative(const std::string &name) const;
  /// The PaykanType (src/Runtime/Runtime.h) a generic native function is
  /// passed for @p ty: a module data global, one per distinct description.
  Val emitTypeDescriptor(ast::Type *ty);
  /// The kind word of @p ty's PaykanType.
  unsigned char nativeTypeKind(ast::Type *ty) const;
  Val emitValueMatch(ast::MatchStmt *node, const Val &subjRaw);
  Val emitEnumMatch(ast::MatchStmt *node, const Val &subjRaw);
  /// Emit `if (check0) body0 else if (check1) body1 ... else default`.
  void emitMatchChain(size_t n, const std::function<Val(size_t)> &emitCheck,
                      const std::function<void(size_t)> &emitBody,
                      const std::function<void()> &emitDefault);
  void emitMatchArmBody(ast::MatchArm *arm);
  ast::MatchArm *findWildcardArm(ast::MatchStmt *node);

  // -- Expressions

  class ExprEmitter : public ast::ExprVisitor<ExprEmitter, Val> {
  public:
    ModuleLowering &L;
    explicit ExprEmitter(ModuleLowering &l) : L(l) {}
    /// Set for one call whose `inout` result is wanted as its address (an
    /// `inout` argument or local, a returned `inout` result) rather than
    /// read; the call consumes it.
    bool WantAddress = false;

    /// Every expression is emitted through here (it hides the visitor's own
    /// dispatch), so a primitive that Sema marked for its optional slot
    /// comes out as the optional value that slot expects, whichever site
    /// consumes it.
    Val visit(ast::Expr *node);

    /// `int<Str>`, `float<Str>`, `bool<Str>`: the optional value of the
    /// runtime's parse; an invalid Val for any other call.
    Val emitParse(ast::CallExpr *node);

    Val emitIdentityCtor(ast::CallExpr *node);
    Val emitConversion(ast::CallExpr *node);
    Val emitBuiltinCall(ast::CallExpr *node);
    /// The arguments of the method call @p node for @p method, whose PIR
    /// signature is @p sig (the receiver first, at index 0), appended to
    /// @p args: a slice as two values, a value held in memory by address,
    /// an `inout` argument as its address (its kept objects in @p keep), a
    /// reference as a +1 box when the method is @p userDefined (a builtin
    /// borrows it), the owned temporaries among the rest in @p ownedArgs.
    /// False when an argument failed.
    bool emitMethodArgs(ast::MethodCallExpr *node,
                        const ast::MethodDecl *method,
                        const pir::Signature &sig, bool userDefined,
                        std::vector<Val> &args,
                        std::vector<ExprValue> &ownedArgs,
                        std::vector<Val> &keep);
    /// A call of a method of the struct @p st (LoweringStruct.cpp): the
    /// receiver's address, then the arguments; @p wantAddress as for a
    /// call's `inout` result.
    Val emitStructMethodCall(ast::MethodCallExpr *node, ast::StructType *st,
                             bool wantAddress);
    Val emitShortCircuit(ast::BinaryExpr *node, bool isAnd);
    Val emitIntDivGuards(const Val &lhs, const Val &rhs, bool isDiv);

#define LW_EXPR(Kind, Name, Cast) Val visit##Name(ast::Cast *node);
    PAYKAN_EXPR_NODES(LW_EXPR)
#undef LW_EXPR
  };
  ExprEmitter Emitter{*this};
  Val emitExpr(ast::Expr *expr);

  // -- Classes (LoweringClass.cpp)

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

  // -- Function values (LoweringClosure.cpp,
  // docs/language/15-function-types.md)

  /// The closure classes made so far: per lambda, and per named function
  /// used as a value.
  std::unordered_map<const ast::LambdaExpr *, std::string> LambdaClasses;
  std::unordered_map<std::string, std::string> FunctionRefClasses;
  unsigned NextClosureId = 0;
  /// The `call` slot's signature for a closure of type @p fnt: the closure
  /// object, then the parameters as a method's, a result held in memory
  /// written through a trailing pointer.
  pir::Signature closureSignature(ast::FunctionType *fnt);
  /// A closure class @p name with a field per capture and the vtable
  /// [destroy, toString, equals, call] (the functions declared, bodies to
  /// come).
  pir::Class *declareClosureClass(const std::string &name,
                                  const std::vector<ast::Type *> &captureTys,
                                  const pir::Signature &callSig);
  void emitClosureDestructor(const std::string &name,
                             const std::vector<ast::Type *> &captureTys);
  /// The class and functions of lambda @p node (made once): its name.
  const std::string &lowerLambda(ast::LambdaExpr *node);
  /// A new closure of @p node, its captures copied in: a fresh owned box.
  Val emitLambdaValue(ast::LambdaExpr *node);
  /// A closure class whose `call` forwards to the function @p fnName (made
  /// once per function): its name.
  const std::string &lowerFunctionRef(const std::string &fnName,
                                      ast::FunctionType *fnt);
  Val emitFunctionRefValue(const std::string &fnName, ast::FunctionType *fnt);
  /// Call the closure object @p closure (borrowed for the call) of type
  /// @p fnt with @p argExprs; @p resultTy is the call's resolved type.
  Val emitClosureCall(const Val &closure, ast::FunctionType *fnt,
                      const std::vector<ast::Expr *> &argExprs,
                      ast::Type *resultTy, bool wantAddress);

  // -- Structs (LoweringStruct.cpp, docs/language/14-structs.md)
  //
  // A struct is a tuple of its fields to the lowering (ast::StructType is a
  // TupleType): held in memory, copied, retained and released like one.
  // Its methods are functions `S.m(self: ptr, ...)` whose `self` is the
  // receiver's address, its constructor `S(..., out: ptr)` writes the new
  // value where the caller's temporary is, and its text and equality go
  // through its layout (names included) like a tuple's.

  /// The signature of the struct method @p md: the receiver's address
  /// first, then the parameters, then the address a result held in memory
  /// is written to (as methodSignature).
  pir::Signature structMethodSignature(ast::MethodDecl *md);
  /// The constructor's: the `__init__` parameters, then the address of the
  /// value to make.
  pir::Signature structConstructorSignature(ast::StructType *st);
  /// Declare the functions of @p sd's struct (its methods and constructor),
  /// before any body is emitted.
  void declareStruct(ast::StructDecl *sd);
  /// Emit the bodies: each method, and the constructor (zero the value,
  /// then `__init__`).
  Val lowerStructDecl(ast::StructDecl *sd);
  /// The struct type of @p e's value, canonical in this module, or nullptr.
  ast::StructType *getExprStructType(ast::Expr *e);
  /// `p.x` read, the receiver a struct: in place, as a tuple element.
  Val lowerStructFieldRead(ast::MemberAccessExpr *node, ast::StructType *st);
  /// `p.x = v`, the receiver a struct: written in place.
  Val lowerStructFieldAssign(ast::MemberAssignStmt *node, ast::StructType *st);

  // -- Values held in memory: fixed-size arrays and tuples (LoweringMem.cpp)

  struct MemLayout {
    int64_t Size = 0;
    int64_t Align = 1;
  };
  /// The bytes and alignment of a value of @p ty held in memory: a
  /// fixed-size array's are its elements', one after another, a tuple's its
  /// elements' at their alignments, like a C struct's fields.
  MemLayout memLayout(ast::Type *ty);
  /// The byte offset of element @p i of a value of the tuple type @p tt.
  int64_t tupleElementOffset(const ast::TupleType *tt, size_t i);
  /// True when @p ty (isMemType) holds references, at any depth: each such
  /// slot owns a reference to its box.
  static bool memHoldsRefs(const ast::Type *ty);
  /// A value held in memory: the address of its bytes, and whether they are
  /// a temporary (a call's result, a literal) whose references the consumer
  /// takes over.
  struct MemValue {
    Val Addr;
    bool Temp = false;
  };
  /// The bytes of @p e, an expression of a type held in memory: a
  /// variable's own (or, for an `inout` one, the value it names), an
  /// element's or a field's, or a temporary.
  MemValue emitMemValue(ast::Expr *e);
  /// emitMemValue's address, borrowed: a temporary holding references is
  /// added to MemTemps, for its consumer to release with releaseMemTemps
  /// once done with it.
  Val emitMemAddress(ast::Expr *e);
  /// What addresses handed out depend on: temporaries holding references
  /// (with their type), and +1 boxes of the objects holding fields (with
  /// none).
  std::vector<std::pair<Val, ast::Type *>> MemTemps;
  /// Release what was added to MemTemps since its size was @p mark.
  void releaseMemTemps(size_t mark);
  /// Retain, or release, every reference held by the value of type @p ty at
  /// @p addr (nothing for one of plain values).
  void emitMemRetain(const Val &addr, ast::Type *ty);
  void emitMemRelease(const Val &addr, ast::Type *ty);
  /// Call @p fn with the box in each reference slot of the value of type
  /// @p ty at @p addr.
  void forEachMemRef(const Val &addr, ast::Type *ty,
                     const std::function<void(Val)> &fn);
  /// Call @p fn with the address of each of the slots @p from to @p to
  /// (exclusive), @p stride bytes apart, of the memory at @p base: one by
  /// one for a few, in a loop for more.
  void forEachSlot(const Val &base, int64_t from, int64_t to, int64_t stride,
                   const std::function<void(Val)> &fn);
  /// The address of element @p index of the fixed-size array @p array,
  /// after a bounds check.
  Val emitFixedElementAddress(ast::Expr *array, ast::Expr *index);
  /// The address of element @p index of the tuple @p tuple.
  Val emitTupleElementAddress(ast::Expr *tuple, size_t index);
  /// Write the value of @p e, of a type held in memory, into the bytes at
  /// @p dst: a literal element by element, anything else copied.  @p e must
  /// not read @p dst (a new variable's storage).
  void emitMemInit(ast::Expr *e, const Val &dst);
  /// Write the value of @p e, of type @p ty (an element of a literal), into
  /// the slot at @p dst: an owned box for a reference, the bytes of a value
  /// held in memory.
  void emitElementInit(ast::Expr *e, ast::Type *ty, const Val &dst);
  /// Initialise the array of type @p ft at @p dst from `[elem; n]`.
  void emitFixedRepeat(ast::Expr *elem, ast::FixedArrayType *ft,
                       const Val &dst);
  /// A variable @p name of a type held in memory, initialised with @p init
  /// (zero without one), declared in the current scope.
  void declareMemVar(const std::string &name, ast::Type *ty, ast::Expr *init);
  /// Copy the value of @p value into the place at @p dst (an assignment:
  /// @p value may read @p dst).
  void emitMemAssign(const Val &dst, ast::Expr *value);
  /// A parameter @p name of a type held in memory, whose value @p arg is the
  /// address of the caller's value: copied for a by-value one, the address
  /// kept for a `view` or `inout` one.
  void declareMemParam(const std::string &name, const pir::Value &arg,
                       ast::Type *ty, ast::ParamMode mode);
  /// A temporary for a call's result of type @p ty (held in memory): its
  /// address, passed as the call's last argument, where the callee writes
  /// the result.
  Val emitMemResultSlot(ast::Type *ty);
  /// The scalar or box element of type @p ty at @p addr, read in place: a
  /// box borrowed from its slot.
  Val emitElementLoad(const Val &addr, ast::Type *ty);
  /// The layout of the tuple type @p tt the runtime reads (Runtime.h,
  /// "Tuples"), as constant data; a struct's holds its name and its fields'
  /// ("Structs").
  Val emitTupleLayout(ast::TupleType *tt);
  /// The text of the tuple or struct @p e, a new string
  /// (PaykanTuple_toString / PaykanStruct_toString).
  Val emitTupleToString(ast::Expr *e);
  /// Whether the tuples or structs @p a and @p b are equal
  /// (PaykanTuple_equals / PaykanStruct_equals).
  Val emitTupleEquals(ast::Expr *a, ast::Expr *b);
  /// True when @p arg is passed for a slice parameter: a fixed-size array
  /// Sema took for one, or a slice passed on.
  static bool isSliceArg(const ast::Expr *arg);
  /// Push what @p arg passes for a slice parameter: the address of its
  /// elements and their number.  False when it failed.
  bool emitSliceArg(ast::Expr *arg, std::vector<Val> &args);
  /// The number of elements of the slice parameter @p e names.
  Val emitSliceLength(ast::Expr *e);

  // -- `inout` parameters (LoweringInout.cpp)

  /// The PIR type of a parameter of type @p ty and mode @p mode: `ptr` for
  /// `inout`, toPIRType's otherwise.
  pir::Type paramPIRType(ast::Type *ty, ast::ParamMode mode);
  /// Append the PIR types of a parameter of type @p ty and mode @p mode to
  /// @p out: one, or a slice's two (the address of its elements and their
  /// number).
  void appendParamTypes(std::vector<pir::Type> &out, ast::Type *ty,
                        ast::ParamMode mode);
  /// Name @p fn's PIR parameters from @p first after @p params (a slice's
  /// second is `<name>.len`).
  static void nameParams(pir::Function &fn, size_t first,
                         const std::vector<ast::Param> &params);
  /// Declare parameter @p name of a function being emitted, of type @p astTy
  /// and mode @p mode, whose value is @p params[@p at] (and the next one for
  /// a slice), in the current scope; returns how many values it takes.
  size_t declareParam(const std::string &name,
                      const std::vector<pir::Value> &params, size_t at,
                      ast::Type *astTy, ast::ParamMode mode);
  /// Store @p v into variable @p name (local @p local) when it is an
  /// `inout` parameter, through its address; false for any other variable.
  bool storeInout(const std::string &name, pir::LocalId local, const Val &v);
  /// True when variable @p name's storage holds a box: an owned reference
  /// variable's own slot, or the caller's slot an `inout` reference
  /// parameter points to.
  bool holdsBox(const std::string &name) const;
  /// The box in variable @p name's storage, borrowed (holdsBox).
  Val loadVarBox(const std::string &name);
  /// Put the +1 box @p newBox in variable @p name's storage (holdsBox),
  /// releasing the box it held.
  void storeVarBox(const std::string &name, const Val &newBox);
  /// The address passed for the `inout` argument @p arg: a variable's
  /// `local.addr`, the address an `inout` parameter holds, or a field's
  /// `field.addr`.  A +1 box the caller keeps for the duration of the call
  /// (a field's object that no variable holds) goes to @p keep, for
  /// releaseAfterCall.
  /// With @p keepObject, a field's object is kept even when a variable
  /// holds it (a local borrow outlives the statement).
  Val emitInoutArg(ast::Expr *arg, std::vector<Val> &keep,
                   bool keepObject = false);
  /// An `inout` local (`x: inout = k;`): a `ptr` local holding the address
  /// of what it names, whose object (for a field) a hidden local keeps.
  Val emitInoutLocal(ast::VarDecl *node);
  /// The address of the storage an `inout` result names: @p place, part of
  /// `self` or of an `inout` parameter, or a conditional of two such.
  Val emitPlaceAddress(ast::Expr *place);
  /// The address a call returning an `inout` result returns.
  Val emitCallAddress(ast::Expr *call);
  /// A call's `inout` result @p addr read as a value of type @p astTy (a
  /// box is retained, as a call's result is owned).
  Val loadInoutResult(const Val &addr, ast::Type *astTy);
  /// An `inout` name @p name, of type @p astTy, for the variable or field
  /// @p place: an `inout` local's, or a `match` arm's `n: inout T`.
  void emitInoutBinding(const std::string &name, ast::Expr *place,
                        ast::Type *astTy);
  /// Release the boxes emitInoutArg kept for a call that has returned.
  void releaseAfterCall(const std::vector<Val> &keep);
  /// For a call of a method of @p ct, a class defined in another module:
  /// an internal error unless @p sig is the signature of vtable slot
  /// @p slot there (so no caller passes a value where an address is due).
  bool checkImportedSlot(ast::ClassType *ct, uint32_t slot,
                         const pir::Signature &sig);
};

} // namespace paykan::lowering
