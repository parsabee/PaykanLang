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
    /// Fixed-size arrays of references declared in this scope, whose
    /// elements are released at its exit.
    std::vector<VarMeta> FixedVars;
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
  std::unordered_map<std::string, std::string> InternedArrayData;
  std::unordered_map<std::string, std::string> InternedTupleKinds;
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
  Val emitNativeCall(const ast::FuncDecl &node, const std::vector<Val> &args);
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

  // -- Tuples

  unsigned char tupleElementKind(ast::Type *elemTy) const;
  /// The first runtime slot of element @p i of @p tt (its slot count for
  /// i == arity): an optional value takes two, its presence word and its
  /// value.
  static int64_t tupleSlot(const ast::TupleType *tt, size_t i);
  /// Store element @p v (of type @p elemTy, not a reference) of a tuple
  /// being built into its slot(s) from @p slot.
  void emitTupleSetValue(const Val &tup, int64_t slot, const Val &v,
                         ast::Type *elemTy);
  /// Read element (of type @p elemTy, not a reference) of tuple @p tup from
  /// its slot(s) at @p slot.
  Val emitTupleGetValue(const Val &tup, int64_t slot, ast::Type *elemTy);
  Val emitTupleKindsGlobal(ast::TupleType *tt);
  Val toSlotBits(const Val &v);
  Val fromSlotBits(const Val &bits, ast::Type *elemTy);

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

  // -- Fixed-size arrays (LoweringFixed.cpp)

  struct FixedLayout {
    int64_t Size = 0;
    int64_t Align = 1;
  };
  /// The bytes and alignment of a value of @p ty held in memory: a
  /// fixed-size array's are its elements', one after another.
  FixedLayout fixedLayout(ast::Type *ty);
  /// True when @p ty is a fixed-size array of references (at any depth):
  /// each element slot owns a reference to its box.
  static bool fixedHoldsRefs(const ast::Type *ty);
  /// A fixed-size array value: the address of its bytes, and whether they
  /// are a temporary (a call's result, a literal) whose elements' references
  /// the consumer takes over.
  struct FixedValue {
    Val Addr;
    bool Temp = false;
  };
  /// The bytes of @p e, an expression of a fixed-size array type: a
  /// variable's own (or, for an `inout` one, the array it names), an
  /// element's, or a temporary.
  FixedValue emitFixedValue(ast::Expr *e);
  /// emitFixedValue's address, borrowed: a temporary of references is
  /// added to FixedTemps, for its consumer to release with
  /// releaseFixedTemps once done with it.
  Val emitFixedAddress(ast::Expr *e);
  /// What fixed-size array addresses handed out depend on: temporaries of
  /// references (with their type), and +1 boxes of the objects holding
  /// field arrays (with none).
  std::vector<std::pair<Val, ast::FixedArrayType *>> FixedTemps;
  /// Release what was added to FixedTemps since its size was @p mark.
  void releaseFixedTemps(size_t mark);
  /// Retain, or release, every reference held by the array of type @p ty at
  /// @p addr (nothing for an array of values).
  void emitFixedRetain(const Val &addr, ast::FixedArrayType *ty);
  void emitFixedRelease(const Val &addr, ast::FixedArrayType *ty);
  /// Call @p fn with the box in each reference slot of the array of type
  /// @p ty at @p addr.
  void forEachFixedRef(const Val &addr, ast::FixedArrayType *ty,
                       const std::function<void(Val)> &fn);
  /// Call @p fn with the address of each of the slots @p from to @p to
  /// (exclusive), @p stride bytes apart, of the memory at @p base: one by
  /// one for a few, in a loop for more.
  void forEachSlot(const Val &base, int64_t from, int64_t to, int64_t stride,
                   const std::function<void(Val)> &fn);
  /// The address of element @p index of the fixed-size array @p array,
  /// after a bounds check.
  Val emitFixedElementAddress(ast::Expr *array, ast::Expr *index);
  /// Write the value of @p e, of a fixed-size array type, into the bytes at
  /// @p dst: a literal element by element, anything else copied.  @p e must
  /// not read @p dst (a new variable's storage).
  void emitFixedInit(ast::Expr *e, const Val &dst);
  /// Initialise the array of type @p ft at @p dst from `[elem; n]`.
  void emitFixedRepeat(ast::Expr *elem, ast::FixedArrayType *ft,
                       const Val &dst);
  /// A fixed-size array variable @p name initialised with @p init (zero
  /// without one), declared in the current scope.
  void declareFixedVar(const std::string &name, ast::FixedArrayType *ty,
                       ast::Expr *init);
  /// Copy the value of @p value into the fixed-size array place at @p dst
  /// (an assignment: @p value may read @p dst).
  void emitFixedAssign(const Val &dst, ast::Expr *value);
  /// A fixed-size array parameter @p name, whose value @p arg is the
  /// caller's array's address: copied for a by-value one, the address kept
  /// for a `view` or `inout` one.
  void declareFixedParam(const std::string &name, const pir::Value &arg,
                         ast::FixedArrayType *ty, ast::ParamMode mode);
  /// A temporary for a call's fixed-size array result of type @p ty: its
  /// address, passed as the call's last argument, where the callee writes
  /// the result.
  Val emitFixedResultSlot(ast::Type *ty);
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
