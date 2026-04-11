// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// CRTP-based AST visitor for the Paykan language

#pragma once

#include "AST.h"
#include <cassert>
#include <type_traits>

namespace paykan {
namespace ast {

// ── CRTP AST Visitor ────────────────────────────────────────────────────────
//
// Derive from ASTVisitor<YourClass, ReturnType> and override any visitXxx()
// method you care about.  Unhandled nodes return RetTy{} by default (or
// return void when RetTy = void).
//
template <typename Derived, typename RetTy = void>
class ASTVisitor {
  static RetTy defaultResult() {
    if constexpr (!std::is_void_v<RetTy>)
      return defaultResult();
  }

public:
  RetTy visit(ASTNode *node) {
    switch (node->getKind()) {
    // Declarations
    case ASTNode::NK_FunctionDecl:
      return derived().visitFunctionDecl(cast<Decl>(node));
    case ASTNode::NK_ParamDecl:
      return derived().visitParamDecl(cast<ParamDecl>(node));
    case ASTNode::NK_VarDecl:
      return derived().visitVarDecl(cast<VarDecl>(node));

    // Statements
    case ASTNode::NK_CompoundStmt:
      return derived().visitCompoundStmt(cast<CompoundStmt>(node));
    case ASTNode::NK_ReturnStmt:
      return derived().visitReturnStmt(cast<ReturnStmt>(node));
    case ASTNode::NK_AssignStmt:
      return derived().visitAssignStmt(cast<AssignStmt>(node));
    case ASTNode::NK_DeclStmt:
      return derived().visitDeclStmt(cast<DeclStmt>(node));
    case ASTNode::NK_ExprStmt:
      return derived().visitExprStmt(cast<ExprStmt>(node));

    // Expressions
    case ASTNode::NK_IntegerLiteral:
      return derived().visitIntegerLiteral(cast<IntegerLiteral>(node));
    case ASTNode::NK_FloatLiteral:
      return derived().visitFloatLiteral(cast<FloatLiteral>(node));
    case ASTNode::NK_BoolLiteral:
      return derived().visitBoolLiteral(cast<BoolLiteral>(node));
    case ASTNode::NK_UnaryExpr:
      return derived().visitUnaryExpr(cast<UnaryExpr>(node));
    case ASTNode::NK_BinaryExpr:
      return derived().visitBinaryExpr(cast<BinaryExpr>(node));
    case ASTNode::NK_Identifier:
      return derived().visitIdentifier(cast<Identifier>(node));
    case ASTNode::NK_CallExpr:
      return derived().visitCallExpr(cast<CallExpr>(node));

    // Types
    case ASTNode::NK_BuiltinType:
      return derived().visitBuiltinType(cast<BuiltinType>(node));

    // Top-level
    case ASTNode::NK_TranslationUnit:
      return derived().visitTranslationUnit(cast<TranslationUnit>(node));
    }
    assert(false && "Unknown NodeKind");
    return defaultResult();
  }

  // Default implementations — override in Derived as needed.
  // Declarations
  RetTy visitFunctionDecl(Decl *) { return defaultResult(); }
  RetTy visitParamDecl(ParamDecl *) { return defaultResult(); }
  RetTy visitVarDecl(VarDecl *) { return defaultResult(); }

  // Statements
  RetTy visitCompoundStmt(CompoundStmt *) { return defaultResult(); }
  RetTy visitReturnStmt(ReturnStmt *) { return defaultResult(); }
  RetTy visitAssignStmt(AssignStmt *) { return defaultResult(); }
  RetTy visitDeclStmt(DeclStmt *) { return defaultResult(); }
  RetTy visitExprStmt(ExprStmt *) { return defaultResult(); }

  // Expressions
  RetTy visitIntegerLiteral(IntegerLiteral *) { return defaultResult(); }
  RetTy visitFloatLiteral(FloatLiteral *) { return defaultResult(); }
  RetTy visitBoolLiteral(BoolLiteral *) { return defaultResult(); }
  RetTy visitUnaryExpr(UnaryExpr *) { return defaultResult(); }
  RetTy visitBinaryExpr(BinaryExpr *) { return defaultResult(); }
  RetTy visitIdentifier(Identifier *) { return defaultResult(); }
  RetTy visitCallExpr(CallExpr *) { return defaultResult(); }

  // Types
  RetTy visitBuiltinType(BuiltinType *) { return defaultResult(); }

  // Top-level
  RetTy visitTranslationUnit(TranslationUnit *) { return defaultResult(); }

protected:
  Derived &derived() { return static_cast<Derived &>(*this); }
};

} // namespace ast
} // namespace paykan
