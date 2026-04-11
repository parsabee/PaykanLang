// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// AST pretty-printer in LLVM-style indented tree format

#pragma once

#include "ASTVisitor.h"
#include <llvm/Support/raw_ostream.h>
#include <vector>

namespace paykan {
namespace ast {

// Prints the AST as an indented tree, similar to clang -ast-dump.
//
// Example output:
//   TranslationUnit <1:1-1:25>
//   `-CompoundStmt <1:1-1:25>
//     |-DeclStmt <1:1-1:12>
//     | `-VarDecl <1:1-1:12> 'x' 'int'
//     |   `-IntegerLiteral <1:12-1:12> 42
//     `-ExprStmt <1:14-1:25>
//       `-BinaryExpr <1:14-1:25> '+'
//         |-Identifier <1:14-1:14> 'x'
//         `-IntegerLiteral <1:18-1:18> 1
//
class ASTPrinter : public ASTVisitor<ASTPrinter, void> {
  llvm::raw_ostream &OS;

  // Characters for drawing the tree.
  static constexpr const char *Pipe   = "| ";
  static constexpr const char *Branch = "|-";
  static constexpr const char *Tail   = "`-";
  static constexpr const char *Blank  = "  ";

  // Stack of "is last child" flags for each depth level.
  std::vector<bool> LastChild;

  void printIndent();
  void printLoc(const ASTNode *node);

  // RAII helper to manage indentation for children.
  struct ChildScope {
    ASTPrinter &P;
    ChildScope(ASTPrinter &p, bool isLast);
    ~ChildScope();
  };

  void visitChildren(CompoundStmt *node);
  void visitChildren(CallExpr *node);

public:
  explicit ASTPrinter(llvm::raw_ostream &os);

  // ── Top-level ───────────────────────────────────────────────────────────
  void visitTranslationUnit(TranslationUnit *node);

  // ── Statements ──────────────────────────────────────────────────────────
  void visitCompoundStmt(CompoundStmt *node);
  void visitReturnStmt(ReturnStmt *node);
  void visitAssignStmt(AssignStmt *node);
  void visitDeclStmt(DeclStmt *node);
  void visitExprStmt(ExprStmt *node);

  // ── Declarations ────────────────────────────────────────────────────────
  void visitVarDecl(VarDecl *node);
  void visitParamDecl(ParamDecl *node);
  void visitFunctionDecl(Decl *node);

  // ── Expressions ─────────────────────────────────────────────────────────
  void visitIntegerLiteral(IntegerLiteral *node);
  void visitFloatLiteral(FloatLiteral *node);
  void visitBoolLiteral(BoolLiteral *node);
  void visitUnaryExpr(UnaryExpr *node);
  void visitBinaryExpr(BinaryExpr *node);
  void visitIdentifier(Identifier *node);
  void visitCallExpr(CallExpr *node);

  // ── Types ───────────────────────────────────────────────────────────────
  void visitBuiltinType(BuiltinType *node);
};

} // namespace ast
} // namespace paykan
