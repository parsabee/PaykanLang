// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// AST pretty-printer in LLVM-style indented tree format

#pragma once

#include "ASTVisitor.h"

#include <iosfwd>
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
  std::ostream &OS;

  // Characters for drawing the tree.
  static constexpr const char *Pipe = "| ";
  static constexpr const char *Branch = "|-";
  static constexpr const char *Tail = "`-";
  static constexpr const char *Blank = "  ";

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
  void visitChildren(MethodCallExpr *node);

public:
  explicit ASTPrinter(std::ostream &os);

  // -- Visitor overrides (generated from X-macros) -------------------------
  // `Cast` is a type name used in a declarator (`Cast *node`) and cannot be
  // parenthesized, so the macro-parentheses check is suppressed here.
  // NOLINTNEXTLINE(bugprone-macro-parentheses)
#define AST_PRINT(Kind, Name, Cast) void visit##Name(Cast *node);
  PAYKAN_ALL_NODES(AST_PRINT)
#undef AST_PRINT
};

} // namespace ast
} // namespace paykan
