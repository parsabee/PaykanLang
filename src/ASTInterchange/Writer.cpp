// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The AST interchange format's writer (paykan/ast/Interchange.h,
// docs/plugins/ast-format.md).  One list per node, each child list on a line
// of its own, indented two spaces per level; atoms stay on their node's line.

#include "paykan/ast/Interchange.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace paykan::ast::interchange {

namespace {

class Writer {
public:
  explicit Writer(std::ostream &os) : OS(os) {}

  bool unit(const TranslationUnit &tu) {
    OS << "(paykan-ast " << kFormatVersion;
    Depth = 1;
    open("unit", &tu);
    // The declaration lists in the order --dump-ast prints them; read()
    // sorts the declarations into the lists by kind again.
    for (auto *d : tu.getImports())
      importDecl(*d);
    for (auto *d : tu.getEnumDecls())
      enumDecl(*d);
    for (auto *d : tu.getGenericClassDecls())
      classDecl(*d);
    for (auto *d : tu.getClassDecls())
      classDecl(*d);
    for (auto *d : tu.getGenericFuncDecls())
      funcDecl(*d);
    for (auto *d : tu.getFuncDecls())
      funcDecl(*d);
    close();
    OS << ")\n";
    return Error.empty();
  }

  std::string Error;

private:
  std::ostream &OS;
  unsigned Depth = 0;

  // -- Output primitives

  /// Start the list `(<tag>[ @loc]` on a new line at the current depth.
  void open(const char *tag, const ASTNode *loc = nullptr) {
    OS << "\n" << std::string(2 * size_t(Depth), ' ') << "(" << tag;
    if (loc)
      location(loc->getLocation());
    ++Depth;
  }
  void openAt(const char *tag, SourceLocation loc) {
    open(tag);
    location(loc);
  }
  void close() {
    --Depth;
    OS << ")";
  }

  void location(SourceLocation l) {
    if (!l.isValid())
      return;
    OS << " @" << l.getLineStart() << ":" << l.getColumnStart() << "-"
       << l.getLineEnd() << ":" << l.getColumnEnd();
  }

  void str(const std::string &s) {
    static const char hex[] = "0123456789abcdef";
    OS << " \"";
    for (unsigned char c : s) {
      switch (c) {
      case '"':
        OS << "\\\"";
        break;
      case '\\':
        OS << "\\\\";
        break;
      case '\n':
        OS << "\\n";
        break;
      case '\t':
        OS << "\\t";
        break;
      case '\r':
        OS << "\\r";
        break;
      default:
        if (c < 0x20 || c >= 0x7f)
          OS << "\\x" << hex[c >> 4] << hex[c & 0xf];
        else
          OS << static_cast<char>(c);
      }
    }
    OS << "\"";
  }
  void sym(const char *s) { OS << " " << s; }
  void none() { OS << " _"; }

  void fail(const std::string &why) {
    if (Error.empty())
      Error = why;
  }

  /// False (with the error) once the output would nest deeper than read()
  /// accepts: such a tree has no interchange form.  It bounds the recursion
  /// here too (the recursive-descent frontend builds an `int[][]...` of any
  /// length, for one).
  bool enter() {
    if (Depth < kMaxDepth)
      return true;
    fail("nesting too deep (more than " + std::to_string(kMaxDepth) +
         " levels)");
    return false;
  }

  // -- Declarations

  void importDecl(const ImportDecl &d) {
    open("import", &d);
    sym(d.isSystem() ? "true" : "false");
    str(d.getBasePath());
    for (const auto &m : d.getModules()) {
      open("module");
      str(*m.Name);
      str(*m.Alias);
      close();
    }
    close();
  }

  void enumDecl(const EnumDecl &d) {
    open("enum", &d);
    str(d.getName());
    for (const std::string *v : d.getVariants())
      str(*v);
    close();
  }

  void typeParams(const std::vector<const std::string *> &params) {
    open("type-params");
    for (const std::string *p : params)
      str(*p);
    close();
  }

