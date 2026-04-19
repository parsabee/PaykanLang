// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "ASTPrinter.h"

namespace paykan {
namespace ast {

// -- Private helpers ---------------------------------------------------------

void ASTPrinter::printIndent() {
  for (unsigned i = 0; i < LastChild.size(); ++i) {
    if (i + 1 == LastChild.size())
      OS << (LastChild[i] ? Tail : Branch);
    else
      OS << (LastChild[i] ? Blank : Pipe);
  }
}

void ASTPrinter::printLoc(const ASTNode *node) {
  auto loc = node->getLocation();
  if (loc.isValid())
    OS << " <" << loc.getLineStart() << ":" << loc.getColumnStart()
       << "-" << loc.getLineEnd() << ":" << loc.getColumnEnd() << ">";
}

ASTPrinter::ChildScope::ChildScope(ASTPrinter &p, bool isLast) : P(p) {
  P.LastChild.push_back(isLast);
}

ASTPrinter::ChildScope::~ChildScope() { P.LastChild.pop_back(); }

void ASTPrinter::visitChildren(CompoundStmt *node) {
  auto &stmts = node->getStatements();
  for (size_t i = 0; i < stmts.size(); ++i) {
    ChildScope cs(*this, i + 1 == stmts.size());
    visit(stmts[i]);
  }
}

void ASTPrinter::visitChildren(CallExpr *node) {
  auto &args = node->getArguments();
  for (size_t i = 0; i < args.size(); ++i) {
    ChildScope cs(*this, i + 1 == args.size());
    visit(args[i]);
  }
}

// -- Constructor -------------------------------------------------------------

ASTPrinter::ASTPrinter(llvm::raw_ostream &os) : OS(os) {}

// -- Top-level ---------------------------------------------------------------

void ASTPrinter::visitTranslationUnit(TranslationUnit *node) {
  printIndent();
  OS << "TranslationUnit";
  printLoc(node);
  OS << "\n";
  for (size_t i = 0; i < node->getFuncDecls().size(); ++i) {
    bool last = (i + 1 == node->getFuncDecls().size());
    ChildScope cs(*this, last);
    visitFuncDecl(node->getFuncDecls()[i]);
  }
}

// -- Statements --------------------------------------------------------------

void ASTPrinter::visitCompoundStmt(CompoundStmt *node) {
  printIndent();
  OS << "CompoundStmt";
  printLoc(node);
  OS << "\n";
  visitChildren(node);
}

void ASTPrinter::visitReturnStmt(ReturnStmt *node) {
  printIndent();
  OS << "ReturnStmt";
  printLoc(node);
  OS << "\n";
  if (node->getReturnValue()) {
    ChildScope cs(*this, true);
    visit(node->getReturnValue());
  }
}

void ASTPrinter::visitAssignStmt(AssignStmt *node) {
  printIndent();
  OS << "AssignStmt";
  printLoc(node);
  OS << " '" << node->getVarName() << "'\n";
  {
    ChildScope cs(*this, true);
    visit(node->getValue());
  }
}

void ASTPrinter::visitDeclStmt(DeclStmt *node) {
  printIndent();
  OS << "DeclStmt";
  printLoc(node);
  OS << "\n";
  {
    ChildScope cs(*this, true);
    visit(node->getDecl());
  }
}

void ASTPrinter::visitExprStmt(ExprStmt *node) {
  printIndent();
  OS << "ExprStmt";
  printLoc(node);
  OS << "\n";
  {
    ChildScope cs(*this, true);
    visit(node->getExpr());
  }
}

void ASTPrinter::visitIfStmt(IfStmt *node) {
  printIndent();
  OS << "IfStmt";
  printLoc(node);
  OS << "\n";
  {
    ChildScope cs(*this, !node->hasElse() && true);
    OS << "  "; // condition label
    visit(node->getCondition());
  }
  {
    ChildScope cs(*this, !node->hasElse());
    visit(node->getThenBranch());
  }
  if (node->hasElse()) {
    ChildScope cs(*this, true);
    visit(node->getElseBranch());
  }
}

void ASTPrinter::visitWhileStmt(WhileStmt *node) {
  printIndent();
  OS << "WhileStmt";
  printLoc(node);
  OS << "\n";
  {
    ChildScope cs(*this, false);
    visit(node->getCondition());
  }
  {
    ChildScope cs(*this, true);
    visit(node->getBody());
  }
}

void ASTPrinter::visitBreakStmt(BreakStmt *node) {
  printIndent();
  OS << "BreakStmt";
  printLoc(node);
  OS << "\n";
}

void ASTPrinter::visitContinueStmt(ContinueStmt *node) {
  printIndent();
  OS << "ContinueStmt";
  printLoc(node);
  OS << "\n";
}

// -- Declarations ------------------------------------------------------------

void ASTPrinter::visitVarDecl(VarDecl *node) {
  printIndent();
  OS << "VarDecl";
  printLoc(node);
  OS << " '" << node->getName() << "'";
  if (node->isConst())
    OS << " const";
  switch (node->getOwnership()) {
  case Ownership::Unique:    OS << " unique"; break;
  case Ownership::Shared:    OS << " shared"; break;
  case Ownership::Reference: OS << " ref";    break;
  }
  if (node->getType())
    OS << " type";
  OS << "\n";
  if (node->getType()) {
    ChildScope cs(*this, node->getInitExpr() == nullptr);
    visit(node->getType());
  }
  if (node->getInitExpr()) {
    ChildScope cs(*this, true);
    visit(node->getInitExpr());
  }
}

// -- Expressions -------------------------------------------------------------

void ASTPrinter::visitIntegerLiteral(IntegerLiteral *node) {
  printIndent();
  OS << "IntegerLiteral";
  printLoc(node);
  OS << " " << node->getValue() << "\n";
}

void ASTPrinter::visitFloatLiteral(FloatLiteral *node) {
  printIndent();
  OS << "FloatLiteral";
  printLoc(node);
  OS << " " << node->getValue() << "\n";
}

void ASTPrinter::visitBoolLiteral(BoolLiteral *node) {
  printIndent();
  OS << "BoolLiteral";
  printLoc(node);
  OS << " " << (node->getValue() ? "true" : "false") << "\n";
}

void ASTPrinter::visitStringLiteral(StringLiteral *node) {
  printIndent();
  OS << "StringLiteral";
  printLoc(node);
  OS << " \"" << node->getValue() << "\"\n";
}

void ASTPrinter::visitMovExpr(MovExpr *node) {
  printIndent();
  OS << "MovExpr";
  printLoc(node);
  OS << "\n";
  {
    ChildScope cs(*this, true);
    visit(node->getOperand());
  }
}

void ASTPrinter::visitRefExpr(RefExpr *node) {
  printIndent();
  OS << "RefExpr";
  printLoc(node);
  OS << "\n";
  {
    ChildScope cs(*this, true);
    visit(node->getOperand());
  }
}



void ASTPrinter::visitUnaryExpr(UnaryExpr *node) {
  printIndent();
  OS << "UnaryExpr";
  printLoc(node);
  OS << " '" << node->getOpcodeStr() << "'\n";
  {
    ChildScope cs(*this, true);
    visit(node->getOperand());
  }
}

void ASTPrinter::visitBinaryExpr(BinaryExpr *node) {
  printIndent();
  OS << "BinaryExpr";
  printLoc(node);
  OS << " '" << node->getOpcodeStr() << "'\n";
  {
    ChildScope cs(*this, false);
    visit(node->getLHS());
  }
  {
    ChildScope cs(*this, true);
    visit(node->getRHS());
  }
}

void ASTPrinter::visitTernaryExpr(TernaryExpr *node) {
  printIndent();
  OS << "TernaryExpr";
  printLoc(node);
  OS << "\n";
  {
    ChildScope cs(*this, false);
    visit(node->getCondition());
  }
  {
    ChildScope cs(*this, false);
    visit(node->getTrueExpr());
  }
  {
    ChildScope cs(*this, true);
    visit(node->getFalseExpr());
  }
}

void ASTPrinter::visitIdentifier(Identifier *node) {
  printIndent();
  OS << "Identifier";
  printLoc(node);
  OS << " '" << node->getName() << "'\n";
}

void ASTPrinter::visitCallExpr(CallExpr *node) {
  printIndent();
  OS << "CallExpr";
  printLoc(node);
  OS << " '" << node->getCalleeName() << "'\n";
  visitChildren(node);
}

// -- Types -------------------------------------------------------------------

void ASTPrinter::visitBuiltinType(BuiltinType *node) {
  printIndent();
  OS << "BuiltinType";
  printLoc(node);
  switch (node->getTypeKind()) {
  case BuiltinType::Int:   OS << " 'int'";   break;
  case BuiltinType::Float: OS << " 'float'"; break;
  case BuiltinType::Bool:  OS << " 'bool'";  break;
  case BuiltinType::Void:  OS << " 'void'";  break;
  }
  OS << "\n";
}

void ASTPrinter::visitClassType(ClassType *node) {
  printIndent();
  OS << "ClassType";
  printLoc(node);
  OS << " '" << node->getName() << "'";
  if (node->getSuperClass())
    OS << " : '" << node->getSuperClass()->getName() << "'";
  OS << "\n";
}

void ASTPrinter::visitFuncDecl(FuncDecl *node) {
  printIndent();
  OS << "FuncDecl";
  printLoc(node);
  OS << " '" << node->getName() << "'";
  if (node->getReturnType())
    OS << " ->";
  OS << "\n";
  // Print params and body as children.
  if (node->getReturnType()) {
    ChildScope cs(*this, false);
    visit(node->getReturnType());
  }
  {
    ChildScope cs(*this, true);
    visit(node->getBody());
  }
}

void ASTPrinter::visitMethodDecl(MethodDecl *node) {
  printIndent();
  OS << "MethodDecl";
  printLoc(node);
  OS << " '" << node->getName() << "'";
  if (node->isStatic())  OS << " static";
  if (node->isPrivate()) OS << " private";
  OS << "\n";
}

} // namespace ast
} // namespace paykan
