// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "ASTPrinter.h"
#include "ASTContext.h"

#include <cstdio>
#include <ostream>

namespace paykan {
namespace ast {

// -- Private helpers

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
    OS << " <" << loc.getLineStart() << ":" << loc.getColumnStart() << "-"
       << loc.getLineEnd() << ":" << loc.getColumnEnd() << ">";
}

void ASTPrinter::header(const char *kind, const ASTNode *node) {
  printIndent();
  OS << kind;
  printLoc(node);
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

// -- Constructor

ASTPrinter::ASTPrinter(std::ostream &os) : OS(os) {}

// -- Top-level

void ASTPrinter::visitTranslationUnit(TranslationUnit *node) {
  header("TranslationUnit", node);
  OS << "\n";
  size_t total =
      node->getImports().size() + node->getEnumDecls().size() +
      node->getGenericStructDecls().size() + node->getStructDecls().size() +
      node->getClassDecls().size() + node->getFuncDecls().size() +
      node->getGenericClassDecls().size() + node->getGenericFuncDecls().size() +
      node->getTraitDecls().size() + node->getImplDecls().size();
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
  for (auto *sd : node->getGenericStructDecls()) {
    ++idx;
    ChildScope cs(*this, idx == total);
    visitStructDecl(sd);
  }
  for (auto *sd : node->getStructDecls()) {
    ++idx;
    ChildScope cs(*this, idx == total);
    visitStructDecl(sd);
  }
  for (auto *cls : node->getGenericClassDecls()) {
    ++idx;
    ChildScope cs(*this, idx == total);
    visitClassDecl(cls);
  }
  for (auto *cls : node->getClassDecls()) {
    ++idx;
    ChildScope cs(*this, idx == total);
    visitClassDecl(cls);
  }
  for (auto *fn : node->getGenericFuncDecls()) {
    ++idx;
    ChildScope cs(*this, idx == total);
    visitFuncDecl(fn);
  }
  for (size_t i = 0; i < node->getFuncDecls().size(); ++i) {
    ++idx;
    ChildScope cs(*this, idx == total);
    visitFuncDecl(node->getFuncDecls()[i]);
  }
  for (auto *tr : node->getTraitDecls()) {
    ++idx;
    ChildScope cs(*this, idx == total);
    visitTraitDecl(tr);
  }
  for (auto *im : node->getImplDecls()) {
    ++idx;
    ChildScope cs(*this, idx == total);
    visitImplDecl(im);
  }
}

// Render `<T: Show + Eq, U>` for a generic declaration's type parameters.
static void printTypeParams(std::ostream &os,
                            const std::vector<const std::string *> &params,
                            const TypeParamBounds &bounds = {}) {
  if (params.empty())
    return;
  os << " <";
  for (size_t i = 0; i < params.size(); ++i) {
    os << (i ? ", " : "") << *params[i];
    if (i < bounds.size())
      for (size_t j = 0; j < bounds[i].size(); ++j)
        os << (j ? " + " : ": ") << typeName(bounds[i][j]);
  }
  os << ">";
}

// -- Statements

void ASTPrinter::visitCompoundStmt(CompoundStmt *node) {
  header("CompoundStmt", node);
  OS << "\n";
  visitChildren(node);
}

void ASTPrinter::visitReturnStmt(ReturnStmt *node) {
  header("ReturnStmt", node);
  OS << "\n";
  if (node->getReturnValue()) {
    ChildScope cs(*this, true);
    visit(node->getReturnValue());
  }
}

void ASTPrinter::visitAssignStmt(AssignStmt *node) {
  header("AssignStmt", node);
  OS << " '" << node->getVarName() << "'\n";
  {
    ChildScope cs(*this, true);
    visit(node->getValue());
  }
}

void ASTPrinter::visitDeclStmt(DeclStmt *node) {
  header("DeclStmt", node);
  OS << "\n";
  {
    ChildScope cs(*this, true);
    visit(node->getDecl());
  }
}

void ASTPrinter::visitExprStmt(ExprStmt *node) {
  header("ExprStmt", node);
  OS << "\n";
  {
    ChildScope cs(*this, true);
    visit(node->getExpr());
  }
}

void ASTPrinter::visitIfStmt(IfStmt *node) {
  header("IfStmt", node);
  OS << "\n";
  {
    ChildScope cs(*this, !node->hasElse());
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
  header("WhileStmt", node);
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
  header("BreakStmt", node);
  OS << "\n";
}

void ASTPrinter::visitContinueStmt(ContinueStmt *node) {
  header("ContinueStmt", node);
  OS << "\n";
}

// -- Declarations

void ASTPrinter::visitVarDecl(VarDecl *node) {
  header("VarDecl", node);
  OS << " '" << node->getName() << "'";
  if (node->isLet())
    OS << " let";
  if (node->getMode() != ParamMode::Value)
    OS << " " << paramModeName(node->getMode());
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

// -- Expressions

void ASTPrinter::visitIntegerLiteral(IntegerLiteral *node) {
  header("IntegerLiteral", node);
  OS << " " << node->getValue() << "\n";
}

void ASTPrinter::visitFloatLiteral(FloatLiteral *node) {
  header("FloatLiteral", node);
  // Exponent form with six fractional digits ("3.140000e+00"): the format
  // the dump has always used, kept stable for the differential frontend check.
  char buf[64];
  std::snprintf(buf, sizeof buf, "%e", node->getValue());
  OS << " " << buf << "\n";
}

void ASTPrinter::visitBoolLiteral(BoolLiteral *node) {
  header("BoolLiteral", node);
  OS << " " << (node->getValue() ? "true" : "false") << "\n";
}

void ASTPrinter::visitCharLiteral(CharLiteral *node) {
  header("CharLiteral", node);
  char c = node->getValue();
  if (c == '\n')
    OS << " '\\n'\n";
  else if (c == '\t')
    OS << " '\\t'\n";
  else if (c == '\r')
    OS << " '\\r'\n";
  else if (c == '\\')
    OS << " '\\\\'\n";
  else if (c == '\'')
    OS << " '\\''\n";
  else
    OS << " '" << c << "'\n";
}

void ASTPrinter::visitNoneLiteral(NoneLiteral *node) {
  header("NoneLiteral", node);
  OS << " None\n";
}

void ASTPrinter::visitStringLiteral(StringLiteral *node) {
  header("StringLiteral", node);
  OS << " \"" << node->getValue() << "\"\n";
}

void ASTPrinter::visitUnaryExpr(UnaryExpr *node) {
  header("UnaryExpr", node);
  OS << " '" << node->getOpcodeStr() << "'\n";
  {
    ChildScope cs(*this, true);
    visit(node->getOperand());
  }
}

void ASTPrinter::visitBinaryExpr(BinaryExpr *node) {
  header("BinaryExpr", node);
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
  header("TernaryExpr", node);
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
  header("Identifier", node);
  OS << " '" << node->getName() << "'\n";
}

void ASTPrinter::visitCallExpr(CallExpr *node) {
  header("CallExpr", node);
  OS << " '" << node->getCalleeName() << "'";
  if (node->hasTypeArgs())
    OS << " " << node->getTypeArgs().size() << " type args";
  OS << "\n";
  // Explicit type arguments are printed first, then the value arguments.
  const auto &targs = node->getTypeArgs();
  const auto &args = node->getArguments();
  for (size_t i = 0; i < targs.size(); ++i) {
    ChildScope cs(*this, args.empty() && i + 1 == targs.size());
    visit(targs[i]);
  }
  visitChildren(node);
}

void ASTPrinter::visitMethodCallExpr(MethodCallExpr *node) {
  header("MethodCallExpr", node);
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

// -- Types

void ASTPrinter::visitBuiltinType(BuiltinType *node) {
  header("BuiltinType", node);
  switch (node->getTypeKind()) {
  case BuiltinType::Int:
    OS << " 'int'";
    break;
  case BuiltinType::Float:
    OS << " 'float'";
    break;
  case BuiltinType::Bool:
    OS << " 'bool'";
    break;
  case BuiltinType::Char:
    OS << " 'char'";
    break;
  case BuiltinType::Void:
    OS << " 'void'";
    break;
  }
  OS << "\n";
}

void ASTPrinter::visitClassType(ClassType *node) {
  header("ClassType", node);
  OS << " '" << node->getName() << "'";
  if (node->getSuperClass())
    OS << " : '" << node->getSuperClass()->getName() << "'";
  OS << "\n";
}

void ASTPrinter::visitArrayType(ArrayType *node) {
  header("ArrayType", node);
  OS << "\n";
  ChildScope cs(*this, true);
  visit(node->getElementType());
}

void ASTPrinter::visitFixedArrayType(FixedArrayType *node) {
  header("FixedArrayType", node);
  if (node->isInferred())
    OS << " _\n";
  else
    OS << " " << node->getLength() << "\n";
  ChildScope cs(*this, true);
  visit(node->getElementType());
}

void ASTPrinter::visitGenericType(GenericType *node) {
  header("GenericType", node);
  OS << " '" << node->getName() << "'\n";
  for (size_t i = 0; i < node->getNumArgs(); ++i) {
    ChildScope cs(*this, i + 1 == node->getNumArgs());
    visit(node->getArgs()[i]);
  }
}

void ASTPrinter::visitOptionalType(OptionalType *node) {
  header("OptionalType", node);
  OS << "\n";
  ChildScope cs(*this, true);
  visit(node->getInnerType());
}

void ASTPrinter::visitTupleType(TupleType *node) {
  header("TupleType", node);
  OS << " " << node->getArity() << " elements\n";
  for (size_t i = 0; i < node->getArity(); ++i) {
    ChildScope cs(*this, i + 1 == node->getArity());
    visit(node->getElementType(i));
  }
}

void ASTPrinter::visitFunctionType(FunctionType *node) {
  // "FunctionType 2 params view 1 ->": the modes of the parameters that have
  // one (by index), then the parameter types and the result type as children.
  header("FunctionType", node);
  OS << " " << node->getNumParams() << " params";
  for (size_t i = 0; i < node->getNumParams(); ++i)
    if (node->getMode(i) != ParamMode::Value)
      OS << ' ' << paramModeName(node->getMode(i)) << ' ' << i;
  OS << " ->\n";
  for (size_t i = 0; i < node->getNumParams(); ++i) {
    ChildScope cs(*this, false);
    visit(node->getParamType(i));
  }
  ChildScope cs(*this, true);
  visit(node->getReturnType());
}

void ASTPrinter::visitStructType(StructType *node) {
  header("StructType", node);
  OS << " '" << node->getName() << "'\n";
}

void ASTPrinter::visitSliceType(SliceType *node) {
  header("SliceType", node);
  OS << "\n";
  ChildScope cs(*this, true);
  visit(node->getElementType());
}

void ASTPrinter::visitPoisonType(PoisonType *node) {
  header("PoisonType", node);
  OS << "\n";
}

void ASTPrinter::visitCallValueExpr(CallValueExpr *node) {
  header("CallValueExpr", node);
  OS << " " << node->getNumArguments() << " args\n";
  {
    ChildScope cs(*this, node->getNumArguments() == 0);
    visit(node->getCallee());
  }
  for (size_t i = 0; i < node->getNumArguments(); ++i) {
    ChildScope cs(*this, i + 1 == node->getNumArguments());
    visit(node->getArguments()[i]);
  }
}

void ASTPrinter::visitLambdaExpr(LambdaExpr *node) {
  // Like a FuncDecl without a name: the modes of the parameters that have
  // one, `->` when it has a result type, then the result type and the body.
  header("LambdaExpr", node);
  for (const Param &p : node->getParams())
    if (p.Mode != ParamMode::Value)
      OS << ' ' << paramModeName(p.Mode) << " '" << p.getName() << "'";
  if (node->getReturnType())
    OS << " ->";
  OS << "\n";
  if (node->getReturnType()) {
    ChildScope cs(*this, false);
    visit(node->getReturnType());
  }
  ChildScope cs(*this, true);
  visit(node->getBody());
}

void ASTPrinter::visitTupleLiteralExpr(TupleLiteralExpr *node) {
  header("TupleLiteralExpr", node);
  OS << " " << node->getNumElements() << " elements\n";
  for (size_t i = 0; i < node->getNumElements(); ++i) {
    ChildScope cs(*this, i + 1 == node->getNumElements());
    visit(node->getElements()[i]);
  }
}

void ASTPrinter::visitTupleIndexExpr(TupleIndexExpr *node) {
  header("TupleIndexExpr", node);
  OS << " ." << node->getIndex() << "\n";
  ChildScope cs(*this, true);
  visit(node->getTuple());
}

void ASTPrinter::visitDestructureStmt(DestructureStmt *node) {
  header("DestructureStmt", node);
  OS << " targets={";
  bool first = true;
  for (const auto &t : node->getTargets()) {
    if (!first)
      OS << ", ";
    OS << (t.isSkip() ? "_" : t.getName());
    if (t.DeclType)
      OS << ": type";
    first = false;
  }
  OS << "}\n";
  // Typed targets print their annotation as children, then the value.
  std::vector<Type *> declTypes;
  for (const auto &t : node->getTargets())
    if (t.DeclType)
      declTypes.push_back(t.DeclType);
  for (size_t i = 0; i < declTypes.size(); ++i) {
    ChildScope cs(*this, false);
    visit(declTypes[i]);
  }
  {
    ChildScope cs(*this, true);
    visit(node->getValue());
  }
}

void ASTPrinter::visitArrayLiteralExpr(ArrayLiteralExpr *node) {
  header("ArrayLiteralExpr", node);
  if (node->isRepeat())
    OS << " " << node->getRepeatCount() << " copies\n";
  else
    OS << " " << node->getNumElements() << " elements\n";
  for (size_t i = 0; i < node->getNumElements(); ++i) {
    ChildScope cs(*this, i + 1 == node->getNumElements());
    visit(node->getElements()[i]);
  }
}

void ASTPrinter::visitSubscriptAssignStmt(SubscriptAssignStmt *node) {
  header("SubscriptAssignStmt", node);
  OS << "\n";
  {
    ChildScope cs(*this, false);
    visit(node->getArray());
  }
  {
    ChildScope cs(*this, false);
    visit(node->getIndex());
  }
  {
    ChildScope cs(*this, true);
    visit(node->getValue());
  }
}

void ASTPrinter::visitSubscriptExpr(SubscriptExpr *node) {
  header("SubscriptExpr", node);
  OS << "\n";
  {
    ChildScope cs(*this, false);
    visit(node->getArray());
  }
  {
    ChildScope cs(*this, true);
    visit(node->getIndex());
  }
}

void ASTPrinter::visitFuncDecl(FuncDecl *node) {
  header("FuncDecl", node);
  if (node->isView())
    OS << " view"; // `view fn`, as written
  if (node->isStatic())
    OS << " static";
  OS << " '" << node->getName() << "'";
  printTypeParams(OS, node->getTypeParams(), node->getTypeParamBounds());
  if (node->isNative())
    OS << " native '" << node->getNativeSymbol() << "'";
  // Parameters are not printed, except the mode of one that has it.
  for (const Param &p : node->getParams())
    if (p.Mode != ParamMode::Value)
      OS << ' ' << paramModeName(p.Mode) << " '" << p.getName() << "'";
  if (node->getReturnType())
    OS << " ->";
  if (node->getResultMode() != ParamMode::Value)
    OS << ' ' << paramModeName(node->getResultMode());
  OS << "\n";
  if (node->getReturnType()) {
    ChildScope cs(*this, !node->getBody());
    visit(node->getReturnType());
  }
  if (node->getBody()) {
    ChildScope cs(*this, true);
    visit(node->getBody());
  }
}

void ASTPrinter::visitMethodDecl(MethodDecl *node) {
  header("MethodDecl", node);
  OS << " '" << node->getName() << "'";
  if (node->isPrivate())
    OS << " private";
  if (node->isView())
    OS << " view";
  if (node->isImpl())
    OS << " impl";
  OS << "\n";
}

void ASTPrinter::visitClassDecl(ClassDecl *node) {
  header("ClassDecl", node);
  OS << " '" << node->getName() << "'";
  printTypeParams(OS, node->getTypeParams(), node->getTypeParamBounds());
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

void ASTPrinter::visitStructDecl(StructDecl *node) {
  header("StructDecl", node);
  OS << " '" << node->getName() << "'";
  printTypeParams(OS, node->getTypeParams(), node->getTypeParamBounds());
  OS << "\n";
  size_t total = node->getNumFields() + node->getMethods().size();
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

void ASTPrinter::visitImplDecl(ImplDecl *node) {
  header("ImplDecl", node);
  if (node->getTrait())
    OS << " '" << typeName(node->getTrait()) << "' for";
  OS << " '" << node->getTypeName() << "'";
  printTypeParams(OS, node->getTypeParams());
  OS << "\n";
  const auto &methods = node->getMethods();
  for (size_t i = 0; i < methods.size(); ++i) {
    ChildScope cs(*this, i + 1 == methods.size());
    visitFuncDecl(methods[i]);
  }
}

void ASTPrinter::visitTraitDecl(TraitDecl *node) {
  header("TraitDecl", node);
  OS << " '" << node->getName() << "'";
  printTypeParams(OS, node->getTypeParams());
  OS << "\n";
  const auto &methods = node->getMethods();
  for (size_t i = 0; i < methods.size(); ++i) {
    ChildScope cs(*this, i + 1 == methods.size());
    visitFuncDecl(methods[i]);
  }
}

void ASTPrinter::visitEnumDecl(EnumDecl *node) {
  header("EnumDecl", node);
  OS << " '" << node->getName() << "' {";
  bool first = true;
  for (const auto *v : node->getVariants()) {
    if (!first)
      OS << ", ";
    OS << *v;
    first = false;
  }
  OS << "}\n";
}

void ASTPrinter::visitEnumType(EnumType *node) {
  header("EnumType", node);
  OS << " '" << node->getName() << "'\n";
}

void ASTPrinter::visitEnumValueExpr(EnumValueExpr *node) {
  header("EnumValueExpr", node);
  OS << " '" << node->getEnumName() << "::" << node->getVariantName() << "'\n";
}

void ASTPrinter::visitMemberAssignStmt(MemberAssignStmt *node) {
  printIndent();
  OS << "MemberAssignStmt '" << node->getFieldName() << "'";
  printLoc(node);
  OS << "\n";
  {
    ChildScope cs(*this, false);
    visit(node->getReceiver());
  }
  {
    ChildScope cs(*this, true);
    visit(node->getValue());
  }
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
  header("ImportDecl", node);
  OS << (node->isSystem() ? " system" : " user");
  if (!node->getBasePath().empty())
    OS << " base='" << node->getBasePath() << "'";
  OS << " modules={";
  bool first = true;
  for (const auto &m : node->getModules()) {
    if (!first)
      OS << ", ";
    OS << *m.Name;
    if (!m.Alias->empty())
      OS << " as " << *m.Alias;
    first = false;
  }
  OS << "}\n";
}

void ASTPrinter::visitMatchArm(MatchArm *) {
  // MatchArm nodes are printed inline from visitMatchStmt; never dispatched
  // through the generic visit() path.
}

void ASTPrinter::visitMatchStmt(MatchStmt *node) {
  header("MatchStmt", node);
  OS << "\n";
  {
    ChildScope cs(*this, false);
    visit(node->getSubject());
  }
  const auto &arms = node->getArms();
  for (size_t i = 0; i < arms.size(); ++i) {
    MatchArm *arm = arms[i];
    bool isLast = (i + 1 == arms.size());
    ChildScope cs(*this, isLast);
    printIndent();
    OS << (arm->isWildcard()  ? "MatchArm wildcard"
           : arm->isLiteral() ? "MatchArm literal"
                              : "MatchArm");
    if (arm->hasBinding())
      OS << " binding='" << arm->getBinding() << "'";
    if (arm->getMode() != ParamMode::Value)
      OS << " " << paramModeName(arm->getMode());
    OS << "\n";
    // A non-wildcard arm carries either a literal pattern (value arm) or a
    // matched type (type arm) — never both; a literal arm's ArmType is null.
    if (arm->isLiteral()) {
      ChildScope cs2(*this, false);
      visit(arm->getLiteralPattern());
    } else if (!arm->isWildcard()) {
      ChildScope cs2(*this, false);
      visit(arm->getArmType());
    }
    {
      ChildScope cs2(*this, true);
      visit(arm->getBody());
    }
  }
}

} // namespace ast
} // namespace paykan