  void classDecl(const ClassDecl &d) {
    open("class", &d);
    str(d.getName());
    str(d.getSuperClassName());
    typeParams(d.getTypeParams());
    open("fields");
    for (const VarDecl *f : d.getFields())
      varDecl(*f);
    close();
    open("methods");
    for (const FuncDecl *m : d.getMethods())
      funcDecl(*m);
    close();
    close();
  }

  void funcDecl(const FuncDecl &d) {
    open("fn", &d);
    str(d.getName());
    typeParams(d.getTypeParams());
    open("params");
    for (const Param &p : d.getParams()) {
      open("param");
      str(p.getName());
      type(p.ParamType);
      if (p.Mode != ParamMode::Value) {
        open("qual");
        sym(paramModeName(p.Mode));
        close();
      }
      close();
    }
    close();
    optionalType(d.getReturnType());
    if (d.getBody())
      stmt(d.getBody());
    else
      fail("function '" + d.getName() + "' has no body");
    close();
  }

  void varDecl(const VarDecl &d) {
    open("var", &d);
    str(d.getName());
    optionalType(d.getType());
    optionalExpr(d.getInitExpr());
    close();
  }

  // -- Types

  void optionalType(const Type *t) {
    if (t)
      type(t);
    else
      none();
  }

  void type(const Type *t) {
    if (!enter())
      return;
    if (!t) {
      fail("a type is missing");
      none();
      return;
    }
    if (const auto *bt = dyn_cast<BuiltinType>(t)) {
      static const char *const names[] = {"int", "float", "bool", "char",
                                          "void"};
      open("named-type", bt);
      str(names[bt->getTypeKind()]);
      close();
    } else if (const auto *ct = dyn_cast<ClassType>(t)) {
      open("named-type", ct);
      str(ct->getName());
      close();
    } else if (const auto *at = dyn_cast<ArrayType>(t)) {
      open("array-type", at);
      type(at->getElementType());
      close();
    } else if (const auto *ot = dyn_cast<OptionalType>(t)) {
      open("optional-type", ot);
      type(ot->getInnerType());
      close();
    } else if (const auto *tt = dyn_cast<TupleType>(t)) {
      open("tuple-type", tt);
      for (const Type *e : tt->getElementTypes())
        type(e);
      close();
    } else if (const auto *gt = dyn_cast<GenericType>(t)) {
      open("generic-type", gt);
      str(gt->getName());
      for (const Type *a : gt->getArgs())
        type(a);
      close();
    } else {
      // EnumType and PoisonType exist only after Sema.
      fail("a type no frontend produces (the AST was already checked)");
      none();
    }
  }

  // -- Statements

  void optionalStmt(const Stmt *s) {
    if (s)
      stmt(s);
    else
      none();
  }

  void block(const CompoundStmt *b) {
    open("block", b);
    for (const Stmt *s : b->getStatements())
      stmt(s);
    close();
  }

