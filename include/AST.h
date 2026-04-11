// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Abstract Syntax Tree node definitions for the Paykan language

#pragma once

#include <cassert>
#include <concepts>
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
    NK_FunctionDecl,
    NK_ParamDecl,
    NK_VarDecl,

    // Statements
    NK_CompoundStmt,
    NK_ReturnStmt,
    NK_AssignStmt,
    NK_DeclStmt,
    NK_ExprStmt,

    // Expressions
    NK_IntegerLiteral,
    NK_FloatLiteral,
    NK_BoolLiteral,
    NK_UnaryExpr,
    NK_BinaryExpr,
    NK_Identifier,
    NK_CallExpr,

    // Types
    NK_BuiltinType,

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
// LLVM-style RTTI via a static classof method.  Extend as needed.
template <typename T>
concept ASTNodeType = std::derived_from<T, ASTNode> &&
    requires(const ASTNode *n) {
      { T::classof(n) } -> std::same_as<bool>;
    };

// ── LLVM-style RTTI free functions ──────────────────────────────────────────

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
    return N->getKind() >= NK_FunctionDecl && N->getKind() <= NK_VarDecl;
  }
};

// Base for all statements
class Stmt : public ASTNode {
public:
  Stmt(NodeKind K, SourceLocation loc) : ASTNode(K, loc) {}

  static bool classof(const ASTNode *N) {
    return N->getKind() >= NK_CompoundStmt && N->getKind() <= NK_ExprStmt;
  }
};

// Base for all expressions
class Expr : public ASTNode {
public:
  Expr(NodeKind K, SourceLocation loc) : ASTNode(K, loc) {}

  static bool classof(const ASTNode *N) {
    return N->getKind() >= NK_IntegerLiteral && N->getKind() <= NK_CallExpr;
  }
};

// Base for all types
class Type : public ASTNode {
public:
  Type(NodeKind K, SourceLocation loc) : ASTNode(K, loc) {}

  static bool classof(const ASTNode *N) {
    return N->getKind() >= NK_BuiltinType && N->getKind() <= NK_BuiltinType;
  }
};

// Parameter declaration
class ParamDecl : public Decl {
private:
  std::string Name;
  Type *ParamType;

public:
  ParamDecl(SourceLocation loc, const std::string &name, Type *type)
      : Decl(NK_ParamDecl, loc), Name(name), ParamType(type) {}

  const std::string &getName() const { return Name; }
  Type *getType() const { return ParamType; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_ParamDecl; }
};

// Variable declaration with explicit type (x: int = 10)
class VarDecl : public Decl {
private:
  std::string Name;
  Type *VarType;
  Expr *InitExpr;

public:
  VarDecl(SourceLocation loc, const std::string &name, Type *type, Expr *init)
      : Decl(NK_VarDecl, loc), Name(name), VarType(type), InitExpr(init) {}

  const std::string &getName() const { return Name; }
  Type *getType() const { return VarType; }
  Expr *getInitExpr() const { return InitExpr; }

  static bool classof(const ASTNode *N) { return N->getKind() == NK_VarDecl; }
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

// Binary expression
class BinaryExpr : public Expr {
public:
  enum Opcode {
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
  };

private:
  Opcode Op;
  Expr *LHS;
  Expr *RHS;

public:
  BinaryExpr(SourceLocation loc, Opcode op, Expr *lhs, Expr *rhs)
      : Expr(NK_BinaryExpr, loc), Op(op), LHS(lhs), RHS(rhs) {}

  Opcode getOpcode() const { return Op; }
  Expr *getLHS() const { return LHS; }
  Expr *getRHS() const { return RHS; }

  const char *getOpcodeStr() const {
    switch (Op) {
    case Add: return "+";
    case Sub: return "-";
    case Mul: return "*";
    case Div: return "/";
    case Mod: return "%";
    case Lt:  return "<";
    case Gt:  return ">";
    case Le:  return "<=";
    case Ge:  return ">=";
    case Eq:  return "==";
    case Ne:  return "!=";
    }
    return "?";
  }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_BinaryExpr;
  }
};

// Unary expression
class UnaryExpr : public Expr {
public:
  enum Opcode {
    Neg, // -
    Not, // !
  };

private:
  Opcode Op;
  Expr *Operand;

public:
  UnaryExpr(SourceLocation loc, Opcode op, Expr *operand)
      : Expr(NK_UnaryExpr, loc), Op(op), Operand(operand) {}

  Opcode getOpcode() const { return Op; }
  Expr *getOperand() const { return Operand; }

  const char *getOpcodeStr() const {
    switch (Op) {
    case Neg: return "-";
    case Not: return "!";
    }
    return "?";
  }

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

// Builtin type (int, void, etc.)
class BuiltinType : public Type {
public:
  enum Kind {
    Int,
    Float,
    Str,
    Bool,
    Void,
  };

private:
  Kind TypeKind;

public:
  BuiltinType(SourceLocation loc, Kind kind) : Type(NK_BuiltinType, loc), TypeKind(kind) {}

  Kind getTypeKind() const { return TypeKind; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_BuiltinType;
  }
};

// Translation unit (top-level container)
class TranslationUnit : public ASTNode {
private:
  CompoundStmt *Body;

public:
  TranslationUnit(SourceLocation loc, CompoundStmt *body)
      : ASTNode(NK_TranslationUnit, loc), Body(body) {}

  CompoundStmt *getBody() const { return Body; }

  static bool classof(const ASTNode *N) {
    return N->getKind() == NK_TranslationUnit;
  }
};

// Arena-style memory pool that owns every AST node.
// All nodes are destroyed when the ASTContext goes out of scope.
class ASTContext {
  std::vector<std::unique_ptr<ASTNode>> Pool;

public:
  /// Create an AST node of type T, store it in the pool, return a raw pointer.
  template <ASTNodeType T, typename... Args>
  T *make(Args &&...args) {
    auto node = std::make_unique<T>(std::forward<Args>(args)...);
    T *ptr = node.get();
    Pool.push_back(std::move(node));
    return ptr;
  }
};

} // namespace ast
} // namespace paykan