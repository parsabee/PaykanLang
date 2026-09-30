// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Abstract Syntax Tree node definitions for the Paykan language

#pragma once

#include <cassert>
#include <concepts>
#include <cstdint>
#include <string>
#include <unordered_map>
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
    NK_MovExpr,
    NK_TupleLiteralExpr,
    NK_TupleIndexExpr,

    // Types
    NK_BuiltinType,
    NK_ClassType,
    NK_ArrayType,
    NK_OptionalType,
    NK_EnumType,
    NK_TupleType,

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

// -- LLVM-style RTTI free functions ------------------------------------------

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
    return N->getKind() >= NK_VarDecl && N->getKind() <= NK_EnumDecl;
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
  // site.  Currently the only such conversion is an optional `T?` flowing into
  // an `Obj` slot: CodeGen must then materialise the `None` singleton for a
  // null box (see OptionalType).  nullptr = no conversion.
  Type *CoercedType = nullptr;

public:
  Expr(NodeKind K, SourceLocation loc) : ASTNode(K, loc) {}

  void setResolvedType(Type *ty) { ResolvedType = ty; }
  Type *getResolvedType() const { return ResolvedType; }

  void setCoercedType(Type *ty) { CoercedType = ty; }
  Type *getCoercedType() const { return CoercedType; }

  static bool classof(const ASTNode *N) {
    return N->getKind() >= NK_IntegerLiteral &&
           N->getKind() <= NK_TupleIndexExpr;
  }
};

// -- Operator enums (at namespace scope so Type can reference them) ----------

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
    return N->getKind() >= NK_BuiltinType && N->getKind() <= NK_TupleType;
  }
};

// Variable declaration with explicit type (x: int = 10)
class VarDecl : public Decl {
private:
  const std::string *Name; // points into ASTContext::StringPool (stable)
  Type *VarType;
  Expr *InitExpr;

public:
  VarDecl(SourceLocation loc, const std::string &internedName, Type *type,
          Expr *init)
      : Decl(NK_VarDecl, loc), Name(&internedName), VarType(type),
        InitExpr(init) {}

  const std::string &getName() const { return *Name; }
  Type *getType() const { return VarType; }
  Expr *getInitExpr() const { return InitExpr; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_VarDecl; }
};

// A single function parameter.
struct Param {
  const std::string *Name; // points into ASTContext::StringPool (stable)
  Type *ParamType;

  const std::string &getName() const { return *Name; }
};

// Forward declaration for FuncDecl body.
class CompoundStmt;

// Free function declaration:  fn name(params) -> retType { body }
class FuncDecl : public Decl {
private:
  const std::string *Name; // points into ASTContext::StringPool (stable)
  std::vector<Param> Params;
  Type *ReturnType; // nullptr means void
  CompoundStmt *Body;

public:
  FuncDecl(SourceLocation loc, const std::string &internedName,
           std::vector<Param> params, Type *retTy, CompoundStmt *body)
      : Decl(NK_FuncDecl, loc), Name(&internedName), Params(std::move(params)),
        ReturnType(retTy), Body(body) {}

  const std::string &getName() const { return *Name; }
  const std::vector<Param> &getParams() const { return Params; }
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

public:
  Identifier(SourceLocation loc, const std::string &internedName)
      : Expr(NK_Identifier, loc), Name(&internedName) {}

  const std::string &getName() const { return *Name; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_Identifier;
  }
};

// Deferred inline: AssignStmt::getVarName() requires complete Identifier type.
inline const std::string &AssignStmt::getVarName() const {
  return LHS->getName();
}

// Function call expression
class CallExpr : public Expr {
private:
  const std::string *CalleeName; // points into ASTContext::StringPool (stable)
  std::vector<Expr *> Arguments;

public:
  CallExpr(SourceLocation loc, const std::string &internedCallee,
           std::vector<Expr *> args)
      : Expr(NK_CallExpr, loc), CalleeName(&internedCallee),
        Arguments(std::move(args)) {}

