// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Abstract Syntax Tree node definitions for the Paykan language

#pragma once

#include <cassert>
#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace paykan {
namespace ast {

// Forward declarations
class ASTContext;

// Source location information
class SourceLocation {
  size_t LineStart;
  size_t ColumnStart;
  size_t LineEnd;
  size_t ColumnEnd;

public:
  SourceLocation() : LineStart(0), ColumnStart(0), LineEnd(0), ColumnEnd(0) {}
  SourceLocation(size_t lineStart, size_t colStart, size_t lineEnd,
                 size_t colEnd)
      : LineStart(lineStart), ColumnStart(colStart), LineEnd(lineEnd),
        ColumnEnd(colEnd) {}

  size_t getLineStart() const { return LineStart; }
  size_t getColumnStart() const { return ColumnStart; }
  size_t getLineEnd() const { return LineEnd; }
  size_t getColumnEnd() const { return ColumnEnd; }
  bool isValid() const { return LineStart > 0; }
};

// Forward declaration — Type is defined later in this header.
class Type;

// Forward declaration — EnumDecl is defined later (after TranslationUnit) but
// referenced by TranslationUnit's member vector.
class EnumDecl;
class ImplDecl;
class TraitDecl;
class StructDecl;

// Base AST node with LLVM-style RTTI
class ASTNode {
public:
  enum NodeKind {
    // Declarations
    NK_VarDecl,
    NK_FuncDecl,
    NK_MethodDecl,
    NK_ImportDecl,
    NK_ClassDecl,
    NK_EnumDecl,
    NK_ImplDecl,
    NK_TraitDecl,
    NK_StructDecl,

    // Statements
    NK_StmtBegin,
    NK_CompoundStmt = NK_StmtBegin,
    NK_ReturnStmt,
    NK_AssignStmt,
    NK_DeclStmt,
    NK_ExprStmt,
    NK_IfStmt,
    NK_WhileStmt,
    NK_BreakStmt,
    NK_ContinueStmt,
    NK_MemberAssignStmt,
    NK_MatchStmt,
    NK_SubscriptAssignStmt,
    NK_DestructureStmt,
    NK_StmtEnd = NK_DestructureStmt,

    // Expressions
    NK_IntegerLiteral,
    NK_FloatLiteral,
    NK_BoolLiteral,
    NK_CharLiteral,
    NK_NoneLiteral,
    NK_StringLiteral,
    NK_UnaryExpr,
    NK_BinaryExpr,
    NK_Identifier,
    NK_CallExpr,
    NK_MethodCallExpr,
    NK_TernaryExpr,
    NK_MemberAccessExpr,
    NK_ArrayLiteralExpr,
    NK_SubscriptExpr,
    NK_EnumValueExpr,
    NK_TupleLiteralExpr,
    NK_TupleIndexExpr,
    NK_CallValueExpr,
    NK_LambdaExpr, // must stay the last Expr kind (see Expr::classof)

    // Types
    NK_BuiltinType,
    NK_ClassType,
    NK_ArrayType,
    NK_FixedArrayType,
    NK_OptionalType,
    NK_EnumType,
    NK_TupleType,
    NK_StructType, // a TupleType with a name and named fields (see TupleType)
    NK_SliceType,
    NK_PoisonType,
    NK_FunctionType,
    NK_GenericType, // must stay the last Type kind (see Type::classof)

    // Match arm (child of MatchStmt, not a Stmt itself)
    NK_MatchArm,

    // Top-level
    NK_TranslationUnit,
  };

private:
  const NodeKind Kind;
  const SourceLocation Loc;

public:
  ASTNode(NodeKind K, SourceLocation loc) : Kind(K), Loc(loc) {}
  virtual ~ASTNode() = default;

  NodeKind getKind() const { return Kind; }
  SourceLocation getLocation() const { return Loc; }
};

// Concept for valid AST node types: must derive from ASTNode and provide
// LLVM-style RTTI via a static classof method.
template <typename T>
concept ASTNodeType =
    std::derived_from<T, ASTNode> && requires(const ASTNode *n) {
      { T::classof(n) } -> std::same_as<bool>;
    };

// -- LLVM-style RTTI free functions

template <ASTNodeType T> bool isa(const ASTNode *n) { return T::classof(n); }

template <ASTNodeType T> T *cast(ASTNode *n) {
  assert(isa<T>(n) && "Invalid cast");
  return static_cast<T *>(n);
}

template <ASTNodeType T> const T *cast(const ASTNode *n) {
  assert(isa<T>(n) && "Invalid cast");
  return static_cast<const T *>(n);
}

template <ASTNodeType T> T *dyn_cast(ASTNode *n) {
  return n && isa<T>(n) ? cast<T>(n) : nullptr;
}

template <ASTNodeType T> const T *dyn_cast(const ASTNode *n) {
  return n && isa<T>(n) ? cast<T>(n) : nullptr;
}

// Base for all declarations
class Decl : public ASTNode {
public:
  Decl(NodeKind K, SourceLocation loc) : ASTNode(K, loc) {}

  static bool classof(const ASTNode *N) {
    return N->getKind() >= NK_VarDecl && N->getKind() <= NK_StructDecl;
  }
};

// Base for all statements
class Stmt : public ASTNode {
public:
  Stmt(NodeKind K, SourceLocation loc) : ASTNode(K, loc) {}

  static bool classof(const ASTNode *N) {
    return N->getKind() >= NK_StmtBegin && N->getKind() <= NK_StmtEnd;
  }
};

// Base for all expressions
class Expr : public ASTNode {
  Type *ResolvedType = nullptr; // set by Sema after type-checking
  // Set by Sema when the value undergoes an implicit conversion at its use
  // site: a primitive flowing into an optional primitive slot (`int` ->
  // `int?`: the coerced type is the optional and the lowering makes the
  // value its present value, see wrapsInOptional).  nullptr = no conversion.
  Type *CoercedType = nullptr;
  // Set by Sema on a call of a function or method with an `inout` result
  // (`-> inout T`): its value is the storage it names, not a temporary.
  bool InoutResult = false;

public:
  Expr(NodeKind K, SourceLocation loc) : ASTNode(K, loc) {}

  void setResolvedType(Type *ty) { ResolvedType = ty; }
  Type *getResolvedType() const { return ResolvedType; }

  void setCoercedType(Type *ty) { CoercedType = ty; }
  Type *getCoercedType() const { return CoercedType; }

  void setInoutResult(bool inout) { InoutResult = inout; }
  bool isInoutResult() const { return InoutResult; }

  static bool classof(const ASTNode *N) {
    return N->getKind() >= NK_IntegerLiteral && N->getKind() <= NK_LambdaExpr;
  }
};

// -- Operator enums (at namespace scope so Type can reference them)

enum class UnaryOpcode {
  Neg, // -
  Not, // !
  Count,
};

enum class BinaryOpcode {
  // Arithmetic
  Add, // +
  Sub, // -
  Mul, // *
  Div, // /
  Mod, // %
  // Relational
  Lt, // <
  Gt, // >
  Le, // <=
  Ge, // >=
  Eq, // ==
  Ne, // !=
  // Logical
  And, // &&
  Or,  // ||
  Count,
};

// Base for all types
class Type : public ASTNode {
  uint32_t UnaryOps = 0;  // bitmask of supported UnaryOpcode values
  uint32_t BinaryOps = 0; // bitmask of supported BinaryOpcode values

public:
  Type(NodeKind K, SourceLocation loc) : ASTNode(K, loc) {}

  /// Register a unary operator. Returns true if newly added, false if already
  /// present.
  bool addUnaryOp(UnaryOpcode op) {
    uint32_t bit = 1u << static_cast<unsigned>(op);
    if (UnaryOps & bit)
      return false;
    UnaryOps |= bit;
    return true;
  }

  /// Register a binary operator. Returns true if newly added, false if already
  /// present.
  bool addBinaryOp(BinaryOpcode op) {
    uint32_t bit = 1u << static_cast<unsigned>(op);
    if (BinaryOps & bit)
      return false;
    BinaryOps |= bit;
    return true;
  }

  /// Returns true if the unary operator is defined for this type.
  bool hasUnaryOp(UnaryOpcode op) const {
    return (UnaryOps >> static_cast<unsigned>(op)) & 1u;
  }

  /// Returns true if the binary operator is defined for (this, rhs).
  bool hasBinaryOp(BinaryOpcode op, const Type *rhs) const {
    return hasOp(op) && rhs->hasOp(op);
  }

  /// Returns true if this type supports the given binary operator at all.
  bool hasOp(BinaryOpcode op) const {
    return (BinaryOps >> static_cast<unsigned>(op)) & 1u;
  }

  static bool classof(const ASTNode *N) {
    return N->getKind() >= NK_BuiltinType && N->getKind() <= NK_GenericType;
  }
};

// Variable declaration with explicit type (x: int = 10), a `let` local
// (let x = 10;), which cannot be reassigned, or a `weak` local or field
// (weak y: Point? = x;), which holds a weak reference
// (docs/language/16-memory.md, "Weak references").
// How a parameter or a local is held
// (docs/language/03-functions.md, "Parameter modes"): by value (the
// default), `view` (read-only) or `inout` (another name for the caller's
// storage, or for a place).
enum class ParamMode : uint8_t { Value, View, Inout };

/// The keyword of @p m: "view" or "inout" ("" for Value).
const char *paramModeName(ParamMode m);

class VarDecl : public Decl {
private:
  const std::string *Name; // points into ASTContext::StringPool (stable)
  Type *VarType;
  Expr *InitExpr;
  bool Let = false;
  bool Weak = false;
  ParamMode Mode = ParamMode::Value; // `x: view = e;`, `x: inout = p;`

public:
  VarDecl(SourceLocation loc, const std::string &internedName, Type *type,
          Expr *init)
      : Decl(NK_VarDecl, loc), Name(&internedName), VarType(type),
        InitExpr(init) {}

