// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Semantic analysis pass for the Paykan language

#pragma once

#include "ASTContext.h"
#include "ASTVisitor.h"
#include "DiagEngine.h"

#include "StringMap.h"
#include "paykan/Status.h"
#include "paykan/pir/PIR.h"
#include "paykan/pkm/Interface.h"

#include <array>
#include <cassert>
#include <deque>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
  /// The module's instantiations of other modules' templates, checked here
  /// but not part of the module: the lowering emits each once per program
  /// (docs/design/pkm.md §4.5).
  std::vector<ast::ClassDecl *> ForeignClasses;
  std::vector<ast::FuncDecl *> ForeignFunctions;
  std::vector<ast::StructDecl *> ForeignStructs;
  /// A file an AST's source locations point into: its name as diagnostics
  /// show it, and its lines (null when its source is not at hand, as for
  /// the templates of a module file), which Owner keeps alive.  For an
  /// instantiation of another module's template, Module is that module's
  /// canonical name, which qualifies its functions where the bodies call
  /// them (`lib::m::f`, written `f` in the file); empty for the module's
  /// own file.
  struct SourceFile {
    std::string Name;
    const std::vector<std::string> *Lines = nullptr;
    std::shared_ptr<void> Owner;
    std::string Module;
  };
  /// The file each of ForeignClasses, ForeignFunctions and ForeignStructs
  /// points into: its template's module's, whose locations a clone keeps.
  std::unordered_map<const ast::Decl *, SourceFile> ForeignSources;
  /// The resolver Sema made when its caller set none (one without a
  /// cache), which the lowering then reads the modules loaded from `.pkm`
  /// files through; null when the caller's was used.
  std::shared_ptr<modules::ModuleResolver> Resolver;
  /// The names the module calls other modules' native functions by
  /// (`lib::f`, and the hidden qualifier's for what their templates call),
  /// as their interfaces say (FunctionSig::Native): what such a call does
  /// is known though another module compiles it.
  StringSet ImportedNatives;
  /// Copies of the instantiations of other modules' templates that the
  /// modules this one imports export and its code uses (the `List<Str>` of
  /// `collections::split`): checked here from their templates, though the
  /// program links the exporting module's, so that the lowering reads what
  /// their methods do as it reads its own copies' (docs/design/pkm.md
  /// §4.8).  Nothing of them is lowered.
  std::vector<ast::ClassDecl *> ImportedInstanceClasses;
  std::vector<ast::StructDecl *> ImportedInstanceStructs;
  /// The module's own file, which its other declarations' locations point
  /// into (the lowering's warnings name it).
  SourceFile Source;

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

  /// What restricts a variable beyond its type: nothing, `let` (it cannot be
  /// reassigned), or a parameter mode: `view` (it cannot be changed) or
  /// `inout` (it is the caller's storage).
  /// View is a `view` parameter, ViewLocal a `view` local, ViewBinding a
  /// `match` arm's name for (part of) a `view`.
  enum class VarKind : uint8_t {
    Plain,
    Let,
    View,
    ViewLocal,
    ViewBinding,
    Inout,
    /// A variable of an enclosing function, captured by copy by a lambda
    /// (docs/language/14-function-types.md): read-only inside it.
    Captured,
    /// A `weak` variable of an enclosing function, captured weakly by a
    /// lambda: read-only inside it, as Captured is, and captured weakly by a
    /// lambda inside it, as Weak is.
    CapturedWeak,
    /// A `weak` local (docs/language/16-memory.md): it holds a weak
    /// reference, so no `inout` may name it.
    Weak,
    /// A `weak` local first read from a `view` (a cursor over `self`'s nodes
    /// in a `view fn`): as Weak, and each read of it is a `view` as well, so
    /// nothing changes through it and nothing read from it is stored or
    /// returned except as a `view`.  It may be assigned other `view`s.
    WeakView,
    /// The variable of a `for` loop (docs/language/02-basics.md, "for"):
    /// read-only, as a `let` is, so the loop's values are the range's.
    ForVar
  };

  /// A single lexical scope. Each scope has its own local bindings and a
  /// pointer to its enclosing (parent) scope.
  struct Scope {
    Scope *Parent = nullptr;
    StringMap<ast::Type *> Locals;
    /// The kind of each variable bound here that is not Plain.
    StringMap<VarKind> Kinds;
    /// The subject a ViewBinding names a `view` of: a `view` result may be
    /// part of it.
    StringMap<const ast::Expr *> ViewSubjects;

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

  // -- Lambdas (SemaLambda.cpp, docs/language/14-function-types.md)

  struct ClassContext;
  struct FunctionSig;
  struct MethodUse;
  struct Borrow;
  /// A lambda whose body is being checked: its node, the scope its
  /// parameters live in (the names used inside that resolve outside it are
  /// captured, and declared there as Captured copies), and the method
  /// context it was written in (whose `self` it may capture).
  struct LambdaContext {
    ast::LambdaExpr *Node;
    Scope *Boundary;
    const ClassContext *Outer;
    /// The enclosing method's use of `self`: capturing `self` counts as
    /// changing it (the closure may, later).
    MethodUse *OuterUse;
    /// The local borrows live where the lambda is written: capturing a
    /// variable uses it, and copies a reference it holds.
    std::vector<Borrow> OuterBorrows;
  };
  std::vector<LambdaContext> LambdaStack;
  /// Check a lambda: its signature, its body as a function's, and the
  /// variables it captures.  Its function type, or nullptr after an error.
  ast::Type *checkLambda(ast::LambdaExpr *node);
  /// The result type of the checked lambda @p node written
  /// `fn(...) => expression`: its expression's type, written back as its
  /// result type; nullptr (after an error) when the expression has none.
  ast::Type *inferredResult(ast::LambdaExpr *node);
  /// Record that the enclosing lambdas capture the variable @p name (of type
  /// @p ty, bound in @p owner) when it is bound outside them; false (after
  /// reporting it) when it cannot be captured.
  bool captureVariable(const std::string &name, ast::Type *ty, Scope *owner,
                       ast::SourceLocation loc);
  /// The function type a function value of signature @p sig has.
  ast::FunctionType *functionTypeOf(const FunctionSig &sig);
  /// Whether the function @p name may be a value (`f = square;`): a
  /// function of the program with a signature a function type can spell;
  /// reports why not otherwise.
  bool checkFunctionValue(const std::string &name, const FunctionSig &sig,
                          ast::SourceLocation loc);
  /// Reports, at @p loc, that the generic function @p name (`g`, `io::println`)
  /// cannot be a function value, and returns false.
  bool errorGenericFunctionValue(const std::string &name,
                                 ast::SourceLocation loc);
  /// Check @p call, a call of the function value of type @p fnt (`f(args)`,
  /// a call through a variable or a field) named @p what in diagnostics:
  /// the arguments' types and modes.  The result type, or nullptr.
  ast::Type *checkFunctionValueCall(ast::FunctionType *fnt,
                                    const std::vector<ast::Expr *> &args,
                                    const std::string &what,
                                    const ast::Expr *call);

  /// The kind of variable @p name where it is bound (innermost first).
  VarKind varKind(std::string_view name) const;
  /// Error at @p loc (and false) unless variable @p name may be assigned.
  bool checkReassignable(const std::string &name, ast::SourceLocation loc);

  /// The expected return type of the current function (nullptr = top-level /
  /// void).
  ast::Type *CurrentReturnType = nullptr;
  /// How the current function returns its result, and the function and the
  /// scope of its parameters (and `self`), which a borrowed result must come
  /// from.
  struct ResultContext {
    ast::ParamMode Mode = ast::ParamMode::Value;
    const ast::FuncDecl *Fn = nullptr;
    const Scope *Params = nullptr;
  };
  ResultContext CurrentResult;

  /// Loop nesting depth (0 = not inside a loop).
  unsigned LoopDepth = 0;

  // -- Class analysis context
  //
  // Populated while visitClassDecl is running; nullptr outside of a class.
  //
  struct ClassContext {
    ast::ClassType *ClassType = nullptr; // the class being checked, or
    ast::StructType *Struct = nullptr;   // the struct (one of the two)
    std::string MethodName;  // method currently being checked (empty = none)
    bool ViewMethod = false; // it is a `view fn`: `self` is read-only
    bool SuperInitRequired = false; // __init__ must call __super__
    /// The `__super__(...)` that is `__init__`'s first statement: the one
    /// call it may make (null when it does not begin with one).
    const ast::CallExpr *FrontSuperCall = nullptr;
  };

  ClassContext *CurrentClassCtx = nullptr;

  /// What a method body does to `self`, for the warning on a method that
  /// never changes it but is not a `view fn` (SemaViewFn.cpp).
  struct MethodUse {
    ast::ClassType *Class = nullptr;   // the method's class, or
    ast::StructType *Struct = nullptr; // its struct
    const ast::FuncDecl *Decl = nullptr;
    bool ChangesSelf = false; // writes `self`, calls a changing method on
                              // something reached through it, or returns
                              // part of it for its caller to change
    /// Returns part of `self` that holds a type argument (`self.v` of a
    /// `T` field): another instance's caller could change it, so whether
    /// the template's method could be a `view fn` depends on the instance.
    bool ReturnsTypeArgument = false;
    std::vector<std::string> SelfCalls; // `self.m()` of a non-`view fn` m
  };
  std::deque<MethodUse> MethodUses; // stable addresses for CurrentMethodUse
  /// The use being recorded: the method body being checked (null outside).
  MethodUse *CurrentMethodUse = nullptr;

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
    /// The parameters' modes: a user function's, or a constructor's
    /// `__init__`'s.
    ast::ParamModes Modes;
    /// A `native fn`, this module's or (as its interface says) an imported
    /// one: its body is C, which keeps nothing it is lent and changes only
    /// what its `inout` arguments name (docs/language/03-functions.md,
    /// "Native Functions").
    bool Native = false;
  };

  /// Maps function names to their signatures.
  StringMap<FunctionSig> FunctionTable;

  /// Register a function signature.
  void declareFunction(std::string_view name, ast::Type *retTy,
                       std::vector<ast::Type *> paramTys,
                       bool isBuiltin = false);

  /// Look up a function signature, or nullptr if unknown.
  const FunctionSig *lookupFunction(std::string_view name) const;

  // -- Inferred results (`fn double(x: int) => x * 2;`,
  // docs/language/03-functions.md, "Expression bodies")
  //
  // A free function written with an expression and no result type is
  // registered with no result type; its body is checked before any other
  // body (inferResults), the result type taken from its expression and
  // written back.  One whose expression uses another such function infers
  // that one first, with the context of a body of its own; one whose
  // expression depends on itself is an error.  An instantiation of a
  // generic one is inferred where it is first used, as its body would be
  // checked at the end (checkPendingInstantiations).
  enum class Inference : uint8_t { Pending, InProgress, Done, Failed };
  struct PendingInstantiation;
  struct InferredFunction {
    ast::FuncDecl *Decl = nullptr;
    Inference State = Inference::Pending;
    /// An instantiation's: how its body is checked (null otherwise).
    std::shared_ptr<PendingInstantiation> Instance;
  };
  StringMap<InferredFunction> InferredFunctions;
  /// Set once every free function's signature is registered: inference can
  /// start.
  bool InferenceReady = false;
  /// Check the bodies of the functions in @p fns with an inferred result.
  bool inferResults(const std::vector<ast::FuncDecl *> &fns);
  /// Whether the function named @p name has a result type to use at @p loc:
  /// true unless it is a function with an inferred result whose inference
  /// failed (reported) or depends on itself (reported here).  Infers it
  /// first when it has not been yet.
  bool inferResultOf(const std::string &name, ast::SourceLocation loc);
  /// The type of the value the one `return` of @p body returns, for a
  /// function or a lambda written `=> expression`; nullptr after an error
  /// (an expression with no value: @p subject is what its result is, and
  /// @p braces the form to write instead).
  ast::Type *returnedType(ast::CompoundStmt *body, const std::string &subject,
                          const std::string &braces);
  /// The checked function @p node's inferred result type, checked as a
  /// written one is and written back (to the declaration and its
  /// signature); nullptr after an error.
  ast::Type *inferredResultOf(ast::FuncDecl *node);
  /// A method, or a generic function, written `=> expression` needs its
  /// result type: report one with none (and give it the poison type, so its
  /// uses are not reported again).
  bool rejectInferredMethods(ast::TranslationUnit *tu);
  void reportInferredMethod(const ast::FuncDecl *fn);
  /// Queue the instantiation @p p for its body to be checked, and record
  /// how, for one whose result is inferred.
  void queueInstantiation(PendingInstantiation p);

  /// The kind of top-level entity being declared, for checkDeclNameAvailable.
  enum class DeclKind { Function, Class, Enum, Trait, Struct };

  /// Top-level declarations — functions, classes and structs (whose
  /// constructors are called by their name), and enums — share one namespace
  /// with the compiler builtins: a name identifies exactly one entity.
  /// Returns true if @p name is free; otherwise emits a diagnostic at @p loc
  /// naming the existing entity (builtin function / builtin class / class /
  /// struct / enum / function) and returns false.
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

  // Type equality.  Resolved types are canonical (ASTContext interns them),
  // so this is pointer identity with a structural fallback.
  static bool typesEqual(ast::Type *a, ast::Type *b);

  // Returns true if a value of type `src` can be assigned to a location of
  // type `dst`.  This includes exact match, int->float promotion, ClassType
  // subtyping, and the optional-type rules (`T` -> `T?`, `S?` -> `T?` when
  // `S` -> `T`, `T?` -> `Obj`; never `T?` -> `T`).
  bool isAssignable(ast::Type *dst, ast::Type *src) const;

  /// The type the two operands of a binary operator, of the different types
  /// @p lhs and @p rhs, are both converted to, or nullptr when they have
  /// none: `float` for `int` with `float`, either way round, the one
  /// promotion between operands (docs/language/02-basics.md, "Sized numeric
  /// types").  Arithmetic, the ordering compares and `==` / `!=` all ask
  /// this one rule.
  ast::Type *promotedOperandType(const ast::Type *lhs,
                                 const ast::Type *rhs) const;

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
  // A class that lost its superclass (or a subclass of one) against a class
  // that can have subclasses is not checked: the error that lost it the
  // superclass was reported, and the mismatch counts as a suppressed
  // follow-on (lostSuperclassMismatch, SW-32).
  bool checkAssignable(ast::Type *dst, ast::Type *srcTy, ast::Expr *src);
  /// Whether @p srcTy, which is not assignable to @p dst, might be had each
  /// class on its class's superclass chain kept its superclass (one lost
  /// it, SuperclassLost): @p dst is a class that can have subclasses, or an
  /// optional of one, and @p srcTy a class, or an optional of one into an
  /// optional.
  bool lostSuperclassMismatch(ast::Type *dst, ast::Type *srcTy) const;

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

  // Emit the unwrap diagnostic for an operation that is not defined on an
  // optional value (member access, method call, subscript, arithmetic, …).
  void errorOptionalUnwrap(ast::SourceLocation loc, ast::OptionalType *optTy);
  /// Report `x.toString()` on @p ty, a tuple, struct, enum or optional with
  /// no text, @p why being noTextReason's.
  void errorNoToString(ast::SourceLocation loc, ast::Type *ty,
                       const std::string &why);

  // Returns the lowest common ancestor in the class hierarchy of two class
  // types, or nullptr if they share no common ancestor.
  static ast::ClassType *findLowestCommonAncestor(ast::ClassType *a,
                                                  ast::ClassType *b);

  /// What a type resolveType resolves is, the start of its diagnostics
  /// ("field 'n' in class 'L1<int>'", and " element 2" for that type's
  /// tuple element): its pieces are joined only when one is reported, as
  /// an instantiation's name in them can be long (SW-04).  It lives for a
  /// resolveType call: a nested type's points at its parent's, and a piece
  /// views text that outlives the call.
  class TypeContext {
  public:
    // A description whole; implicit, so a call site passes a string.
    TypeContext(std::string text) : Text(std::move(text)) {}
    TypeContext(const char *text) : Text(text) {}
    /// @p pieces, joined.
    TypeContext(std::initializer_list<std::string_view> pieces)
        : TypeContext(nullptr, pieces) {}
    /// @p parent's description, then @p pieces.
    TypeContext(const TypeContext &parent,
                std::initializer_list<std::string_view> pieces)
        : TypeContext(&parent, pieces) {}
    TypeContext(const TypeContext &) = delete;
    TypeContext &operator=(const TypeContext &) = delete;
    std::string str() const;

  private:
    TypeContext(const TypeContext *parent,
                std::initializer_list<std::string_view> pieces);
    static constexpr size_t kMaxPieces = 7;
    const TypeContext *Parent = nullptr;
    std::string Text;
    std::array<std::string_view, kMaxPieces> Pieces{};
    size_t NumPieces = 0;
  };
  // Resolve a declared AST Type* to its canonical equivalent from
  // ASTContext (e.g. a BuiltinType(Int) node -> Ctx.getIntTy(), a
  // ClassType("Str") -> Ctx.getStrTy()).  Returns nullptr and
  // emits an error on failure.
  ast::Type *resolveType(ast::Type *ty, ast::SourceLocation loc,
                         const TypeContext &context);
  // resolveType for what holds a value (a variable, a parameter, a field):
  // `void` has none, so it is reported (`<context> cannot have type
  // 'void'`) and nullptr returned.
  ast::Type *resolveValueType(ast::Type *ty, ast::SourceLocation loc,
                              const TypeContext &context);
  /// The tuple, optional and function types resolveType has made, each
  /// checked whole, whatever the context: given again (a type argument put
  /// into a template, a signature written back), one is itself at once,
  /// without walking its elements again; an argument nested N deep has
  /// 3^N of them, and was walked once per level (SW-04).  One whose check
  /// waits for the structs (DeferredOptionalChecks) is not, so that it is
  /// checked again once they are known.
  std::unordered_set<const ast::Type *> ResolvedTypes;
  // False (after reporting it) when @p ty, the type of the value a variable
  // @p name is declared with, is `void`: the expression produces none.
  bool checkNotVoidValue(ast::Type *ty, const std::string &name,
                         ast::SourceLocation loc);

  // -- Fixed-size arrays (docs/language/09-arrays.md)
  //
  // For now only a local variable, a parameter, a result and a class field
  // have a `T[n]` type (set around the resolveType of its annotation, which
  // takes the flag), of scalars, enums, references or other such arrays.

  bool FixedArrayAllowed = false;
  /// Set around the resolveType of a `view` or `inout` parameter's type:
  /// only there is `T[]` (a slice) a type.
  bool SliceAllowed = false;
  /// Set around the resolveType of a type that cannot be an array yet,
  /// though a fixed-size array may parse there to be reported later (a
  /// struct's or a variant's field) or is reported at once (a tuple's
  /// element): what holds it, for the slice error's hint, which suggests
  /// a fixed-size array only where one would do.
  const char *NoArrayHolder = nullptr;
  /// Set around the resolveType of a variable declaration's annotation when
  /// it has an initializer: only there can a length be `_` (inferred).
  bool InferredLengthAllowed = false;
  /// The most elements a fixed-size array holds, counted across its rows
  /// (`int[2][3]` holds 6).
  static constexpr int64_t kMaxFixedArrayElements = 16384;
  /// The bytes @p ty takes inline in a fixed-size array; -1 when it cannot
  /// be an element (yet) or holds over kMaxFixedArrayElements elements.
  static int64_t fixedElementBytes(const ast::Type *ty);
  /// True when @p ty is, or holds, a fixed-size array.
  static bool holdsFixedArray(const ast::Type *ty);
  /// True when a length of the fixed-size array @p ty is `_`.
  static bool hasInferredLength(const ast::Type *ty);
  /// @p ty with each `_` length replaced by @p from's at the same depth;
  /// nullptr when @p from has no fixed-size array there.
  ast::Type *fillLengths(ast::Type *ty, ast::Type *from);
  /// Reports that @p from, at @p loc, gives the `_` lengths of @p ty none.
  void errorNoLengths(ast::SourceLocation loc, ast::Type *ty, ast::Type *from);
  /// Why @p arg cannot be passed for the slice @p param, appended to the
  /// argument's error ("" when @p param is not a slice).
  static std::string sliceArgNote(const ast::Type *param, const ast::Type *arg);
  /// Report a fixed-size array (@p ty) where only a local can have one yet,
  /// @p where; returns false.
  bool errorFixedArrayHere(ast::SourceLocation loc, ast::Type *ty,
                           const std::string &where);
  /// False (after reporting it) when a type argument of @p name can hold no
  /// value of its own: a fixed-size array (not yet) or `void` (none).
  bool checkTypeArgs(const std::string &name,
                     const std::vector<ast::Type *> &args,
                     ast::SourceLocation loc);
  /// False (after reporting it) for a `view` or `inout` result (@p mode) of
  /// a fixed-size array @p retTy, which is not supported yet.
  bool checkFixedResult(ast::Type *retTy, ast::ParamMode mode,
                        ast::SourceLocation loc);
  /// The `let` local the place @p e is part of (its value): an element of a
  /// fixed-size array, a field of a struct, or a chain of those; "" when it
  /// is not one.
  std::string letValueRoot(const ast::Expr *e);
  /// The tuple element, if any, that holds the struct whose field @p e is
  /// (through any structs in between): part of a tuple, so immutable.
  const ast::TupleIndexExpr *tupleElementHolding(const ast::Expr *e);

  // -- Private members (docs/design/visibility.md; SemaVisibility.cpp)

  /// The class or struct whose `static fn` body is being checked (its
  /// private members are reachable there), or nullptr.
  ast::Type *CurrentStaticOwner = nullptr;
  /// Set while a default `__init__` is declared: its parameters repeat the
  /// fields, whose types were checked already.
  bool DeclaringDefaultInit = false;
  /// The class, from @p ct up, that declares field @p name, or nullptr.
  static ast::ClassType *fieldOwner(ast::ClassType *ct,
                                    const std::string &name);
  /// The class, from @p ct up, that declares or overrides method @p name,
  /// or nullptr.
  static ast::ClassType *declaringClass(ast::ClassType *ct,
                                        const std::string &name);
  /// A use of member @p member of @p owner at @p loc: fine unless the name
  /// is private and the use is outside the owner's methods (reported).
  bool checkMemberAccess(ast::Type *owner, const std::string &member,
                         ast::SourceLocation loc);
  /// A call or function value `C::__m`: checkMemberAccess for the static
  /// method of class or struct `C`, when @p qualified has that form.
  bool checkStaticMemberUse(const std::string &qualified,
                            ast::SourceLocation loc);
  /// The private type @p ty names, anywhere in it, or "".
  std::string privateTypeIn(ast::Type *ty);
  /// A public declaration's type must not name a private type: @p what
  /// ("'f' is public but its result"), @p fix ("function"), reported.
  bool checkPublicSurface(const std::string &what, const std::string &fix,
                          ast::Type *ty, ast::SourceLocation loc);
  /// `m::__name` written in an importer: reported as private to its
  /// module, true; false when @p name is not of that form.
  bool reportPrivateImport(const std::string &name, ast::SourceLocation loc);
  /// Whether code here may construct (@p verb "constructed"), or extend
  /// through `__super__` ("extended"), the type @p typeName whose
  /// initializer is @p init: always, unless the initializer is `___init__`
  /// and the type is another module's, outside that module's own templates.
  /// Reports it otherwise, naming the type as @p written.
  bool checkConstructible(const std::string &typeName,
                          const std::string &written,
                          const ast::MethodDecl *init, const char *verb,
                          ast::SourceLocation loc);
  /// Whether a declaration's name is its module's own (`_f`, `_T`,
  /// `_T::f`): the first segment is private to the module.
  static bool isModuleOwnName(const std::string &name);
  /// The canonical name of the module that declares @p ty (for an
  /// instantiation, its template's), "" for this module's own or a builtin.
  std::string moduleOf(ast::Type *ty);
  /// The canonical name of the module whose code is being checked, "" for
  /// this module's own.
  std::string currentModule() const;
  /// `__` on a free function or a type, which only a member can have:
  /// reported, false.
  bool rejectTopLevelTypePrivate(ast::TranslationUnit *tu);
  /// False (after reporting it) when @p index is a constant out of
  /// @p fixed's bounds.
  bool checkFixedIndex(ast::FixedArrayType *fixed, const ast::Expr *index);

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
  /// True if @p name is a function of the module whose code is checked (a
  /// generic one or a `native fn` included): this module's, or in another
  /// module's template that module's.  No variable may be bound to it, so a
  /// call by the name always means the function.
  bool isFunctionNameForVariable(const std::string &name) const;
  /// True (counting a suppressed follow-on) if @p name is such a function's
  /// and the binding of it in scope is poisoned: the assignment that bound
  /// it was reported (whether or not its value had an error of its own),
  /// and a later assignment or destructuring target binding it again is
  /// that error's follow-on (it is reported once).
  bool rebindsPoisonedFunctionName(const std::string &name);
  /// This module's top-level functions, generic and `native` ones included,
  /// by name: known before any signature is checked, so a method's
  /// parameter is checked against them too.
  StringSet ModuleFunctionNames;

  /// The guard every binder applies to the name it binds (#126): an inferred
  /// or typed declaration, a destructuring target, a function or method
  /// parameter, a lambda's, a `for` loop's variable, a match binding, and a
  /// field.  Reports, at @p loc, a name that cannot be a variable (see
  /// isTypeNameForVariable and isFunctionNameForVariable) as what it cannot
  /// be used as, @p what ("a parameter name"), and returns false.  @p binds
  /// is false for a name no code of the program reads as a variable (a
  /// native function's parameter, a field, a default `__init__`'s
  /// parameter): it is not refused for a function's name.
  bool checkBinderName(const std::string &name, ast::SourceLocation loc,
                       bool binds = true, std::string_view what = "a variable");
  /// In a method's own body (not a lambda's in it, which is a function of
  /// its own), `self` names what the method was called on, and the rules on
  /// `self` (an `__init__`'s field assignments, a `view fn`'s read-only
  /// `self`) and the lowering take it so: a declaration, a destructuring
  /// target, a `for` loop's variable or a match binding named `self` there
  /// is reported at @p loc, and false returned.
  bool checkNotSelf(const std::string &name, ast::SourceLocation loc);
  /// Reports a parameter of @p fn named like an earlier one, as a
  /// duplicate in @p what ("function 'f'") at it, and returns false.
  bool checkDistinctParams(const ast::FuncDecl *fn, const std::string &what);

  /// checkBinderName for a match arm's binding (of type @p bindTy), and its
  /// kind: `n: view T` is a `view` local, `n: inout T` the storage of
  /// @p subject, which must be a place.  A rejected binding is bound
  /// poisoned in the arm's scope.
  bool bindArmName(ast::MatchArm *arm, const ast::Expr *subject,
                   ast::Type *bindTy);

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
    ///
    /// A chain's links below @p e are typed first, innermost out
    /// (typeChainBelow), so that typing a chain of any length recurses no
    /// deeper than a few links: each link's visitor finds the type of the
    /// operand it visits first (ast::chainOperand) already there.
    ast::Type *visit(ast::Expr *e) {
      if (!ChainTypes.empty()) {
        auto typed = ChainTypes.find(e);
        if (typed != ChainTypes.end()) {
          ast::Type *ty = typed->second;
          ChainTypes.erase(typed);
          return ty;
        }
      }
      typeChainBelow(e);
      ast::Type *ty = ast::ExprVisitor<ExprChecker, ast::Type *>::visit(e);
      assert(!ChainTypes.count(ast::chainOperand(e)) &&
             "a chain link's visitor visits its operand");
      if (ty)
        e->setResolvedType(ty);
      return ty;
    }

    /// When @p e is a link of a chain whose operand is a link too: visit
    /// the links below @p e, innermost first, each as its operand's visitor
    /// would, and keep their types (null for an error, reported) in
    /// ChainTypes for the visit that comes to each.  The links' visitors
    /// take their operand before anything else, so this is the order they
    /// are visited in anyway.
    void typeChainBelow(ast::Expr *e);
    /// The types of the links typeChainBelow visited and nothing has taken
    /// yet.
    std::unordered_map<const ast::Expr *, ast::Type *> ChainTypes;

    /// visit(@p e) where the value flows into a slot of type @p expected
    /// (nullptr: unknown).  An array or tuple literal uses it: its
    /// elements are visited against the slot's element types, and an array
    /// literal whose elements disagree (`[("a", 1), ("b", n)]` with `n:
    /// int?`) takes the slot's element type when every element fits it.  A
    /// numeric literal (`5`, `-5`, `1.5`) flowing into a slot of a sized
    /// numeric type takes that type when it fits it
    /// (docs/design/sized-types.md).  A generic enum's variant written
    /// without type arguments takes the slot's (`Maybe::Nothing` into a
    /// `Maybe<int>`), in a conditional expression's arms too.
    ast::Type *visitExpecting(ast::Expr *e, ast::Type *expected) {
      if (expected && (ast::isa<ast::ArrayLiteralExpr>(e) ||
                       ast::isa<ast::TupleLiteralExpr>(e)))
        S.LiteralExpectation = expected;
      else if (ast::isa<ast::EnumValueExpr>(e) || ast::isa<ast::CallExpr>(e) ||
               ast::isa<ast::TernaryExpr>(e))
        S.VariantExpectation = expected;
      else if (ast::Type *sized = sizedLiteralTarget(e, expected))
        S.LiteralExpectation = sized;
      return visit(e);
    }

    /// Whether @p e is an integer or float literal, the negation of one, or
    /// the complement of an integer literal (`~0x0F`).
    static bool isNumericLiteral(const ast::Expr *e);
    /// The sized numeric type a literal @p e flowing into a slot of type
    /// @p expected (or a `T?` of it) takes, nullptr when @p expected is not
    /// one or @p e is not a numeric literal.  `int` and `float` slots leave
    /// a literal its own type, but for a `float` one an integer literal
    /// above INT64_MAX takes, and only an unsigned slot gives the
    /// complement of a literal a type (docs/design/bitwise.md).
    static ast::Type *sizedLiteralTarget(const ast::Expr *e,
                                         ast::Type *expected);
    /// The type of the literal @p node flowing into the sized type
    /// @p target: @p target when its value fits it, else an error (`the
    /// literal 256 does not fit in 'u8' (0 to 255)`).  An integer literal's
    /// value is a sign and a @p magnitude up to 2^64-1, a float literal's
    /// @p floatValue.
    ast::Type *checkSizedLiteral(ast::Expr *node, ast::Type *target,
                                 bool isInteger, bool negative,
                                 uint64_t magnitude, double floatValue);
    /// `x << n` and `x >> n` (visitBinaryExpr's): the value typed first,
    /// on its own, a literal count as an `unsigned` when the value is
    /// unsigned; both unsigned, of any widths; the value's type.  A literal
    /// count at or above the width is a warning (docs/design/bitwise.md).
    ast::Type *visitShift(ast::BinaryExpr *node);
    /// visit(@p operand) of a binary operator whose other operand has type
    /// @p otherTy: a numeric literal takes the other operand's sized type,
    /// and a generic enum's variant its type arguments (`m ==
    /// Maybe::Nothing`).
    ast::Type *visitOperand(ast::Expr *operand, ast::Type *otherTy) {
      if (otherTy &&
          (isNumericLiteral(operand) || ast::isa<ast::EnumValueExpr>(operand) ||
           ast::isa<ast::CallExpr>(operand) ||
           ast::isa<ast::TernaryExpr>(operand)))
        return visitExpecting(operand, otherTy);
      return visit(operand);
    }
    /// How much an operand of `==` or `!=`, or an arm of a conditional
    /// expression, needs the type of the other operand or arm: a generic
    /// enum's variant, or a call of a generic function, written without
    /// type arguments takes them from it.
    enum class OtherSide {
      NotNeeded, ///< typed on its own
      Helps,     ///< `Maybe::Just(n)`: may infer them from its fields
      Needed,    ///< `Maybe::Nothing`, `Result::Ok(1)`, `none()`: cannot
                 ///< infer them all from its fields or arguments
    };
    /// What @p e needs (a conditional, what the less needy of its arms
    /// does).  Of two sides, the one that needs the other less is typed
    /// first and gives its type to the other, so `Maybe::Nothing == m`,
    /// `if c then Maybe::Nothing else Maybe::Just(n)` and `Maybe::Just(1)
    /// == none()` work as their mirrors do; of two that need it as much,
    /// the first.
    OtherSide needsOtherSide(const ast::Expr *e) const;

    /// A literal flowing into a fixed-size array slot: exactly its length
    /// of elements, each of its element type.
    ast::Type *checkFixedArrayLiteral(ast::ArrayLiteralExpr *node,
                                      ast::FixedArrayType *fixed);
    /// `E::V(a, b)`: variant @p variant of @p enumTy made with its fields,
    /// the arguments, as a struct's constructor takes them; the enum.
    /// @p argTypes: the arguments' types when they were visited already.
    ast::Type *
    checkVariantCall(ast::CallExpr *node, ast::EnumType *enumTy,
                     const std::string &variant,
                     const std::vector<ast::Type *> *argTypes = nullptr);
    /// The instantiation of the generic enum @p tmplName that @p node,
    /// its variant @p variant, makes: by the written type arguments
    /// (@p typeArgAnns, `Maybe<int>::Nothing`), else by the type the value
    /// flows into (@p expected, an instantiation of the template), else,
    /// for a call (@p argTypes non-null), by those inferred from its
    /// arguments (`Maybe::Just(1)`), which it visits into @p argTypes.
    /// nullptr after reporting an error.
    ast::EnumType *genericVariantEnum(
        ast::Expr *node, const std::string &tmplName,
        const std::string &variant, const std::vector<ast::Type *> &typeArgAnns,
        ast::Type *expected, std::optional<std::vector<ast::Type *>> *argTypes);
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

  /// The places (file, line, column) of the literal shift counts warned
  /// about (ExprChecker::visitShift): once each, though several
  /// instantiations of a generic body reach one.
  std::set<std::tuple<std::string, size_t, size_t>> WarnedShiftCounts;

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

  /// A type arm that tests for the exact class @p armCt, which is abstract:
  /// no object has that class, so the arm can never run.
  void reportAbstractArm(const ast::MatchArm *arm, const ast::ClassType *armCt);

  // -- Import resolution

  /// The directory of the file currently being analyzed.
  std::string ProjectRoot;

  /// The frontend imported modules are parsed with ("" = the default).
  std::string FrontendName;

  /// Canonical names of the modules currently being imported (for cycle
  /// detection).
  StringSet *ImportStack = nullptr;

  /// How deep the import being followed is: the imports, from the main
  /// file's down, whose modules are being located or loaded, shared like
  /// ImportStack by every Sema of one compilation.  Each module built from
  /// source on the way is analysed by a nested Sema on the native stack,
  /// and each cache entry's dependency is located in a nested call, so a
  /// chain deeper than kMaxImportDepth is an error (docs/grammar.md
  /// section 9), whether its modules come from source or from module files.
  unsigned *ImportDepth = nullptr;
  static constexpr unsigned kMaxImportDepth = 256;
  /// One more import followed (ImportDepth), for as long as it lives.
  class ImportLevel {
  public:
    explicit ImportLevel(unsigned *depth) : Depth(depth) {
      if (Depth)
        ++*Depth;
    }
    ~ImportLevel() {
      if (Depth)
        --*Depth;
    }
    ImportLevel(const ImportLevel &) = delete;
    ImportLevel &operator=(const ImportLevel &) = delete;

  private:
    unsigned *Depth;
  };
  /// Whether the import of @p canonical, the ImportLevel just entered, is
  /// nested deeper than kMaxImportDepth; reported at @p loc when it is.
  bool importTooDeep(const std::string &canonical, ast::SourceLocation loc);

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
  /// Sema has reconstructed from an import, or instantiated from another
  /// module's template, keyed by canonical type name.
  /// Type names are global across the import graph, so reconstructing a type
  /// whose name is already bound to a type from a different module is an
  /// error rather than a silent merge of two unrelated types.
  StringMap<std::string> ImportedTypeOrigins;

  /// Every module qualifier this file's imports bind (each import's alias or
  /// last path segment, and its full module path) -> the module it names.
  /// One qualifier cannot name two modules: `util::f` would be ambiguous.
  struct ImportQualifier {
    std::string Module; ///< canonical module name (`::io`, `a::util`)
  };
  StringMap<ImportQualifier> ImportQualifiers;

  /// The qualifiers this file's imports claim, before any is processed ->
  /// the module each names: every import's alias or last path segment, and
  /// the full path of an import from the source root.  A system import's
  /// full path is its canonical name (`::math`), which no other import
  /// spells, so it claims nothing: it binds the path without the `::`
  /// (`math::sqrt` after `import ::math as m;`) only when no import claims
  /// that name for another module (OD-8), as `import math as mine;` does.
  StringMap<std::string> ClaimedImportQualifiers;

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

  /// True (counting a suppressed follow-on) if @p name, or the type part of
  /// `name::member`, is an imported type's name that a declaration of this
  /// module took as well (SW-33): the declaration's error was reported, and
  /// a use of the name here means that declaration, not the imported type.
  bool isImportClashUse(std::string_view name);

  /// Errors reported inside imported modules, through their own DiagEngine
  /// (with their own file name and source lines) and so not counted by Diags.
  /// They fail this module too: run() adds them to its error count.
  unsigned ImportedModuleErrors = 0;

  /// @p path for a diagnostic (module_utils::displayPath): under the source
  /// root, ProjectRoot as the main file's path gives it followed by the
  /// path from the root; as given otherwise.
  std::string displayPath(const std::filesystem::path &path) const;

  /// Where imports come from: source files, cache entries or prebuilt
  /// `.pkm` files.  The driver's (setResolver), or one run() makes, which
  /// neither reads nor writes the cache (OwnedResolver).
  modules::ModuleResolver *Resolver = nullptr;
  std::shared_ptr<modules::ModuleResolver> OwnedResolver;

