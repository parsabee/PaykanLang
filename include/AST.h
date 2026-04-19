// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Abstract Syntax Tree node definitions for the Paykan language

#pragma once

#include <cassert>
#include <concepts>
#include <cstdint>
#include <memory>
#include <string>
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
  SourceLocation(size_t lineStart, size_t colStart, size_t lineEnd, size_t colEnd)
      : LineStart(lineStart), ColumnStart(colStart), LineEnd(lineEnd), ColumnEnd(colEnd) {}

  size_t getLineStart() const { return LineStart; }
  size_t getColumnStart() const { return ColumnStart; }
  size_t getLineEnd() const { return LineEnd; }
  size_t getColumnEnd() const { return ColumnEnd; }
  bool isValid() const { return LineStart > 0; }
};

// Base AST node with LLVM-style RTTI
class ASTNode {
public:
  enum NodeKind {
    // Declarations
    NK_VarDecl,
    NK_FuncDecl,
    NK_MethodDecl,

    // Statements
    NK_CompoundStmt,
    NK_ReturnStmt,
    NK_AssignStmt,
    NK_DeclStmt,
    NK_ExprStmt,
    NK_IfStmt,
    NK_WhileStmt,
    NK_BreakStmt,
    NK_ContinueStmt,

    // Expressions
    NK_IntegerLiteral,
    NK_FloatLiteral,
    NK_BoolLiteral,
    NK_StringLiteral,
    NK_UnaryExpr,
    NK_BinaryExpr,
    NK_Identifier,
    NK_CallExpr,
    NK_TernaryExpr,
    NK_MovExpr,
    NK_RefExpr,

    // Types
    NK_BuiltinType,
    NK_ClassType,

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
concept ASTNodeType = std::derived_from<T, ASTNode> &&
    requires(const ASTNode *n) {
      { T::classof(n) } -> std::same_as<bool>;
    };

// -- LLVM-style RTTI free functions ------------------------------------------

template <ASTNodeType T>
bool isa(const ASTNode *n) { return T::classof(n); }

template <ASTNodeType T>
T *cast(ASTNode *n) {
  assert(isa<T>(n) && "Invalid cast");
  return static_cast<T *>(n);
}

template <ASTNodeType T>
const T *cast(const ASTNode *n) {
  assert(isa<T>(n) && "Invalid cast");
  return static_cast<const T *>(n);
}

template <ASTNodeType T>
T *dyn_cast(ASTNode *n) { return isa<T>(n) ? cast<T>(n) : nullptr; }

template <ASTNodeType T>
const T *dyn_cast(const ASTNode *n) {
  return isa<T>(n) ? cast<T>(n) : nullptr;
}

// Base for all declarations
class Decl : public ASTNode {
public:
  Decl(NodeKind K, SourceLocation loc) : ASTNode(K, loc) {}

  static bool classof(const ASTNode *N) {
    return N->getKind() >= NK_VarDecl && N->getKind() <= NK_MethodDecl;
  }
};

// Base for all statements
class Stmt : public ASTNode {
public:
  Stmt(NodeKind K, SourceLocation loc) : ASTNode(K, loc) {}

  static bool classof(const ASTNode *N) {
    return N->getKind() >= NK_CompoundStmt && N->getKind() <= NK_ContinueStmt;
  }
};

// Base for all expressions
class Expr : public ASTNode {
public:
  Expr(NodeKind K, SourceLocation loc) : ASTNode(K, loc) {}

  static bool classof(const ASTNode *N) {
    return N->getKind() >= NK_IntegerLiteral && N->getKind() <= NK_RefExpr;
  }
};

// -- Operator enums (at namespace scope so Type can reference them) ----------

enum class UnaryOpcode {
  Neg, // -
  Not, // !
};

enum class BinaryOpcode {
  // Arithmetic
  Add, // +
  Sub, // -
  Mul, // *
  Div, // /
  Mod, // %
  // Relational
  Lt,  // <
  Gt,  // >
  Le,  // <=
  Ge,  // >=
  Eq,  // ==
  Ne,  // !=
  // Logical
  And, // &&
  Or,  // ||
};

/// Ownership qualifier for class-type variables.
/// Builtins (int, float, bool) always have automatic (stack) storage;
/// ownership only applies to heap-allocated class types.
enum class Ownership : uint8_t {
  Unique,    // default — single owner, destroyed at scope exit
  Shared,    // reference-counted
  Reference, // borrowed pointer (&), no ownership
};

// Base for all types
class Type : public ASTNode {
  uint32_t UnaryOps = 0;   // bitmask of supported UnaryOpcode values
  uint32_t BinaryOps = 0;  // bitmask of supported BinaryOpcode values

public:
  Type(NodeKind K, SourceLocation loc) : ASTNode(K, loc) {}