  const std::string &getName() const { return *Name; }
  Type *getType() const { return VarType; }
  /// Sema writes the canonical (resolved) type back over the parser's
  /// annotation so later passes never see a source-located stub or an
  /// unresolved generic type application (GenericType).
  void setType(Type *ty) { VarType = ty; }
  Expr *getInitExpr() const { return InitExpr; }

  /// Declared with `let`: the variable cannot be reassigned.
  bool isLet() const { return Let; }
  void setLet(bool let) { Let = let; }

  /// Declared with `weak`: the variable or field holds a weak reference to
  /// its object (its type is `T?` of a class `T`, and it reads as None once
  /// the object is destroyed).
  bool isWeak() const { return Weak; }
  void setWeak(bool weak) { Weak = weak; }

  /// A local borrow (docs/language/02-basics.md, "Local borrows"):
  /// `view` or `inout`; Value for any other declaration.
  ParamMode getMode() const { return Mode; }
  void setMode(ParamMode mode) { Mode = mode; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_VarDecl; }
};

// A single function parameter.
struct Param {
  const std::string *Name; // points into ASTContext::StringPool (stable)
  Type *ParamType;
  ParamMode Mode = ParamMode::Value; // `x: view int`, `x: inout int`

  const std::string &getName() const { return *Name; }
};

/// The modes of a callee's parameters, with their names for diagnostics.
/// Empty when every parameter is passed by value, as a callee's whose
/// declaration is not at hand (a builtin) is.
struct ParamModes {
  std::vector<ParamMode> Modes;
  std::vector<std::string> Names;
  /// How the result is returned: a copy, or a borrow (`-> view T`) of what
  /// the function was given as one (docs/language/03-functions.md,
  /// "Borrowed results").
  ParamMode Result = ParamMode::Value;

  bool empty() const { return Modes.empty(); }
  ParamMode mode(size_t i) const {
    return i < Modes.size() ? Modes[i] : ParamMode::Value;
  }
  std::string name(size_t i) const {
    return i < Names.size() ? Names[i] : std::string();
  }
  bool operator==(const ParamModes &) const = default;
};

/// The modes of @p params (empty when none has one).
ParamModes paramModes(const std::vector<Param> &params);

// Forward declaration for FuncDecl body.
class CompoundStmt;

/// The traits each type parameter must implement (`<T: Show + Eq>`), as
/// written: empty, or one (possibly empty) list per type parameter.
using TypeParamBounds = std::vector<std::vector<Type *>>;

// Free function declaration:  fn name(params) -> retType { body }
//
// A generic function (`fn first<T>(xs: view T[]) -> T`) carries its type
// parameter names in TypeParams.  Such a declaration is a template: Sema never
// checks it directly but instantiates it per distinct type-argument tuple by
// cloning it with the type parameters substituted (see ASTClone.h).
class FuncDecl : public Decl {
private:
  const std::string *Name; // points into ASTContext::StringPool (stable)
  std::vector<Param> Params;
  Type *ReturnType; // nullptr means void
  CompoundStmt *Body;
  // Type parameter names (interned); empty for an ordinary function.
  std::vector<const std::string *> TypeParams;
  TypeParamBounds Bounds;
  // An instantiation's type arguments, in TypeParams order; empty for a
  // declaration written by hand.
  std::vector<Type *> TypeArgs;
  // An instance of a generic native function: the parameters, and whether
  // the result, its template typed by a bare type parameter (`x: view T`,
  // `-> T`), which cross into C by address.
  std::vector<bool> ByAddress;
  bool ResultByAddress = false;
  // The C symbol of a `native fn` (interned), which has no Body; nullptr for
  // an ordinary function (#198).
  const std::string *NativeSymbol = nullptr;
  bool View = false;   // `view fn`: a method that does not change `self`
  bool Static = false; // `static fn`: an `impl` block's function, no `self`
  // Written `___init__` (Names.h): the initializer, private to its module.
  bool ModulePrivateInit = false;
  ParamMode ResultMode = ParamMode::Value; // `-> view T`

public:
  FuncDecl(SourceLocation loc, const std::string &internedName,
           std::vector<Param> params, Type *retTy, CompoundStmt *body,
           std::vector<const std::string *> typeParams = {})
      : Decl(NK_FuncDecl, loc), Name(&internedName), Params(std::move(params)),
        ReturnType(retTy), Body(body), TypeParams(std::move(typeParams)) {}

  const std::string &getName() const { return *Name; }
  /// Sema names an `impl` block's `static fn` after its type (`A::make`).
  void setName(const std::string &internedName) { Name = &internedName; }
  const std::vector<Param> &getParams() const { return Params; }
  /// Mutable access for Sema's canonical-type write-back (see VarDecl).
  std::vector<Param> &getMutableParams() { return Params; }
  Type *getReturnType() const { return ReturnType; }
  void setReturnType(Type *ty) { ReturnType = ty; }
  CompoundStmt *getBody() const { return Body; }

  const std::vector<const std::string *> &getTypeParams() const {
    return TypeParams;
  }
  bool isGeneric() const { return !TypeParams.empty(); }
  const TypeParamBounds &getTypeParamBounds() const { return Bounds; }
  void setTypeParamBounds(TypeParamBounds b) { Bounds = std::move(b); }

  /// The type arguments Sema instantiated this function with (`first<int>`);
  /// a native one passes C a description of each.
  const std::vector<Type *> &getTypeArgs() const { return TypeArgs; }
  void setTypeArgs(std::vector<Type *> args) { TypeArgs = std::move(args); }

  /// For an instance of a generic native function: parameter @p i, or the
  /// result, crosses into C by address (its template typed it `T`).
  bool paramByAddress(size_t i) const {
    return i < ByAddress.size() && ByAddress[i];
  }
  bool resultByAddress() const { return ResultByAddress; }
  void setByAddress(std::vector<bool> params, bool result) {
    ByAddress = std::move(params);
    ResultByAddress = result;
  }

  bool isNative() const { return NativeSymbol != nullptr; }
  const std::string &getNativeSymbol() const { return *NativeSymbol; }
  void setNativeSymbol(const std::string &internedSymbol) {
    NativeSymbol = &internedSymbol;
  }

  /// Declared `view fn`: a method whose body does not change `self`
  /// (docs/language/05-classes.md, "Methods that don't change self").
  bool isView() const { return View; }
  void setView(bool view) { View = view; }

  /// Declared `static fn` in an `impl` block: called as `Type::name()`.
  bool isStatic() const { return Static; }
  void setStatic(bool isStatic) { Static = isStatic; }
  /// Written `___init__`: the initializer, private to its module (Names.h).
  bool isModulePrivateInit() const { return ModulePrivateInit; }
  void setModulePrivateInit(bool v) { ModulePrivateInit = v; }

  /// `-> view T` (or `-> inout T`): the result is a borrow of what the
  /// function was given as one; Value for a copy.
  ParamMode getResultMode() const { return ResultMode; }
  void setResultMode(ParamMode mode) { ResultMode = mode; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_FuncDecl; }
};

// Compound statement (block)
class CompoundStmt : public Stmt {
private:
  std::vector<Stmt *> Statements;

public:
  explicit CompoundStmt(SourceLocation loc) : Stmt(NK_CompoundStmt, loc) {}
  CompoundStmt(SourceLocation loc, std::vector<Stmt *> stmts)
      : Stmt(NK_CompoundStmt, loc), Statements(std::move(stmts)) {}

  void addStatement(Stmt *stmt) { Statements.push_back(stmt); }
  const std::vector<Stmt *> &getStatements() const { return Statements; }
  size_t size() const { return Statements.size(); }
  bool empty() const { return Statements.empty(); }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_CompoundStmt;
  }
};

// Return statement
class ReturnStmt : public Stmt {
private:
  Expr *ReturnValue;

public:
  ReturnStmt(SourceLocation loc, Expr *value = nullptr)
      : Stmt(NK_ReturnStmt, loc), ReturnValue(value) {}

  Expr *getReturnValue() const { return ReturnValue; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_ReturnStmt;
  }
};

// Forward declaration — Identifier is defined below with the other Expr nodes.
class Identifier;

// Assignment statement
class AssignStmt : public Stmt {
private:
  Identifier *LHS; // preserves source location of the target variable
  Expr *Value;

public:
  AssignStmt(SourceLocation loc, Identifier *lhs, Expr *value)
      : Stmt(NK_AssignStmt, loc), LHS(lhs), Value(value) {}

  Identifier *getLHS() const { return LHS; }
  // Convenience: callers that only need the name string.
  // Defined as inline below, after Identifier is fully declared.
  inline const std::string &getVarName() const;
  Expr *getValue() const { return Value; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_AssignStmt;
  }
};

// Declaration statement (wraps a Decl to use it in statement context)
class DeclStmt : public Stmt {
private:
  Decl *Declaration;

public:
  DeclStmt(SourceLocation loc, Decl *decl)
      : Stmt(NK_DeclStmt, loc), Declaration(decl) {}

  Decl *getDecl() const { return Declaration; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_DeclStmt; }
};

// Expression statement (wraps an Expr to use it in statement context)
class ExprStmt : public Stmt {
private:
  Expr *Expression;

public:
  ExprStmt(SourceLocation loc, Expr *expr)
      : Stmt(NK_ExprStmt, loc), Expression(expr) {}

  Expr *getExpr() const { return Expression; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_ExprStmt; }
};

// If statement (with optional else branch — else-if chains are nested IfStmts)
class IfStmt : public Stmt {
private:
  Expr *Condition;
  Stmt *ThenBranch;
  Stmt *ElseBranch; // nullptr if no else; IfStmt* for else-if chains

public:
  IfStmt(SourceLocation loc, Expr *cond, Stmt *thenBranch,
         Stmt *elseBranch = nullptr)
      : Stmt(NK_IfStmt, loc), Condition(cond), ThenBranch(thenBranch),
        ElseBranch(elseBranch) {}