public:
  struct ModuleTemplates;

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
      ast::ParamModes Modes; // `n: inout int`
      /// A `native fn` (FunctionSig::Native).
      bool Native = false;
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
        bool Weak = false; // declared `weak` (a class field only)
        bool operator==(const FieldInfo &) const = default;
      };
      struct MethodInfo {
        std::string Name;
        std::string ReturnTypeName;
        std::vector<std::string> ParamTypeNames;
        uint8_t Flags = 0;     // ast::MethodDecl::Flags
        ast::ParamModes Modes; // `n: inout int`
        bool operator==(const MethodInfo &) const = default;
      };
      std::vector<FieldInfo> Fields;
      std::vector<MethodInfo> Methods;
      bool operator==(const ClassInfo &) const = default;
    };
    std::vector<ClassInfo> ExportedClasses;

    // Serialised struct-type descriptions: the fields in declaration order
    // and the methods (`__init__` first, then the `impl` methods), as a
    // ClassInfo's.
    struct StructInfo {
      std::string Name;
      bool IsLocal = true;      // see ClassInfo::IsLocal
      std::string OriginModule; // see ClassInfo::OriginModule
      std::vector<ClassInfo::FieldInfo> Fields;
      std::vector<ClassInfo::MethodInfo> Methods;
      bool operator==(const StructInfo &) const = default;
    };
    std::vector<StructInfo> ExportedStructs;

    // Serialised enum-type descriptions: the enum name plus its variant names
    // in declaration order (the index is the variant's underlying value).
    // A tagged union's variants with fields are listed too: each one's
    // fields are the struct `Name::variant` among ExportedStructs.
    struct EnumInfo {
      std::string Name;
      std::vector<std::string> Variants;
      bool IsLocal = true;      // see ClassInfo::IsLocal
      std::string OriginModule; // see ClassInfo::OriginModule
      std::vector<std::string> PayloadVariants;
      bool operator==(const EnumInfo &) const = default;
    };
    std::vector<EnumInfo> ExportedEnums;
    /// The conformances of ExportedClasses and ExportedStructs to the
    /// built-in traits an `impl ... for` block gives
    /// (names::isImplBuiltinTrait), each recorded on its type itself (a
    /// subclass has its superclass's): `impl Hashable for Key`.  Local or
    /// reached, private or not, instantiations too: an importer reconstructs
    /// the type from this interface alone and needs them for bounds.  Sorted
    /// by (TypeName, Trait).
    struct ImplInfo {
      std::string TypeName;
      std::string Trait;
      bool operator==(const ImplInfo &) const = default;
    };
    std::vector<ImplInfo> ExportedImpls;
    /// The module's generic classes and functions; null when it has none.
    std::shared_ptr<const ModuleTemplates> Templates;
    bool operator==(const ModuleInfo &) const = default;
  };

  /// The generic classes and functions a module exports (docs/design/pkm.md
  /// §4), as their module resolved them: every name in a body that the
  /// module declares or imports is spelled with the hidden qualifier
  /// `$<canonical module>::` (hiddenQualifier), which an importer registers
  /// for the module and for each module in Uses, and no user code can write.
  struct ModuleTemplates {
    std::string Module;                    ///< canonical module name
    std::vector<ast::ClassDecl *> Classes; ///< in the module's ASTContext
    std::vector<ast::FuncDecl *> Functions;
    std::vector<ast::StructDecl *> Structs;
    std::vector<ast::EnumDecl *> Enums;
    /// The `impl` blocks of each of Classes and Structs, `impl Trait for`
    /// blocks too: a built-in trait's gives an importer's instantiation the
    /// trait, a block of the module's own trait only its methods (#147,
    /// checkTraitImpl).
    std::unordered_map<const ast::Decl *, std::vector<ast::ImplDecl *>> Impls;
    /// The modules the bodies name, the module itself first: canonical
    /// name and the file ModuleCache knows it by.
    std::vector<std::pair<std::string, std::string>> Uses;
    /// Where the bodies' source locations point, for diagnostics.
    std::string File;
    const std::vector<std::string> *Lines = nullptr;
    /// Keeps the ASTContext the bodies live in alive.
    std::shared_ptr<void> Owner;
  };

  /// The qualifier under which an importer registers module @p canonical
  /// for the bodies of its templates.
  static std::string hiddenQualifier(const std::string &canonical);
  /// @p name as diagnostics show it: a name under a hidden qualifier
  /// (`$lib::box::f`) by its module's path (`lib::box::f`).
  static std::string displayName(const std::string &name);

  /// Global cache of already-analyzed modules, keyed by the resolved path
  /// of the file they came from (a source, or a .pkm).
  static StringMap<ModuleInfo> ModuleCache;
  /// The analysis of each module of ModuleCache built from its source, by
  /// the same key: a later compilation in the process that imports the
  /// module gets it too (its importer's ImportedContexts), which an
  /// in-process lowering of the program needs for the module's code.
  static StringMap<std::shared_ptr<SemaContext>> ModuleContextCache;

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

  /// The templates of the module this Sema analysed, resolved for its
  /// importers; null when it has none.  @p canonical is its name.
  std::shared_ptr<ModuleTemplates>
  exportTemplates(ast::TranslationUnit *tu, const std::string &canonical);
  /// @p mt as the TMPL section of a .pkm (docs/design/pkm.md §4.3), and
  /// back.  The section records @p file as the bodies' file: the source's
  /// path from the source root (module_utils::relativeToRoot), not
  /// mt.File, which diagnostics show from where the compiler runs.
  /// encodeTemplates returns no bytes, with the reason in @p error, for a
  /// body the interchange format cannot write.  decodeTemplates builds the
  /// bodies in an ASTContext of their own (Owner); Uses names the modules,
  /// and the importer fills in the files they are cached under; File is
  /// the recorded one and the bodies have no source lines.
  static std::vector<uint8_t> encodeTemplates(const ModuleTemplates &mt,
                                              const std::string &file,
                                              std::string &error);
  static StatusOr<std::shared_ptr<ModuleTemplates>>
  decodeTemplates(std::span<const uint8_t> bytes, const std::string &canonical);

  /// Whether every type the interface @p iface names, in its fields and
  /// signatures, is a builtin or one it describes, and no parameter is
  /// `void`: what an importer needs to reconstruct them (injectModule).
  /// The first that is not when not.
  static Status checkInterfaceTypes(const pkm::Interface &iface);
  /// What Sema reads of a module file of @p canonical when the file is
  /// loaded (modules::FrontendRead): its interface's types
  /// (checkInterfaceTypes) and its TMPL section @p templates, decoded
  /// (decodeTemplates; the ModuleTemplates loadModule takes, null without
  /// templates).  Or why the file cannot be used ("has a bad TMPL section:
  /// ...").
  static StatusOr<std::shared_ptr<void>>
  readImport(const pkm::Interface &iface, std::span<const uint8_t> templates,
             const std::string &canonical);

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
  /// Returns nullptr after reporting the failure, with a note that @p loc
  /// imports the module unless @p importedHere is false (a cache entry's
  /// dependency, loaded before the entry is used: the entry's importer
  /// does not import it).
  const ModuleInfo *loadModule(const std::string &canonical,
                               const LocatedModule &located,
                               ast::SourceLocation loc,
                               bool importedHere = true);
  /// Make a module's exports visible under @p qualifier (and
  /// @p fullModulePath): reconstruct its types in Ctx and declare its
  /// functions.  Returns false after reporting a failure.
  bool injectModule(const ModuleInfo &info, const std::string &qualifier,
                    const std::string &fullModulePath,
                    const std::string &moduleName, ast::SourceLocation loc);

  /// Record the qualifiers @p imports claim (ClaimedImportQualifiers).
  void claimImportQualifiers(const std::vector<ast::ImportDecl *> &imports);

  /// Process a single import declaration.
  bool processImport(ast::ImportDecl *node);

  // -- `view` and `inout` parameters (SemaParamModes.cpp)

  /// Record the kinds of @p fn's `view` / `inout` parameters, just declared
  /// in the current scope, and how it returns its result (CurrentResult).
  void declareParamKinds(const ast::FuncDecl *fn);
  /// The rules of the local borrow @p node (`x: view = e;`, `x: inout =
  /// k;`), just declared in the current scope: records its kind.
  bool checkLocalBorrow(const ast::VarDecl *node);
  /// Whether @p ty may be the type of a `weak` declaration: `T?` of a
  /// class `T` (docs/language/16-memory.md); an error names @p what
  /// otherwise.
  bool checkWeakType(ast::Type *ty, ast::SourceLocation loc,
                     const std::string &what);
  /// Why the place @p place cannot be @p use ("passed to 'inout' parameter
  /// 'n'", "named by 'inout' local 'x'"): an array element (not yet), a
  /// string's character, `self`, a `let` local or a `view`; "" when it can.
  std::string inoutPlaceError(const ast::Expr *place, const std::string &use);

  // -- What cannot change through a `view` (SemaViews.cpp)

  /// Why the place @p e (a variable, or fields and elements reached from
  /// one) cannot be changed here, as the start of a diagnostic: "'n' is a
  /// 'view' parameter", "'self' is read-only in 'view fn get'"; "" when it
  /// can.
  /// With @p borrows, a variable viewed by a live `view` local cannot
  /// change either.  With @p copy, only whether a copy of @p e may share
  /// what can change through it is asked (viewOf): a lambda's captured
  /// copy is its own, read-only but not a `view`.
  std::string frozenPlace(const ast::Expr *e, bool borrows = true,
                          bool copy = false);
  /// Error at @p loc (and false) when the place @p e cannot be changed;
  /// @p what ends the diagnostic ("cannot assign to its field 'n'").
  bool checkChangeable(const ast::Expr *e, const std::string &what,
                       ast::SourceLocation loc);
  /// A call of @p method on the receiver of @p call: unless it is a `view
  /// fn`, the receiver must be changeable.
  bool checkMethodReceiver(const ast::MethodCallExpr *call,
                           const ast::MethodDecl *method);
  /// Whether a value of type @p ty shares what it holds when it is copied
  /// (an object, string, array or tuple, or an optional of one), so that a
  /// copy of a `view` of it could change it.
  static bool sharesStorage(const ast::Type *ty);
  /// Why @p e is a `view` (frozenPlace, or a conditional with such a
  /// branch), as the start of a diagnostic; "" when it is not.
  std::string viewOf(const ast::Expr *e, bool borrows = true);
  /// A `view` can only be passed on to a `view` parameter, as it is (no
  /// conversion to the optional of a `view T?`): each of @p args against
  /// the parameters' @p modes and types @p paramTys of a call of @p callee.
  bool checkViewArgs(const ast::ParamModes &modes,
                     const std::vector<ast::Type *> &paramTys,
                     const std::vector<ast::Expr *> &args,
                     const std::string &callee);
  /// A `view` local @p name of type @p declTy whose initializer @p init (of
  /// type @p initTy) is a `view`: it names it as it is, so it cannot be its
  /// optional.
  bool checkViewLocalNotConverted(const std::string &name, ast::Type *declTy,
                                  const ast::Expr *init, ast::Type *initTy);
  /// @p e, of type @p ty, is being stored (assigned, put in an array or
  /// tuple, pushed): not a `view` that shares what it holds.
  bool checkViewNotStored(const ast::Expr *e, const ast::Type *ty);
  /// @p e, of type @p ty, is being returned: not a `view` that shares what
  /// it holds, though part of `self` may be.
  bool checkViewNotReturned(const ast::Expr *e, const ast::Type *ty);
  /// Whether the place @p e, part of `self` in a method of an instance of
  /// a generic class or struct, holds a type argument: it is `self`, or
  /// starts with a field whose type in the template names a type parameter.
  bool holdsTypeArgument(const ast::Expr *e);
  /// While a `match` arm binds a name: whether the subject is a `view`, so
  /// that the name is one too.
  bool MatchSubjectIsView = false;
  /// The variable a place starts from: `c` for `c`, `c.a.n`, `c.xs[i]` and
  /// `c.t.0`; null for anything else (a call).
  static const ast::Identifier *placeRoot(const ast::Expr *e);
  /// What a place starts from: the variable, or the call, under its fields,
  /// elements and tuple slots.
  static const ast::Expr *placeBase(const ast::Expr *e);

  // -- Borrowed results (SemaResults.cpp)

  /// A call whose result is a borrow (`-> view T`) of what it was given:
  /// its receiver and the arguments to its `view` / `inout` parameters.
  struct BorrowResult {
    std::string Callee;
    ast::ParamMode Mode = ast::ParamMode::View;
    std::vector<const ast::Expr *> From;
    /// For each of From, whether the result can be in an object one
    /// reference past it: the object a class's receiver refers to, or one
    /// that a class-typed argument, or a struct's, a tuple's or a
    /// fixed-size array's storage, refers to and that can hold a place of
    /// the result's type (reachesPlace).
    std::vector<bool> PastFrom;
    /// The receiver, the first of From; null for a function's call.
    const ast::Expr *Receiver = nullptr;
  };
  std::unordered_map<const ast::Expr *, BorrowResult> BorrowResults;
  /// The result-mode rules of the declaration of @p fn (a method when
  /// @p method): `main` returns a copy, and a `view fn` no `inout`.
  bool checkResultDecl(const ast::FuncDecl *fn, bool method);
  /// Record that @p call, of @p callee with @p modes, parameter types
  /// @p paramTys and result type @p resultTy, returns a borrow of
  /// @p receiver (or null) and its borrowed @p args.
  void noteBorrowResult(const ast::Expr *call, const std::string &callee,
                        const ast::ParamModes &modes,
                        const std::vector<ast::Type *> &paramTys,
                        ast::Type *resultTy,
                        const std::vector<ast::Expr *> &args,
                        const ast::Expr *receiver);
  /// Whether storage of type @p ty holds a place of type @p place in its
  /// own bytes: it is one, or a struct, a tuple or a fixed-size array
  /// holding one.  Not through a reference, which leads to another object.
  static bool holdsPlace(ast::Type *ty, ast::Type *place);
  /// Whether an object a reference held in storage of type @p ty refers to
  /// can hold a place of type @p place, which an `inout` result of that
  /// type can then point to: @p ty is a class (not `Str` or `Obj`, whose
  /// storage no place reaches), or a struct, a tuple or a fixed-size array
  /// holding one in its own bytes, and a field of that class holds such a
  /// place, or the `inout` result of one of its methods does (an override,
  /// in a subclass, can return a field of its own).  Not a slice: its
  /// caller keeps no object in the array alive.  No place reaches into an
  /// optional, an enum or a function value.
  static bool reachesPlace(ast::Type *ty, ast::Type *place);
  /// The borrowed result the place @p e is part of, or null.
  const BorrowResult *borrowResultOf(const ast::Expr *e) const;
  /// Whether @p e is a call returning an `inout` result: a place.
  bool isInoutResult(const ast::Expr *e) const;
  /// Why @p e cannot be returned as the current function's borrowed result;
  /// "" when it is part of `self` or of a `view` / `inout` parameter (an
  /// `inout` one, and an `inout` argument's place, for an `inout` result).
  /// With @p holder, @p e is not what is returned but what holds it, a
  /// conditional's branch (`a` in `(if c then a else b).n`): what it may
  /// lend is checked, not that it is a place.
  std::string borrowedResultError(const ast::Expr *e, bool holder = false);
  /// How many more references the place @p e, one an `inout` result may
  /// borrow (borrowedResultError), could follow and stay in what the
  /// function's caller keeps alive: the object a class's `self` is, and the
  /// storage of a struct's `self` or of an `inout` parameter with each
  /// object a reference held there refers to (none in a slice's array).
  /// `self` and a parameter have one to follow, and each field of a class
  /// on the way takes one.  Below zero, the storage is in a separate
  /// object, which @p past refers to or (a value) holds; @p via is the call
  /// whose result is there, null when the place itself goes there.
  int inoutReach(const ast::Expr *e, const ast::Expr *&past,
                 const BorrowResult *&via) const;
  /// Why @p e, which the current function may borrow (borrowedResultError),
  /// cannot be its `inout` result: it is in an object its caller does not
  /// keep alive (inoutReach); "" when it is not.
  std::string separateObjectError(const ast::Expr *e) const;
  /// Where the `inout` result of call @p r, which a diagnostic follows a
  /// reference from (so it borrowed something), can point: the receiver or
  /// argument it can be farthest in (`'n'`, `an object 's' holds`).
  std::string resultPlaceText(const BorrowResult &r) const;

  // -- Where a `view fn` may be declared (SemaViewFn.cpp)

  /// The `view fn` rules of class method @p method, overriding @p base (null
  /// when it overrides nothing): `__init__` is never one, and an override is
  /// one exactly when @p base is.
  bool checkViewFnDecl(const ast::FuncDecl *method,
                       const ast::MethodDecl *base);
  /// Only a method can be a `view fn`: error on each free one in @p tu.
  bool rejectFreeViewFns(ast::TranslationUnit *tu);
  /// After every body is checked: warn about each method, with its
  /// overrides, that never changes `self` but is not a `view fn`.
  void warnMissingViewFns();

  // -- Exclusivity of local borrows (SemaBorrows.cpp)

  /// A live local borrow: `By` names (`inout`) or views `Root`, declared in
  /// `RootScope`, until statement `End` of `Block`.
  struct Borrow {
    std::string Root;
    const Scope *RootScope;
    std::string By;
    bool Inout;
    const std::vector<ast::Stmt *> *Block;
    size_t End;
  };
  std::vector<Borrow> Borrows;
  /// The (variable, line) pairs reported by checkNotBorrowed.
  std::set<std::pair<std::string, unsigned>> BorrowedUsesReported;
  /// Check @p stmts in order, starting each local borrow after its
  /// declaration and ending it after its last use there.
  bool visitStatements(const std::vector<ast::Stmt *> &stmts);
  /// A function or method body: visitStatements, without the borrows of
  /// whatever is being checked around it.
  bool visitBody(const std::vector<ast::Stmt *> &stmts);
  /// Start the borrow of the place @p place by @p by (a local borrow, a
  /// `match` arm's binding), live until statement @p end of @p block.
  void startBorrow(const ast::Expr *place, const std::string &by, bool inout,
                   const std::vector<ast::Stmt *> *block, size_t end);
  /// Check the body of @p arm, a match over @p subject, its binding
  /// borrowing the subject when it is `n: view T` or `n: inout T` until its
  /// last use (MatchArm::bindingUses).
  bool visitArmBody(ast::MatchArm *arm, const ast::Expr *subject);
  /// The live borrow (`inout` or `view`) of the variable @p name, or null.
  const Borrow *activeBorrow(const std::string &name, bool inout) const;
  /// Error at @p loc (and false) when variable @p name is named by a live
  /// `inout` local, and so cannot be used.
  bool checkNotBorrowed(const std::string &name, ast::SourceLocation loc);
  /// Why variable @p name cannot change while a `view` local of it is live,
  /// as the start of a diagnostic; "" when none is.
  std::string viewedBy(const std::string &name) const;
  /// An override @p method keeps each parameter's mode of @p base.
  bool checkOverrideModes(const ast::FuncDecl *method,
                          const ast::MethodDecl *base);
  /// The arguments @p args of @p call, a call of @p callee whose parameters
  /// have the modes @p modes and the types @p paramTys: an `inout`
  /// parameter takes a variable or a field of exactly its type, never a
  /// `view` parameter or a `let` local, and what it takes shares no storage
  /// with another `inout` or `view` argument, nor with the @p receiver of a
  /// method call of @p method.  Nor does a call in a later argument write
  /// what a `view` argument, or the receiver of a struct's `view fn`,
  /// borrows (CallWrites).  Arguments of the wrong type were reported
  /// already.
  bool checkInoutArgs(const ast::Expr *call, const ast::ParamModes &modes,
                      const std::vector<ast::Type *> &paramTys,
                      const std::vector<ast::Expr *> &args,
                      const std::string &callee,
                      const ast::Expr *receiver = nullptr,
                      const ast::MethodDecl *method = nullptr);
  /// A place a call writes, named as its caller names it: one passed to an
  /// `inout` parameter, or the receiver of a method that may change it.
  struct CallWrite {
    std::string Path;
    /// The call's callee, and the `inout` parameter (none for the receiver).
    std::string Callee;
    std::optional<std::string> Param;
    /// The argument or the receiver naming it, where a conflict is reported.
    const ast::Expr *At = nullptr;
    /// Whether a call it is nested in reported it already.
    bool Reported = false;
  };
  /// What each call checkInoutArgs checked writes, for the calls whose
  /// arguments it is nested in.
  std::unordered_map<const ast::Expr *, std::vector<CallWrite>> CallWrites;
  /// The places passing @p e borrows, as `checkInoutArgs` compares them: a
  /// variable or a chain of fields on one; an element borrows its array, a
  /// conditional both branches, a borrowed result what its call borrowed.
  void borrowedPaths(const ast::Expr *e, std::vector<std::string> &out);

  /// Register classes: names, hierarchy, field types, method signatures, and
  /// constructors (phases 1–4b).  Method bodies are deferred to
  /// checkClassBodies() so that free functions can be forward-declared in
  /// between.  Stores the topological class order in SortedClasses.
  bool checkClassDecls(const std::vector<ast::ClassDecl *> &classDecls);

  /// Type-check class method bodies (phase 5) over SortedClasses.  Run after
  /// checkClassDecls and after free-function signatures are registered.
  bool checkClassBodies();

  /// Definite-assignment check for a constructor body: verify every own
  /// field in @p fields (a class's own, or a struct's) is assigned
  /// (self.field = …) on every control-flow path out of `body` (each return
  /// and the final fall-through).  Emits an error per field left
  /// possibly-unassigned, naming the @p kind ("class" or "struct") and
  /// @p typeName.  Returns true if all fields are definitely assigned.
  bool checkInitFieldsAssigned(
      const std::vector<std::pair<std::string, ast::Type *>> &fields,
      ast::CompoundStmt *body, ast::SourceLocation initLoc,
      const std::string &kind, const std::string &typeName);

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
  /// The classes registered on Obj because their superclass is undefined,
  /// rejected, or closes a cycle: what they would be a subclass of is not
  /// known.  A subset of ErroneousClasses.
  StringSet SuperclassLost;

  /// Resolve a free function's signature (return + parameter types) and
  /// register it in the function table.  Run as a forward-declaration pass
  /// before any function or method body is checked, so calls resolve regardless
  /// of the order declarations appear in the module.
  bool declareFunctionSignature(ast::FuncDecl *node);
  bool checkNativeSignature(ast::FuncDecl *node, ast::Type *retTy,
                            const std::vector<ast::Type *> &paramTypes);
  bool checkNativeParamModes(const ast::FuncDecl *node,
                             const std::vector<ast::Type *> &types);
  /// Whether the C symbol native function @p node binds has one C
  /// signature (nativeSignature): the one an earlier native function of the
  /// module binds it with, and that of a function compiled code calls
  /// itself (pir::isRuntimeSymbol, pir::runtimeSignature).  Records the
  /// binding.
  bool checkNativeSymbol(const ast::FuncDecl *node);

  /// The native function that first binds each C symbol in this module,
  /// and the C signature it binds it with (checkNativeSymbol).  Natives of
  /// other modules are checked against these once the program is lowered
  /// (lowering::checkNativeSymbols).
  struct NativeBinding {
    const ast::FuncDecl *Decl = nullptr;
    pir::Signature Sig;
    /// The source it is declared in (another module's, for an instance of
    /// its generic native function), for a note that points at it.
    std::string File;
    const std::vector<std::string> *Lines = nullptr;
  };
  std::unordered_map<std::string, NativeBinding> NativeSymbols;

  /// Phase 4 for one class: resolve field types and method signatures into
  /// the pre-registered ClassType (superclass must already be populated).
  bool populateClassType(ast::ClassDecl *cd, ast::ClassType *ct);

  /// Phase 4b for one class: register the constructor function `Name(...)`.
  void declareConstructor(ast::ClassDecl *cd, ast::ClassType *ct);

  /// `cannot instantiate abstract class 'C': 'B.f' has no implementation`,
  /// naming every pure virtual slot of the abstract class @p ct, each by the
  /// class that declares it pure.
  void reportAbstractConstruction(const ast::ClassType *ct,
                                  ast::SourceLocation loc);

  /// The signature of @p method of @p owner (a class or a struct) as a
  /// MethodDecl with @p flags, its annotations resolved and written back;
  /// nullptr if one does not resolve.  @p overridden is the superclass
  /// method it overrides, if any.  A mistake that leaves the signature
  /// usable (an override that does not match) is reported and clears @p ok.
  ast::MethodDecl *declareMethod(ast::FuncDecl *method, ast::Type *owner,
                                 const ast::MethodDecl *overridden,
                                 uint8_t flags, bool &ok);

  /// Phase 4 for the `impl` blocks of @p cd's class: their methods join it.
  bool populateImplMethods(ast::ClassDecl *cd, ast::ClassType *ct);

  /// Give @p cd, which declares no `__init__`, the default one: it takes the
  /// superclass constructor's arguments and then one per field of the class,
  /// passes the former to `__super__` and stores each of the latter.
  bool declareDefaultInit(ast::ClassDecl *cd, ast::ClassType *ct);
  /// That default `__init__` of @p ct, at @p loc, not declared yet.
  ast::FuncDecl *defaultInit(ast::ClassType *ct, ast::SourceLocation loc);

  /// The constructor of @p ct does nothing: it is the default `__init__` of
  /// a class with no fields, and so are its superclasses'.  An `__init__` of
  /// a subclass need not call it.
  bool doesNothing(const ast::ClassType *ct) const;

  /// Report that @p cls's superclass @p super, which names no class, is not
  /// defined, or what it is instead: a struct, a trait, a builtin type, a
  /// type parameter of @p tmpl (the template @p cls is an instance of, or
  /// null), ...  A private import's name, or a failed import's, is not
  /// reported again.
  void reportUndefinedSuperclass(const std::string &super,
                                 const std::string &cls,
                                 const ast::ClassDecl *tmpl,
                                 ast::SourceLocation loc);

  /// `x: int, s: Str`: the parameters of @p fn, for a diagnostic.
  static std::string paramList(const ast::FuncDecl *fn);

  /// Reject an `__init__` in a class body: it goes in an `impl` block.
  bool rejectBodyInits(ast::TranslationUnit *tu);

  /// `destroy` names a class's compiler-generated destructor, and a struct
  /// has none: a method or `static fn` @p fn of @p owner (its class body's
  /// or an `impl` block's) named so is reported, and true returned.
  bool rejectReservedDestroy(const ast::FuncDecl *fn, ast::Type *owner);

  /// Report each field of this module's generic class, struct or enum
  /// @p tmpl (a variant's too) named like a type, once, at the field: its
  /// first instantiation asks (populateClassType, populateStructType), and
  /// after the declarations every template does, so one never instantiated
  /// is checked too.  An instantiation leaves such a field out as a
  /// follow-on of that error.
  void checkTemplateFieldNames(const ast::Decl *tmpl);
  /// The local template @p tmpl, or nullptr for another module's (whose
  /// fields its module checked) and for none.
  const ast::Decl *localTemplate(const ast::Decl *tmpl) const {
    return tmpl && !foreignOrigin(tmpl) ? tmpl : nullptr;
  }
  /// The templates checkTemplateFieldNames looked at.
  std::unordered_set<const ast::Decl *> TemplateFieldsChecked;

  /// Check that every `impl` block names a class or a struct of this
  /// module, and make its `static fn`s the module's functions `Type::name`.
  bool checkImplDecls(ast::TranslationUnit *tu);

  // -- Structs (SemaStruct.cpp, docs/language/11-structs.md)
  //
  // A struct is a value type with named fields and the methods of its `impl`
  // blocks; its type is an ast::StructType, a TupleType by its name.  Its
  // declaration goes through the same phases as a class's: the name, a
  // registered stub (so that class fields and signatures can name it), its
  // fields and methods, its constructor, and last its method bodies.

  /// Phases 1-2: check the struct names and register a stub for each.
  bool declareStructs(ast::TranslationUnit *tu);
  /// Phases 3-4: resolve the fields (in an order where every struct a field
  /// holds comes first, so a struct that contains itself is reported), the
  /// `impl` methods, the `__init__` (the default one when none is written)
  /// and the constructor function.
  bool populateStructs();
  bool populateStructType(ast::StructDecl *sd, ast::StructType *st);
  /// Phase 4 for the `impl` blocks of @p sd's struct: their methods join it.
  bool populateImplMethods(ast::StructDecl *sd, ast::StructType *st);
  /// Give @p sd, which declares no `__init__`, the default one: one
  /// parameter per field, in order, stored into the field.
  bool declareDefaultInit(ast::StructDecl *sd, ast::StructType *st);
  /// That default `__init__` of @p st, at @p loc, not declared yet.
  ast::FuncDecl *defaultInit(ast::StructType *st, ast::SourceLocation loc);
  /// Register the constructor function `Name(...)`.
  void declareConstructor(ast::StructDecl *sd, ast::StructType *st);
  /// Phase 5: the method bodies, over SortedStructs.
  bool checkStructBodies();
  /// The module's structs declareStructs registered, in source order.
  std::vector<ast::StructDecl *> DeclaredStructs;
  /// `struct 'P'`, or for the struct of an enum's variant's fields
  /// `variant 'V' of enum 'E'`.
  static std::string describeStruct(const ast::StructType *st);
  /// The structs of the module's enums' variants' fields (`E::V`), by enum
  /// name: a struct that holds the enum holds them in place.
  StringMap<std::vector<std::string>> EnumPayloadStructs;
  /// The structs in the order populateStructs resolved them: every struct
  /// before the structs that hold it.
  std::vector<ast::StructDecl *> SortedStructs;

  // -- Generic structs: templates and instantiations, as for classes
  // (registerGenericTemplates registers them, resolveType and
  // resolveGenericCall instantiate them).

  /// Struct templates declared by this module (and, qualified, imported),
  /// by name.
  StringMap<ast::StructDecl *> StructTemplates;
  /// Instantiation cache by canonical instantiation name; nullptr records a
  /// failure.
  StringMap<ast::StructType *> StructInstantiations;
  /// What an instantiated StructType was made from (for inference through
  /// `Pair<T>` parameters).
  struct StructInstantiationInfo {
    const ast::StructDecl *Template;
    std::vector<ast::Type *> Args;
  };
  std::unordered_map<ast::StructType *, StructInstantiationInfo>
      StructInstantiationInfos;
  /// What @p st was instantiated from: an instantiation made here, or one
  /// an imported module made of a template this module sees.  nullptr for
  /// any other struct.
  const StructInstantiationInfo *structInstantiationInfo(ast::StructType *st);
  /// Instantiate struct template @p name with canonical @p args, or return
  /// the cached instantiation.  Registers the struct (stub, fields, `impl`
  /// methods, constructor) immediately and queues its bodies.  Returns
  /// nullptr after reporting an error.
  ast::StructType *instantiateStruct(const std::string &name,
                                     const std::vector<ast::Type *> &args,
                                     ast::SourceLocation loc);
  /// Whether a value of @p ty holds a @p target in place: it is one, or a
  /// tuple or a struct holding one.
  static bool holdsStruct(const ast::Type *ty, const ast::StructType *target);
  /// holdsStruct, but not looking into the aggregates in @p seen, which
  /// hold no @p target, and adding those it looks into: one reached by
  /// many paths (each variant of an enum holding the same enum) is looked
  /// into once, where the paths to the innermost of n such enums are 3^n.
  static bool holdsStruct(const ast::Type *ty, const ast::StructType *target,
                          std::unordered_set<const ast::Type *> &seen);
  /// Instantiated structs in creation order (injectInstantiations).
  std::vector<ast::StructDecl *> InstantiatedStructDecls;
  std::vector<ast::StructDecl *> ForeignInstantiatedStructDecls;

  // -- Generic enums: templates and instantiations, as for structs
  // (registerGenericTemplates registers them; resolveType and a variant
  // with its type arguments, written or inferred, instantiate them).

  /// Enum templates declared by this module (and, qualified, imported), by
  /// name.
  StringMap<ast::EnumDecl *> EnumTemplates;
  /// Instantiation cache by canonical instantiation name; nullptr records a
  /// failure.
  StringMap<ast::EnumType *> EnumInstantiations;
  /// What an instantiated EnumType was made from (for inference through
  /// `Maybe<T>` parameters).
  struct EnumInstantiationInfo {
    const ast::EnumDecl *Template;
    std::vector<ast::Type *> Args;
  };
  std::unordered_map<ast::EnumType *, EnumInstantiationInfo>
      EnumInstantiationInfos;
  /// What @p et was instantiated from: an instantiation made here, or one
  /// an imported module made of a template this module sees.  nullptr for
  /// any other enum.
  const EnumInstantiationInfo *enumInstantiationInfo(ast::EnumType *et);
  /// Instantiate enum template @p name with canonical @p args, or return
  /// the cached instantiation.  Registers the enum and its variants' fields
  /// (the structs `Maybe<int>::Just`) immediately.  Returns nullptr after
  /// reporting an error.
  ast::EnumType *instantiateEnum(const std::string &name,
                                 const std::vector<ast::Type *> &args,
                                 ast::SourceLocation loc);
  /// The module's own instantiations, in creation order: they join the
  /// module's enums (injectInstantiations), which its interface exports.
  std::vector<ast::EnumDecl *> InstantiatedEnumDecls;
  /// The type a variant of a generic enum written without its type
  /// arguments (`Maybe::Nothing`, `Maybe::Just(1)`) flows into (set by
  /// visitExpecting): an instantiation of its template gives them.
  ast::Type *VariantExpectation = nullptr;
  /// Whether @p name is a class, struct or function template.
  bool isTemplate(const std::string &name) const {
    return ClassTemplates.count(name) || StructTemplates.count(name) ||
           FuncTemplates.count(name);
  }
  /// Whether @p name is `Tmpl::member`, a function of the `impl` block of a
  /// class or struct template (`Box<int>::of(1)`, resolveGenericCall).
  bool isTemplateStatic(const std::string &name) const {
    const size_t sep = name.rfind("::");
    if (sep == std::string::npos)
      return false;
    const std::string owner = name.substr(0, sep);
    return ClassTemplates.count(owner) || StructTemplates.count(owner);
  }
  /// An optional of a tuple or a struct holds the value in place: one of
  /// more than this many bytes is rejected (checkOptionalValueSize), to be
  /// boxed.
  static constexpr int64_t kOptionalValueLimit = 64;
  /// The size in bytes of a value of @p ty held in place, as the lowering
  /// lays it out (ModuleLowering::memLayout); 8 for a reference.
  static int64_t valueSize(const ast::Type *ty);
  /// Report `inner?` (the @p context at @p loc) when @p inner, a tuple or a
  /// struct, is more than kOptionalValueLimit bytes.
  bool checkOptionalValueSize(ast::Type *inner, ast::SourceLocation loc,
                              const TypeContext &context);
  /// Split the canonical instantiation name @p name (`Tmpl<arg1, arg2>`)
  /// into the template's name and its arguments, resolved in this context;
  /// false when it is not one or an argument does not resolve.
  bool parseInstantiationName(const std::string &name, std::string &base,
                              std::vector<ast::Type *> &args);
  /// The method @p name of @p ty, a class (its own or inherited, vtable or
  /// `impl`) or a struct (`impl`); nullptr for any other type.
  static ast::MethodDecl *methodOf(ast::Type *ty, const std::string &name);
  /// "class" or "struct", for a diagnostic about @p ty.
  static const char *typeKindWord(const ast::Type *ty);

  /// The `impl` blocks by the name of the class they add to: an
  /// instantiation's are copies of its template's.
  StringMap<std::vector<ast::ImplDecl *>> ImplsByClass;
  /// The `impl` blocks of the module's generic classes and structs, by
  /// template.
  std::unordered_map<const ast::Decl *, std::vector<ast::ImplDecl *>>
      TemplateImpls;
  /// The type parameters of the `impl` block an instantiation's method or
  /// `static fn` was copied from, by that block's own names (`impl Box<U>`):
  /// its declaration and its body are checked with those.
  std::unordered_map<const ast::FuncDecl *,
                     const std::vector<const std::string *> *>
      ImplMethodTypeParams;
  /// Record the methods of @p copy, an instantiation's copy of @p im, there.
  void noteImplTypeParams(const ast::ImplDecl *im, const ast::ImplDecl *copy);
  /// Set once checkImplDecls has run (see PendingTraitImpls).
  bool ImplsChecked = false;

  // -- Traits (SemaTraits.cpp)

  struct TraitInfo {
    ast::TraitDecl *Decl = nullptr;
    bool IsBuiltin = false;
    /// A requirement's signature did not resolve (reported with the trait).
    bool Erroneous = false;
  };
  /// A trait with its type arguments: `Into<int>`.
  struct TraitRef {
    const TraitInfo *Trait = nullptr;
    std::vector<ast::Type *> Args;
  };
  /// A requirement's signature for one implementation of its trait.
  struct RequiredSig {
    ast::Type *Ret = nullptr;
    std::vector<ast::Type *> Params;
    ast::ParamModes Modes;
  };
  using Subst = std::unordered_map<std::string, ast::Type *>;

  StringMap<TraitInfo> Traits;
  /// The traits each type implements itself (a class also implements its
  /// superclasses'), by canonical type.
  std::unordered_map<const ast::Type *, std::vector<TraitRef>> TraitImpls;
  /// The type `Self` names while a method's signature is resolved (in its
  /// body, the class or struct being checked).
  ast::Type *SelfType = nullptr;

  /// Register the builtin trait `Show`, which `Obj` implements.
  void registerBuiltinTraits();
  /// Register the module's traits by name (their signatures are checked by
  /// checkTraitDecls, once classes can be named).
  bool declareTraits(ast::TranslationUnit *tu);
  bool checkTraitDecls(ast::TranslationUnit *tu);
  /// @p fn's signature with @p subst applied (`Self` and the trait's type
  /// parameters); false after reporting a type that does not resolve.
  bool resolveRequirement(const ast::FuncDecl *fn, const Subst &subst,
                          const std::string &traitName, RequiredSig &out);
  /// The trait the annotation @p ref names (`Show`, `Into<T>` with @p subst
  /// applied); nullopt after an error.
  std::optional<TraitRef> resolveTraitRef(ast::Type *ref, const Subst &subst,
                                          ast::SourceLocation loc);
  static std::string traitRefName(const TraitRef &tr);
  /// Whether the trait annotation @p ref names a built-in trait (`Show`,
  /// `Hashable`, `ValueType`): the same trait in every module.
  bool namesBuiltinTrait(const ast::Type *ref) const;
  /// The trait name the annotation @p ref spells (`Named` of `Named<T>`);
  /// "" for an annotation that names no trait.
  static std::string traitAnnotationName(const ast::Type *ref);
  /// The type whose implementation of @p tr @p ty has (itself or, for a
  /// class, a superclass); nullptr if it does not implement it.
  const ast::Type *traitImplOwner(ast::Type *ty, const TraitRef &tr);
  /// Whether @p ty has a text: implements the builtin `Show`, whose
  /// `toString()` the print functions call.  A number, bool, char and
  /// object have one; a tuple, struct, enum or optional has one when every
  /// element, field, variant field or value does, so one holding a function
  /// value (which has none) has none.  Every rule about which types show
  /// themselves asks this.  When it has none, @p why (if given) says which
  /// part has none, from @p ty down ("field 'f' has type '(int) -> int'"),
  /// or is empty for a type without parts.
  bool hasText(ast::Type *ty, std::string *why = nullptr);
  /// The aggregates one hasText question looks into: the ones it is still
  /// deciding (the cycle guard, which takes one that holds itself to have
  /// a text: its error is reported where it is declared), and the ones it
  /// decided, each with why it has no text if it has none, so that an
  /// aggregate reached by many paths is looked into once.
  struct TextQuery {
    std::set<const ast::Type *> Open;
    std::unordered_map<const ast::Type *, std::optional<std::string>> Decided;
    /// How many times the cycle guard answered: a text decided after it
    /// answered may rest on it, and is not kept in Decided.
    unsigned Assumed = 0;
  };
  /// hasText, as part of @p query.
  bool hasText(ast::Type *ty, std::string *why, TextQuery &query);
  /// The end of a message that @p ty, which has no text, does not implement
  /// `Show`: "its field 'f' has type '(int) -> int', which does not
  /// implement it", naming the part that has none (hasText); "" for a type
  /// without parts.
  std::string noTextReason(ast::Type *ty);
  /// Record `impl Trait for A` (@p ty, a class or a struct) and check that
  /// A has every method the trait requires, with its signature.
  bool checkTraitImpl(ast::ImplDecl *im, ast::Type *ty);
  /// Whether method @p m has the signature @p sig of requirement @p req.
  static bool sameSignature(const ast::MethodDecl *m, const ast::FuncDecl *req,
                            const RequiredSig &sig);
  /// "" when @p ty has every method @p tr requires, with its signature;
  /// otherwise why not: "it has no method 'view fn hash() -> int'", "its
  /// 'view fn hash() -> Str' does not match 'view fn hash() -> int'".
  std::string unmetRequirement(ast::Type *ty, const TraitRef &tr);

  // A type parameter's bounds are checked when its template is instantiated,
  // once the `impl ... for` blocks are recorded (TraitImplsKnown); until
  // then the check waits in PendingBoundChecks.
  bool TraitImplsKnown = false;
  /// Templates with a bound that names no trait (reported with them).
  std::set<const ast::Decl *> BadBounds;

  /// Check that the bounds of the module's templates name traits.
  bool checkBoundDecls(ast::TranslationUnit *tu);
  /// Check that @p args implement the traits template @p tmpl (named
  /// @p name) requires of its type parameters.
  bool checkBounds(const ast::Decl *tmpl, const std::string &name,
                   const std::vector<const std::string *> &params,
                   const ast::TypeParamBounds &bounds,
                   const std::vector<ast::Type *> &args,
                   ast::SourceLocation loc);
  bool checkPendingBounds();
  /// Report that @p tmpl, another module's template, requires its type
  /// parameter @p param to implement @p trait, a trait of that module,
  /// @p module (#147).  At this module's own request of the instantiation
  /// that led to @p loc, when one is on InstantiationStack; at @p loc
  /// otherwise.  Once per place, template, type parameter and trait
  /// (ReportedForeignBounds).
  void reportForeignTraitBound(const std::string &tmpl,
                               const std::string &param,
                               const std::string &trait,
                               const std::string &module,
                               ast::SourceLocation loc);
  StringSet ReportedForeignBounds;

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
  /// `Box<T>` parameters).
  struct InstantiationInfo {
    const ast::ClassDecl *Template;
    std::vector<ast::Type *> Args;
  };
  std::unordered_map<ast::ClassType *, InstantiationInfo>
      ClassInstantiationInfo;
  /// What @p ct was instantiated from: an instantiation made here, or one an
  /// imported module made of a template this module sees.  nullptr for any
  /// other class.
  const InstantiationInfo *instantiationInfo(ast::ClassType *ct);

  /// A template one of whose instantiations is in progress, and that
  /// instantiation's name (interned, as its copy of the template has it).
  struct InstantiatingTemplate {
    const ast::Decl *Template;
    const std::string *Name;
  };
  /// Instantiations whose bodies still have to be checked.
  struct PendingInstantiation {
    ast::ClassDecl *Class = nullptr; // exactly one of Class / Func / Struct
    ast::FuncDecl *Func = nullptr;
    ast::StructDecl *Struct = nullptr;
    /// How many instantiations deep the request was made (instantiationDepth
    /// there), so that a body instantiating a template with an ever-growing
    /// argument (`Deep<Deep<T>>`) is stopped as a field doing it is.
    size_t Depth = 0;
    std::string Name;               // canonical instantiation name
    ast::SourceLocation RequestLoc; // where the instantiation was requested
    StringSet TypeParams;           // the template's type parameter names
    /// The module of a template of another module (null for this one's):
    /// its body's locations are in that module's file.
    const ModuleTemplates *Origin = nullptr;
    /// The file RequestLoc is in (see InstantiationFrame).
    std::string RequestFile;
    const std::vector<std::string> *RequestLines = nullptr;
    /// The templates its body is checked inside: InstantiatingTemplates
    /// where it was requested, then its own.
    std::vector<InstantiatingTemplate> Templates;
  };
  std::vector<PendingInstantiation> PendingInstantiations;
  /// Declare the `static fn`s of the `impl` blocks of the instantiation
  /// @p instName of @p type (ImplsByClass) as the functions `Inst::name`,
  /// their bodies pending like @p base, the instantiation's own entry.
  void declareInstantiationStatics(const std::string &instName, ast::Type *type,
                                   const PendingInstantiation &base);

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
    /// The file RequestLoc is in (another module's, inside a template of it).
    std::string File;
    const std::vector<std::string> *Lines = nullptr;
  };
  std::vector<InstantiationFrame> InstantiationStack;
  /// Where each class, struct and enum instantiation made here was first
  /// requested, by instantiation name: its template, the request, and the
  /// instantiations it was requested in (InstantiationStack then).  Type
  /// names are global, so another template's instantiation of the name
  /// clashes with it (OD-9), and the clash is shown at both (SW-28).
  struct InstantiationSite {
    const ast::Decl *Template = nullptr;
    InstantiationFrame Request;
    std::vector<InstantiationFrame> Frames;
  };
  StringMap<InstantiationSite> InstantiationSites;
  /// Records @p request, of an instantiation of @p tmpl, in
  /// InstantiationSites.
  void recordInstantiationSite(const ast::Decl *tmpl,
                               const InstantiationFrame &request);
  /// The instantiation names whose clash was reported: once per name.
  StringSet ReportedInstantiationClashes;
  /// Reports that @p instName, the instantiation of @p tmpl requested at
  /// @p loc, has the name of another template's instantiation or of an
  /// imported type, once per name (a later clash of the name is a suppressed
  /// follow-on).  The error is at the request this module's own code made
  /// (the other instantiation's, when only it is one), with notes at the
  /// other template's declaration and its instantiation's request; for an
  /// imported type, at the import it came in through.
  void reportInstantiationClash(const ast::Decl *tmpl,
                                const std::string &instName,
                                ast::SourceLocation loc);
  /// The import of this module each imported type came in through, by name.
  StringMap<ast::SourceLocation> ImportedTypeImports;
  /// A bound check waiting for TraitImplsKnown.
  struct PendingBoundCheck {
    const ast::Decl *Template;
    const std::vector<const std::string *> *Params; // the template's own
    const ast::TypeParamBounds *Bounds;             // the template's own
    std::string Name;
    std::vector<ast::Type *> Args;
    ast::SourceLocation Loc;
    /// Where it was asked for: the diagnostic's file and instantiation notes.
    const ModuleTemplates *Origin;
    std::vector<InstantiationFrame> Frames;
  };
  std::vector<PendingBoundCheck> PendingBoundChecks;
  /// The instantiations whose deferred bound check failed (KF-6): made
  /// before the check could run, they would not have been made after it,
  /// so their bodies are not checked, and a bound check of an instantiation
  /// made in one of them is a follow-on.  By instantiation name.
  StringSet FailedBoundInstances;
  /// Who asked for each instantiation before the bounds could be checked
  /// (TraitImplsKnown false): the innermost instantiation in progress at
  /// the request, or "" outside one.  By instantiation name.
  StringMap<StringSet> EarlyRequesters;
  /// The instantiations of EarlyRequesters asked for again afterwards
  /// outside every instantiation made only for failed bounds.
  StringSet LateRequested;
  /// Note a request, here, of the instantiation @p instName.
  void noteInstantiationRequest(const std::string &instName);
  /// Whether the instantiation @p instName was made only for those whose
  /// deferred bounds failed: each request of it was made inside one of
  /// them, or inside one made only for them.  Its bodies are a follow-on.
  bool madeOnlyForFailedBounds(const std::string &instName);
  /// Whether @p instName was asked for outside each of those (the
  /// instantiations in @p visiting, on the way, not counting).
  bool requestedFreely(const std::string &instName, StringSet &visiting);
  /// The name the instantiation of @p tmpl, named @p name where it was
  /// requested, with @p args has: InstantiationFrame::Name and
  /// PendingInstantiation::Name of it.
  std::string instanceNameOf(const ast::Decl *tmpl, const std::string &name,
                             const std::vector<ast::Type *> &args);
  /// Whether every struct's fields are known (populateStructs): until then
  /// a struct's size is not, and the checks of the optionals resolved so far
  /// (a class's fields and signatures) wait in DeferredOptionalChecks.
  bool StructsPopulated = false;
  struct DeferredOptionalCheck {
    ast::Type *Inner;
    ast::SourceLocation Loc;
    std::string Context;
    /// Where it was resolved: the diagnostic's file and instantiation notes.
    const ModuleTemplates *Origin;
    std::vector<InstantiationFrame> Frames;
  };
  std::vector<DeferredOptionalCheck> DeferredOptionalChecks;
  /// The types reported (by name): a second use of one is a follow-on.
  StringSet ReportedOptionalLimits;
  /// The depth of the instantiation whose body is being checked
  /// (checkPendingInstantiations); 0 in hand-written code.
  size_t CurrentInstantiationDepth = 0;
  /// How many instantiations deep the current point is: the body's own
  /// depth plus the instantiations in progress within it.
  size_t instantiationDepth() const {
    return CurrentInstantiationDepth + InstantiationStack.size();
  }
  /// The `impl Trait for` copies of instantiations made before the module's
  /// `impl` blocks are checked (ImplsChecked): checked with them.
  struct PendingTraitImpl {
    ast::ImplDecl *Impl;
    ast::Type *Type; // the class (or struct) implementing the trait
    InstantiationFrame Request;
    const ModuleTemplates *Origin;
  };
  std::vector<PendingTraitImpl> PendingTraitImpls;
  static constexpr size_t kMaxInstantiationDepth = 16;
  /// The longest name an instantiation made inside an instantiation of its
  /// own template, with type arguments nested more deeply than that one's,
  /// may have, as diagnostics spell it (OD-2): a longer one is the
  /// ever-growing type argument the depth limit is for, stopped before the
  /// names grow to 2^16 or 3^16 types (SW-04).
  static constexpr size_t kMaxGrowingInstantiationName = 4096;
  /// The templates of the instantiations in progress here, outermost
  /// first: those being declared or populated, and, in an instantiation's
  /// body, the ones it was requested inside (PendingInstantiation::Templates).
  std::vector<InstantiatingTemplate> InstantiatingTemplates;
  /// How deeply the instantiation name @p name nests its type arguments:
  /// the most parentheses and angle brackets open at once.  Instantiating
  /// a template with its arguments swapped or repeated (`Pair<B, A>`,
  /// `Pair<A, A>`) keeps it; an argument built from them (`C<(T, T)>`)
  /// adds to it.
  static size_t argumentNesting(const std::string &name);
  /// False, after reporting it, when the instantiation @p instName of
  /// @p tmpl, requested at @p loc inside an instantiation of @p tmpl
  /// (InstantiatingTemplates, the innermost one), nests its type arguments
  /// more deeply than that one and has a name over
  /// kMaxGrowingInstantiationName characters.
  bool checkGrowingInstantiation(const ast::Decl *tmpl,
                                 const std::string &instName,
                                 ast::SourceLocation loc);

  /// Type parameter names of the instantiation whose body or declarations
  /// are being checked (nullptr outside one).  Used to diagnose a type
  /// parameter used as a value, or as the name of a field, a parameter or a
  /// local (isTypeNameForVariable).
  const StringSet *CurrentTypeParams = nullptr;
  /// While alive, CurrentTypeParams is @p params: a template's declarations
  /// (its fields, its parameters, its methods' and `static fn`s') are
  /// checked with its own type parameters, not those of the generic body
  /// that instantiates it.
  class TypeParamScope {
    Sema &S;
    const StringSet *Saved;
    StringSet Params;

  public:
    TypeParamScope(Sema &s, const std::vector<const std::string *> &params);
    ~TypeParamScope() { S.CurrentTypeParams = Saved; }
    TypeParamScope(const TypeParamScope &) = delete;
    TypeParamScope &operator=(const TypeParamScope &) = delete;
  };
  /// Open in @p scope a TypeParamScope of the names of the `impl` block
  /// @p fn was copied from, when it is an instantiation's (see
  /// ImplMethodTypeParams); otherwise leave it empty.
  void implTypeParamScope(std::optional<TypeParamScope> &scope,
                          const ast::FuncDecl *fn);

  /// Classes constructed inside each class's method bodies (by name).  Used to
  /// order instantiated classes for the lowering: a constructor must be
  /// emitted before a method that calls it.
  StringMap<StringSet> ConstructsEdges;

  // -- Templates of other modules (docs/design/pkm.md §4)

  /// Each registered template of another module, and its module.
  std::unordered_map<const ast::Decl *, const ModuleTemplates *>
      ForeignTemplates;
  /// Modules registered under their hidden qualifier (injectHidden).
  StringSet HiddenModules;
  /// The file each module this Sema loaded is cached under, by canonical
  /// name (ModuleTemplates::Uses).
  StringMap<std::string> ModulePaths;
  /// This module's instantiations of other modules' templates (see
  /// SemaContext::ForeignClasses).
  std::vector<ast::ClassDecl *> ForeignInstantiatedClassDecls;
  std::vector<ast::FuncDecl *> ForeignInstantiatedFuncDecls;
  /// See SemaContext::ForeignSources.
  std::unordered_map<const ast::Decl *, SemaContext::SourceFile>
      ForeignInstantiationSources;
  /// Record that @p d, an instantiation of a template of @p origin's
  /// module, points into that module's file.
  void noteForeignSource(const ast::Decl *d, const ModuleTemplates &origin);
  /// See SemaContext::ImportedNatives.
  StringSet ImportedNatives;
  /// See SemaContext::ImportedInstanceClasses.
  std::vector<ast::ClassDecl *> ImportedInstanceClassDecls;
  std::vector<ast::StructDecl *> ImportedInstanceStructDecls;
  /// The instantiations copyImportedInstance has seen, by name.
  StringSet CopiedInstances;
  /// When @p type, which this module's code uses at @p loc, is a class or
  /// struct an imported module's interface gives as an instantiation of a
  /// template this module sees (its module's TMPL), copy it and queue the
  /// copy's bodies (SemaContext::ImportedInstanceClasses), once; and the
  /// instantiations its fields hold (copyImportedParts).  Its uses are the
  /// ones that would make a copy of this module's own: a name for the type
  /// (instantiateClass, instantiateStruct), and a call of its method.
  void copyImportedInstance(ast::Type *type, ast::SourceLocation loc);
  /// Register module @p canonical, the origin of an imported instantiation
  /// (ImportedTypeOrigins), under its hidden qualifier, so that its
  /// templates are seen, when this module neither imports it nor sees it
  /// through an import's templates.
  void seeTemplatesOf(const std::string &canonical, ast::SourceLocation loc);
  /// The same for every class and struct a value of @p type holds in place
  /// or names: an optional's, an array's, a tuple's, a variant's.
  void copyImportedParts(ast::Type *type, ast::SourceLocation loc);
  /// The methods of the copy of the imported instantiation @p type of
  /// @p tmpl with @p args: @p methods, its class body's (cloned with it),
  /// then the copies of its template's `impl` blocks' and, when none is an
  /// `__init__`, its default one, which this appends.  Each signature is
  /// resolved, as an instantiation's are; the type's own methods are the
  /// ones the exporting module's interface gave it.  False, after which
  /// the copy is dropped, when one of them is not the type's.
  bool copiedMethods(const ast::Decl *tmpl,
                     const std::vector<ast::Type *> &args, ast::Type *type,
                     std::vector<ast::FuncDecl *> &methods);

  /// Register the templates of the module @p info describes under
  /// @p qualifier and @p fullModulePath, and every module their bodies
  /// name under its hidden qualifier.
  bool injectTemplates(const ModuleInfo &info, const std::string &qualifier,
                       const std::string &fullModulePath,
                       ast::SourceLocation loc);
  /// Register module @p canonical (described by @p info) under its hidden
  /// qualifier, once.
  bool injectHidden(const ModuleInfo &info, const std::string &canonical,
                    ast::SourceLocation loc);
  /// The module of template @p tmpl when it is another module's.
  const ModuleTemplates *foreignOrigin(const ast::Decl *tmpl) const;

  /// The module whose template is being checked (CurrentModule), or nullptr
  /// for this module's own code.
  const ModuleTemplates *CurrentOrigin = nullptr;
  /// This module's own file (Sema::run): an InstantiationFrame in it is a
  /// request this module's code made.
  std::string OwnSourceName;
  const std::vector<std::string> *OwnSourceLines = nullptr;
  /// While alive, the code checked is @p origin's module's (a template of
  /// another module, or this module's own code for a null origin), and
  /// diagnostics point into that module's file.
  class OriginSource {
    Sema &S;
    const ModuleTemplates *SavedOrigin;
    std::string SavedName;
    const std::vector<std::string> *SavedLines;
    bool Active;

  public:
    OriginSource(Sema &s, const ModuleTemplates *origin);
    ~OriginSource();
    OriginSource(const OriginSource &) = delete;
    OriginSource &operator=(const OriginSource &) = delete;
  };
  /// The instantiation frame for @p name requested at @p loc, in the file
  /// diagnostics currently point into.
  InstantiationFrame frameAt(const std::string &name, ast::SourceLocation loc);

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
  /// The default `__init__` of each class given one (declareDefaultInit).
  StringMap<const ast::FuncDecl *> DefaultInits;

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
  /// @p out holds one canonical type per type parameter, in order.  The
  /// hint on failure spells the call with them as @p spelling
  /// (`Tmpl<...>(...)` by default).  The type parameters the arguments
  /// leave open are taken from @p expected, the type the call's value flows
  /// into, matched against the template's result type @p resultPattern
  /// (`r: Result<int, Error> = ok(1)` gives `ok`'s `E`).
  bool inferTypeArgs(const std::string &templateName,
                     const std::vector<const std::string *> &typeParams,
                     const std::vector<ast::Type *> &paramTypes,
                     const std::vector<ast::Type *> &argTypes,
                     ast::SourceLocation loc, std::vector<ast::Type *> &out,
                     const std::string &spelling = "",
                     ast::Type *resultPattern = nullptr,
                     ast::Type *expected = nullptr);

  /// Resolve a call whose callee is a template (or carries explicit type
  /// arguments): instantiate and rewrite the callee name.  A generic
  /// function's type arguments its arguments leave open come from
  /// @p expected, the type its value flows into.  Returns false after
  /// reporting an error.
  bool resolveGenericCall(ast::CallExpr *node,
                          const std::vector<ast::Type *> &argTypes,
                          ast::Type *expected = nullptr);

  /// True when @p name is a conversion target (`Str`, `int`, `float`,
  /// `bool`, `char`), i.e. `name<Source>(value)` is
  /// a conversion, or the spelled form (`int<float>`) checkConversion rebinds
  /// such a call to.
  static bool isConversionTarget(std::string_view name);

  /// True when @p node is a call of the `Str` constructor rather than a
  /// conversion: `Str(s)` with a `Str` argument, or a `Str(...)` call whose
  /// argument count or argument is already wrong.
  bool isStrConstruction(ast::CallExpr *node,
                         const std::vector<ast::Type *> &argTypes) const;

  /// The conversion `Str<source>` of a type no runtime conversion takes, in
  /// @p pair: a type with a text (hasText) is its `toString()`, spelled
  /// `Str<T>` (the lowering's showCallOf), and a `Str` is the constructor,
  /// itself.  Returns false after reporting, at @p loc, a type without one.
  bool textConversion(ast::Type *source, ast::SourceLocation loc,
                      ConversionPair &pair);

  /// Check a conversion constructor without its type argument, `Target(value)`
  /// (#88): it picks the specialization whose source is exactly the
  /// argument's type (`Str(n)` with an `int` is `Str<int>(n)`), or reports
  /// that there is none.  Rebinds the callee like checkConversion.
  ast::Type *inferConversion(ast::CallExpr *node,
                             const std::vector<ast::Type *> &argTypes);

  /// Rebind a checked conversion @p node to @p pair's spelled form and
  /// return its result type, the target.
  ast::Type *applyConversion(ast::CallExpr *node, const ConversionPair *pair);

  /// Check a conversion constructor `Target<Source>(value)` (#64, #88): the
  /// pair must be one of a primitive target's closed set of specializations,
  /// or a `Str` of a type with a text (textConversion), and the argument
  /// must have exactly the Source type.  Rebinds the callee to the spelled
  /// conversion (`names::kConvIntFloat`, ...) for the lowering and returns
  /// the result type, or nullptr after reporting an error.
  ast::Type *checkConversion(ast::CallExpr *node,
                             const std::vector<ast::Type *> &argTypes);

  /// Check the bodies of every pending instantiation (transitively).
  bool checkPendingInstantiations();
  /// Check the bodies of the pending instantiations, and of those they
  /// queue in turn, and empty the list; move to @p held each one made only
  /// for failed bounds so far (madeOnlyForFailedBounds).
  bool checkPendingInstantiationBodies(std::vector<PendingInstantiation> &held);

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
  /// built or run needs `fn main() -> int` or `fn main(args: view Str[]) ->
  /// int`.
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