  void stmt(const Stmt *s) {
    if (!enter())
      return;
    switch (s->getKind()) {
    case ASTNode::NK_CompoundStmt:
      block(cast<CompoundStmt>(s));
      return;
    case ASTNode::NK_ReturnStmt:
      open("return", s);
      optionalExpr(cast<ReturnStmt>(s)->getReturnValue());
      close();
      return;
    case ASTNode::NK_AssignStmt: {
      const auto *a = cast<AssignStmt>(s);
      open("assign", a);
      expr(a->getLHS());
      expr(a->getValue());
      close();
      return;
    }
    case ASTNode::NK_DeclStmt: {
      const auto *d = cast<DeclStmt>(s);
      open("decl", d);
      if (const auto *v = dyn_cast<VarDecl>(d->getDecl()))
        varDecl(*v);
      else
        fail("a declaration statement that declares no variable");
      close();
      return;
    }
    case ASTNode::NK_ExprStmt:
      open("expr", s);
      expr(cast<ExprStmt>(s)->getExpr());
      close();
      return;
    case ASTNode::NK_IfStmt: {
      const auto *i = cast<IfStmt>(s);
      open("if", i);
      expr(i->getCondition());
      stmt(i->getThenBranch());
      optionalStmt(i->getElseBranch());
      close();
      return;
    }
    case ASTNode::NK_WhileStmt: {
      const auto *w = cast<WhileStmt>(s);
      open("while", w);
      expr(w->getCondition());
      stmt(w->getBody());
      close();
      return;
    }
    case ASTNode::NK_BreakStmt:
      open("break", s);
      close();
      return;
    case ASTNode::NK_ContinueStmt:
      open("continue", s);
      close();
      return;
    case ASTNode::NK_MemberAssignStmt: {
      const auto *m = cast<MemberAssignStmt>(s);
      open("member-assign", m);
      expr(m->getReceiver());
      str(m->getFieldName());
      expr(m->getValue());
      close();
      return;
    }
    case ASTNode::NK_SubscriptAssignStmt: {
      const auto *m = cast<SubscriptAssignStmt>(s);
      open("subscript-assign", m);
      expr(m->getArray());
      expr(m->getIndex());
      expr(m->getValue());
      close();
      return;
    }
    case ASTNode::NK_MatchStmt: {
      const auto *m = cast<MatchStmt>(s);
      open("match", m);
      expr(m->getSubject());
      for (const MatchArm *arm : m->getArms()) {
        if (arm->getArmType()) {
          open("type-arm", arm);
          str(arm->getBinding());
          type(arm->getArmType());
        } else if (arm->getLiteralPattern()) {
          open("value-arm", arm);
          str(arm->getBinding());
          expr(arm->getLiteralPattern());
        } else {
          open("wildcard-arm", arm);
          str(arm->getBinding());
        }
        block(arm->getBody());
        close();
      }
      close();
      return;
    }
    case ASTNode::NK_DestructureStmt: {
      const auto *d = cast<DestructureStmt>(s);
      open("destructure", d);
      open("targets");
      for (const auto &t : d->getTargets()) {
        openAt("target", t.Loc);
        str(t.getName());
        optionalType(t.DeclType);
        close();
      }
      close();
      expr(d->getValue());
      close();
      return;
    }
    default:
      fail("an unknown statement");
      none();
    }
  }

  // -- Expressions

  void optionalExpr(const Expr *e) {
    if (e)
      expr(e);
    else
      none();
  }

  void exprs(const std::vector<Expr *> &es) {
    for (const Expr *e : es)
      expr(e);
  }

  static const char *binaryName(BinaryOpcode op) {
    switch (op) {
    case BinaryOpcode::Add:
      return "add";
    case BinaryOpcode::Sub:
      return "sub";
    case BinaryOpcode::Mul:
      return "mul";
    case BinaryOpcode::Div:
      return "div";
    case BinaryOpcode::Mod:
      return "mod";
    case BinaryOpcode::Lt:
      return "lt";
    case BinaryOpcode::Gt:
      return "gt";
    case BinaryOpcode::Le:
      return "le";
    case BinaryOpcode::Ge:
      return "ge";
    case BinaryOpcode::Eq:
      return "eq";
    case BinaryOpcode::Ne:
      return "ne";
    case BinaryOpcode::And:
      return "and";
    case BinaryOpcode::Or:
    case BinaryOpcode::Count:
      break;
    }
    return "or";
  }

  void floating(double v) {
    if (std::isnan(v)) {
      sym("nan");
      return;
    }
    if (std::isinf(v)) {
      sym(v < 0 ? "-inf" : "inf");
      return;
    }
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    std::string text = buf;
    if (text.find_first_of(".e") == std::string::npos)
      text += ".0";
    OS << " " << text;
  }