  const std::string &getCalleeName() const { return *CalleeName; }
  const std::vector<Expr *> &getArguments() const { return Arguments; }
  size_t getNumArguments() const { return Arguments.size(); }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_CallExpr; }
};

// Method call expression: receiver.method(args)
class MethodCallExpr : public Expr {
private:
  Expr *Receiver;
  const std::string *MethodName; // points into ASTContext::StringPool (stable)
  std::vector<Expr *> Arguments;

public:
  MethodCallExpr(SourceLocation loc, Expr *receiver,
                 const std::string &internedMethod, std::vector<Expr *> args)
      : Expr(NK_MethodCallExpr, loc), Receiver(receiver),
        MethodName(&internedMethod), Arguments(std::move(args)) {}

  Expr *getReceiver() const { return Receiver; }
  const std::string &getMethodName() const { return *MethodName; }
  const std::vector<Expr *> &getArguments() const { return Arguments; }
  size_t getNumArguments() const { return Arguments.size(); }

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
  };

private:
  Kind TypeKind;

  void initOps();

public:
  BuiltinType(SourceLocation loc, Kind kind)
      : Type(NK_BuiltinType, loc), TypeKind(kind) {
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
    None = 0,
    Private = 1 << 0,
  };

private:
  const std::string *Name; // points into ASTContext::StringPool (stable)
  Type *ReturnType;
  std::vector<Type *> ParamTypes;
  uint8_t MethodFlags;

public:
  MethodDecl(SourceLocation loc, const std::string &internedName, Type *retTy,
             std::vector<Type *> params, uint8_t flags = None)
      : Decl(NK_MethodDecl, loc), Name(&internedName), ReturnType(retTy),
        ParamTypes(std::move(params)), MethodFlags(flags) {}

  const std::string &getName() const { return *Name; }
  Type *getReturnType() const { return ReturnType; }
  const std::vector<Type *> &getParamTypes() const { return ParamTypes; }
  size_t getNumParams() const { return ParamTypes.size(); }

  bool isPrivate() const { return MethodFlags & Private; }
  bool isVirtual() const { return !isPrivate(); }

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

  // All virtual methods in layout order (vtable). __init__ is NOT here.
  std::vector<MethodDecl *> VTable;
  // O(1) index: method name -> vtable slot index. Kept in sync with VTable.
  std::unordered_map<std::string, int> VTableIndex;

  // The class initialiser (__init__), stored separately because it is a
  // static function, not a virtual method, and has no vtable slot.
  MethodDecl *InitMethod = nullptr;

  // When true, no user class may inherit from this type.
  bool Final = false;

  // When true, this type is a compiler builtin (Obj, Str, Array, File, Error,
  // Int, Float, Bool) registered by the ASTContext bootstrap: its methods are
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

  // -- Fields ---------------------------------------------------------------

  void addField(const std::string &name, Type *ty) {
    Fields.emplace_back(name, ty);
  }
  const std::vector<std::pair<std::string, Type *>> &getFields() const {
    return Fields;
  }
  size_t getNumFields() const { return Fields.size(); }

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

  // -- Methods --------------------------------------------------------------

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

  /// Find the vtable slot index for a method name, or -1 if not found.
  int getVTableIndex(const std::string &name) const;

  /// Look up a method by name (searches this class and parents).
  MethodDecl *findMethod(const std::string &name) const;

  /// Returns true if this type is a subtype of (or equal to) Other.
  bool isSubtypeOf(const ClassType *other) const;

  static bool classof(const ASTNode *N) { return N->getKind() == NK_ClassType; }
};

