// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0

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

const char *paramModeName(ParamMode m) {
  switch (m) {
  case ParamMode::View:
    return "view";
  case ParamMode::Inout:
    return "inout";
  case ParamMode::Value:
    break;
  }
  return "";
}

ParamModes paramModes(const std::vector<Param> &params) {
  ParamModes out;
  for (const Param &p : params)
    if (p.Mode != ParamMode::Value) {
      for (const Param &each : params) {
        out.Modes.push_back(each.Mode);
        out.Names.push_back(each.getName());
      }
      break;
    }
  return out;
}

// -- ForStmt

std::optional<int64_t> ForStmt::constantStep() const {
  if (!Step)
    return 1;
  const Expr *e = Step;
  bool negated = false;
  if (const auto *u = dyn_cast<UnaryExpr>(e);
      u && u->getOpcode() == UnaryOpcode::Neg) {
    negated = true;
    e = u->getOperand();
  }
  const auto *lit = dyn_cast<IntegerLiteral>(e);
  if (!lit)
    return std::nullopt;
  // A literal is at most INT64_MAX (docs/grammar.md), so its negation fits.
  return negated ? -lit->getValue() : lit->getValue();
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

unsigned BuiltinType::integerBits(Kind k) {
  switch (k) {
  case Int:
  case U64:
    return 64;
  case I32:
  case U32:
    return 32;
  case I16:
  case U16:
    return 16;
  case I8:
  case U8:
    return 8;
  case Float:
  case Bool:
  case Char:
  case Void:
  case F16:
  case F32:
    break;
  }
  return 0;
}

unsigned BuiltinType::floatBits(Kind k) {
  switch (k) {
  case Float:
    return 64;
  case F32:
    return 32;
  case F16:
    return 16;
  case Int:
  case Bool:
  case Char:
  case Void:
  case I8:
  case I16:
  case I32:
  case U8:
  case U16:
  case U32:
  case U64:
    break;
  }
  return 0;
}

const char *builtinKindName(BuiltinType::Kind k) {
  switch (k) {
  case BuiltinType::Int:
    return names::kTypeInt;
  case BuiltinType::Float:
    return names::kTypeFloat;
  case BuiltinType::Bool:
    return names::kTypeBool;
  case BuiltinType::Char:
    return names::kTypeChar;
  case BuiltinType::Void:
    return names::kTypeVoid;
  case BuiltinType::I8:
    return names::kTypeI8;
  case BuiltinType::I16:
    return names::kTypeI16;
  case BuiltinType::I32:
    return names::kTypeI32;
  case BuiltinType::U8:
    return names::kTypeU8;
  case BuiltinType::U16:
    return names::kTypeU16;
  case BuiltinType::U32:
    return names::kTypeU32;
  case BuiltinType::U64:
    return names::kTypeUnsigned;
  case BuiltinType::F16:
    return names::kTypeF16;
  case BuiltinType::F32:
    return names::kTypeF32;
  }
  return names::kTypeVoid;
}

bool builtinKindOfName(std::string_view name, BuiltinType::Kind &kind) {
  static const struct {
    const char *Name;
    BuiltinType::Kind Kind;
  } kNames[] = {
      {names::kTypeInt, BuiltinType::Int},
      {names::kTypeI64, BuiltinType::Int},
      {names::kTypeFloat, BuiltinType::Float},
      {names::kTypeF64, BuiltinType::Float},
      {names::kTypeBool, BuiltinType::Bool},
      {names::kTypeChar, BuiltinType::Char},
      {names::kTypeVoid, BuiltinType::Void},
      {names::kTypeI8, BuiltinType::I8},
      {names::kTypeI16, BuiltinType::I16},
      {names::kTypeI32, BuiltinType::I32},
      {names::kTypeU8, BuiltinType::U8},
      {names::kTypeU16, BuiltinType::U16},
      {names::kTypeU32, BuiltinType::U32},
      {names::kTypeU64, BuiltinType::U64},
      {names::kTypeUnsigned, BuiltinType::U64},
      {names::kTypeF16, BuiltinType::F16},
      {names::kTypeF32, BuiltinType::F32},
  };
  for (const auto &entry : kNames)
    if (name == entry.Name) {
      kind = entry.Kind;
      return true;
    }
  return false;
}

void BuiltinType::initOps() {
  switch (TypeKind) {
  case U8:
  case U16:
  case U32:
  case U64:
    // An unsigned type has no negation; otherwise the integer operators.
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
  case Int:
  case Float:
  case I8:
  case I16:
  case I32:
  case F16:
  case F32:
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
  if (m->isImpl()) {
    ImplMethods.push_back(m);
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

void ClassType::updateAbstract() { Abstract = !pureSlots().empty(); }

std::vector<const MethodDecl *> ClassType::pureSlots() const {
  std::vector<const MethodDecl *> out;
  for (const MethodDecl *m : VTable)
    if (m->isPure())
      out.push_back(m);
  return out;
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
  for (auto *m : ImplMethods)
    if (m->getName() == name)
      return m;
  // Walk up the inheritance chain.
  if (SuperClass)
    return SuperClass->findMethod(name);
  return nullptr;
}

ClassType *ClassType::implOwner(const std::string &name) {
  for (ClassType *c = this; c; c = c->SuperClass)
    for (auto *m : c->ImplMethods)
      if (m->getName() == name)
        return c;
  return nullptr;
}

bool ClassType::isSubtypeOf(const ClassType *other) const {
  if (this == other)
    return true;
  return SuperClass ? SuperClass->isSubtypeOf(other) : false;
}

// -- StructType

void StructType::addMethod(MethodDecl *m) {
  if (m->getName() == names::kMethodInit) {
    InitMethod = m;
    return;
  }
  ImplMethods.push_back(m);
}

MethodDecl *StructType::findMethod(const std::string &name) const {
  if (name == names::kMethodInit)
    return InitMethod;
  for (auto *m : ImplMethods)
    if (m->getName() == name)
      return m;
  return nullptr;
}

// -- ASTCloner

Type *ASTCloner::named(Type *ty, const std::string &name) {
  std::string spelled = Names ? Names->type(name) : name;
  if (!Foreign && spelled == name)
    return ty;
  return Ctx.make<ClassType>(ty->getLocation(), in(spelled), nullptr);
}

Type *ASTCloner::cloneType(Type *ty) {
  if (!ty)
    return nullptr;
  if (auto *ct = dyn_cast<ClassType>(ty)) {
    auto it = Subst.find(ct->getName());
    return it != Subst.end() ? it->second : named(ty, ct->getName());
  }
  if (auto *et = dyn_cast<EnumType>(ty))
    return named(ty, et->getName());
  // Before TupleType, which a StructType is: a struct is named, never
  // rebuilt from its fields.
  if (auto *st = dyn_cast<StructType>(ty))
    return named(ty, st->getName());
  if (auto *bt = dyn_cast<BuiltinType>(ty))
    return Foreign ? Ctx.getBuiltinType(bt->getTypeKind()) : ty;
  if (auto *at = dyn_cast<ArrayType>(ty)) {
    Type *elem = cloneType(at->getElementType());
    if (elem == at->getElementType() && !Foreign)
      return ty;
    return Ctx.make<ArrayType>(at->getLocation(), elem);
  }
  if (auto *ft = dyn_cast<FixedArrayType>(ty)) {
    Type *elem = cloneType(ft->getElementType());
    if (elem == ft->getElementType() && !Foreign)
      return ty;
    return Ctx.make<FixedArrayType>(ft->getLocation(), elem, ft->getLength());
  }
  if (auto *gt = dyn_cast<GenericType>(ty)) {
    std::string name = Names ? Names->generic(gt->getName()) : gt->getName();
    bool changed = Foreign || name != gt->getName();
    std::vector<Type *> args;
    args.reserve(gt->getNumArgs());
    for (auto *a : gt->getArgs()) {
      Type *c = cloneType(a);
      changed |= c != a;
      args.push_back(c);
    }
    if (!changed)
      return ty;
    return Ctx.make<GenericType>(gt->getLocation(), in(name), std::move(args));
  }
  if (auto *ot = dyn_cast<OptionalType>(ty)) {
    Type *inner = cloneType(ot->getInnerType());
    // `T?` of a type argument that is an optional already is that optional:
    // a template's `T?` adds a None only where T has none (a written `X??`
    // is still an error).
    if (auto *param = dyn_cast<ClassType>(ot->getInnerType());
        param && Subst.count(param->getName()) && isa<OptionalType>(inner))
      return inner;
    if (inner == ot->getInnerType() && !Foreign)
      return ty;
    return Ctx.make<OptionalType>(ot->getLocation(), inner);
  }
  if (auto *tt = dyn_cast<TupleType>(ty)) {
    bool changed = Foreign;
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
  if (auto *fnt = dyn_cast<FunctionType>(ty)) {
    bool changed = Foreign;
    std::vector<Type *> params;
    params.reserve(fnt->getNumParams());
    for (auto *p : fnt->getParamTypes()) {
      Type *c = cloneType(p);
      changed |= c != p;
      params.push_back(c);
    }
    Type *ret = cloneType(fnt->getReturnType());
    changed |= ret != fnt->getReturnType();
    if (!changed)
      return ty;
    return Ctx.make<FunctionType>(fnt->getLocation(), std::move(params),
                                  fnt->getModes(), ret);
  }
  // Nothing else is written in a template.
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
  case ASTNode::NK_BinaryExpr: {
    auto *b = cast<BinaryExpr>(e);
    return Ctx.make<BinaryExpr>(loc, b->getOpcode(), cloneExpr(b->getLHS()),
                                cloneExpr(b->getRHS()));
  }
  case ASTNode::NK_Identifier:
    return Ctx.make<Identifier>(loc, in(cast<Identifier>(e)->getName()));
  case ASTNode::NK_CallExpr: {
    auto *c = cast<CallExpr>(e);
    std::vector<Expr *> args;
    for (auto *a : c->getArguments())
      args.push_back(cloneExpr(a));
    std::vector<Type *> typeArgs;
    for (auto *t : c->getTypeArgs())
      typeArgs.push_back(cloneType(t));
    const std::string &callee = c->getCalleeName();
    return Ctx.make<CallExpr>(loc, in(Names ? Names->callee(callee) : callee),
                              std::move(args), std::move(typeArgs));
  }
  case ASTNode::NK_MethodCallExpr: {
    auto *m = cast<MethodCallExpr>(e);
    std::vector<Expr *> args;
    for (auto *a : m->getArguments())
      args.push_back(cloneExpr(a));
    return Ctx.make<MethodCallExpr>(loc, cloneExpr(m->getReceiver()),
                                    in(m->getMethodName()), std::move(args));
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
                                      in(m->getFieldName()));
  }
  case ASTNode::NK_ArrayLiteralExpr: {
    auto *a = cast<ArrayLiteralExpr>(e);
    std::vector<Expr *> elems;
    for (auto *el : a->getElements())
      elems.push_back(cloneExpr(el));
    return Ctx.make<ArrayLiteralExpr>(loc, std::move(elems),
                                      a->getRepeatCount());
  }
  case ASTNode::NK_SubscriptExpr: {
    auto *s = cast<SubscriptExpr>(e);
    return Ctx.make<SubscriptExpr>(loc, cloneExpr(s->getArray()),
                                   cloneExpr(s->getIndex()));
  }
  case ASTNode::NK_EnumValueExpr: {
    auto *ev = cast<EnumValueExpr>(e);
    const std::string &en = ev->getEnumName();
    // A generic enum's (`Option<int>::Nothing`) is named as a template is.
    std::vector<Type *> typeArgs;
    for (auto *t : ev->getTypeArgs())
      typeArgs.push_back(cloneType(t));
    const std::string spelled = !Names              ? en
                                : ev->hasTypeArgs() ? Names->generic(en)
                                                    : Names->type(en);
    return Ctx.make<EnumValueExpr>(loc, in(spelled), in(ev->getVariantName()),
                                   std::move(typeArgs));
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
  case ASTNode::NK_CallValueExpr: {
    auto *c = cast<CallValueExpr>(e);
    std::vector<Expr *> args;
    for (auto *a : c->getArguments())
      args.push_back(cloneExpr(a));
    return Ctx.make<CallValueExpr>(loc, cloneExpr(c->getCallee()),
                                   std::move(args));
  }
  case ASTNode::NK_LambdaExpr: {
    // The copy has no captures: Sema records them when it checks the copy.
    auto *l = cast<LambdaExpr>(e);
    std::vector<Param> params;
    for (const Param &p : l->getParams())
      params.push_back(Param{&in(p.getName()), cloneType(p.ParamType), p.Mode});
    auto *copy = Ctx.make<LambdaExpr>(loc, std::move(params),
                                      cloneType(l->getReturnType()),
                                      cloneCompound(l->getBody()));
    copy->setInferredResult(l->hasInferredResult());
    return copy;
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
                                     in(a->getLHS()->getName()));
    return Ctx.make<AssignStmt>(loc, lhs, cloneExpr(a->getValue()));
  }
  case ASTNode::NK_DeclStmt: {
    auto *d = cast<DeclStmt>(s);
    auto *vd = cast<VarDecl>(d->getDecl());
    auto *nvd = Ctx.make<VarDecl>(vd->getLocation(), in(vd->getName()),
                                  cloneType(vd->getType()),
                                  cloneExpr(vd->getInitExpr()));
    nvd->setLet(vd->isLet());
    nvd->setWeak(vd->isWeak());
    nvd->setMode(vd->getMode());
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
  case ASTNode::NK_ForStmt: {
    auto *f = cast<ForStmt>(s);
    VarDecl *v = f->getVar();
    auto *nv = Ctx.make<VarDecl>(v->getLocation(), in(v->getName()),
                                 cloneType(v->getType()), nullptr);
    return Ctx.make<ForStmt>(loc, nv, cloneExpr(f->getStart()),
                             cloneExpr(f->getEnd()), cloneExpr(f->getStep()),
                             f->isInclusive(), cloneStmt(f->getBody()));
  }
  case ASTNode::NK_BreakStmt:
    return Ctx.make<BreakStmt>(loc);
  case ASTNode::NK_ContinueStmt:
    return Ctx.make<ContinueStmt>(loc);
  case ASTNode::NK_MemberAssignStmt: {
    auto *m = cast<MemberAssignStmt>(s);
    return Ctx.make<MemberAssignStmt>(loc, cloneExpr(m->getReceiver()),
                                      in(m->getFieldName()),
                                      cloneExpr(m->getValue()));
  }
  case ASTNode::NK_MatchStmt: {
    auto *m = cast<MatchStmt>(s);
    std::vector<MatchArm *> arms;
    for (auto *arm : m->getArms()) {
      auto *body = cloneCompound(arm->getBody());
      if (arm->isLiteral())
        arms.push_back(
            Ctx.make<MatchArm>(arm->getLocation(), in(arm->getBinding()),
                               cloneExpr(arm->getLiteralPattern()), body));
      else
        arms.push_back(Ctx.make<MatchArm>(arm->getLocation(),
                                          in(arm->getBinding()),
                                          cloneType(arm->getArmType()), body));
      arms.back()->setMode(arm->getMode());
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
      targets.push_back({&in(t.getName()), cloneType(t.DeclType), t.Loc});
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
    params.push_back(Param{&in(p.getName()), cloneType(p.ParamType), p.Mode});
  auto *clone = Ctx.make<FuncDecl>(
      fn->getLocation(), in(newName), std::move(params),
      cloneType(fn->getReturnType()), cloneCompound(fn->getBody()));
  clone->setView(fn->isView());
  clone->setStatic(fn->isStatic());
  clone->setModulePrivateInit(fn->isModulePrivateInit());
  clone->setResultMode(fn->getResultMode());
  clone->setInferredResult(fn->hasInferredResult());
  if (fn->isNative())
    clone->setNativeSymbol(in(fn->getNativeSymbol()));
  return clone;
}

ClassDecl *ASTCloner::cloneClassDecl(ClassDecl *cd,
                                     const std::string &newName) {
  std::vector<VarDecl *> fields;
  for (auto *f : cd->getFields()) {
    fields.push_back(Ctx.make<VarDecl>(f->getLocation(), in(f->getName()),
                                       cloneType(f->getType()), nullptr));
    fields.back()->setWeak(f->isWeak());
  }
  std::vector<FuncDecl *> methods;
  for (auto *m : cd->getMethods())
    methods.push_back(cloneFuncDecl(m, m->getName()));
  const std::string &super = cd->getSuperClassName();
  return Ctx.make<ClassDecl>(
      cd->getLocation(), in(newName),
      in(Names && !super.empty() ? Names->type(super) : super),
      std::move(fields), std::move(methods));
}

StructDecl *ASTCloner::cloneStructDecl(StructDecl *sd,
                                       const std::string &newName) {
  std::vector<VarDecl *> fields;
  for (auto *f : sd->getFields())
    fields.push_back(Ctx.make<VarDecl>(f->getLocation(), in(f->getName()),
                                       cloneType(f->getType()), nullptr));
  auto *out =
      Ctx.make<StructDecl>(sd->getLocation(), in(newName), std::move(fields));
  for (auto *m : sd->getMethods())
    out->addMethod(cloneFuncDecl(m, m->getName()));
  return out;
}

StructDecl *ASTCloner::cloneTemplate(StructDecl *sd) {
  StructDecl *copy = cloneStructDecl(sd, sd->getName());
  std::vector<const std::string *> params;
  for (auto *p : sd->getTypeParams())
    params.push_back(&in(*p));
  auto *out = Ctx.make<StructDecl>(copy->getLocation(), copy->getName(),
                                   copy->getFields(), std::move(params));
  for (auto *m : copy->getMethods())
    out->addMethod(m);
  out->setTypeParamBounds(cloneBounds(sd->getTypeParamBounds()));
  return out;
}

EnumDecl *ASTCloner::cloneEnumDecl(EnumDecl *ed, const std::string &newName) {
  std::vector<const std::string *> variants;
  std::vector<std::vector<VarDecl *>> payloads;
  for (size_t i = 0; i < ed->getNumVariants(); ++i) {
    variants.push_back(&in(*ed->getVariants()[i]));
    std::vector<VarDecl *> fields;
    for (auto *f : ed->getPayload(i))
      fields.push_back(Ctx.make<VarDecl>(f->getLocation(), in(f->getName()),
                                         cloneType(f->getType()), nullptr));
    payloads.push_back(std::move(fields));
  }
  return Ctx.make<EnumDecl>(ed->getLocation(), in(newName), std::move(variants),
                            std::move(payloads));
}

EnumDecl *ASTCloner::cloneTemplate(EnumDecl *ed) {
  EnumDecl *copy = cloneEnumDecl(ed, ed->getName());
  std::vector<const std::string *> params;
  for (auto *p : ed->getTypeParams())
    params.push_back(&in(*p));
  std::vector<std::vector<VarDecl *>> payloads;
  payloads.reserve(copy->getNumVariants());
  for (size_t i = 0; i < copy->getNumVariants(); ++i)
    payloads.push_back(copy->getPayload(i));
  auto *out = Ctx.make<EnumDecl>(copy->getLocation(), copy->getName(),
                                 copy->getVariants(), std::move(payloads),
                                 std::move(params));
  out->setTypeParamBounds(cloneBounds(ed->getTypeParamBounds()));
  return out;
}

ImplDecl *ASTCloner::cloneImplDecl(ImplDecl *im,
                                   const std::string &newTypeName) {
  std::vector<FuncDecl *> methods;
  for (auto *m : im->getMethods())
    methods.push_back(cloneFuncDecl(m, m->getName()));
  return Ctx.make<ImplDecl>(im->getLocation(), in(newTypeName),
                            std::move(methods), cloneType(im->getTrait()));
}

ImplDecl *ASTCloner::cloneTemplate(ImplDecl *im) {
  ImplDecl *copy = cloneImplDecl(im, im->getTypeName());
  std::vector<const std::string *> params;
  for (auto *p : im->getTypeParams())
    params.push_back(&in(*p));
  return Ctx.make<ImplDecl>(copy->getLocation(), copy->getTypeName(),
                            copy->getMethods(), copy->getTrait(),
                            std::move(params));
}

FuncDecl *ASTCloner::cloneTemplate(FuncDecl *fn) {
  FuncDecl *copy = cloneFuncDecl(fn, fn->getName());
  std::vector<const std::string *> params;
  for (auto *p : fn->getTypeParams())
    params.push_back(&in(*p));
  auto *out = Ctx.make<FuncDecl>(copy->getLocation(), copy->getName(),
                                 copy->getParams(), copy->getReturnType(),
                                 copy->getBody(), std::move(params));
  out->setView(copy->isView());
  out->setStatic(copy->isStatic());
  out->setModulePrivateInit(copy->isModulePrivateInit());
  out->setResultMode(copy->getResultMode());
  out->setInferredResult(copy->hasInferredResult());
  out->setTypeParamBounds(cloneBounds(fn->getTypeParamBounds()));
  if (copy->isNative())
    out->setNativeSymbol(copy->getNativeSymbol());
  return out;
}

ClassDecl *ASTCloner::cloneTemplate(ClassDecl *cd) {
  ClassDecl *copy = cloneClassDecl(cd, cd->getName());
  std::vector<const std::string *> params;
  for (auto *p : cd->getTypeParams())
    params.push_back(&in(*p));
  auto *out = Ctx.make<ClassDecl>(copy->getLocation(), copy->getName(),
                                  copy->getSuperClassName(), copy->getFields(),
                                  copy->getMethods(), std::move(params));
  out->setTypeParamBounds(cloneBounds(cd->getTypeParamBounds()));
  return out;
}

TypeParamBounds ASTCloner::cloneBounds(const TypeParamBounds &bounds) {
  TypeParamBounds out;
  for (const auto &traits : bounds) {
    auto &cloned = out.emplace_back();
    for (Type *t : traits)
      cloned.push_back(cloneType(t));
  }
  return out;
}

} // namespace ast
} // namespace paykan
