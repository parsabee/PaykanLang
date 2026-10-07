// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "AST.h"
#include "ASTClone.h"
#include "Names.h"

namespace paykan {
namespace ast {

// -- BinaryExpr

const char *BinaryExpr::getOpcodeStr() const {
  switch (Op) {
  case BinaryOpcode::Add:
    return "+";
  case BinaryOpcode::Sub:
    return "-";
  case BinaryOpcode::Mul:
    return "*";
  case BinaryOpcode::Div:
    return "/";
  case BinaryOpcode::Mod:
    return "%";
  case BinaryOpcode::Lt:
    return "<";
  case BinaryOpcode::Gt:
    return ">";
  case BinaryOpcode::Le:
    return "<=";
  case BinaryOpcode::Ge:
    return ">=";
  case BinaryOpcode::Eq:
    return "==";
  case BinaryOpcode::Ne:
    return "!=";
  case BinaryOpcode::And:
    return "&&";
  case BinaryOpcode::Or:
    return "||";
  case BinaryOpcode::Count:
    break;
  }
  __builtin_unreachable();
}

const char *qualifierName(Qualifier q) {
  switch (q) {
  case Qualifier::Mut:
    return "mut";
  case Qualifier::Own:
    return "own";
  case Qualifier::View:
    break;
  }
  return "view";
}

// -- UnaryExpr

const char *UnaryExpr::getOpcodeStr() const {
  switch (Op) {
  case UnaryOpcode::Neg:
    return "-";
  case UnaryOpcode::Not:
    return "!";
  case UnaryOpcode::Count:
    break;
  }
  __builtin_unreachable();
}

// -- BuiltinType

void BuiltinType::initOps() {
  switch (TypeKind) {
  case Int:
  case Float:
    addUnaryOp(UnaryOpcode::Neg);
    addBinaryOp(BinaryOpcode::Add);
    addBinaryOp(BinaryOpcode::Sub);
    addBinaryOp(BinaryOpcode::Mul);
    addBinaryOp(BinaryOpcode::Div);
    addBinaryOp(BinaryOpcode::Mod);
    addBinaryOp(BinaryOpcode::Lt);
    addBinaryOp(BinaryOpcode::Gt);
    addBinaryOp(BinaryOpcode::Le);
    addBinaryOp(BinaryOpcode::Ge);
    addBinaryOp(BinaryOpcode::Eq);
    addBinaryOp(BinaryOpcode::Ne);
    break;
  case Bool:
    addUnaryOp(UnaryOpcode::Not);
    addBinaryOp(BinaryOpcode::Eq);
    addBinaryOp(BinaryOpcode::Ne);
    addBinaryOp(BinaryOpcode::And);
    addBinaryOp(BinaryOpcode::Or);
    break;
  case Char:
    addBinaryOp(BinaryOpcode::Eq);
    addBinaryOp(BinaryOpcode::Ne);
    addBinaryOp(BinaryOpcode::Lt);
    addBinaryOp(BinaryOpcode::Gt);
    addBinaryOp(BinaryOpcode::Le);
    addBinaryOp(BinaryOpcode::Ge);
    break;
  case Void:
    break;
  }
}

// -- ClassType

void ClassType::addMethod(MethodDecl *m) {
  // __init__ is a static constructor helper — it has no vtable slot.
  if (m->getName() == names::kMethodInit) {
    InitMethod = m;
    return;
  }
  // Check if this overrides an existing slot.
  auto it = VTableIndex.find(m->getName());
  if (it != VTableIndex.end()) {
    VTable[it->second] = m; // override, slot index unchanged
    return;
  }
  // New slot.
  VTableIndex[m->getName()] = static_cast<int>(VTable.size());
  VTable.push_back(m);
}

int ClassType::getVTableIndex(const std::string &name) const {
  auto it = VTableIndex.find(name);
  return it != VTableIndex.end() ? it->second : -1;
}

MethodDecl *ClassType::findMethod(const std::string &name) const {
  // __init__ lives outside the vtable.
  if (name == names::kMethodInit)
    return InitMethod; // nullptr if not declared in this class
  auto it = VTableIndex.find(name);
  if (it != VTableIndex.end())
    return VTable[it->second];
  // Walk up the inheritance chain.
  if (SuperClass)
    return SuperClass->findMethod(name);
  return nullptr;
}

bool ClassType::isSubtypeOf(const ClassType *other) const {
  if (this == other)
    return true;
  return SuperClass ? SuperClass->isSubtypeOf(other) : false;
}

// -- ASTCloner

Type *ASTCloner::cloneType(Type *ty) {
  if (!ty)
    return nullptr;
  if (auto *ct = dyn_cast<ClassType>(ty)) {
    auto it = Subst.find(ct->getName());
    return it != Subst.end() ? it->second : ty;
  }
  if (auto *at = dyn_cast<ArrayType>(ty)) {
    Type *elem = cloneType(at->getElementType());
    if (elem == at->getElementType())
      return ty;
    return Ctx.make<ArrayType>(at->getLocation(), elem);
  }
  if (auto *gt = dyn_cast<GenericType>(ty)) {
    bool changed = false;
    std::vector<Type *> args;
    args.reserve(gt->getNumArgs());
    for (auto *a : gt->getArgs()) {
      Type *c = cloneType(a);
      changed |= c != a;
      args.push_back(c);
    }
    if (!changed)
      return ty;
    return Ctx.make<GenericType>(gt->getLocation(), gt->getName(),
                                 std::move(args));
  }
  if (auto *ot = dyn_cast<OptionalType>(ty)) {
    Type *inner = cloneType(ot->getInnerType());
    if (inner == ot->getInnerType())
      return ty;
    return Ctx.make<OptionalType>(ot->getLocation(), inner);
  }
  if (auto *tt = dyn_cast<TupleType>(ty)) {
    bool changed = false;
    std::vector<Type *> elems;
    elems.reserve(tt->getArity());
    for (auto *el : tt->getElementTypes()) {
      Type *c = cloneType(el);
      changed |= c != el;
      elems.push_back(c);
    }
    if (!changed)
      return ty;
    return Ctx.make<TupleType>(tt->getLocation(), std::move(elems));
  }
  // Builtin and enum types are canonical singletons.
  return ty;
}

Expr *ASTCloner::cloneExpr(Expr *e) {
  if (!e)
    return nullptr;
  auto loc = e->getLocation();
  switch (e->getKind()) {
  case ASTNode::NK_IntegerLiteral:
    return Ctx.make<IntegerLiteral>(loc, cast<IntegerLiteral>(e)->getValue());
  case ASTNode::NK_FloatLiteral:
    return Ctx.make<FloatLiteral>(loc, cast<FloatLiteral>(e)->getValue());
  case ASTNode::NK_BoolLiteral:
    return Ctx.make<BoolLiteral>(loc, cast<BoolLiteral>(e)->getValue());
  case ASTNode::NK_CharLiteral:
    return Ctx.make<CharLiteral>(loc, cast<CharLiteral>(e)->getValue());
  case ASTNode::NK_NoneLiteral:
    return Ctx.make<NoneLiteral>(loc);
  case ASTNode::NK_StringLiteral:
    return Ctx.make<StringLiteral>(loc, cast<StringLiteral>(e)->getValue());
  case ASTNode::NK_UnaryExpr: {
    auto *u = cast<UnaryExpr>(e);
    return Ctx.make<UnaryExpr>(loc, u->getOpcode(), cloneExpr(u->getOperand()));
  }
  case ASTNode::NK_CopyExpr:
    return Ctx.make<CopyExpr>(loc, cloneExpr(cast<CopyExpr>(e)->getOperand()));
  case ASTNode::NK_MoveExpr:
    return Ctx.make<MoveExpr>(loc, cloneExpr(cast<MoveExpr>(e)->getOperand()));
  case ASTNode::NK_BinaryExpr: {
    auto *b = cast<BinaryExpr>(e);
    return Ctx.make<BinaryExpr>(loc, b->getOpcode(), cloneExpr(b->getLHS()),
                                cloneExpr(b->getRHS()));
  }
  case ASTNode::NK_Identifier:
    return Ctx.make<Identifier>(loc, cast<Identifier>(e)->getName());
  case ASTNode::NK_CallExpr: {
    auto *c = cast<CallExpr>(e);
    std::vector<Expr *> args;
    for (auto *a : c->getArguments())
      args.push_back(cloneExpr(a));
    std::vector<Type *> typeArgs;
    for (auto *t : c->getTypeArgs())
      typeArgs.push_back(cloneType(t));
    return Ctx.make<CallExpr>(loc, c->getCalleeName(), std::move(args),
                              std::move(typeArgs));
  }
  case ASTNode::NK_MethodCallExpr: {
    auto *m = cast<MethodCallExpr>(e);
    std::vector<Expr *> args;
    for (auto *a : m->getArguments())
      args.push_back(cloneExpr(a));
    return Ctx.make<MethodCallExpr>(loc, cloneExpr(m->getReceiver()),
                                    m->getMethodName(), std::move(args));
  }
  case ASTNode::NK_TernaryExpr: {
    auto *t = cast<TernaryExpr>(e);
    return Ctx.make<TernaryExpr>(loc, cloneExpr(t->getCondition()),
                                 cloneExpr(t->getTrueExpr()),
                                 cloneExpr(t->getFalseExpr()));
  }
  case ASTNode::NK_MemberAccessExpr: {
    auto *m = cast<MemberAccessExpr>(e);
    return Ctx.make<MemberAccessExpr>(loc, cloneExpr(m->getReceiver()),
                                      m->getFieldName());
  }
  case ASTNode::NK_ArrayLiteralExpr: {
    auto *a = cast<ArrayLiteralExpr>(e);
    std::vector<Expr *> elems;
    for (auto *el : a->getElements())
      elems.push_back(cloneExpr(el));
    return Ctx.make<ArrayLiteralExpr>(loc, std::move(elems));
  }
  case ASTNode::NK_SubscriptExpr: {
    auto *s = cast<SubscriptExpr>(e);
    return Ctx.make<SubscriptExpr>(loc, cloneExpr(s->getArray()),
                                   cloneExpr(s->getIndex()));
  }
  case ASTNode::NK_EnumValueExpr: {
    auto *ev = cast<EnumValueExpr>(e);
    return Ctx.make<EnumValueExpr>(loc, ev->getEnumName(),
                                   ev->getVariantName());
  }
  case ASTNode::NK_TupleLiteralExpr: {
    auto *t = cast<TupleLiteralExpr>(e);
    std::vector<Expr *> elems;
    for (auto *el : t->getElements())
      elems.push_back(cloneExpr(el));
    return Ctx.make<TupleLiteralExpr>(loc, std::move(elems));
  }
  case ASTNode::NK_TupleIndexExpr: {
    auto *ti = cast<TupleIndexExpr>(e);
    return Ctx.make<TupleIndexExpr>(loc, cloneExpr(ti->getTuple()),
                                    ti->getIndex());
  }
  default:
    break;
  }
  assert(false && "ASTCloner::cloneExpr: unhandled expression kind");
  return nullptr;
}

CompoundStmt *ASTCloner::cloneCompound(CompoundStmt *cs) {
  if (!cs)
    return nullptr;
  auto *out = Ctx.make<CompoundStmt>(cs->getLocation());
  for (auto *s : cs->getStatements())
    out->addStatement(cloneStmt(s));
  return out;
}

Stmt *ASTCloner::cloneStmt(Stmt *s) {
  if (!s)
    return nullptr;
  auto loc = s->getLocation();
  switch (s->getKind()) {
  case ASTNode::NK_CompoundStmt:
    return cloneCompound(cast<CompoundStmt>(s));
  case ASTNode::NK_ReturnStmt:
    return Ctx.make<ReturnStmt>(
        loc, cloneExpr(cast<ReturnStmt>(s)->getReturnValue()));
  case ASTNode::NK_AssignStmt: {
    auto *a = cast<AssignStmt>(s);
    auto *lhs = Ctx.make<Identifier>(a->getLHS()->getLocation(),
                                     a->getLHS()->getName());
    return Ctx.make<AssignStmt>(loc, lhs, cloneExpr(a->getValue()));
  }
  case ASTNode::NK_DeclStmt: {
    auto *d = cast<DeclStmt>(s);
    auto *vd = cast<VarDecl>(d->getDecl());
    auto *nvd = Ctx.make<VarDecl>(vd->getLocation(), vd->getName(),
                                  cloneType(vd->getType()),
                                  cloneExpr(vd->getInitExpr()));
    nvd->setQualifier(vd->getQualifier());
    nvd->setLet(vd->isLet());
    return Ctx.make<DeclStmt>(loc, nvd);
  }
  case ASTNode::NK_ExprStmt:
    return Ctx.make<ExprStmt>(loc, cloneExpr(cast<ExprStmt>(s)->getExpr()));
  case ASTNode::NK_IfStmt: {
    auto *i = cast<IfStmt>(s);
    return Ctx.make<IfStmt>(loc, cloneExpr(i->getCondition()),
                            cloneStmt(i->getThenBranch()),
                            cloneStmt(i->getElseBranch()));
  }
  case ASTNode::NK_WhileStmt: {
    auto *w = cast<WhileStmt>(s);
    return Ctx.make<WhileStmt>(loc, cloneExpr(w->getCondition()),
                               cloneStmt(w->getBody()));
  }
  case ASTNode::NK_BreakStmt:
    return Ctx.make<BreakStmt>(loc);
  case ASTNode::NK_ContinueStmt:
    return Ctx.make<ContinueStmt>(loc);
  case ASTNode::NK_MemberAssignStmt: {
    auto *m = cast<MemberAssignStmt>(s);
    return Ctx.make<MemberAssignStmt>(loc, cloneExpr(m->getReceiver()),
                                      m->getFieldName(),
                                      cloneExpr(m->getValue()));
  }
  case ASTNode::NK_MatchStmt: {
    auto *m = cast<MatchStmt>(s);
    std::vector<MatchArm *> arms;
    for (auto *arm : m->getArms()) {
      auto *body = cloneCompound(arm->getBody());
      if (arm->isLiteral())
        arms.push_back(Ctx.make<MatchArm>(arm->getLocation(), arm->getBinding(),
                                          cloneExpr(arm->getLiteralPattern()),
                                          body));
      else
        arms.push_back(Ctx.make<MatchArm>(arm->getLocation(), arm->getBinding(),
                                          cloneType(arm->getArmType()), body));
    }
    return Ctx.make<MatchStmt>(loc, cloneExpr(m->getSubject()),
                               std::move(arms));
  }
  case ASTNode::NK_SubscriptAssignStmt: {
    auto *sa = cast<SubscriptAssignStmt>(s);
    return Ctx.make<SubscriptAssignStmt>(loc, cloneExpr(sa->getArray()),
                                         cloneExpr(sa->getIndex()),
                                         cloneExpr(sa->getValue()));
  }
  case ASTNode::NK_DestructureStmt: {
    auto *d = cast<DestructureStmt>(s);
    std::vector<DestructureStmt::Target> targets;
    targets.reserve(d->getNumTargets());
    for (const auto &t : d->getTargets())
      targets.push_back({t.Name, cloneType(t.DeclType), t.Loc});
    return Ctx.make<DestructureStmt>(loc, std::move(targets),
                                     cloneExpr(d->getValue()));
  }
  default:
    break;
  }
  assert(false && "ASTCloner::cloneStmt: unhandled statement kind");
  return nullptr;
}

FuncDecl *ASTCloner::cloneFuncDecl(FuncDecl *fn, const std::string &newName) {
  std::vector<Param> params;
  params.reserve(fn->getParams().size());
  for (auto &p : fn->getParams())
    params.push_back(Param{p.Name, cloneType(p.ParamType), p.Qual});
  auto *clone = Ctx.make<FuncDecl>(
      fn->getLocation(), Ctx.intern(newName), std::move(params),
      cloneType(fn->getReturnType()), cloneCompound(fn->getBody()));
  clone->setResultQualifier(fn->getResultQualifier());
  if (fn->hasExplicitSelf())
    clone->setExplicitSelf(fn->getSelfQualifier());
  return clone;
}

ClassDecl *ASTCloner::cloneClassDecl(ClassDecl *cd,
                                     const std::string &newName) {
  std::vector<VarDecl *> fields;
  for (auto *f : cd->getFields()) {
    fields.push_back(Ctx.make<VarDecl>(f->getLocation(), f->getName(),
                                       cloneType(f->getType()), nullptr));
    fields.back()->setQualifier(f->getQualifier());
  }
  std::vector<FuncDecl *> methods;
  for (auto *m : cd->getMethods())
    methods.push_back(cloneFuncDecl(m, m->getName()));
  return Ctx.make<ClassDecl>(cd->getLocation(), Ctx.intern(newName),
                             cd->getSuperClassName(), std::move(fields),
                             std::move(methods));
}

} // namespace ast
} // namespace paykan