// Import declaration
//   import path::module;                      — SelectedNames empty, Alias
//   empty import path::{a, b};                      — SelectedNames = {a, b},
//   Alias empty import path::module as alias;             — SelectedNames
//   empty, Alias = "alias" import ::system_module;                   — IsSystem
//   true
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
class ClassDecl : public Decl {
private:
  const std::string *Name; // points into ASTContext::StringPool (stable)
  const std::string *SuperClassName; // interned; "" = no explicit superclass
  std::vector<VarDecl *> Fields;
  std::vector<FuncDecl *> Methods;

public:
  ClassDecl(SourceLocation loc, const std::string &internedName,
            const std::string &internedSuperName, std::vector<VarDecl *> fields,
            std::vector<FuncDecl *> methods)
      : Decl(NK_ClassDecl, loc), Name(&internedName),
        SuperClassName(&internedSuperName), Fields(std::move(fields)),
        Methods(std::move(methods)) {}

  const std::string &getName() const { return *Name; }
  const std::string &getSuperClassName() const { return *SuperClassName; }
  bool hasSuperClass() const { return !SuperClassName->empty(); }

  const std::vector<VarDecl *> &getFields() const { return Fields; }
  const std::vector<FuncDecl *> &getMethods() const { return Methods; }
  size_t getNumFields() const { return Fields.size(); }
  size_t getNumMethods() const { return Methods.size(); }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_ClassDecl; }
};

// Translation unit (top-level container)
class TranslationUnit : public ASTNode {
private:
  std::vector<ImportDecl *> Imports;
  std::vector<ClassDecl *> ClassDecls;
  std::vector<FuncDecl *> FuncDecls;
  std::vector<EnumDecl *> EnumDecls;

public:
  TranslationUnit(SourceLocation loc, std::vector<ImportDecl *> imports,
                  std::vector<ClassDecl *> classes,
                  std::vector<FuncDecl *> funcs,
                  std::vector<EnumDecl *> enums = {})
      : ASTNode(NK_TranslationUnit, loc), Imports(std::move(imports)),
        ClassDecls(std::move(classes)), FuncDecls(std::move(funcs)),
        EnumDecls(std::move(enums)) {}

  const std::vector<ImportDecl *> &getImports() const { return Imports; }
  const std::vector<ClassDecl *> &getClassDecls() const { return ClassDecls; }
  const std::vector<FuncDecl *> &getFuncDecls() const { return FuncDecls; }
  const std::vector<EnumDecl *> &getEnumDecls() const { return EnumDecls; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_TranslationUnit;
  }
};

// Array type: T[]  (e.g. int[], Str[], float[])
//
// The array itself is heap-allocated (a subclass of Obj at runtime).
// ElementType points to the type of each stored element.
class ArrayType : public Type {
  Type *ElementType;

public:
  ArrayType(SourceLocation loc, Type *elemTy)
      : Type(NK_ArrayType, loc), ElementType(elemTy) {
    // Arrays support == / != (lowered to the virtual `equals`, which compares
    // reference identity — see PaykanArray_equals).  The mask must live on the
    // constructor: besides the canonical per-element instances interned by
    // ASTContext::getArrayType, the parser allocates a source-located node for
    // every `T[]` annotation, so unlike the ClassType singletons there is no
    // single bootstrap point to patch.
    addBinaryOp(BinaryOpcode::Eq);
    addBinaryOp(BinaryOpcode::Ne);
  }

  Type *getElementType() const { return ElementType; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_ArrayType; }
};

// Optional type: T?  (e.g. Node?, Str?, int[]?)
//
// PROTOTYPE (issue #5).  A `T?` holds either a value of the reference type T
// or `None`.  Only reference types may be optional (Sema rejects `int?` and
// friends, and nested `T??`), so at runtime a `T?` is the very same
// PaykanShared* box as a `T`, with a NULL box meaning `None` — no layout
// change, and every runtime entry point that touches boxes tolerates NULL.
// Like ArrayType, the parser allocates a source-located node per annotation
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
  const std::string *EnumName;      // interned; spelled enum name
  const std::string *VariantName;   // interned; spelled variant name
  EnumType *ResolvedEnum = nullptr; // set by Sema
  int64_t Value = -1;               // set by Sema (variant index)