  void expr(const Expr *e) {
    if (!enter())
      return;
    if (!e) {
      fail("an expression is missing");
      none();
      return;
    }
    switch (e->getKind()) {
    case ASTNode::NK_IntegerLiteral:
      open("int", e);
      OS << " " << cast<IntegerLiteral>(e)->getValue();
      close();
      return;
    case ASTNode::NK_FloatLiteral:
      open("float", e);
      floating(cast<FloatLiteral>(e)->getValue());
      close();
      return;
    case ASTNode::NK_BoolLiteral:
      open("bool", e);
      sym(cast<BoolLiteral>(e)->getValue() ? "true" : "false");
      close();
      return;
    case ASTNode::NK_CharLiteral:
      open("char", e);
      OS << " "
         << static_cast<unsigned>(
                static_cast<unsigned char>(cast<CharLiteral>(e)->getValue()));
      close();
      return;
    case ASTNode::NK_NoneLiteral:
      open("none", e);
      close();
      return;
    case ASTNode::NK_StringLiteral:
      open("string", e);
      str(cast<StringLiteral>(e)->getValue());
      close();
      return;
    case ASTNode::NK_UnaryExpr: {
      const auto *u = cast<UnaryExpr>(e);
      open("unary", u);
      sym(u->getOpcode() == UnaryOpcode::Neg ? "neg" : "not");
      expr(u->getOperand());
      close();
      return;
    }
    case ASTNode::NK_BinaryExpr: {
      const auto *b = cast<BinaryExpr>(e);
      open("binary", b);
      sym(binaryName(b->getOpcode()));
      expr(b->getLHS());
      expr(b->getRHS());
      close();
      return;
    }
    case ASTNode::NK_Identifier:
      open("ident", e);
      str(cast<Identifier>(e)->getName());
      close();
      return;
    case ASTNode::NK_CallExpr: {
      const auto *c = cast<CallExpr>(e);
      open("call", c);
      str(c->getCalleeName());
      open("type-args");
      for (const Type *t : c->getTypeArgs())
        type(t);
      close();
      exprs(c->getArguments());
      close();
      return;
    }
    case ASTNode::NK_MethodCallExpr: {
      const auto *m = cast<MethodCallExpr>(e);
      open("method-call", m);
      expr(m->getReceiver());
      str(m->getMethodName());
      exprs(m->getArguments());
      close();
      return;
    }
    case ASTNode::NK_TernaryExpr: {
      const auto *t = cast<TernaryExpr>(e);
      open("ternary", t);
      expr(t->getCondition());
      expr(t->getTrueExpr());
      expr(t->getFalseExpr());
      close();
      return;
    }
    case ASTNode::NK_MemberAccessExpr: {
      const auto *m = cast<MemberAccessExpr>(e);
      open("member", m);
      expr(m->getReceiver());
      str(m->getFieldName());
      close();
      return;
    }
    case ASTNode::NK_ArrayLiteralExpr:
      open("array", e);
      exprs(cast<ArrayLiteralExpr>(e)->getElements());
      close();
      return;
    case ASTNode::NK_SubscriptExpr: {
      const auto *s = cast<SubscriptExpr>(e);
      open("subscript", s);
      expr(s->getArray());
      expr(s->getIndex());
      close();
      return;
    }
    case ASTNode::NK_EnumValueExpr: {
      const auto *v = cast<EnumValueExpr>(e);
      open("enum-value", v);
      str(v->getEnumName());
      str(v->getVariantName());
      close();
      return;
    }
    case ASTNode::NK_TupleLiteralExpr:
      open("tuple", e);
      exprs(cast<TupleLiteralExpr>(e)->getElements());
      close();
      return;
    case ASTNode::NK_TupleIndexExpr: {
      const auto *t = cast<TupleIndexExpr>(e);
      open("tuple-index", t);
      expr(t->getTuple());
      OS << " " << t->getIndex();
      close();
      return;
    }
    default:
      fail("an unknown expression");
      none();
    }
  }
};

} // namespace

bool write(const TranslationUnit &unit, std::ostream &os, std::string &error) {
  Writer w(os);
  bool ok = w.unit(unit);
  error = w.Error;
  return ok;
}

} // namespace paykan::ast::interchange