  /// Register a unary operator. Returns true if newly added, false if already present.
  bool addUnaryOp(UnaryOpcode op) {
    uint32_t bit = 1u << static_cast<unsigned>(op);
    if (UnaryOps & bit) return false;
    UnaryOps |= bit;
    return true;
  }

  /// Register a binary operator. Returns true if newly added, false if already present.
  bool addBinaryOp(BinaryOpcode op) {
    uint32_t bit = 1u << static_cast<unsigned>(op);
    if (BinaryOps & bit) return false;
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
    return N->getKind() >= NK_BuiltinType && N->getKind() <= NK_ClassType;
  }
};

// Variable declaration with explicit type (x: int = 10)
class VarDecl : public Decl {
private:
  std::string Name;
  Type *VarType;
  Expr *InitExpr;
  Ownership OwnershipKind;
  bool IsConst;

public:
  VarDecl(SourceLocation loc, const std::string &name, Type *type, Expr *init,
          Ownership ownership = Ownership::Unique, bool isConst = false)
      : Decl(NK_VarDecl, loc), Name(name), VarType(type), InitExpr(init),
        OwnershipKind(ownership), IsConst(isConst) {}

  const std::string &getName() const { return Name; }
  Type *getType() const { return VarType; }
  Expr *getInitExpr() const { return InitExpr; }
  Ownership getOwnership() const { return OwnershipKind; }
  bool isConst() const { return IsConst; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_VarDecl; }
};

// A single function parameter (name + type + ownership).
struct Param {
  std::string Name;
  Type *ParamType;
  Ownership Own = Ownership::Unique;
  bool IsConst = false;
};

// Forward declaration for FuncDecl body.
class CompoundStmt;

// Free function declaration:  fn name(params) -> retType { body }
class FuncDecl : public Decl {
private:
  std::string Name;
  std::vector<Param> Params;
  Type *ReturnType;       // nullptr means void
  CompoundStmt *Body;

public:
  FuncDecl(SourceLocation loc, const std::string &name,
           std::vector<Param> params, Type *retTy, CompoundStmt *body)
      : Decl(NK_FuncDecl, loc), Name(name), Params(std::move(params)),
        ReturnType(retTy), Body(body) {}

  const std::string &getName() const { return Name; }
  const std::vector<Param> &getParams() const { return Params; }
  size_t getNumParams() const { return Params.size(); }
  Type *getReturnType() const { return ReturnType; }
  CompoundStmt *getBody() const { return Body; }

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

// Assignment statement
class AssignStmt : public Stmt {
private:
  std::string VarName;
  Expr *Value;

public:
  AssignStmt(SourceLocation loc, const std::string &varName, Expr *value)
      : Stmt(NK_AssignStmt, loc), VarName(varName), Value(value) {}

  const std::string &getVarName() const { return VarName; }
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
  DeclStmt(SourceLocation loc, Decl *decl) : Stmt(NK_DeclStmt, loc), Declaration(decl) {}

  Decl *getDecl() const { return Declaration; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_DeclStmt; }
};

// Expression statement (wraps an Expr to use it in statement context)
class ExprStmt : public Stmt {
private:
  Expr *Expression;

public:
  ExprStmt(SourceLocation loc, Expr *expr) : Stmt(NK_ExprStmt, loc), Expression(expr) {}

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
  static bool classof(const ASTNode *N) { return N->getKind() == NK_ContinueStmt; }
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
  FloatLiteral(SourceLocation loc, double value) : Expr(NK_FloatLiteral, loc), Value(value) {}

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
  BoolLiteral(SourceLocation loc, bool value) : Expr(NK_BoolLiteral, loc), Value(value) {}

  bool getValue() const { return Value; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_BoolLiteral;
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

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_UnaryExpr;
  }
};

// Identifier expression
class Identifier : public Expr {
private:
  std::string Name;

public:
  Identifier(SourceLocation loc, const std::string &name)
      : Expr(NK_Identifier, loc), Name(name) {}

  const std::string &getName() const { return Name; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_Identifier;
  }
};

// Function call expression
class CallExpr : public Expr {
private:
  std::string CalleeName;
  std::vector<Expr *> Arguments;

public:
  CallExpr(SourceLocation loc, const std::string &callee, std::vector<Expr *> args)
      : Expr(NK_CallExpr, loc), CalleeName(callee), Arguments(std::move(args)) {}

  const std::string &getCalleeName() const { return CalleeName; }
  const std::vector<Expr *> &getArguments() const { return Arguments; }
  size_t getNumArguments() const { return Arguments.size(); }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_CallExpr; }
};

// Move expression (mov x)
class MovExpr : public Expr {
private:
  Identifier *Operand;

public:
  MovExpr(SourceLocation loc, Identifier *operand)
      : Expr(NK_MovExpr, loc), Operand(operand) {}