  Expr *getCondition() const { return Condition; }
  Stmt *getThenBranch() const { return ThenBranch; }
  Stmt *getElseBranch() const { return ElseBranch; }
  bool hasElse() const { return ElseBranch != nullptr; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_IfStmt; }
};

// While loop
class WhileStmt : public Stmt {
private:
  Expr *Condition;
  Stmt *Body;

public:
  WhileStmt(SourceLocation loc, Expr *cond, Stmt *body)
      : Stmt(NK_WhileStmt, loc), Condition(cond), Body(body) {}

  Expr *getCondition() const { return Condition; }
  Stmt *getBody() const { return Body; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_WhileStmt; }
};

// Break statement
class BreakStmt : public Stmt {
public:
  explicit BreakStmt(SourceLocation loc) : Stmt(NK_BreakStmt, loc) {}
  static bool classof(const ASTNode *N) { return N->getKind() == NK_BreakStmt; }
};

// Continue statement
class ContinueStmt : public Stmt {
public:
  explicit ContinueStmt(SourceLocation loc) : Stmt(NK_ContinueStmt, loc) {}
  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_ContinueStmt;
  }
};

// Member field assignment: receiver.field = value  (e.g. self.x = 1)
class MemberAssignStmt : public Stmt {
private:
  Expr *Receiver;
  const std::string *FieldName; // points into ASTContext::StringPool (stable)
  Expr *Value;

public:
  MemberAssignStmt(SourceLocation loc, Expr *receiver,
                   const std::string &internedField, Expr *value)
      : Stmt(NK_MemberAssignStmt, loc), Receiver(receiver),
        FieldName(&internedField), Value(value) {}

  Expr *getReceiver() const { return Receiver; }
  const std::string &getFieldName() const { return *FieldName; }
  Expr *getValue() const { return Value; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_MemberAssignStmt;
  }
};

// Index-assignment statement: array[index] = value
class SubscriptAssignStmt : public Stmt {
private:
  Expr *Array;
  Expr *Index;
  Expr *Value;

public:
  SubscriptAssignStmt(SourceLocation loc, Expr *array, Expr *index, Expr *value)
      : Stmt(NK_SubscriptAssignStmt, loc), Array(array), Index(index),
        Value(value) {}

  Expr *getArray() const { return Array; }
  Expr *getIndex() const { return Index; }
  Expr *getValue() const { return Value; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_SubscriptAssignStmt;
  }
};

// A single arm of a match statement.
//
//   TypeName { body }             -- type arm, no variable binding
//   binding: TypeName { body }    -- type arm, with variable binding
//   binding: view TypeName { }    -- type arm whose binding borrows the
//                                    subject (`view` or `inout`)
//   literal { body }              -- value arm (e.g. "Hi" { ... }, 42 { ... })
//   _ { body }                    -- wildcard (catch-all)
//
// An arm is exactly one of: type arm (ArmType != nullptr), value arm
// (LiteralPattern != nullptr), or wildcard (both nullptr).  ArmType and
// LiteralPattern are never both set.
class MatchArm : public ASTNode {
  const std::string
      *Binding;  // points into ASTContext::StringPool (stable); "" = no binding
  Type *ArmType; // type arm: the matched type; nullptr otherwise
  Expr *LiteralPattern; // value arm: the literal to compare against; nullptr
                        // otherwise
  CompoundStmt *Body;
  ParamMode Mode = ParamMode::Value; // a binding's borrow of the subject

public:
  // Type arm (or wildcard when armType == nullptr).
  MatchArm(SourceLocation loc, const std::string &internedBinding,
           Type *armType, CompoundStmt *body)
      : ASTNode(NK_MatchArm, loc), Binding(&internedBinding), ArmType(armType),
        LiteralPattern(nullptr), Body(body) {}

  // Value arm: matches when the subject equals the literal pattern.
  MatchArm(SourceLocation loc, const std::string &internedBinding,
           Expr *literalPattern, CompoundStmt *body)
      : ASTNode(NK_MatchArm, loc), Binding(&internedBinding), ArmType(nullptr),
        LiteralPattern(literalPattern), Body(body) {}

  bool isWildcard() const {
    return ArmType == nullptr && LiteralPattern == nullptr;
  }
  bool isLiteral() const { return LiteralPattern != nullptr; }
  bool hasBinding() const { return !Binding->empty(); }
  const std::string &getBinding() const { return *Binding; }
  Type *getArmType() const { return ArmType; }
  void setArmType(Type *t) { ArmType = t; }
  Expr *getLiteralPattern() const { return LiteralPattern; }
  CompoundStmt *getBody() const { return Body; }
  /// `n: view T` or `n: inout T`: how the binding holds the subject
  /// (docs/language/08-match.md, "Borrowing the subject").
  ParamMode getMode() const { return Mode; }
  void setMode(ParamMode mode) { Mode = mode; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_MatchArm; }
};

// Match statement (downcasting switch):
//
//   match expr {
//       TypeA { body }
//       ty_b: TypeB { body }
//       _ { body }
//   }
//
// Each arm either names a class type, optionally giving it a local binding,
// or is the wildcard arm (_) that catches any unmatched value.
class MatchStmt : public Stmt {
private:
  Expr *Subject;
  std::vector<MatchArm *> Arms;

public:
  MatchStmt(SourceLocation loc, Expr *subject, std::vector<MatchArm *> arms)
      : Stmt(NK_MatchStmt, loc), Subject(subject), Arms(std::move(arms)) {}

  Expr *getSubject() const { return Subject; }
  std::vector<MatchArm *> &getArms() { return Arms; }
  const std::vector<MatchArm *> &getArms() const { return Arms; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_MatchStmt; }
};

// Integer literal
class IntegerLiteral : public Expr {
private:
  int64_t Value;

public:
  IntegerLiteral(SourceLocation loc, int64_t value)
      : Expr(NK_IntegerLiteral, loc), Value(value) {}

  int64_t getValue() const { return Value; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_IntegerLiteral;
  }
};

// Float literal
class FloatLiteral : public Expr {
private:
  double Value;

public:
  FloatLiteral(SourceLocation loc, double value)
      : Expr(NK_FloatLiteral, loc), Value(value) {}

  double getValue() const { return Value; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_FloatLiteral;
  }
};

// Bool literal
class BoolLiteral : public Expr {
private:
  bool Value;

public:
  BoolLiteral(SourceLocation loc, bool value)
      : Expr(NK_BoolLiteral, loc), Value(value) {}

  bool getValue() const { return Value; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_BoolLiteral;
  }
};

// Char literal  ('a', '\n', etc.)
class CharLiteral : public Expr {
  char Value;

public:
  CharLiteral(SourceLocation loc, char value)
      : Expr(NK_CharLiteral, loc), Value(value) {}

  char getValue() const { return Value; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_CharLiteral;
  }
};

// None literal (null reference for class types)
class NoneLiteral : public Expr {
public:
  explicit NoneLiteral(SourceLocation loc) : Expr(NK_NoneLiteral, loc) {}

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_NoneLiteral;
  }
};

// String literal
class StringLiteral : public Expr {
private:
  std::string Value;

public:
  StringLiteral(SourceLocation loc, const std::string &value)
      : Expr(NK_StringLiteral, loc), Value(value) {}

  const std::string &getValue() const { return Value; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_StringLiteral;
  }
};

// Binary expression
class BinaryExpr : public Expr {
private:
  BinaryOpcode Op;
  Expr *LHS;
  Expr *RHS;

public:
  BinaryExpr(SourceLocation loc, BinaryOpcode op, Expr *lhs, Expr *rhs)
      : Expr(NK_BinaryExpr, loc), Op(op), LHS(lhs), RHS(rhs) {}

  BinaryOpcode getOpcode() const { return Op; }
  Expr *getLHS() const { return LHS; }
  Expr *getRHS() const { return RHS; }

  const char *getOpcodeStr() const;

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_BinaryExpr;
  }
};

// Unary expression
class UnaryExpr : public Expr {
private:
  UnaryOpcode Op;
  Expr *Operand;

public:
  UnaryExpr(SourceLocation loc, UnaryOpcode op, Expr *operand)
      : Expr(NK_UnaryExpr, loc), Op(op), Operand(operand) {}

  UnaryOpcode getOpcode() const { return Op; }
  Expr *getOperand() const { return Operand; }

  const char *getOpcodeStr() const;

  static bool classof(const ASTNode *N) { return N->getKind() == NK_UnaryExpr; }
};

// Identifier expression
class Identifier : public Expr {
private:
  const std::string *Name; // points into ASTContext::StringPool (stable)
  // Set by Sema when the name is a function used as a value (`f = square;`,
  // docs/language/14-function-types.md): the function's name as the lowering
  // knows it (the spelling, or, inside another module's template, that
  // module's qualified name), of which the lowering makes a function value.
  const std::string *FunctionRef = nullptr;

public:
  Identifier(SourceLocation loc, const std::string &internedName)
      : Expr(NK_Identifier, loc), Name(&internedName) {}

  const std::string &getName() const { return *Name; }
  void setFunctionRef(const std::string &internedName) {
    FunctionRef = &internedName;
  }
  bool isFunctionRef() const { return FunctionRef != nullptr; }
  const std::string &getFunctionRef() const { return *FunctionRef; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_Identifier;
  }
};

// Deferred inline: AssignStmt::getVarName() requires complete Identifier type.
inline const std::string &AssignStmt::getVarName() const {
  return LHS->getName();
}

// Function call expression.
//
// Also covers constructor calls (`Point(1, 2)`) and, with TypeArgs, explicit
// generic calls: `first<int>(xs)` / `Box<int>(3)`.  Sema resolves a call to a
// generic function or class template to a concrete instantiation and rewrites
// the callee name to the instantiation's canonical name (`first<int>`), so
// the lowering only ever sees ordinary calls.
class CallExpr : public Expr {
private:
  const std::string *CalleeName; // points into ASTContext::StringPool (stable)
  std::vector<Expr *> Arguments;
  std::vector<Type *> TypeArgs; // explicit type arguments; empty if none
  // Set by Sema when the callee is a variable or parameter of function type
  // (`f(1)` with `f: (int) -> int`): a call through a function value.
  bool CalleeIsValue = false;

public:
  CallExpr(SourceLocation loc, const std::string &internedCallee,
           std::vector<Expr *> args, std::vector<Type *> typeArgs = {})
      : Expr(NK_CallExpr, loc), CalleeName(&internedCallee),
        Arguments(std::move(args)), TypeArgs(std::move(typeArgs)) {}

