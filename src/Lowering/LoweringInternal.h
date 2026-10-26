// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Internal declarations of the AST -> PIR lowering, the single home of the
// ownership rules (docs/pir.md §7): what every expression's value is
// (borrowed or owned), where retains, releases and string-temporary
// teardowns go, and how scopes are cleaned up.

#pragma once

#include "ASTContext.h"
#include "ASTVisitor.h"
#include "Sema.h"
#include "paykan/lowering/Lowering.h"
#include "paykan/modules/ModuleResolver.h"
#include "paykan/pir/Builder.h"
#include "paykan/pir/PIR.h"

#include <deque>
#include <functional>
#include <map>
#include <optional>
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
  /// Each lowered or loaded module's direct imports, by canonical name.
  std::unordered_map<std::string, std::vector<std::string>> ImportsOf;
  /// Every SemaContext reachable from Top, keyed by canonical module name.
  std::unordered_map<std::string, const sema::SemaContext *> Contexts;
  bool Indexed = false;
  /// What the main module's code may know of the program (lowerProgram).
  World MainWorld = World::Open;

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

  /// Lower @p node; a statement once its held reads are noted
  /// (noteHeldReads, noteBorrowedReads).
  Val visit(ast::ASTNode *node);

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
  /// Mark this module's instantiations of templates shared (pir::Function::
  /// Shared): the classes and structs whose names carry type arguments,
  /// their functions, and the copies of other modules' generic functions.
  void markShared();
  /// The instantiations of other modules' templates this module emits:
  /// those of SemaContext::ForeignClasses / ForeignFunctions that no module
  /// lowered before it emits (each is emitted once per program).
  std::vector<ast::ClassDecl *> ForeignClasses;
  std::vector<ast::FuncDecl *> ForeignFuncs;
  std::vector<ast::StructDecl *> ForeignStructs;
  /// The classes and structs this module defines: its own, and its copies
  /// of other modules' instantiations (which another module may have
  /// defined first: ProgramLowering::ClassOrigins).
  std::unordered_set<std::string> OwnTypes;
  /// In the main module of a closed world (World::Closed), the classes
  /// derived from each class it defines: all of them are its own
  /// (indexSubclasses, isClosedClass).
  std::unordered_map<const ast::ClassType *, std::vector<ast::ClassType *>>
      Subclasses;
  void indexSubclasses(const std::vector<ast::ClassDecl *> &classes);

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
    /// `weak` variables (docs/language/16-memory.md): their slot
    /// stores a weak reference, loaded as a fresh +1 box on every read.
    std::unordered_set<std::string> WeakVars;
    struct VarMeta {
      pir::LocalId Local;
      ast::Type *ASTType;
      bool Weak = false; // released with weak.release, not release
    };
    /// Owned variables declared in this scope, in declaration order (the
    /// `weak` ones among them).
    std::vector<VarMeta> DeclOrder;
    /// Values held in memory, holding references, declared in this scope:
    /// their references are released at its exit.
    std::vector<VarMeta> MemVars;
    /// `inout` parameters declared in this scope: their local holds the
    /// address of the caller's storage, of this type.
    std::unordered_map<std::string, pir::Type> InoutVars;
    /// The function's names for storage it never hands on: a method's
    /// `self`, its `inout`, `view` and slice parameters (markStable).
    std::unordered_set<std::string> StableVars;
    /// Extra owned boxes released at scope exit (e.g. a match subject).
    std::vector<Val> PendingReleases;
    /// A `match` arm's `n: inout T` (bindInoutArm): after n's last use in
    /// the arm (emitArmBody), or at the scope's exit by any way out before
    /// it, ahead of every release, n's own value (`Binding`, of type
    /// `BindTy`) is assigned back to the subject's storage, whose address
    /// `Place` holds and whose type is `PlaceTy`.  For a value held in
    /// memory, n is the part at `Offset`, after the word `Header` (a
    /// variant's tag, an optional's presence flag; none for the whole).
    struct WriteBack {
      const ast::MatchArm *Arm;
      pir::LocalId Place;
      pir::LocalId Binding;
      ast::Type *PlaceTy;
      ast::Type *BindTy;
      int64_t Offset;
      Val Header;
    };
    std::vector<WriteBack> WriteBacks;

    explicit Scope(Scope *parent = nullptr) : Parent(parent) {}

    bool hasLocal(const std::string &name) const;
    pir::LocalId lookup(const std::string &name) const; // asserts found
    bool isOwned(const std::string &name) const;
    /// Whether @p name is a `weak` variable (of this scope or an enclosing
    /// one, up to the scope that declares it).
    bool isWeak(const std::string &name) const;
    ast::Type *lookupASTType(const std::string &name) const;
    void updateASTType(const std::string &name, ast::Type *newTy);
    void declare(const std::string &name, pir::LocalId local, ast::Type *astTy);
    /// A `weak` variable of type @p astTy (`T?` of a class), whose local
    /// (of type `weak`) is released with weak.release at scope exit.
    void declareWeak(const std::string &name, pir::LocalId local,
                     ast::Type *astTy);
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
    /// Mark @p name, declared in this scope, as storage the function never
    /// hands on: what it names outlives the function's own variables, which
    /// it may hand on at their last use (docs/pir.md §12).
    void markStable(const std::string &name) { StableVars.insert(name); }
    /// Whether the variable @p name is such storage (markStable).
    bool isStable(const std::string &name) const;
    Scope *findOwner(const std::string &name);
  };

  Scope *CurrentScope = nullptr;

  /// AST return type of the function being emitted (nullptr at top level).
  ast::Type *CurrentFuncReturnASTType = nullptr;

  /// ClassType of the method being emitted (nullptr otherwise).
  ast::ClassType *CurrentMethodClassType = nullptr;
  /// The method being emitted is that class's `__init__`.
  bool EmittingInit = false;
  /// The assignments of the `__init__` being emitted that give a field of
  /// its class's body its first value, in the object `new` zeroed: one of a
  /// reference field releases no old value (emitInitBody,
  /// lowerMemberAssignStmt).
  std::unordered_set<const ast::MemberAssignStmt *> FirstFieldStores;

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
  std::unordered_map<std::string, std::string> InternedEnumLayouts;
  /// Each tagged union's carrier (enumCarrier).
  std::unordered_map<const ast::EnumType *, ast::TupleType *> EnumCarriers;
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
  /// of another module's template that no dependency of this module has a
  /// copy of (typeOrigin), the module that emits it is this program's
  /// choice, so this module is not cached (docs/design/pkm.md §4.8).
  void noteExternUse(const std::string &name);
  /// The module whose class or struct @p type this module's externs name,
  /// or null.  For an instantiation (`Box<int>`) that is the first of this
  /// module's dependencies, nearest first, whose PIR has a copy of it:
  /// every program this module is in has that module, so this module's PIR
  /// does not depend on the program.  Otherwise, and for an instantiation
  /// no dependency has, it is the module that defines it in this program.
  const std::string *typeOrigin(const std::string &type);
  /// The instantiations typeOrigin found in a dependency.
  std::unordered_set<std::string> DependencyOrigins;
  /// typeOrigin's answers (pointers into module names and ClassOrigins,
  /// which do not move).
  std::unordered_map<std::string, const std::string *> TypeOrigins;
  /// The canonical names of the modules this one imports, in import order.
  std::vector<std::string> DirectImports;

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

  /// Saves and restores all per-function emission state around one body,
  /// which starts with the parameters' prologue (emitParamPrologue).
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
  /// A tagged union's variant made here: `E::B(x)`, or `E::A` of an enum
  /// with fields.
  static bool isVariantExpr(const ast::Expr *expr);
  ExprValue classifyExpr(ast::Expr *expr, const Val &val) const;
  void releaseIfOwned(const ExprValue &ev);

  /// The reads noteHeldReads and noteBorrowedReads found, which take a
  /// reference of their own.
  std::unordered_set<const ast::Expr *> HeldReads;
  /// The box reads of storage the function never hands on that
  /// noteBorrowedReads found in use where a call that may let go of
  /// references is made, though of none of their slot's class: read in
  /// place, the slot keeps their reference while they are used, and their
  /// load says so (`stable`, docs/pir.md §6), unless another call holds
  /// them (isStableRead).
  std::unordered_set<const ast::Expr *> StableReads;
  /// Note the reads among statement @p s's own expressions (not those of
  /// the statements in it) that a later part of the statement may replace
  /// while their value is in use: of a variable that holds a reference, in
  /// a statement that also passes it to an `inout` parameter or calls a
  /// method of its struct that may change it (ast::Expr::isChangedByCall).
  /// Such a read takes a reference of its own, released once it is used: a
  /// box, retained (exprProducesFreshBox), or a value held in memory,
  /// copied into a temporary (emitMemValue).
  void noteHeldReads(ast::Stmt *s);
  /// Note the reads among statement @p s's own expressions of storage the
  /// function does not own (an object's, or what an `inout` parameter, a
  /// value passed by address or a struct method's `self` names) that a call
  /// may replace while their value or storage is in use: one given an
  /// object, or that storage to change (LoweringHolds.cpp).  A box read is
  /// retained (exprProducesFreshBox); for a value held in memory, the
  /// object it is in is (MemTemps).
  void noteBorrowedReads(ast::Stmt *s);
  struct BorrowedReads;
  /// The variables noteHeldReads found the statement it last read may
  /// change (given to an `inout` parameter, or the receiver of a struct
  /// method that may change it): no read of their storage is lent.
  std::unordered_set<std::string> HeldVars;
  /// The box reads noteBorrowedReads found given to a lending callee
  /// (docs/pir.md §3) as a box of their own, a copy the callee may be lent
  /// instead (`retain lend`, docs/pir.md §6): of a field, an element, or
  /// what an `inout` parameter names, in storage of no variable of HeldVars.
  std::unordered_set<const ast::Expr *> LentReads;
  /// The reads of LentReads in use where a call is made that may let go of
  /// what they read: their copy is the caller's (not lent).
  std::unordered_set<const ast::Expr *> NotLendable;
  /// Whether the box read @p e, an argument of a lending callee (or the
  /// argument `Str(e)` is), may be lent: in LentReads, of none of whose
  /// calls in use it is NotLendable, and not held (isHeldRead).
  bool isLentRead(const ast::Expr *e) const;
  bool isHeldRead(const ast::Expr *e) const { return HeldReads.count(e) > 0; }
  bool isStableRead(const ast::Expr *e) const {
    return StableReads.count(e) > 0 && !isHeldRead(e);
  }
  /// Whether a value of type @p ty may reach an object (any class but Str,
  /// a function value, or what holds one): given it, a call can change
  /// whatever the object reaches.
  static bool reachesObject(const ast::Type *ty);
  /// This module's functions and the methods of the classes and structs it
  /// defines, by PIR name (`f`, `C.m`, `S.m`): the bodies effectOf reads.
  std::unordered_map<std::string, const ast::FuncDecl *> Bodies;
  /// The class or struct whose method each body of Bodies is.
  std::unordered_map<const ast::FuncDecl *, ast::Type *> BodyOwners;
  /// The function or method named @p name in Bodies, or null.
  const ast::FuncDecl *bodyOf(const std::string &name) const;

  /// What running some code may do to references held by storage it does
  /// not own (LoweringHolds.cpp, docs/pir.md §7): the classes whose
  /// references it may let go of, or not known.
  struct Effect {
    /// What it does is not known: it may let go of any reference.
    bool Unknown = false;
    /// It may let go of a reference at all: it stores one where its caller
    /// may hold one, or makes a call that may (the rule without the types,
    /// which a read rooted in the function's own variables keeps).
    bool Releases = false;
    /// The classes of the slots whose references it may let go of: the
    /// ones it may store into, and every one an object or a value it lets
    /// go of may hold (whose dying may let them go).
    std::unordered_set<const ast::Type *> Types;

    static Effect unknown() { return {true, true, {}}; }
    void add(const Effect &other);
  };
  /// What a call of @p fn does: Unknown for null or a function without a
  /// body (one another module compiles, a pure virtual method).
  const Effect &effectOf(const ast::FuncDecl *fn);
  struct EffectScan;
  /// effectOf's answers; Unknown for a function being read (a recursion).
  std::unordered_map<const ast::FuncDecl *, Effect> Effects;
  /// The classes letting go of a value of type @p ty may let go of: its
  /// own, and those of what it holds, and what dying objects of exact
  /// classes hold (isExactClass), or of closed classes and the classes
  /// derived from them (isClosedClass); Unknown for an object of another
  /// class, whose subclass may hold anything, and for a function value.
  const Effect &dropEffect(ast::Type *ty);
  std::unordered_map<const ast::Type *, Effect> DropEffects;
  /// A class no class derives from: Str, or an instance of a generic class
  /// (a superclass is never generic).
  static bool isExactClass(const ast::ClassType *ct);
  /// A class of the main module of a closed world (World::Closed): every
  /// class derived from it is one this module defines (Subclasses).
  bool isClosedClass(const ast::ClassType *ct) const;
  /// The canonical type of a slot holding @p ty: optionals stripped (a
  /// `T?` slot holds a T or None).
  ast::Type *slotType(ast::Type *ty);
  /// What the method @p name, run on an object of class @p ct, does: the
  /// one an `impl` block or, for an exact class, its vtable names, or for
  /// a closed class, that one or an override of a class derived from it;
  /// Unknown when another module compiles one of those or a subclass this
  /// module does not know may override it.
  Effect methodEffect(ast::ClassType *ct, const std::string &name);
  /// What the runtime's `toString` or `==` (@p method) of a value of type
  /// @p ty does: each object's in it.
  Effect valueMethodEffect(ast::Type *ty, const std::string &method);
  /// What the native function @p native does, given @p args: a Buffer
  /// function of the runtime's what kPaykanBufferFunctions says, any other
  /// nothing but through an `inout` argument, which makes it Unknown.
  Effect nativeEffect(const ast::FuncDecl &native,
                      const std::vector<ast::Expr *> &args);
  /// What constructing the class or struct @p name does (its `__init__`'s),
  /// or null when @p name is no class or struct.
  std::optional<Effect> constructorEffect(const std::string &name);
  /// What a call of the function or constructor @p name, given @p args,
  /// does: another module's native function (SemaContext::ImportedNatives)
  /// nothing but through an `inout` argument, as one of this module's.
  Effect calleeEffect(const std::string &name,
                      const std::vector<ast::Expr *> &args);
  /// The type of the values the private `Obj` field @p field of the struct
  /// @p st holds when that field is a runtime buffer: every store to it in
  /// the struct's methods is a new buffer of that type (PaykanBuffer_new)
  /// and every other use an argument of a Buffer function.  Null
  /// otherwise.
  ast::Type *bufferElementOf(ast::StructType *st, const std::string &field);
  struct BufferUses;
  /// bufferElementOf's answers, by `<struct>.<field>`.
  std::unordered_map<std::string, ast::Type *> BufferElements;
  /// The fields of classes that this module's code stores into once their
  /// objects are made (scanFieldStores, LoweringHolds.cpp).
  struct FieldStores {
    /// The code read does something the scan does not recognize: it may
    /// store into any field.
    bool Unknown = false;
    /// The fields stored into, as `<class>.<field>` of the class declaring
    /// each.
    std::unordered_set<std::string> Fields;
    /// The names of the fields a template stores into, whose types are not
    /// resolved in its body.
    std::unordered_set<std::string> Names;
  };
  FieldStores StoredFields;
  struct FieldStoreScan;
  /// The classes this module declares (its own and the instances of its
  /// templates), the fields of which isFixedField may know of.
  std::unordered_set<std::string> DeclaredClasses;
  /// Read the stores into class fields of every body of @p tu's module
  /// (Bodies) and, outside a program's main module, of its templates.
  void scanFieldStores(ast::TranslationUnit *tu);
  /// Whether the field @p field of class @p ct holds, for as long as its
  /// object lives, the reference its object's `__init__` gave it: a box
  /// field (not a weak one) of a class this module declares, which no code
  /// that may name it stores into but as `self.f = v` in an `__init__`'s
  /// own body.  Outside a program's main module, where an importer may
  /// name the class, only a private field's stores are all known.
  bool isFixedField(ast::ClassType *ct, const std::string &field);
  /// Whether the box read @p read is `self.f`, `self.f.g`, ... of fixed
  /// fields (isFixedField) in a class's method other than its `__init__`:
  /// the caller keeps `self` alive for the call, so no call it makes can
  /// let go of what the read finds.
  bool isFixedRead(const ast::Expr *read);
  /// The element @p elem of @p node, read in place from storage whose
  /// temporaries and holds MemTemps has past @p mark, which are released
  /// now: a box retained first when @p node is a +1 one
  /// (exprProducesFreshBox: a temporary's, a held read).
  Val ownElementRead(ast::Expr *node, const Val &elem, size_t mark);
  /// The variable whose own storage @p place is or is part of (`s`, `s.f`
  /// of a struct, `t.0`, `a[i]` of a fixed-size array), or null (a field of
  /// an object, a call's result).
  ast::Identifier *storageRoot(ast::Expr *place);
  /// @p val, the box of @p expr, as a reference of its own: a fresh +1 box
  /// as it is, a borrowed one retained, by a `retain lend` when @p lend
  /// and isLentRead(@p expr).
  Val takeSharedOwnership(ast::Expr *expr, const Val &val, bool lend = false);
  Val promoteIntToFloat(const Val &v, ast::Type *targetTy);
  /// @p v, computed for the numeric type @p ty, in the type's canonical form
  /// (docs/design/sized-types.md): a narrow integer's low bits sign- or
  /// zero-extended, a narrow float rounded; a constant is folded.  `int`,
  /// `unsigned`, `float` and every other type are left as they are.
  Val canonicalize(const Val &v, ast::Type *ty);
  /// The kind of the builtin type @p ty; Void for any other type.
  static ast::BuiltinType::Kind builtinKind(ast::Type *ty);
  /// The runtime function that prints a value of the builtin kind @p k.
  static const char *toStringRuntime(ast::BuiltinType::Kind k);
  /// The target and source of the conversion callee @p callee (`u8<int>`,
  /// `Str<float>`, `int<Str>`): false when it is not one (a generic class
  /// instantiation, say).
  bool conversionTypes(const std::string &callee, ast::Type *&target,
                       ast::Type *&source) const;
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
  /// The storage of a call's `inout` arguments (and of a struct receiver),
  /// by address and type: what an `inout` result of the call may be part of
  /// (takeInoutResult).
  using InoutPlaces = std::vector<std::pair<Val, ast::Type *>>;

  void emitRetain(const Val &shared, bool lend = false) {
    B.retain(shared, lend);
  }
  void emitRelease(const Val &shared) { B.release(shared); }
  Val emitSharedNew(const Val &raw, std::string name = "shared");
  Val emitSharedGet(const Val &shared, std::string name = "obj");
  Val emitUnwrappedRef(ast::Expr *expr, std::string name = "ref.obj");
  /// @p expr as a box of its own (a +1 one).  With @p lend, for a box
  /// argument with a bit in its lending callee's ownership mask
  /// (hasOwnershipBit), a copy of a variable that noteHeldReads does not
  /// hold, or of a read of LentReads, is a `retain lend` (docs/pir.md §6).
  Val emitAsShared(ast::Expr *expr, bool lend = false);
  /// Whether the parameter @p param of a function of signature @p sig that
  /// lends when @p lends says so (pir::lends, pir::lendsType) is a box with
  /// a bit in its ownership mask (docs/pir.md §3).
  static bool hasOwnershipBit(const pir::Signature &sig, bool lends,
                              size_t param);
  /// @p arg's box for a native function to borrow: a variable's or a
  /// field's as it is, anything else a +1 box added to @p keep, which the
  /// caller releases after the call.
  Val emitBorrowedBox(ast::Expr *arg, std::vector<Val> &keep);
  Val emitAsSharedRaw(ast::Expr *expr, bool lend);

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
  /// A tuple or a struct made the present value of its optional (Sema's
  /// coercion): held in memory as the optional's `(bool, T)`.
  static bool isMemOptionalWrap(const ast::Expr *expr);
  /// The type of the value @p e makes in memory: the optional a wrapped
  /// tuple or struct, or a None, becomes (Sema's coercion), else its own.
  ast::Type *memTypeOf(ast::Expr *e);
  /// Write the optional @p e (a tuple or a struct, isMemOptionalWrap) is
  /// wrapped into at @p dst: present, then @p e's value.
  void emitMemOptionalWrap(ast::Expr *e, const Val &dst);
  /// The expression whose value emitMemOptionalWrap is writing, which is
  /// not wrapped again.
  const ast::Expr *UnwrappedExpr = nullptr;
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
  /// The text of the optional value @p opt, of the AST type @p astTy when
  /// known (an `unsigned?` prints unsigned).
  Val emitOptionalToString(const Val &opt, ast::Type *astTy = nullptr);
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
  /// The byte offsets of the words of a @p ty value at @p base that are all
  /// 0 only in storage never set, its PaykanType's `nunset` words: none
  /// when all zero bytes are a value of @p ty.
  std::vector<int64_t> unsetWords(ast::Type *ty, int64_t base);
  /// The kind word of @p ty's PaykanType.
  unsigned char nativeTypeKind(ast::Type *ty) const;
  Val emitValueMatch(ast::MatchStmt *node, const Val &subjRaw);
  Val emitEnumMatch(ast::MatchStmt *node, const Val &subjRaw);
  void emitMatchSwitch(const Val &subj,
                       const std::vector<std::pair<Val, ast::MatchArm *>> &arms,
                       ast::MatchArm *wildcard);
  /// Emit what `if (check0) body0 else if (check1) body1 ... else default`
  /// does, flat: the index of the first check that holds, the checks after
  /// it not evaluated, then a switch on it.
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
    /// consumes it.  A chain's links below @p node are emitted first,
    /// innermost out (emitChainBelow), so that emitting a chain of any
    /// length recurses no deeper than a few links.
    Val visit(ast::Expr *node);

    /// The expression the visitor of the chain link @p e passes to visit()
    /// before it emits anything else, or null: a binary operator's left
    /// operand (but `==`'s and `!=`'s, which may compare in place); a class's
    /// or a builtin type's receiver, or a class's field's; and for a struct's
    /// method or field, a tuple index, a subscript of a fixed-size array or
    /// a call of a function value, what the operand's bytes are found from
    /// (memValueVisitsFirst).  Each mirrors its visitor's branches.
    ast::Expr *visitedFirst(ast::Expr *e);
    /// When the link @p e visits a link first: emit the links below @p e
    /// that are visited so, innermost first, and keep their values in
    /// ChainValues for the visit that comes to each.  That is the order
    /// they are emitted in anyway.
    void emitChainBelow(ast::Expr *e);
    /// The values of the links emitChainBelow emitted and nothing has taken
    /// yet.
    std::unordered_map<const ast::Expr *, Val> ChainValues;

    /// The optional value of the runtime's parse of the string object
    /// @p str into @p target, the core of `s.toInt()` & co.; @p owned, a
    /// temporary @p str is part of, is released after the parse.
    Val emitParseOf(const Val &str, const ExprValue &owned,
                    ast::BuiltinType::Kind target);

    Val emitIdentityCtor(ast::CallExpr *node);
    Val emitConversion(ast::CallExpr *node);
    Val emitBuiltinCall(ast::CallExpr *node);
    /// The arguments of the method call @p node for @p method, whose PIR
    /// signature is @p sig (the receiver first, at index 0), appended to
    /// @p args: a slice as two values, a value held in memory by address,
    /// an `inout` argument as its address (its kept objects in @p keep, and
    /// its storage in @p places when the method returns `inout`), a
    /// reference as a +1 box when the method is @p userDefined (a builtin
    /// borrows it), the owned temporaries among the rest in @p ownedArgs.
    /// False when an argument failed.
    bool emitMethodArgs(ast::MethodCallExpr *node,
                        const ast::MethodDecl *method,
                        const pir::Signature &sig, bool userDefined,
                        std::vector<Val> &args,
                        std::vector<ExprValue> &ownedArgs,
                        std::vector<Val> &keep, InoutPlaces &places);
    /// A call of a method of the struct @p st (LoweringStruct.cpp): the
    /// receiver's address, then the arguments; @p wantAddress as for a
    /// call's `inout` result.
    Val emitStructMethodCall(ast::MethodCallExpr *node, ast::StructType *st,
                             bool wantAddress);
    /// visitMethodCallExpr's kinds of call, each in a frame of its own: a
    /// field's function value, `hash()`, a Str's parse into @p target
    /// (`s.toInt()` & co.), an `unsigned`'s `toInt()`, an optional's
    /// `toString()`, a tuple's, a struct's, an enum's or a builtin type's,
    /// and a class's method.
    Val emitFieldCall(ast::MethodCallExpr *node);
    Val emitHashCall(ast::MethodCallExpr *node);
    Val emitStrParseCall(ast::MethodCallExpr *node,
                         ast::BuiltinType::Kind target);
    Val emitUnsignedToInt(ast::MethodCallExpr *node);
    Val emitOptionalToStringCall(ast::MethodCallExpr *node);
    Val emitShowCall(ast::MethodCallExpr *node);
    Val emitClassMethodCall(ast::MethodCallExpr *node, bool wantAddress);
    Val emitShortCircuit(ast::BinaryExpr *node, bool isAnd);
    /// The panics before an integer `/` or `%`: a zero divisor, and for a
    /// division of `int` values (@p checkOverflow) INT64_MIN / -1.
    Val emitIntDivGuards(const Val &lhs, const Val &rhs, bool isDiv,
                         bool checkOverflow);

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
  /// The body of @p fn, the function of @p ct's pure virtual @p method:
  /// Paykan_panic_pure_virtual, then unreachable.
  void emitPureVirtualTrap(pir::Function *fn, ast::ClassType *ct,
                           const std::string &method);
  /// The @p body of @p ct's `__init__`, whose `self` is the local @p self:
  /// its `__super__(...)` call, then the rest, with `vtable.store` of @p ct
  /// after the call when no field of @p ct needs assigning (else after the
  /// assignment Sema marks completesInit, lowerMemberAssignStmt).
  void emitInitBody(ast::CompoundStmt *body, ast::ClassType *ct,
                    pir::LocalId self);
  Val lowerMemberAssignStmt(ast::MemberAssignStmt *node);
  Val lowerMemberAccessExpr(ast::MemberAccessExpr *node);
  /// Runtime symbol implementing @p name for the builtin class @p ct, or "".
  const char *builtinMethodSymbol(ast::ClassType *ct, const std::string &name);

  // -- Function values (LoweringClosure.cpp,
  // docs/language/14-function-types.md)

  /// A function value is held in place as the tuple `(int, Obj?)` this
  /// returns: its code's address (as an i64), then the box of its closure,
  /// the object holding its captures, or null when it captures nothing.  An
  /// optional function value is the same, None a zero code word.  The
  /// layout, retains, releases, equality and type descriptors of function
  /// values are this tuple's.
  ast::TupleType *functionValueShape();
  /// Where a function value's code word is in functionValueShape(): never
  /// 0 in a value, so a type descriptor names it among the words that tell
  /// storage never set (emitTypeDescriptor).
  static int64_t functionCodeOffset();
  /// What a value of @p ty held in memory is laid out as: @p ty itself, or
  /// a tuple for a function value (functionValueShape()), for the optional
  /// of a tuple, a struct or a tagged union (`(bool, T)`: present, then the
  /// value, None all zeros) and for a tagged union (enumCarrier()).
  ast::Type *memShapeOf(ast::Type *ty);
  /// Where the value of the optional tuple or struct @p optTy starts.
  int64_t memOptionalPayloadOffset(ast::Type *optTy);
  /// Whether the optional tuple, struct or function value at @p addr (of
  /// type @p optTy) is None.
  Val emitMemOptionalIsNone(const Val &addr, ast::Type *optTy);

  /// The code of each lambda (its `call` function, made once), and of each
  /// named function used as a value (a function forwarding to it).
  std::unordered_map<const ast::LambdaExpr *, std::string> LambdaCode;
  std::unordered_map<std::string, std::string> FunctionRefCode;
  unsigned NextClosureId = 0;
  /// The signature of the code of a function value of type @p fnt: the
  /// closure object (null when nothing is captured), then the parameters as
  /// a method's, a result held in memory written through a trailing pointer.
  pir::Signature closureSignature(ast::FunctionType *fnt);
  /// A closure class @p name with a field per capture and an object's
  /// vtable [destroy, toString, equals] (the destructor declared, its body
  /// to come).  @p captureWeak marks the captures of `weak` variables: a
  /// weak field of the closure, loaded as a strong reference for a call's
  /// duration.
  pir::Class *declareClosureClass(const std::string &name,
                                  const std::vector<ast::Type *> &captureTys,
                                  const std::vector<bool> &captureWeak);
  void emitClosureDestructor(const std::string &name,
                             const std::vector<ast::Type *> &captureTys,
                             const std::vector<bool> &captureWeak);
  /// The code of lambda @p node (and its closure class, when it captures
  /// anything), made once: the code's function name.  Its body is lowered
  /// before this returns, unless this is in another lambda's body: then
  /// after that body (PendingLambdas).
  const std::string &lowerLambda(ast::LambdaExpr *node);
  /// The body of the lambda @p node, which lowerLambda declared.
  void emitLambdaBody(ast::LambdaExpr *node);
  /// The lambdas whose bodies are lowered next, in order, while
  /// LoweringLambdaBodies: each one met in a body lowered before it.
  std::vector<ast::LambdaExpr *> PendingLambdas;
  bool LoweringLambdaBodies = false;
  /// Write a function value of lambda @p node to @p dst: its code, and a new
  /// closure with its captures copied in (owned by @p dst).
  void emitLambdaInto(ast::LambdaExpr *node, const Val &dst);
  /// The code forwarding to the function @p fnName (made once): its name.
  const std::string &lowerFunctionRef(const std::string &fnName,
                                      ast::FunctionType *fnt);
  /// Write the function value of the named function @p fnName to @p dst:
  /// its forwarding code and no closure.
  void emitFunctionRefInto(const std::string &fnName, ast::FunctionType *fnt,
                           const Val &dst);
  /// Call the function value at @p addr (whose closure the call keeps for
  /// its duration) of type @p fnt with @p argExprs; @p resultTy is the
  /// call's resolved type.
  Val emitClosureCall(const Val &addr, ast::FunctionType *fnt,
                      const std::vector<ast::Expr *> &argExprs,
                      ast::Type *resultTy);
  /// Call the function value @p callee (an expression of function type)
  /// with @p args; @p resultTy is the call's resolved type.
  Val emitFunctionValueCall(ast::Expr *callee,
                            const std::vector<ast::Expr *> &args,
                            ast::Type *resultTy);
  /// Whether the optional function value at @p addr is None.
  Val emitFunctionValueIsNone(const Val &addr);
  /// `match` on an optional held in memory: a function value's, a tuple's
  /// or a struct's.
  Val emitMemOptionalMatch(ast::MatchStmt *node);

  // -- Structs (LoweringStruct.cpp, docs/language/11-structs.md)
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
  /// The expression emitMemValue(@p e) passes to visit() before it emits
  /// anything else, or null: a call itself, the object of a class's field,
  /// and for an element or a struct's field, the same for the whole it is
  /// in.  Null for a call with an `inout` result held in memory, which
  /// leaves what holds its storage in MemTemps for the consumer, past the
  /// mark that consumer took before it (ExprEmitter::emitChainBelow).
  ast::Expr *memValueVisitsFirst(ast::Expr *e);
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
  /// The value held in memory that @p e is a part of, when it is one, read
  /// in place: a struct's field (`s.f`), a tuple's element (`t.0`) or an
  /// element of a fixed-size array or a slice (`a[i]`); else null.
  ast::Expr *wholeOf(ast::Expr *e);
  /// The address of the part @p e (wholeOf) of the value at @p whole: its
  /// element's or field's offset, an index after its bounds check.
  Val emitPartAddress(ast::Expr *e, const Val &whole);
  /// emitFixedElementAddress, and emitTupleElementAddress, of the array or
  /// the tuple at @p base.
  Val elementAddressIn(ast::Expr *array, ast::Expr *index, const Val &base);
  Val tupleElementIn(ast::Expr *tuple, size_t index, const Val &base);
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
  /// (zero without one), declared in the current scope: its local.
  pir::LocalId declareMemVar(const std::string &name, ast::Type *ty,
                             ast::Expr *init);
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
  /// box borrowed from its slot, by a `stable` load when @p stable
  /// (isStableRead).
  Val emitElementLoad(const Val &addr, ast::Type *ty, bool stable = false);
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
  /// Append to @p words the layout of the tuple or struct @p tt
  /// (emitTupleLayout).
  void appendTupleLayout(std::vector<int64_t> &words, ast::TupleType *tt);
  /// Append to @p words the layout of the enum @p et (Runtime.h, "Enums").
  void appendEnumLayout(std::vector<int64_t> &words, ast::EnumType *et);
  /// The data global holding @p words, one per distinct layout, named
  /// @p prefix and a number.
  Val internLayout(std::unordered_map<std::string, std::string> &interned,
                   const char *prefix, std::vector<int64_t> words);

  // -- Enums (LoweringEnum.cpp, docs/language/07-enums.md)
  /// What the tagged union @p et is laid out as: its tag, an int, then each
  /// variant's fields, a struct, in place (a variant without fields has
  /// none).  Only the current variant's are set; the others are all zeros,
  /// so a copy's retains and a release's releases reach every variant's
  /// references alike (null ones are skipped).
  ast::TupleType *enumCarrier(ast::EnumType *et);
  /// Where the fields of variant @p i of the tagged union @p et are.
  int64_t variantPayloadOffset(ast::EnumType *et, size_t i);
  /// Write the variant the call @p call makes (`E::V(x, y)`) to @p dst.
  void emitVariantInit(ast::CallExpr *call, const Val &dst);
  /// The layout of the enum @p et the runtime reads, as constant data.
  Val emitEnumLayout(ast::EnumType *et);
  /// The text of the enum value @p e (PaykanEnum_toString), a new string.
  Val emitEnumToString(ast::Expr *e);
  /// The text of the enum held at @p addr, as emitEnumToString.
  Val emitEnumToStringAt(const Val &addr, ast::EnumType *et);
  /// Whether the tagged unions @p a and @p b are equal (PaykanEnum_equals).
  Val emitEnumEquals(ast::Expr *a, ast::Expr *b);
  /// `match` on a tagged union: a switch on its tag, each arm's binding the
  /// variant's fields.
  Val emitPayloadEnumMatch(ast::MatchStmt *node);
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
  /// The start of the body of @p fn, when it lends (docs/pir.md §3): for
  /// each box parameter %p with a bit in the ownership mask, `%p.own =
  /// owned %p`, `%p.lent = not %p.own` and `if %p.lent { retain %p }`, so
  /// that the body owns every parameter, as it did before the mask,
  /// whether its caller handed it over or lent it.
  void emitParamPrologue(const pir::Function &fn);
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
  /// The box in variable @p name's storage, borrowed (holdsBox); an
  /// `inout` parameter's slot is read by a `stable` load when @p stable
  /// (isStableRead).
  Val loadVarBox(const std::string &name, bool stable = false);
  /// Put the +1 box @p newBox in variable @p name's storage (holdsBox),
  /// releasing the box it held.
  void storeVarBox(const std::string &name, const Val &newBox);
  /// A fresh +1 box (or null) loaded from the `weak` variable @p name.
  Val loadWeakVar(const std::string &name);
  /// Make the `weak` variable @p name refer to what the +1 box @p box names
  /// (consuming @p box), dropping the weak reference it held.
  void storeWeakVar(const std::string &name, const Val &box);
  /// Whether @p e reads a `weak` field of a class (`obj.next` with `weak
  /// next: Node?`): such a read is a fresh +1 box, like a call's result.
  bool isWeakFieldAccess(const ast::Expr *e) const;
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
  /// The `inout` result @p addr of @p call, in storage that the boxes
  /// @p keep and @p recv and MemTemps past @p mark hold, or in an object
  /// held where one of @p places is (the storage of the call's `inout`
  /// arguments, and of a struct receiver): used where it is (@p wantAddress,
  /// or a value held in memory), they go to MemTemps for its user to
  /// release, with what @p places hold (holdObjectsIn); read, they are
  /// released once it is.
  Val takeInoutResult(const Val &addr, ast::Expr *call, bool wantAddress,
                      const std::vector<Val> &keep, size_t mark,
                      const InoutPlaces &places,
                      const ExprValue &recv = ExprValue());
  /// Hold in MemTemps what the storage of type @p ty at @p addr holds that
  /// reaches an object, as it is now: the object a reference there names
  /// (a +1 box), or a copy of a value held in memory, its references
  /// retained.
  void holdObjectsIn(const Val &addr, ast::Type *ty);
  /// Hold each box of @p keep, the object of a field a borrow @p name
  /// names, and what MemTemps past @p mark holds, in hidden owned locals of
  /// the current scope: a field's object stays alive as long as the borrow
  /// can be used.
  void holdObjects(const std::string &name, std::vector<Val> keep, size_t mark);
  /// An `inout` local @p name, of type @p astTy, for the variable or field
  /// @p place.
  void emitInoutBinding(const std::string &name, ast::Expr *place,
                        ast::Type *astTy);
  /// Whether an arm of @p node binds `n: inout T`.
  static bool hasInoutArm(const ast::MatchStmt *node);
  /// The storage of @p subject, the subject of a `match` with an
  /// `n: inout T` arm: a local holding its address, found once, before any
  /// arm runs, so that a call on the way to it runs once and n goes back
  /// where the matched value was read.  The objects it is in are held by
  /// hidden locals of the current scope, the match's, until the match ends.
  /// None when it could not be found (an internal error, reported).
  std::optional<pir::LocalId> emitMatchPlace(ast::Expr *subject);
  /// The subject of @p node, a value held in memory, copied to @p dst, its
  /// references retained; with an `n: inout T` arm, read from its storage,
  /// which @p place then holds the address of (emitMatchPlace).  False when
  /// that could not be found.
  bool emitMemMatchSubject(ast::MatchStmt *node, const Val &dst,
                           std::optional<pir::LocalId> &place);
  /// @p arm's `n: inout T`, n already bound to its own copy of the matched
  /// value (the local @p binding, of type @p bindTy; the part at @p offset
  /// after @p header of a value held in memory): n goes back to the
  /// subject's storage, of type @p placeTy, whose address @p place holds
  /// (emitMatchPlace), after its last use, or on a way out of the arm's
  /// scope before it (Scope::WriteBack).
  void bindInoutArm(const ast::MatchArm *arm, pir::LocalId binding,
                    pir::LocalId place, ast::Type *placeTy, ast::Type *bindTy,
                    int64_t offset = 0, const Val &header = Val());
  /// The body of @p arm, in the arm's scope: an `inout` binding bound by
  /// bindInoutArm is written back after the statement of its last use
  /// (MatchArm::bindingUses), so what the arm does to the subject after
  /// that stays.
  void emitArmBody(const ast::MatchArm *arm);
  /// Assign an `inout` arm's binding back to its subject (Scope::WriteBack).
  void emitWriteBack(const Scope::WriteBack &wb);
  /// Release the boxes emitInoutArg kept for a call that has returned.
  void releaseAfterCall(const std::vector<Val> &keep);
  /// For a call of a method of @p ct, a class defined in another module:
  /// an internal error unless @p sig is the signature of vtable slot
  /// @p slot there (so no caller passes a value where an address is due).
  bool checkImportedSlot(ast::ClassType *ct, uint32_t slot,
                         const pir::Signature &sig);
};

} // namespace paykan::lowering