  Identifier *getOperand() const { return Operand; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_MovExpr; }
};

// Reference expression (&x) — borrow a variable or expression
class RefExpr : public Expr {
private:
  Expr *Operand;

public:
  RefExpr(SourceLocation loc, Expr *operand)
      : Expr(NK_RefExpr, loc), Operand(operand) {}

  Expr *getOperand() const { return Operand; }

  /// If the operand is an Identifier, return it; otherwise nullptr.
  Identifier *getIdentOperand() const {
    return dyn_cast<Identifier>(Operand);
  }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_RefExpr; }
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

// Builtin type (int, void, etc.)
class BuiltinType : public Type {
public:
  enum Kind {
    Int,
    Float,
    Bool,
    Void,
  };

private:
  Kind TypeKind;

  void initOps();

public:
  BuiltinType(SourceLocation loc, Kind kind) : Type(NK_BuiltinType, loc), TypeKind(kind) {
    initOps();
  }

  Kind getTypeKind() const { return TypeKind; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_BuiltinType;
  }
};

// Method declaration (name + signature + flags)
class MethodDecl : public Decl {
public:
  enum Flags : uint8_t {
    None    = 0,
    Static  = 1 << 0,
    Private = 1 << 1,
  };

private:
  std::string Name;
  Type *ReturnType;
  std::vector<Type *> ParamTypes;
  uint8_t MethodFlags;

public:
  MethodDecl(SourceLocation loc, const std::string &name, Type *retTy,
             std::vector<Type *> params, uint8_t flags = None)
      : Decl(NK_MethodDecl, loc), Name(name), ReturnType(retTy),
        ParamTypes(std::move(params)), MethodFlags(flags) {}

  const std::string &getName() const { return Name; }
  Type *getReturnType() const { return ReturnType; }
  const std::vector<Type *> &getParamTypes() const { return ParamTypes; }
  size_t getNumParams() const { return ParamTypes.size(); }

  /// Replace the return type (used for forward-reference patching).
  void setReturnType(Type *ty) { ReturnType = ty; }

  /// Replace a parameter type at index i (used for forward-reference patching).
  void setParamType(size_t i, Type *ty) {
    assert(i < ParamTypes.size() && "param index out of range");
    ParamTypes[i] = ty;
  }

  bool isStatic() const { return MethodFlags & Static; }
  bool isPrivate() const { return MethodFlags & Private; }
  bool isVirtual() const { return !isStatic() && !isPrivate(); }

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
//      - If a same-named method exists in the parent vtable, override that slot.
//      - Otherwise, append a new slot.
//
class ClassType : public Type {
  std::string Name;
  ClassType *SuperClass;

  // Instance fields: (name, type) pairs.
  std::vector<std::pair<std::string, Type *>> Fields;

  // All methods declared in this class.
  std::vector<MethodDecl *> Methods;

  // Flattened vtable: virtual methods in layout order.
  std::vector<MethodDecl *> VTable;

public:
  ClassType(SourceLocation loc, const std::string &name,
            ClassType *superClass = nullptr)
      : Type(NK_ClassType, loc), Name(name), SuperClass(superClass) {
    // Inherit parent vtable.
    if (SuperClass)
      VTable = SuperClass->VTable;
    // Inherit parent operators (Eq/Ne).
    if (SuperClass) {
      // Object-typed values can at least be compared for equality.
    }
  }

  const std::string &getName() const { return Name; }
  ClassType *getSuperClass() const { return SuperClass; }

  // -- Fields ---------------------------------------------------------------

  void addField(const std::string &name, Type *ty) {
    Fields.emplace_back(name, ty);
  }
  const std::vector<std::pair<std::string, Type *>> &getFields() const {
    return Fields;
  }
  size_t getNumFields() const { return Fields.size(); }

  // -- Methods --------------------------------------------------------------

  void addMethod(MethodDecl *m);

  const std::vector<MethodDecl *> &getMethods() const { return Methods; }
  const std::vector<MethodDecl *> &getVTable() const { return VTable; }
  size_t getVTableSize() const { return VTable.size(); }

  /// Find the vtable slot index for a method name, or -1 if not found.
  int getVTableIndex(const std::string &name) const;

  /// Look up a method by name (searches this class and parents).
  MethodDecl *findMethod(const std::string &name) const;

  /// Returns true if this type is a subtype of (or equal to) Other.
  bool isSubtypeOf(const ClassType *other) const;

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_ClassType;
  }
};

// Translation unit (top-level container)
class TranslationUnit : public ASTNode {
private:
  std::vector<FuncDecl *> FuncDecls;

public:
  TranslationUnit(SourceLocation loc, std::vector<FuncDecl *> funcs)
      : ASTNode(NK_TranslationUnit, loc), FuncDecls(std::move(funcs)) {}

  const std::vector<FuncDecl *> &getFuncDecls() const { return FuncDecls; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_TranslationUnit;
  }
};

} // namespace ast
} // namespace paykan