  const std::string &getCalleeName() const { return *CalleeName; }
  /// Rebind the callee (Sema: generic call -> instantiation name).
  void setCalleeName(const std::string &internedCallee) {
    CalleeName = &internedCallee;
  }
  const std::vector<Expr *> &getArguments() const { return Arguments; }
  size_t getNumArguments() const { return Arguments.size(); }
  const std::vector<Type *> &getTypeArgs() const { return TypeArgs; }
  bool hasTypeArgs() const { return !TypeArgs.empty(); }
  void setCalleeIsValue(bool v) { CalleeIsValue = v; }
  bool calleeIsValue() const { return CalleeIsValue; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_CallExpr; }
};

// A call of a function value that is not named: `xs[0](1)`, `t.0(x)`,
// `make()(2)` (docs/language/14-function-types.md).  A call by name is a
// CallExpr (`f(1)`, a variable of function type) or a MethodCallExpr
// (`obj.cb(1)`, a field of function type); the callee here is any other
// expression, of function type.
class CallValueExpr : public Expr {
  Expr *Callee;
  std::vector<Expr *> Arguments;

public:
  CallValueExpr(SourceLocation loc, Expr *callee, std::vector<Expr *> args)
      : Expr(NK_CallValueExpr, loc), Callee(callee),
        Arguments(std::move(args)) {}

  Expr *getCallee() const { return Callee; }
  const std::vector<Expr *> &getArguments() const { return Arguments; }
  size_t getNumArguments() const { return Arguments.size(); }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_CallValueExpr;
  }
};

// Lambda expression: `fn(x: int) -> int { return x + 1; }`
// (docs/language/14-function-types.md).
//
// A function without a name, in expression position, whose value is a
// closure: its body may use the variables of the function it is written in,
// which it captures by copy when it is evaluated.  Sema records the captures
// (the names the body uses that resolve outside the lambda, in first-use
// order, with their types) and the lambda's function type; the lowering
// makes a class per lambda holding the captures.
class LambdaExpr : public Expr {
public:
  struct Capture {
    const std::string *Name; // interned
    Type *Ty;                // canonical
    bool Weak = false;       // a `weak` variable: captured as a weak reference
    const std::string &getName() const { return *Name; }
  };

private:
  std::vector<Param> Params;
  Type *ReturnType; // nullptr means void
  CompoundStmt *Body;
  std::vector<Capture> Captures; // set by Sema
  bool CapturesSelf = false;     // `self` of a class method is among them

public:
  LambdaExpr(SourceLocation loc, std::vector<Param> params, Type *retTy,
             CompoundStmt *body)
      : Expr(NK_LambdaExpr, loc), Params(std::move(params)), ReturnType(retTy),
        Body(body) {}

  const std::vector<Param> &getParams() const { return Params; }
  std::vector<Param> &getMutableParams() { return Params; }
  Type *getReturnType() const { return ReturnType; }
  void setReturnType(Type *ty) { ReturnType = ty; }
  CompoundStmt *getBody() const { return Body; }

  const std::vector<Capture> &getCaptures() const { return Captures; }
  void addCapture(const std::string &internedName, Type *ty,
                  bool weak = false) {
    Captures.push_back({&internedName, ty, weak});
  }
  bool capturesSelf() const { return CapturesSelf; }
  void setCapturesSelf(bool v) { CapturesSelf = v; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_LambdaExpr;
  }
};

// Method call expression: receiver.method(args)
class MethodCallExpr : public Expr {
private:
  Expr *Receiver;
  const std::string *MethodName; // points into ASTContext::StringPool (stable)
  std::vector<Expr *> Arguments;
  // Set by Sema when the name is a field of function type, not a method
  // (`obj.cb(1)` with `cb: (int) -> int`): a call through the field's value.
  bool FieldCall = false;

public:
  MethodCallExpr(SourceLocation loc, Expr *receiver,
                 const std::string &internedMethod, std::vector<Expr *> args)
      : Expr(NK_MethodCallExpr, loc), Receiver(receiver),
        MethodName(&internedMethod), Arguments(std::move(args)) {}

  Expr *getReceiver() const { return Receiver; }
  const std::string &getMethodName() const { return *MethodName; }
  const std::vector<Expr *> &getArguments() const { return Arguments; }
  size_t getNumArguments() const { return Arguments.size(); }
  void setFieldCall(bool v) { FieldCall = v; }
  bool isFieldCall() const { return FieldCall; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_MethodCallExpr;
  }
};

// Ternary expression (cond ? then : else)
class TernaryExpr : public Expr {
private:
  Expr *Condition;
  Expr *TrueExpr;
  Expr *FalseExpr;

public:
  TernaryExpr(SourceLocation loc, Expr *cond, Expr *trueExpr, Expr *falseExpr)
      : Expr(NK_TernaryExpr, loc), Condition(cond), TrueExpr(trueExpr),
        FalseExpr(falseExpr) {}

  Expr *getCondition() const { return Condition; }
  Expr *getTrueExpr() const { return TrueExpr; }
  Expr *getFalseExpr() const { return FalseExpr; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_TernaryExpr;
  }
};

// Member field access expression: receiver.field  (e.g. self.x, obj.count)
class MemberAccessExpr : public Expr {
private:
  Expr *Receiver;
  const std::string *FieldName; // points into ASTContext::StringPool (stable)

public:
  MemberAccessExpr(SourceLocation loc, Expr *receiver,
                   const std::string &internedField)
      : Expr(NK_MemberAccessExpr, loc), Receiver(receiver),
        FieldName(&internedField) {}

  Expr *getReceiver() const { return Receiver; }
  const std::string &getFieldName() const { return *FieldName; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_MemberAccessExpr;
  }
};

// Builtin type (int, void, etc.)
class BuiltinType : public Type {
public:
  enum Kind {
    Int,
    Float,
    Bool,
    Char,
    Void,
    // The sized numeric types (docs/design/sized-types.md).  `int` is the
    // 64-bit signed integer and `float` the 64-bit float (Int and Float
    // above); `unsigned` is U64.  Every one is held in an 8-byte slot.
    I8,
    I16,
    I32,
    U8,
    U16,
    U32,
    U64,
    F16,
    F32,
  };
  static constexpr unsigned kNumKinds = F32 + 1;

  /// Int and the sized integer kinds, signed and unsigned.
  static bool isIntegerKind(Kind k) {
    return k == Int || (k >= I8 && k <= U64);
  }
  /// U8 .. U64.
  static bool isUnsignedKind(Kind k) { return k >= U8 && k <= U64; }
  /// Float, F16 and F32.
  static bool isFloatKind(Kind k) { return k == Float || k == F16 || k == F32; }
  static bool isNumericKind(Kind k) {
    return isIntegerKind(k) || isFloatKind(k);
  }
  /// The width in bits of an integer kind, 0 for another kind.
  static unsigned integerBits(Kind k);
  /// The width in bits of a floating-point kind, 0 for another kind.
  static unsigned floatBits(Kind k);

private:
  Kind TypeKind;

  void initOps();

public:
  BuiltinType(SourceLocation loc, Kind kind)
      : Type(NK_BuiltinType, loc), TypeKind(kind) {
    initOps();
  }

  Kind getTypeKind() const { return TypeKind; }
  bool isInteger() const { return isIntegerKind(TypeKind); }
  bool isUnsigned() const { return isUnsignedKind(TypeKind); }
  bool isFloat() const { return isFloatKind(TypeKind); }
  bool isNumeric() const { return isNumericKind(TypeKind); }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_BuiltinType;
  }
};

/// The keyword a builtin kind is printed as: `int`, `unsigned`, `float`,
/// `i8`, ... (`i64`, `u64` and `f64` are other spellings of the first three).
const char *builtinKindName(BuiltinType::Kind k);
/// The kind a type keyword names, with its alternative spellings; false when
/// @p name is not a builtin type's.
bool builtinKindOfName(std::string_view name, BuiltinType::Kind &kind);

// Method declaration (name + signature + flags)
class MethodDecl : public Decl {
public:
  enum Flags : uint8_t {
    None = 0,
    Private = 1 << 0,
    View = 1 << 1, // `view fn`: does not change `self`
    Impl = 1 << 2, // in an `impl` block: called directly, never overridden
  };

private:
  const std::string *Name; // points into ASTContext::StringPool (stable)
  Type *ReturnType;
  std::vector<Type *> ParamTypes;
  uint8_t MethodFlags;
  ParamModes Modes;               // `n: inout int`
  bool ModulePrivateInit = false; // the type's `___init__` (Names.h)

public:
  MethodDecl(SourceLocation loc, const std::string &internedName, Type *retTy,
             std::vector<Type *> params, uint8_t flags = None)
      : Decl(NK_MethodDecl, loc), Name(&internedName), ReturnType(retTy),
        ParamTypes(std::move(params)), MethodFlags(flags) {}

  const std::string &getName() const { return *Name; }
  Type *getReturnType() const { return ReturnType; }
  const std::vector<Type *> &getParamTypes() const { return ParamTypes; }
  size_t getNumParams() const { return ParamTypes.size(); }
  const ParamModes &getParamModes() const { return Modes; }
  void setParamModes(ParamModes modes) { Modes = std::move(modes); }