public:
  EnumValueExpr(SourceLocation loc, const std::string &internedEnum,
                const std::string &internedVariant)
      : Expr(NK_EnumValueExpr, loc), EnumName(&internedEnum),
        VariantName(&internedVariant) {}

  const std::string &getEnumName() const { return *EnumName; }
  const std::string &getVariantName() const { return *VariantName; }

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

// Array literal expression: [expr, expr, ...] or []
//
// An empty literal ([]) requires an explicit type annotation on the
// surrounding declaration; Sema rejects bare `y = []`.
// A non-empty literal's element type is inferred by Sema.
class ArrayLiteralExpr : public Expr {
  std::vector<Expr *> Elements;

public:
  ArrayLiteralExpr(SourceLocation loc, std::vector<Expr *> elems)
      : Expr(NK_ArrayLiteralExpr, loc), Elements(std::move(elems)) {}

  const std::vector<Expr *> &getElements() const { return Elements; }
  size_t getNumElements() const { return Elements.size(); }
  bool isEmpty() const { return Elements.empty(); }

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

// Move expression:  mov <operand>
//
// Transfers ownership of the operand to the consuming context.  When the
// operand is a local variable, that variable is considered "moved out" — Sema
// forbids any later use of it, and CodeGen hands the variable's owning
// reference to the destination without an extra retain (and without a release
// at scope exit).  When the operand is a temporary (any rvalue), `mov` is a
// transparent pass-through: the temporary is already an owned value being
// forwarded.  Only local variables and temporaries may be moved; moving out of
// an aggregate slot (obj.field / arr[i]) is rejected by Sema.
class MovExpr : public Expr {
  Expr *Operand;

public:
  MovExpr(SourceLocation loc, Expr *operand)
      : Expr(NK_MovExpr, loc), Operand(operand) {}

  Expr *getOperand() const { return Operand; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_MovExpr; }
};

// Tuple type: (T1, T2, ...)  (e.g. (int, Str), (int, (Str, bool))[])
//
// A fixed-arity (>= 2), heterogeneous, immutable product type.  Like arrays,
// a tuple value is a heap-allocated Obj subtype at runtime (see
// src/Runtime/Tuple.c), so it flows through ARC exactly like a class value.
// The parser allocates a source-located node per annotation; Sema resolves
// each one to the canonical instance interned by ASTContext::getTupleType, so
// resolved tuple types compare by pointer identity.
class TupleType : public Type {
  std::vector<Type *> ElementTypes;

public:
  TupleType(SourceLocation loc, std::vector<Type *> elemTys)
      : Type(NK_TupleType, loc), ElementTypes(std::move(elemTys)) {
    // == / != lower to the virtual `equals` (element-wise, see
    // PaykanTuple_equals).  Set on the constructor for the same reason as
    // ArrayType: there is no single bootstrap point for parser-emitted nodes.
    addBinaryOp(BinaryOpcode::Eq);
    addBinaryOp(BinaryOpcode::Ne);
  }

  const std::vector<Type *> &getElementTypes() const { return ElementTypes; }
  size_t getArity() const { return ElementTypes.size(); }
  Type *getElementType(size_t i) const { return ElementTypes[i]; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_TupleType; }
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
// (lexed as a single TUPLE_INDEX token, see Lexer.lpp); Sema checks it
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

/// Returns true for any type whose values are heap-allocated and
/// reference-counted at runtime: ClassType, ArrayType, TupleType, and
/// OptionalType (an optional only ever wraps a reference type and shares its
/// representation — a possibly-NULL PaykanShared* box).
/// Use this instead of spelling out the `||` condition everywhere.
inline bool isRefType(const Type *ty) {
  return ty && (isa<ClassType>(ty) || isa<ArrayType>(ty) ||
                isa<TupleType>(ty) || isa<OptionalType>(ty));
}

/// If @p ty is an optional type, return its inner type; otherwise @p ty.
inline Type *stripOptional(Type *ty) {
  if (auto *ot = dyn_cast<OptionalType>(ty))
    return ot->getInnerType();
  return ty;
}

} // namespace ast
} // namespace paykan