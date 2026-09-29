// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// CRTP-based AST visitor for the Paykan language

#pragma once

#include "AST.h"
#include <cassert>
#include <type_traits>

namespace paykan {
namespace ast {

// -- X-macro node table ------------------------------------------------------
//
// Each entry: NODE(Kind, VisitName, CastTo)
// where:
//   Kind      -- the ASTNode::NK_xxx enumerator
//   VisitName -- the visitXxx method name suffix (and the class name)
//   CastTo    -- the type to cast to in the dispatch
//
// Grouped by category so we can selectively expand subsets.

#define PAYKAN_DECL_NODES(NODE)                                                \
  NODE(NK_VarDecl, VarDecl, VarDecl)                                           \
  NODE(NK_FuncDecl, FuncDecl, FuncDecl)                                        \
  NODE(NK_MethodDecl, MethodDecl, MethodDecl)                                  \
  NODE(NK_ImportDecl, ImportDecl, ImportDecl)                                  \
  NODE(NK_ClassDecl, ClassDecl, ClassDecl)                                     \
  NODE(NK_EnumDecl, EnumDecl, EnumDecl)

#define PAYKAN_STMT_NODES(NODE)                                                \
  NODE(NK_CompoundStmt, CompoundStmt, CompoundStmt)                            \
  NODE(NK_ReturnStmt, ReturnStmt, ReturnStmt)                                  \
  NODE(NK_AssignStmt, AssignStmt, AssignStmt)                                  \
  NODE(NK_DeclStmt, DeclStmt, DeclStmt)                                        \
  NODE(NK_ExprStmt, ExprStmt, ExprStmt)                                        \
  NODE(NK_IfStmt, IfStmt, IfStmt)                                              \
  NODE(NK_WhileStmt, WhileStmt, WhileStmt)                                     \
  NODE(NK_BreakStmt, BreakStmt, BreakStmt)                                     \
  NODE(NK_ContinueStmt, ContinueStmt, ContinueStmt)                            \
  NODE(NK_MemberAssignStmt, MemberAssignStmt, MemberAssignStmt)                \
  NODE(NK_MatchStmt, MatchStmt, MatchStmt)                                     \
  NODE(NK_SubscriptAssignStmt, SubscriptAssignStmt, SubscriptAssignStmt)

#define PAYKAN_EXPR_NODES(NODE)                                                \
  NODE(NK_IntegerLiteral, IntegerLiteral, IntegerLiteral)                      \
  NODE(NK_FloatLiteral, FloatLiteral, FloatLiteral)                            \
  NODE(NK_BoolLiteral, BoolLiteral, BoolLiteral)                               \
  NODE(NK_CharLiteral, CharLiteral, CharLiteral)                               \
  NODE(NK_NoneLiteral, NoneLiteral, NoneLiteral)                               \
  NODE(NK_StringLiteral, StringLiteral, StringLiteral)                         \
  NODE(NK_UnaryExpr, UnaryExpr, UnaryExpr)                                     \
  NODE(NK_BinaryExpr, BinaryExpr, BinaryExpr)                                  \
  NODE(NK_Identifier, Identifier, Identifier)                                  \
  NODE(NK_CallExpr, CallExpr, CallExpr)                                        \
  NODE(NK_MethodCallExpr, MethodCallExpr, MethodCallExpr)                      \
  NODE(NK_TernaryExpr, TernaryExpr, TernaryExpr)                               \
  NODE(NK_MemberAccessExpr, MemberAccessExpr, MemberAccessExpr)                \
  NODE(NK_ArrayLiteralExpr, ArrayLiteralExpr, ArrayLiteralExpr)                \
  NODE(NK_SubscriptExpr, SubscriptExpr, SubscriptExpr)                         \
  NODE(NK_EnumValueExpr, EnumValueExpr, EnumValueExpr)                         \
  NODE(NK_MovExpr, MovExpr, MovExpr)

#define PAYKAN_TYPE_NODES(NODE)                                                \
  NODE(NK_BuiltinType, BuiltinType, BuiltinType)                               \
  NODE(NK_ClassType, ClassType, ClassType)                                     \
  NODE(NK_ArrayType, ArrayType, ArrayType)                                     \
  NODE(NK_OptionalType, OptionalType, OptionalType)                            \
  NODE(NK_EnumType, EnumType, EnumType)

#define PAYKAN_TOPLEVEL_NODES(NODE)                                            \
  NODE(NK_TranslationUnit, TranslationUnit, TranslationUnit)

#define PAYKAN_MATCHARM_NODES(NODE) NODE(NK_MatchArm, MatchArm, MatchArm)

// All non-expression nodes.
#define PAYKAN_NON_EXPR_NODES(NODE)                                            \
  PAYKAN_DECL_NODES(NODE)                                                      \
  PAYKAN_STMT_NODES(NODE)                                                      \
  PAYKAN_TYPE_NODES(NODE)                                                      \
  PAYKAN_MATCHARM_NODES(NODE)                                                  \
  PAYKAN_TOPLEVEL_NODES(NODE)

// Every node kind.
#define PAYKAN_ALL_NODES(NODE)                                                 \
  PAYKAN_NON_EXPR_NODES(NODE)                                                  \
  PAYKAN_EXPR_NODES(NODE)

// -- CRTP AST Visitor --------------------------------------------------------
//
// Derive from ASTVisitor<YourClass, ReturnType> and override any visitXxx()
// method you care about.  Unhandled nodes return RetTy{} by default (or
// return void when RetTy = void).
//
template <typename Derived, typename RetTy = void> class ASTVisitor {
  static RetTy defaultResult() {
    if constexpr (!std::is_void_v<RetTy>)
      return RetTy{};
  }

public:
  RetTy visit(ASTNode *node) {
    switch (node->getKind()) {
#define DISPATCH(Kind, Name, Cast)                                             \
  case ASTNode::Kind:                                                          \
    return derived().visit##Name(cast<Cast>(node));
      PAYKAN_ALL_NODES(DISPATCH)
#undef DISPATCH
    }
    assert(false && "Unknown NodeKind");
    return defaultResult();
  }

  // Default implementations -- override in Derived as needed.
#define DEFAULT_VISIT(Kind, Name, Cast)                                        \
  RetTy visit##Name(Cast *) { return defaultResult(); }
  PAYKAN_ALL_NODES(DEFAULT_VISIT)
#undef DEFAULT_VISIT

protected:
  Derived &derived() { return static_cast<Derived &>(*this); }
};

// -- CRTP Expression Visitor -------------------------------------------------
//
// Inherits visitXxx defaults from ASTVisitor but provides its own visit()
// that only dispatches expression nodes.  The entry point accepts Expr*
// for compile-time safety.
//
template <typename Derived, typename RetTy = void>
class ExprVisitor : public ASTVisitor<Derived, RetTy> {
  static RetTy defaultResult() {
    if constexpr (!std::is_void_v<RetTy>)
      return RetTy{};
  }

public:
  // Type-safe entry point: accepts only Expr*.
  RetTy visit(Expr *node) {
    switch (node->getKind()) {
#define DISPATCH(Kind, Name, Cast)                                             \
  case ASTNode::Kind:                                                          \
    return this->derived().visit##Name(cast<Cast>(node));
      PAYKAN_EXPR_NODES(DISPATCH)
#undef DISPATCH
    default:
      assert(false && "ExprVisitor: unexpected node kind");
      return defaultResult();
    }
  }

  // Non-expression visitors are deleted -- calling them is a compile error.
#define DELETE_VISIT(Kind, Name, Cast) RetTy visit##Name(Cast *) = delete;
  PAYKAN_NON_EXPR_NODES(DELETE_VISIT)
#undef DELETE_VISIT
};

} // namespace ast
} // namespace paykan