  bool isPrivate() const { return MethodFlags & Private; }
  bool isVirtual() const { return !isPrivate() && !isImpl(); }
  /// Only reads its object, so it can be called through a `view`.
  bool isView() const { return MethodFlags & View; }
  /// Declared in an `impl` block: it has no vtable slot.
  bool isImpl() const { return MethodFlags & Impl; }
  /// The type's `___init__`: constructed, and extended, by its module only
  /// (Sema::checkConstructible).
  bool isModulePrivateInit() const { return ModulePrivateInit; }
  void setModulePrivateInit(bool v) { ModulePrivateInit = v; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_MethodDecl;
  }
};

// Class type (reference type with vtable)
//
// Holds the class name, optional superclass pointer, instance fields,
// all methods, and a flattened vtable of virtual methods in layout order.
//
// Vtable construction:
//   1. Copy the parent's vtable.
//   2. For each virtual method in this class:
//      - If a same-named method exists in the parent vtable, override that
//      slot.
//      - Otherwise, append a new slot.
//
class ClassType : public Type {
  const std::string *Name; // points into ASTContext::StringPool (stable)
  ClassType *SuperClass;

  // Instance fields: (name, type) pairs.
  std::vector<std::pair<std::string, Type *>> Fields;
  // The fields declared `weak` (docs/language/16-memory.md): they hold
  // a weak reference, and their type is `T?` of a class `T`.
  std::unordered_set<std::string> WeakFields;

  // All virtual methods in layout order (vtable). __init__ is NOT here.
  std::vector<MethodDecl *> VTable;
  // O(1) index: method name -> vtable slot index. Kept in sync with VTable.
  std::unordered_map<std::string, int> VTableIndex;

  // The class initialiser (__init__), stored separately because it is a
  // static function, not a virtual method, and has no vtable slot.
  MethodDecl *InitMethod = nullptr;

  // The methods of its `impl` blocks, in declaration order.  They are called
  // directly, so they have no vtable slot, and a subclass inherits them
  // through findMethod.
  std::vector<MethodDecl *> ImplMethods;

  // When true, no user class may inherit from this type.
  bool Final = false;

  // When true, this type is a compiler builtin (Obj, Str, Array, File, Error,
  // Tuple) registered by the ASTContext bootstrap: its methods are
  // implemented in the C runtime rather than emitted from user code, and user
  // code may not declare a class, enum, or function with its name.
  bool Builtin = false;

  void rebuildVTableIndex() {
    VTableIndex.clear();
    for (int i = 0, n = static_cast<int>(VTable.size()); i < n; ++i)
      VTableIndex[VTable[i]->getName()] = i;
  }

  /// Inherit `parent`'s vtable and operator support.  Copying the parent's
  /// operator bitmasks means that, e.g., every class that descends from Obj
  /// automatically supports == and !=.
  void inheritFrom(const ClassType *parent) {
    VTable = parent->VTable;
    rebuildVTableIndex();
    for (int op = 0; op < static_cast<int>(UnaryOpcode::Count); ++op)
      if (parent->hasUnaryOp(static_cast<UnaryOpcode>(op)))
        addUnaryOp(static_cast<UnaryOpcode>(op));
    for (int op = 0; op < static_cast<int>(BinaryOpcode::Count); ++op)
      if (parent->hasOp(static_cast<BinaryOpcode>(op)))
        addBinaryOp(static_cast<BinaryOpcode>(op));
  }

public:
  ClassType(SourceLocation loc, const std::string &internedName,
            ClassType *superClass = nullptr)
      : Type(NK_ClassType, loc), Name(&internedName), SuperClass(superClass) {
    if (SuperClass)
      inheritFrom(SuperClass);
  }

  const std::string &getName() const { return *Name; }
  ClassType *getSuperClass() const { return SuperClass; }

  /// Returns true if no user class may inherit from this type.
  bool isFinal() const { return Final; }
  /// Mark this type as non-inheritable.
  void setFinal(bool v = true) { Final = v; }

  /// Returns true if this is a compiler-provided class type (see Builtin).
  bool isBuiltin() const { return Builtin; }
  /// Mark this type as a compiler builtin.
  void setBuiltin(bool v = true) { Builtin = v; }

  /// Set (or change) the superclass, inheriting its vtable and operator
  /// bitmasks. Used during ASTContext bootstrap to break the Obj/Str cycle.
  void setSuperClass(ClassType *sc) {
    SuperClass = sc;
    if (sc)
      inheritFrom(sc);
  }

  // -- Fields

  void addField(const std::string &name, Type *ty, bool weak = false) {
    Fields.emplace_back(name, ty);
    if (weak)
      WeakFields.insert(name);
  }
  const std::vector<std::pair<std::string, Type *>> &getFields() const {
    return Fields;
  }
  size_t getNumFields() const { return Fields.size(); }

  /// Whether field @p name, of this class or an ancestor, holds a weak
  /// reference (declared `weak`).
  bool isWeakField(const std::string &name) const {
    for (const ClassType *c = this; c; c = c->SuperClass)
      if (c->WeakFields.count(name))
        return true;
    return false;
  }

  /// Look up an instance field by name, searching this class and then each
  /// ancestor in turn.  Returns the field's type, or nullptr if no class in
  /// the hierarchy declares it.
  Type *findField(const std::string &name) const {
    for (const ClassType *c = this; c; c = c->SuperClass)
      for (const auto &[fname, fty] : c->Fields)
        if (fname == name)
          return fty;
    return nullptr;
  }

  // -- Methods

  void addMethod(MethodDecl *m);

  /// Reset the vtable to start with the parent's slots (re-inherit).
  /// Call this after the parent's methods are fully populated.
  void reinheritVTable(ClassType *parent) {
    VTable = parent->VTable;
    rebuildVTableIndex();
    // Do NOT inherit InitMethod — every class has its own or none.
  }

  const std::vector<MethodDecl *> &getVTable() const { return VTable; }
  size_t getVTableSize() const { return VTable.size(); }
  /// The `impl` methods this class declares (not its superclasses').
  const std::vector<MethodDecl *> &getImplMethods() const {
    return ImplMethods;
  }

  /// Find the vtable slot index for a method name, or -1 if not found.
  int getVTableIndex(const std::string &name) const;

  /// Look up a method by name (searches this class and parents).
  MethodDecl *findMethod(const std::string &name) const;

  /// The class, from this one up, whose `impl` block declares method
  /// @p name, or nullptr.
  ClassType *implOwner(const std::string &name);

  /// Returns true if this type is a subtype of (or equal to) Other.
  bool isSubtypeOf(const ClassType *other) const;

  static bool classof(const ASTNode *N) { return N->getKind() == NK_ClassType; }
};

// Import declaration:
//   import path::module;            one Module, Alias empty
//   import path::{a, b};            two Modules
//   import path::module as alias;   Alias = "alias"
//   import ::system_module;         IsSystem
class ImportDecl : public Decl {
public:
  /// A single module being imported, with an optional alias.
  /// qualifier() returns the name used at call sites (alias if set, else Name).
  struct Module {
    // Both point into ASTContext::StringPool (stable).
    const std::string *Name;  // bare module name (final path segment)
    const std::string *Alias; // empty = use Name as qualifier
    const std::string &qualifier() const {
      return Alias->empty() ? *Name : *Alias;
    }
  };

private:
  const std::string *BasePath; // directory portion, e.g. "path::to::file" (may
                               // be empty); interned
  std::vector<Module> Modules; // always ≥1 entry
  bool IsSystem;               // true for :: prefix (stdlib) imports

public:
  ImportDecl(SourceLocation loc, const std::string &internedBasePath,
             bool isSystem, std::vector<Module> modules)
      : Decl(NK_ImportDecl, loc), BasePath(&internedBasePath),
        Modules(std::move(modules)), IsSystem(isSystem) {}

  const std::string &getBasePath() const { return *BasePath; }
  const std::vector<Module> &getModules() const { return Modules; }
  bool isSystem() const { return IsSystem; }

  /// Full module path for module m: "base::name" (or just "name" if base is
  /// empty).
  std::string modulePath(const Module &m) const {
    return BasePath->empty() ? *m.Name : *BasePath + "::" + *m.Name;
  }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_ImportDecl;
  }
};

// Helper struct used during parsing to accumulate class body members.
struct ClassBody {
  std::vector<VarDecl *> Fields;
  std::vector<FuncDecl *> Methods;
};

// User-defined class declaration:
//   class Foo { field: Type; fn method(...) { ... } }
//
// Fields are stored as VarDecl nodes (no initializer).
// Methods are stored as FuncDecl nodes (fn keyword, full body).
// The optional superclass is recorded by name; Sema resolves it to a
// ClassType* and registers the fully-built ClassType in the ASTContext.
//
// A generic class (`class Box<T> { ... }`) carries its type parameter names in
// TypeParams and is a template: Sema registers it separately and instantiates
// it per distinct type-argument tuple into an ordinary ClassDecl/ClassType
// named `Box<int>` (see SemaClass.cpp).
class ClassDecl : public Decl {
private:
  const std::string *Name; // points into ASTContext::StringPool (stable)
  const std::string *SuperClassName; // interned; "" = no explicit superclass
  std::vector<VarDecl *> Fields;
  std::vector<FuncDecl *> Methods;
  // Type parameter names (interned); empty for an ordinary class.
  std::vector<const std::string *> TypeParams;
  TypeParamBounds Bounds;

public:
  ClassDecl(SourceLocation loc, const std::string &internedName,
            const std::string &internedSuperName, std::vector<VarDecl *> fields,
            std::vector<FuncDecl *> methods,
            std::vector<const std::string *> typeParams = {})
      : Decl(NK_ClassDecl, loc), Name(&internedName),
        SuperClassName(&internedSuperName), Fields(std::move(fields)),
        Methods(std::move(methods)), TypeParams(std::move(typeParams)) {}

  const std::string &getName() const { return *Name; }
  const std::string &getSuperClassName() const { return *SuperClassName; }
  bool hasSuperClass() const { return !SuperClassName->empty(); }

