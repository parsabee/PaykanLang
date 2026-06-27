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
  size_t total = node->getImports().size() + node->getEnumDecls().size() +
                 node->getClassDecls().size() + node->getFuncDecls().size();
  size_t idx = 0;
  for (auto *imp : node->getImports()) {
    ++idx;
    ChildScope cs(*this, idx == total);
    visitImportDecl(imp);
  }
  for (auto *en : node->getEnumDecls()) {
    ++idx;
    ChildScope cs(*this, idx == total);
    visitEnumDecl(en);
  }
  for (auto *cls : node->getClassDecls()) {
    ++idx;
    ChildScope cs(*this, idx == total);
    visitClassDecl(cls);
  }
  for (size_t i = 0; i < node->getFuncDecls().size(); ++i) {
    ++idx;
    ChildScope cs(*this, idx == total);
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

void ASTPrinter::visitCharLiteral(CharLiteral *node) {
  printIndent();
  OS << "CharLiteral";
  printLoc(node);
  char c = node->getValue();
  if (c == '\n')      OS << " '\\n'\n";
  else if (c == '\t') OS << " '\\t'\n";
  else if (c == '\r') OS << " '\\r'\n";
  else if (c == '\\') OS << " '\\\\'\n";
  else if (c == '\'') OS << " '\\''\n";
  else                OS << " '" << c << "'\n";
}

void ASTPrinter::visitNoneLiteral(NoneLiteral *node) {
  printIndent();
  OS << "NoneLiteral";
  printLoc(node);
  OS << " None\n";
}

void ASTPrinter::visitStringLiteral(StringLiteral *node) {
  printIndent();
  OS << "StringLiteral";
  printLoc(node);
  OS << " \"" << node->getValue() << "\"\n";
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

void ASTPrinter::visitMethodCallExpr(MethodCallExpr *node) {
  printIndent();
  OS << "MethodCallExpr";
  printLoc(node);
  OS << " '" << node->getMethodName() << "'\n";
  {
    ChildScope cs(*this, node->getNumArguments() == 0);
    visit(node->getReceiver());
  }
  for (size_t i = 0; i < node->getNumArguments(); ++i) {
    ChildScope cs(*this, i + 1 == node->getNumArguments());
    visit(node->getArguments()[i]);
  }
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
  case BuiltinType::Char:  OS << " 'char'";  break;
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

void ASTPrinter::visitArrayType(ArrayType *node) {
  printIndent();
  OS << "ArrayType";
  printLoc(node);
  OS << "\n";
  ChildScope cs(*this, true);
  visit(node->getElementType());
}

void ASTPrinter::visitArrayLiteralExpr(ArrayLiteralExpr *node) {
  printIndent();
  OS << "ArrayLiteralExpr";
  printLoc(node);
  OS << " " << node->getNumElements() << " elements\n";
  for (size_t i = 0; i < node->getNumElements(); ++i) {
    ChildScope cs(*this, i + 1 == node->getNumElements());
    visit(node->getElements()[i]);
  }
}

void ASTPrinter::visitSubscriptAssignStmt(SubscriptAssignStmt *node) {
  printIndent();
  OS << "SubscriptAssignStmt";
  printLoc(node);
  OS << "\n";
  { ChildScope cs(*this, false); visit(node->getArray()); }
  { ChildScope cs(*this, false); visit(node->getIndex()); }
  { ChildScope cs(*this, true);  visit(node->getValue()); }
}

void ASTPrinter::visitSubscriptExpr(SubscriptExpr *node) {
  printIndent();
  OS << "SubscriptExpr";
  printLoc(node);
  OS << "\n";
  { ChildScope cs(*this, false); visit(node->getArray()); }
  { ChildScope cs(*this, true);  visit(node->getIndex()); }
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

void ASTPrinter::visitClassDecl(ClassDecl *node) {
  printIndent();
  OS << "ClassDecl";
  printLoc(node);
  OS << " '" << node->getName() << "'";
  if (node->hasSuperClass())
    OS << " : " << node->getSuperClassName();
  OS << "\n";
  size_t total = node->getNumFields() + node->getNumMethods();
  size_t idx = 0;
  for (auto *field : node->getFields()) {
    ++idx;
    ChildScope cs(*this, idx == total);
    visitVarDecl(field);
  }
  for (auto *method : node->getMethods()) {
    ++idx;
    ChildScope cs(*this, idx == total);
    visitFuncDecl(method);
  }
}

void ASTPrinter::visitEnumDecl(EnumDecl *node) {
  printIndent();
  OS << "EnumDecl";
  printLoc(node);
  OS << " '" << node->getName() << "' {";
  bool first = true;
  for (const auto *v : node->getVariants()) {
    if (!first) OS << ", ";
    OS << *v;
    first = false;
  }
  OS << "}\n";
}

void ASTPrinter::visitEnumType(EnumType *node) {
  printIndent();
  OS << "EnumType";
  printLoc(node);
  OS << " '" << node->getName() << "'\n";
}

void ASTPrinter::visitEnumValueExpr(EnumValueExpr *node) {
  printIndent();
  OS << "EnumValueExpr";
  printLoc(node);
  OS << " '" << node->getEnumName() << "::" << node->getVariantName() << "'\n";
}

void ASTPrinter::visitMemberAssignStmt(MemberAssignStmt *node) {
  printIndent();
  OS << "MemberAssignStmt '" << node->getFieldName() << "'";
  printLoc(node);
  OS << "\n";
  { ChildScope cs(*this, false); visit(node->getReceiver()); }
  { ChildScope cs(*this, true);  visit(node->getValue()); }
}

void ASTPrinter::visitMemberAccessExpr(MemberAccessExpr *node) {
  printIndent();
  OS << "MemberAccessExpr '" << node->getFieldName() << "'";
  printLoc(node);
  OS << "\n";
  ChildScope cs(*this, true);
  visit(node->getReceiver());
}

void ASTPrinter::visitImportDecl(ImportDecl *node) {
  printIndent();
  OS << "ImportDecl";
  printLoc(node);
  OS << (node->isSystem() ? " system" : " user");
  if (!node->getBasePath().empty())
    OS << " base='" << node->getBasePath() << "'";
  OS << " modules={";
  bool first = true;
  for (const auto &m : node->getModules()) {
    if (!first) OS << ", ";
    OS << *m.Name;
    if (!m.Alias->empty()) OS << " as " << *m.Alias;
    first = false;
  }
  OS << "}\n";
}

void ASTPrinter::visitMatchArm(MatchArm *) {
  // MatchArm nodes are printed inline from visitMatchStmt; never dispatched
  // through the generic visit() path.
}

void ASTPrinter::visitMatchStmt(MatchStmt *node) {
  printIndent();
  OS << "MatchStmt";
  printLoc(node);
  OS << "\n";
  { ChildScope cs(*this, false); visit(node->getSubject()); }
  const auto &arms = node->getArms();
  for (size_t i = 0; i < arms.size(); ++i) {
    MatchArm *arm = arms[i];
    bool isLast = (i + 1 == arms.size());
    ChildScope cs(*this, isLast);
    printIndent();
    OS << (arm->isWildcard() ? "MatchArm wildcard" : "MatchArm");
    if (arm->hasBinding())
      OS << " binding='" << arm->getBinding() << "'";
    OS << "\n";
    if (!arm->isWildcard()) {
      { ChildScope cs2(*this, false); visit(arm->getArmType()); }
    }
    { ChildScope cs2(*this, true); visit(arm->getBody()); }
  }
}

} // namespace ast
} // namespace paykan