  const std::vector<VarDecl *> &getFields() const { return Fields; }
  const std::vector<FuncDecl *> &getMethods() const { return Methods; }
  /// Sema adds the methods of the class's `impl` blocks.
  void addMethod(FuncDecl *m) { Methods.push_back(m); }
  size_t getNumFields() const { return Fields.size(); }
  size_t getNumMethods() const { return Methods.size(); }

  const std::vector<const std::string *> &getTypeParams() const {
    return TypeParams;
  }
  bool isGeneric() const { return !TypeParams.empty(); }
  const TypeParamBounds &getTypeParamBounds() const { return Bounds; }
  void setTypeParamBounds(TypeParamBounds b) { Bounds = std::move(b); }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_ClassDecl; }
};

// User-defined struct declaration (docs/language/11-structs.md):
//   struct Point { x: int; y: int; }
//
// Fields are VarDecl nodes (no initializer), at least one.  A struct body
// holds no methods: they come from its `impl` blocks, which Sema adds here
// (addMethod) so that the lowering emits them with the struct.  Sema
// registers the corresponding StructType in the ASTContext.  A generic
// struct (`struct Pair<T> { ... }`) carries its type parameter names in
// TypeParams and is a template that Sema instantiates per distinct
// type-argument list, like a generic class.
class StructDecl : public Decl {
  const std::string *Name; // points into ASTContext::StringPool (stable)
  std::vector<VarDecl *> Fields;
  std::vector<FuncDecl *> Methods; // the `impl` blocks' (Sema)
  std::vector<const std::string *> TypeParams;
  TypeParamBounds Bounds;

public:
  StructDecl(SourceLocation loc, const std::string &internedName,
             std::vector<VarDecl *> fields,
             std::vector<const std::string *> typeParams = {})
      : Decl(NK_StructDecl, loc), Name(&internedName),
        Fields(std::move(fields)), TypeParams(std::move(typeParams)) {}

  const std::string &getName() const { return *Name; }
  const std::vector<VarDecl *> &getFields() const { return Fields; }
  size_t getNumFields() const { return Fields.size(); }
  const std::vector<FuncDecl *> &getMethods() const { return Methods; }
  /// Sema adds the methods of the struct's `impl` blocks.
  void addMethod(FuncDecl *m) { Methods.push_back(m); }

  const std::vector<const std::string *> &getTypeParams() const {
    return TypeParams;
  }
  bool isGeneric() const { return !TypeParams.empty(); }
  const TypeParamBounds &getTypeParamBounds() const { return Bounds; }
  void setTypeParamBounds(TypeParamBounds b) { Bounds = std::move(b); }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_StructDecl;
  }
};

// Translation unit (top-level container)
//
// Generic (template) declarations are kept apart from ordinary ones: every
// downstream pass that walks ClassDecls / FuncDecls (Sema, the lowering,
// module export) sees only concrete declarations.  Sema appends the
// instantiations it creates to the concrete lists (addFuncDecl /
// setClassDecls) so they are lowered exactly like hand-written declarations.
class TranslationUnit : public ASTNode {
private:
  std::vector<ImportDecl *> Imports;
  std::vector<ClassDecl *> ClassDecls;
  std::vector<FuncDecl *> FuncDecls;
  std::vector<EnumDecl *> EnumDecls;
  std::vector<ClassDecl *> GenericClassDecls;
  std::vector<FuncDecl *> GenericFuncDecls;
  std::vector<ImplDecl *> ImplDecls;
  std::vector<TraitDecl *> TraitDecls;
  std::vector<StructDecl *> StructDecls;
  std::vector<StructDecl *> GenericStructDecls;

public:
  TranslationUnit(SourceLocation loc, std::vector<ImportDecl *> imports,
                  std::vector<ClassDecl *> classes,
                  std::vector<FuncDecl *> funcs,
                  std::vector<EnumDecl *> enums = {},
                  std::vector<ClassDecl *> genericClasses = {},
                  std::vector<FuncDecl *> genericFuncs = {},
                  std::vector<ImplDecl *> impls = {},
                  std::vector<TraitDecl *> traits = {},
                  std::vector<StructDecl *> structs = {},
                  std::vector<StructDecl *> genericStructs = {})
      : ASTNode(NK_TranslationUnit, loc), Imports(std::move(imports)),
        ClassDecls(std::move(classes)), FuncDecls(std::move(funcs)),
        EnumDecls(std::move(enums)),
        GenericClassDecls(std::move(genericClasses)),
        GenericFuncDecls(std::move(genericFuncs)), ImplDecls(std::move(impls)),
        TraitDecls(std::move(traits)), StructDecls(std::move(structs)),
        GenericStructDecls(std::move(genericStructs)) {}

  const std::vector<ImportDecl *> &getImports() const { return Imports; }
  const std::vector<ClassDecl *> &getClassDecls() const { return ClassDecls; }
  const std::vector<FuncDecl *> &getFuncDecls() const { return FuncDecls; }
  const std::vector<EnumDecl *> &getEnumDecls() const { return EnumDecls; }
  const std::vector<ClassDecl *> &getGenericClassDecls() const {
    return GenericClassDecls;
  }
  const std::vector<FuncDecl *> &getGenericFuncDecls() const {
    return GenericFuncDecls;
  }
  const std::vector<ImplDecl *> &getImplDecls() const { return ImplDecls; }
  const std::vector<TraitDecl *> &getTraitDecls() const { return TraitDecls; }
  const std::vector<StructDecl *> &getStructDecls() const {
    return StructDecls;
  }
  const std::vector<StructDecl *> &getGenericStructDecls() const {
    return GenericStructDecls;
  }
  /// Append an instantiated struct (Sema).
  void addStructDecl(StructDecl *sd) { StructDecls.push_back(sd); }

  /// Append an instantiated function (Sema).
  void addFuncDecl(FuncDecl *fn) { FuncDecls.push_back(fn); }
  /// Replace the concrete class list (Sema: hand-written classes plus the
  /// instantiations, ordered for the lowering; see Sema::injectInstantiations).
  void setClassDecls(std::vector<ClassDecl *> decls) {
    ClassDecls = std::move(decls);
  }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_TranslationUnit;
  }
};

// `T[]` as written (e.g. `view int[]`): the type of a `view` or `inout`
// parameter that is a slice, which Sema resolves to the canonical SliceType.
class ArrayType : public Type {
  Type *ElementType;

public:
  ArrayType(SourceLocation loc, Type *elemTy)
      : Type(NK_ArrayType, loc), ElementType(elemTy) {}

  Type *getElementType() const { return ElementType; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_ArrayType; }
};

// Fixed-size array type: `T[n]`, n (at least 1) elements of T held inline.
// It is a value: copying one copies its elements, and it is not an Obj.  The
// parser allocates a source-located node per annotation and
// Sema resolves it to the canonical instance of
// ASTContext::getFixedArrayType, so these types compare by pointer.
class FixedArrayType : public Type {
  Type *ElementType;
  int64_t Length;

public:
  /// The length of `T[_]`, which a variable declaration infers from its
  /// initializer (Sema replaces it with the initializer's).
  static constexpr int64_t kInferredLength = -1;

  FixedArrayType(SourceLocation loc, Type *elemTy, int64_t length)
      : Type(NK_FixedArrayType, loc), ElementType(elemTy), Length(length) {}

  Type *getElementType() const { return ElementType; }
  int64_t getLength() const { return Length; }
  bool isInferred() const { return Length == kInferredLength; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_FixedArrayType;
  }
};

// Generic type application:  Box<int>, Pair<Str, int>, mod::Box<int>
//
// A parser-level reference to a generic class with type arguments.  It is not
// a type of its own: Sema's resolveType instantiates the named class template
// with the (resolved) arguments and yields the instantiation's canonical
// ClassType (`Box<int>`), then writes that ClassType back over the annotation
// slot that held this node.  The lowering therefore never sees a GenericType.
class GenericType : public Type {
  const std::string *Name; // template name (interned); may be qualified
  std::vector<Type *> Args;

public:
  GenericType(SourceLocation loc, const std::string &internedName,
              std::vector<Type *> args)
      : Type(NK_GenericType, loc), Name(&internedName), Args(std::move(args)) {}

  const std::string &getName() const { return *Name; }
  const std::vector<Type *> &getArgs() const { return Args; }
  size_t getNumArgs() const { return Args.size(); }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_GenericType;
  }
};

// Optional type: T?  (e.g. Node?, Str?, int?)
//
// Issue #5.  A `T?` holds either a value of type T or `None`.  For a
// reference type T (Sema rejects nested `T??`) a `T?` is at runtime the very
// same PaykanShared* box as a `T`, with a NULL box meaning `None` — no layout
// change, and every runtime entry point that touches boxes tolerates NULL.
// An optional primitive (`int?`, #66) is a value: the primitive and a
// presence flag held inline (#96, isValueOptional).
// The parser allocates a source-located node per annotation
// and Sema resolves it to the canonical instance interned by
// ASTContext::getOptionalType, so optional types compare by pointer identity.
class OptionalType : public Type {
  Type *InnerType;

public:
  OptionalType(SourceLocation loc, Type *inner)
      : Type(NK_OptionalType, loc), InnerType(inner) {
    // The only operators defined on an optional value are == / != (against
    // `None`, or against another optional of a compatible type).  Everything
    // else requires unwrapping with `match`.
    addBinaryOp(BinaryOpcode::Eq);
    addBinaryOp(BinaryOpcode::Ne);
  }

  Type *getInnerType() const { return InnerType; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_OptionalType;
  }
};

// Enum type (nominal value type backed by a 64-bit unsigned integer).
//
// Each enum is a distinct nominal type.  Variants are assigned implicit
// values 0, 1, 2, … in declaration order.  Enums are NOT convertible to or
// from any other type, and only support == / != against the *same* enum type.
class EnumType : public Type {
  const std::string *Name; // points into ASTContext::StringPool (stable)
  // Variant names in declaration order; index == underlying value.
  // Each entry points into ASTContext::StringPool (stable).
  std::vector<const std::string *> Variants;

public:
  EnumType(SourceLocation loc, const std::string &internedName)
      : Type(NK_EnumType, loc), Name(&internedName) {
    // Enums support only equality / inequality comparisons.
    addBinaryOp(BinaryOpcode::Eq);
    addBinaryOp(BinaryOpcode::Ne);
  }

  const std::string &getName() const { return *Name; }

  void addVariant(const std::string &internedVariant) {
    Variants.push_back(&internedVariant);
  }
  const std::vector<const std::string *> &getVariants() const {
    return Variants;
  }
  size_t getNumVariants() const { return Variants.size(); }

  /// Return the underlying value (declaration index) of a variant, or -1 if
  /// the name is not a variant of this enum.
  int64_t findVariant(const std::string &name) const {
    for (size_t i = 0; i < Variants.size(); ++i)
      if (*Variants[i] == name)
        return static_cast<int64_t>(i);
    return -1;
  }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_EnumType; }
};

// Enum variant access expression:  MyEnum::Variant
//
// Resolves to the variant's implicit 64-bit value.  The EnumType pointer is
// filled in by Sema (the parser only records the spelled names).
class EnumValueExpr : public Expr {
  const std::string *EnumName;              // interned; spelled enum name
  const std::string *VariantName;           // interned; spelled variant name
  EnumType *ResolvedEnum = nullptr;         // set by Sema
  int64_t Value = -1;                       // set by Sema (variant index)
  const std::string *FunctionRef = nullptr; // set by Sema (see below)

public:
  EnumValueExpr(SourceLocation loc, const std::string &internedEnum,
                const std::string &internedVariant)
      : Expr(NK_EnumValueExpr, loc), EnumName(&internedEnum),
        VariantName(&internedVariant) {}

  const std::string &getEnumName() const { return *EnumName; }
  const std::string &getVariantName() const { return *VariantName; }
  /// Set by Sema when `m::f` names an imported function used as a value
  /// (docs/language/14-function-types.md): the function's qualified name.
  void setFunctionRef(const std::string &internedName) {
    FunctionRef = &internedName;
  }
  const std::string *getFunctionRef() const { return FunctionRef; }

  void setResolvedEnum(EnumType *e) { ResolvedEnum = e; }
  int64_t getValue() const { return Value; }
  void setValue(int64_t v) { Value = v; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_EnumValueExpr;
  }
};

// Enum declaration:
//   enum MyEnum { One, Two, Three }
//
// Variants are recorded in declaration order; their underlying values are
// their indices.  Sema registers a corresponding EnumType in the ASTContext.
class EnumDecl : public Decl {
  const std::string *Name; // points into ASTContext::StringPool (stable)
  std::vector<const std::string *> Variants; // each interned (stable)

public:
  EnumDecl(SourceLocation loc, const std::string &internedName,
           std::vector<const std::string *> variants)
      : Decl(NK_EnumDecl, loc), Name(&internedName),
        Variants(std::move(variants)) {}

  const std::string &getName() const { return *Name; }
  const std::vector<const std::string *> &getVariants() const {
    return Variants;
  }
  size_t getNumVariants() const { return Variants.size(); }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_EnumDecl; }
};

// `impl A { ... }` (#216): methods added to an existing type and dispatched
// statically, and its `static fn`s.  TypeName is the type as written
// (`A`, `geo::Point`).  `impl Show for A { ... }` implements a trait, the
// annotation Trait (`Show`, `Into<int>`); null for a plain `impl` block.
class ImplDecl : public Decl {
  const std::string *TypeName; // interned
  std::vector<FuncDecl *> Methods;
  Type *Trait;
  std::vector<const std::string *> TypeParams; // `impl Box<T>`

public:
  ImplDecl(SourceLocation loc, const std::string &internedTypeName,
           std::vector<FuncDecl *> methods, Type *trait = nullptr,
           std::vector<const std::string *> typeParams = {})
      : Decl(NK_ImplDecl, loc), TypeName(&internedTypeName),
        Methods(std::move(methods)), Trait(trait),
        TypeParams(std::move(typeParams)) {}

  const std::string &getTypeName() const { return *TypeName; }
  const std::vector<FuncDecl *> &getMethods() const { return Methods; }
  Type *getTrait() const { return Trait; }
  /// The generic type's parameters, by the names the block gives them.
  const std::vector<const std::string *> &getTypeParams() const {
    return TypeParams;
  }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_ImplDecl; }
};

// `trait Show { view fn toString() -> Str; }` (#216): the methods a type
// implementing it must have.  Its methods are signatures only (no body);
// `Self` in them is the implementing type.
class TraitDecl : public Decl {
  const std::string *Name; // interned
  std::vector<const std::string *> TypeParams;
  std::vector<FuncDecl *> Methods;

public:
  TraitDecl(SourceLocation loc, const std::string &internedName,
            std::vector<const std::string *> typeParams,
            std::vector<FuncDecl *> methods)
      : Decl(NK_TraitDecl, loc), Name(&internedName),
        TypeParams(std::move(typeParams)), Methods(std::move(methods)) {}

  const std::string &getName() const { return *Name; }
  const std::vector<const std::string *> &getTypeParams() const {
    return TypeParams;
  }
  const std::vector<FuncDecl *> &getMethods() const { return Methods; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_TraitDecl; }
};

// Array literal expression: `[a, b, c]`, or `[v; n]`, `n` copies of one
// value (`n` an integer literal).
//
// Sema types it as a `T[n]` (Sema rejects `[]`, which parses): the length is
// the number of elements, or the repeat count.  The repeated value is
// evaluated once, so `[Node(); 3]` holds one object three times.
class ArrayLiteralExpr : public Expr {
  std::vector<Expr *> Elements;
  // The `n` of `[v; n]` (its single element is `v`), or 0 for a list.
  int64_t RepeatCount;

public:
  ArrayLiteralExpr(SourceLocation loc, std::vector<Expr *> elems,
                   int64_t repeatCount = 0)
      : Expr(NK_ArrayLiteralExpr, loc), Elements(std::move(elems)),
        RepeatCount(repeatCount) {}

  const std::vector<Expr *> &getElements() const { return Elements; }
  size_t getNumElements() const { return Elements.size(); }
  bool isEmpty() const { return Elements.empty(); }
  bool isRepeat() const { return RepeatCount != 0; }
  int64_t getRepeatCount() const { return RepeatCount; }
  // The array's length: the number of elements, or the repeat count.
  int64_t getLength() const {
    return isRepeat() ? RepeatCount : static_cast<int64_t>(Elements.size());
  }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_ArrayLiteralExpr;
  }
};

// Subscript expression: array[index]
class SubscriptExpr : public Expr {
  Expr *Array;
  Expr *Index;

public:
  SubscriptExpr(SourceLocation loc, Expr *array, Expr *index)
      : Expr(NK_SubscriptExpr, loc), Array(array), Index(index) {}

  Expr *getArray() const { return Array; }
  Expr *getIndex() const { return Index; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_SubscriptExpr;
  }
};

// Tuple type: (T1, T2, ...)  (e.g. (int, Str), (int, (Str, bool))[2])
//
// A fixed-arity (>= 2), heterogeneous, immutable product type.  A tuple is a
// value held in place (isMemType): its elements laid out like a C struct's
// fields, a reference element as its box.
// The parser allocates a source-located node per annotation; Sema resolves
// each one to the canonical instance interned by ASTContext::getTupleType, so
// resolved tuple types compare by pointer identity.
//
// A StructType is a TupleType too (isa<TupleType> holds for one): a struct's
// fields are laid out, copied, retained and released exactly as a tuple's
// elements are, so the lowering treats the two alike.  Where the language
// tells them apart -- a struct is nominal, its fields are named and mutable,
// it has methods -- Sema checks isa<StructType> first.
class TupleType : public Type {
  std::vector<Type *> ElementTypes;

protected:
  TupleType(NodeKind kind, SourceLocation loc, std::vector<Type *> elemTys)
      : Type(kind, loc), ElementTypes(std::move(elemTys)) {
    // == / != compare element-wise (PaykanTuple_equals).  Set on the
    // constructor: besides the canonical instances, the parser allocates a
    // node per annotation, so there is no single bootstrap point to set it on.
    addBinaryOp(BinaryOpcode::Eq);
    addBinaryOp(BinaryOpcode::Ne);
  }
  /// A struct's fields are added one by one as they are resolved.
  void addElementType(Type *ty) { ElementTypes.push_back(ty); }

public:
  TupleType(SourceLocation loc, std::vector<Type *> elemTys)
      : TupleType(NK_TupleType, loc, std::move(elemTys)) {}

  const std::vector<Type *> &getElementTypes() const { return ElementTypes; }
  size_t getArity() const { return ElementTypes.size(); }
  Type *getElementType(size_t i) const { return ElementTypes[i]; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_TupleType || N->getKind() == NK_StructType;
  }
};

// Struct type: `struct Point { x: int; y: int; }`
// (docs/language/11-structs.md).
//
// A value type with named fields: a tuple of its field types (see TupleType)
// that is nominal -- two structs with the same fields are different types,
// and a struct is never a tuple of the same shape -- with mutable fields
// (`p.x = 1`), the methods of its `impl` blocks (called directly, there is no
// vtable and no inheritance) and a constructor, its `__init__` (the default
// one takes the fields in order).  Its `toString()` is `Point(x: 1, y: 2)`
// and `==` compares the fields.  One canonical instance per struct,
// registered by name in the ASTContext (registerStructType), so struct types
// compare by pointer identity.
class StructType : public TupleType {
  const std::string *Name; // points into ASTContext::StringPool (stable)
  std::vector<std::string> FieldNames; // parallel to the element types
  // The methods of its `impl` blocks, in declaration order (no vtable: each
  // is called directly), and its `__init__`, kept apart like a class's.
  std::vector<MethodDecl *> ImplMethods;
  MethodDecl *InitMethod = nullptr;

public:
  StructType(SourceLocation loc, const std::string &internedName)
      : TupleType(NK_StructType, loc, {}), Name(&internedName) {}

  const std::string &getName() const { return *Name; }

  // -- Fields (Sema adds them as it resolves their types)

  void addField(const std::string &name, Type *ty) {
    FieldNames.push_back(name);
    addElementType(ty);
  }
  const std::vector<std::string> &getFieldNames() const { return FieldNames; }
  const std::string &getFieldName(size_t i) const { return FieldNames[i]; }
  size_t getNumFields() const { return FieldNames.size(); }
  /// The index of field @p name, or -1.
  int fieldIndex(const std::string &name) const {
    for (size_t i = 0; i < FieldNames.size(); ++i)
      if (FieldNames[i] == name)
        return static_cast<int>(i);
    return -1;
  }
  /// The type of field @p name, or nullptr.
  Type *findField(const std::string &name) const {
    int i = fieldIndex(name);
    return i < 0 ? nullptr : getElementType(static_cast<size_t>(i));
  }

  // -- Methods

  /// Add an `impl` method, or the `__init__`.
  void addMethod(MethodDecl *m);
  /// The `impl` method (or `__init__`) named @p name, or nullptr.
  MethodDecl *findMethod(const std::string &name) const;
  const std::vector<MethodDecl *> &getImplMethods() const {
    return ImplMethods;
  }
  MethodDecl *getInitMethod() const { return InitMethod; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_StructType;
  }
};

// Poison type: the type of a binder whose declaration failed (#89).
//
// When a declaration's initializer, type annotation, or pattern fails, Sema
// still binds the name -- to this type -- so that later uses of the name do
// not report a follow-on "use of undeclared variable".  A use of a
// poison-typed name is absorbed silently: the expression checker yields no
// type (counting a suppressed follow-on) and every enclosing check already
// stays quiet for an operand in error.  Only the original error is reported.
//
// It is not a type a program can name (the standard library's error::Error
// is a struct): a poisoned variable never type-checks as a valid value.  It has
// no spelling, so it can never be named in source; it lives only in Sema's
// symbol table and is never written into the AST, so it cannot reach the
// lowering (a program with a poisoned binder always has an error).  One
// canonical instance per ASTContext (ASTContext::getPoisonTy).
class PoisonType : public Type {
public:
  explicit PoisonType(SourceLocation loc) : Type(NK_PoisonType, loc) {}

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_PoisonType;
  }
};

// The type of a `view T[]` or `inout T[]` parameter: a slice, the address of
// a fixed-size array's elements and their number (docs/language/09-arrays.md,
// "Slices").  It is spelled `T[]` with the parameter's mode; Sema gives it to
// the parameter, so a slice is indexed, measured with `len()` and passed to
// another slice parameter, and nothing else.  One canonical instance per
// element type (ASTContext::getSliceType).
// Function type: `(int, view Str) -> bool`
// (docs/language/14-function-types.md).
//
// The type of a function value: a lambda or a named function used without
// calling it.  Its parameters have types and modes, as a function's do, and
// its result is a copy (never a borrow).  Structural: Sema resolves every
// annotation to the canonical instance of its shape
// (ASTContext::getFunctionType), so function types compare by pointer
// identity.  A value is a closure, a reference-counted object (isRefType).
class FunctionType : public Type {
  std::vector<Type *> ParamTypes;
  std::vector<ParamMode> Modes; // parallel to ParamTypes
  Type *ReturnType;             // never null: the builtin void for none

public:
  FunctionType(SourceLocation loc, std::vector<Type *> params,
               std::vector<ParamMode> modes, Type *retTy)
      : Type(NK_FunctionType, loc), ParamTypes(std::move(params)),
        Modes(std::move(modes)), ReturnType(retTy) {
    // == / != compare closures by identity, as objects without an `equals`
    // of their own do (so a function value is a `List<T>` element).
    addBinaryOp(BinaryOpcode::Eq);
    addBinaryOp(BinaryOpcode::Ne);
  }

  const std::vector<Type *> &getParamTypes() const { return ParamTypes; }
  const std::vector<ParamMode> &getModes() const { return Modes; }
  size_t getNumParams() const { return ParamTypes.size(); }
  Type *getParamType(size_t i) const { return ParamTypes[i]; }
  ParamMode getMode(size_t i) const { return Modes[i]; }
  Type *getReturnType() const { return ReturnType; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_FunctionType;
  }
};

class SliceType : public Type {
  Type *ElementType;

public:
  SliceType(SourceLocation loc, Type *elemTy)
      : Type(NK_SliceType, loc), ElementType(elemTy) {}

  Type *getElementType() const { return ElementType; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_SliceType; }
};

// Tuple literal expression: (e1, e2, ...) — always >= 2 elements; a
// parenthesised single expression is just that expression.
class TupleLiteralExpr : public Expr {
  std::vector<Expr *> Elements;

public:
  TupleLiteralExpr(SourceLocation loc, std::vector<Expr *> elems)
      : Expr(NK_TupleLiteralExpr, loc), Elements(std::move(elems)) {}

  const std::vector<Expr *> &getElements() const { return Elements; }
  size_t getNumElements() const { return Elements.size(); }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_TupleLiteralExpr;
  }
};

// Tuple element access: t.0, t.1, ...  The index is a compile-time constant
// (scanned as one token at the dot, docs/grammar.md); Sema checks it
// against the tuple's arity.
class TupleIndexExpr : public Expr {
  Expr *Tuple;
  size_t Index;

public:
  TupleIndexExpr(SourceLocation loc, Expr *tuple, size_t index)
      : Expr(NK_TupleIndexExpr, loc), Tuple(tuple), Index(index) {}

  Expr *getTuple() const { return Tuple; }
  size_t getIndex() const { return Index; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_TupleIndexExpr;
  }
};

// Destructuring statement:  a, b = expr;   a: int, _ = expr;
//
// Binds each element of a tuple-valued expression to a target, left to
// right.  A target is a name (declared on first assignment or re-assigned if
// already in scope — the same implicit-declaration rule as AssignStmt), a
// name with an explicit type annotation (always a fresh declaration, like
// VarDecl), or `_` to skip the element.  At least two targets are required.
class DestructureStmt : public Stmt {
public:
  struct Target {
    const std::string *Name; // interned; empty string == `_` (skip)
    Type *DeclType;          // explicit annotation, or nullptr
    SourceLocation Loc;

    bool isSkip() const { return Name->empty(); }
    const std::string &getName() const { return *Name; }
  };

private:
  std::vector<Target> Targets;
  Expr *Value;

public:
  DestructureStmt(SourceLocation loc, std::vector<Target> targets, Expr *value)
      : Stmt(NK_DestructureStmt, loc), Targets(std::move(targets)),
        Value(value) {}

  const std::vector<Target> &getTargets() const { return Targets; }
  size_t getNumTargets() const { return Targets.size(); }
  Expr *getValue() const { return Value; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_DestructureStmt;
  }
};

/// Returns true for an optional of a scalar, `int?`, `float?`, `bool?`,
/// `char?` (#96) or an enum's (`Suit?`, its ordinal): a value, the payload and
/// a presence flag held inline (PIR `opt_i64` & co.), with no box and no
/// reference count.
inline bool isValueOptional(const Type *ty) {
  auto *ot = dyn_cast<OptionalType>(ty);
  return ot && (isa<BuiltinType>(ot->getInnerType()) ||
                isa<EnumType>(ot->getInnerType()));
}

/// Returns true for an optional of a value held in memory, a tuple's or a
/// struct's (`(int, Str)?`, `Point?`): held in place as a presence flag and
/// the value (laid out as the tuple `(bool, T)`), None all zeros.
inline bool isMemOptional(const Type *ty) {
  auto *ot = dyn_cast<OptionalType>(ty);
  return ot && isa<TupleType>(ot->getInnerType());
}

/// Returns true for a function value (docs/design/function-types.md): a
/// function type or the optional of one.  It is held in place, as the code
/// to run and the closure holding its captures, so it is a value (isMemType)
/// and not a reference.
inline bool isFunctionValue(const Type *ty) {
  if (const auto *ot = dyn_cast<OptionalType>(ty))
    ty = ot->getInnerType();
  return ty && isa<FunctionType>(ty);
}

/// Returns true for any type whose values are heap-allocated and
/// reference-counted at runtime: ClassType and the OptionalType of one (a
/// possibly-NULL PaykanShared* box: it shares the class's box).  An optional
/// of a value type is a value (isValueOptional), as is a function value.
/// Use this instead of spelling out the `||` condition everywhere.
inline bool isRefType(const Type *ty) {
  return ty && !isFunctionValue(ty) && !isMemOptional(ty) &&
         (isa<ClassType>(ty) ||
          (isa<OptionalType>(ty) && !isValueOptional(ty)));
}

/// Returns true for a type whose values are held in memory, in place: a
/// fixed-size array (`T[n]`), a tuple (a struct is one), a function value,
/// or the optional of a tuple.  A copy copies its bytes and retains the
/// references among them.
inline bool isMemType(const Type *ty) {
  return ty && (isa<FixedArrayType>(ty) || isa<TupleType>(ty) ||
                isFunctionValue(ty) || isMemOptional(ty));
}

/// If @p ty is an optional type, return its inner type; otherwise @p ty.
inline Type *stripOptional(Type *ty) {
  if (auto *ot = dyn_cast<OptionalType>(ty))
    return ot->getInnerType();
  return ty;
}

/// Returns true when a value of type @p src stored into a slot of type
/// @p dst becomes the present value of an optional first: a plain primitive
/// (`int`) or an enum flowing into an optional of one (`int?`, `float?`
/// after int -> float promotion, `Suit?`), or a tuple or a struct into the
/// optional of it.
inline bool wrapsInOptional(const Type *dst, const Type *src) {
  if (!src)
    return false;
  if (isValueOptional(dst))
    return isa<BuiltinType>(src) || isa<EnumType>(src);
  return isMemOptional(dst) && isa<TupleType>(src);
}

} // namespace ast
} // namespace paykan