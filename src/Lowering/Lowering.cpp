// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// AST -> PIR lowering: scopes, ownership classification, ARC placement,
// statements.  Expressions are in LoweringExpr.cpp, `match` in
// LoweringMatch.cpp, classes in LoweringClass.cpp, and the module/import
// driver in LoweringProgram.cpp.

#include "LoweringInternal.h"
#include "Names.h"

#include <cassert>
#include <cstring>

namespace paykan::lowering {

using namespace names;
using pir::Type;

// -- Runtime ABI table (docs/pir.md §8)

namespace {

struct RuntimeSig {
  const char *Name;
  pir::Signature Sig;
};

const Type kObj = Type::Obj;
const Type kBox = Type::Box;
const Type kI64 = Type::I64;
const Type kF64 = Type::F64;
const Type kChr = Type::Char;
const Type kPtr = Type::Ptr;
const Type kVoid = Type::Void;

const RuntimeSig kRuntimeSigs[] = {
    {kPaykanRetain, {{kBox}, kVoid}},
    {kPaykanRelease, {{kBox}, kVoid}},
    {kPaykanSharedNew, {{kObj}, kBox}},
    {kPaykanSharedGet, {{kBox}, kObj}},
    {kPaykanPanicDivByZero, {{}, kVoid}},
    {kPaykanPanicDivOverflow, {{}, kVoid}},
    {kPaykanPanicFloatToInt, {{kF64}, kVoid}},
    {kPaykanPanicIntToChar, {{kI64}, kVoid}},
    {kPaykanStringNew, {{kPtr, kI64}, kObj}},
    {kPaykanStringDestroy, {{kObj}, kVoid}},
    {kPaykanStringConcat, {{kObj, kObj}, kObj}},
    {kPaykanStringCharAt, {{kObj, kI64}, kChr}},
    {kPaykanStringFromInt, {{kI64}, kObj}},
    {kPaykanStringFromFloat, {{kF64}, kObj}},
    {kPaykanStringFromBool, {{kI64}, kObj}},
    {kPaykanStringFromChar, {{kChr}, kObj}},
    {kPaykanStringEquals, {{kObj, kBox}, kI64}},
    {kPaykanStringToString, {{kObj}, kBox}},
    {kPaykanStringLength, {{kObj}, kI64}},
    {kPaykanArrayNew, {{kI64}, kObj}},
    {kPaykanArrayNewObj, {{kI64}, kObj}},
    {kPaykanArrayNewFromData, {{kI64, kPtr}, kObj}},
    {kPaykanArrayGet, {{kObj, kI64}, kI64}},
    {kPaykanArraySet, {{kObj, kI64, kI64}, kVoid}},
    {kPaykanArraySetObj, {{kObj, kI64, kBox}, kVoid}},
    {kPaykanArrayPush, {{kObj, kI64}, kVoid}},
    {kPaykanArrayPushObj, {{kObj, kBox}, kVoid}},
    {kPaykanArrayPop, {{kObj}, kI64}},
    {kPaykanArrayPopObj, {{kObj}, kBox}},
    {kPaykanTupleNew, {{kI64, kPtr}, kObj}},
    {kPaykanTupleGet, {{kObj, kI64}, kI64}},
    {kPaykanTupleSet, {{kObj, kI64, kI64}, kVoid}},
    {kPaykanTupleSetObj, {{kObj, kI64, kBox}, kVoid}},
    {kPaykanFileOpen, {{kObj, kObj}, kBox}},
    {kPaykanIntFromStr, {{kObj}, kBox}},
    {kPaykanIntNew, {{kI64}, kObj}},
    {kPaykanFloatNew, {{kF64}, kObj}},
    {kPaykanBoolNew, {{kI64}, kObj}},
    {kPaykanCharNew, {{kChr}, kObj}},
    {kPaykanIntValue, {{kObj}, kI64}},
    {kPaykanFloatValue, {{kObj}, kF64}},
    {kPaykanBoolValue, {{kObj}, kI64}},
    {kPaykanCharValue, {{kObj}, kChr}},
    {kPaykanFloatFromStr, {{kObj}, kBox}},
    {kPaykanBoolFromStr, {{kObj}, kBox}},
    {kPaykanPrint, {{kObj}, kVoid}},
    {kPaykanPrintln, {{kObj}, kVoid}},
    {kPaykanErrPrint, {{kObj}, kVoid}},
    {kPaykanErrPrintln, {{kObj}, kVoid}},
    {kPaykanObjectDestroy, {{kObj}, kVoid}},
    {kPaykanObjectToString, {{kObj}, kBox}},
    {kPaykanObjectEquals, {{kObj, kBox}, kI64}},
    {kPaykanFileDestroy, {{kObj}, kVoid}},
    {kPaykanFileToString, {{kObj}, kBox}},
    {kPaykanFileEquals, {{kObj, kBox}, kI64}},
    {kPaykanFileWrite, {{kObj, kObj}, kVoid}},
    {kPaykanFileReadln, {{kObj}, kBox}},
};

const RuntimeSig *findRuntimeSig(const std::string &name) {
  for (const auto &rs : kRuntimeSigs)
    if (name == rs.Name)
      return &rs;
  return nullptr;
}

} // namespace

// -- Constructor

ModuleLowering::ModuleLowering(ProgramLowering &program,
                               const sema::SemaContext &semaCtx,
                               std::string moduleName)
    : PL(program), ASTCtx(*semaCtx.ASTCtx), SemaCtx(semaCtx),
      Errs(program.Errs) {
  Mod.Name = std::move(moduleName);
}

pir::Module ModuleLowering::takeModule() {
  pir::Module m = std::move(Mod);
  m.Functions.clear();
  for (auto &f : Funcs)
    m.Functions.push_back(std::move(f));
  m.Classes.clear();
  for (auto &c : Classes_)
    m.Classes.push_back(std::move(c));
  Funcs.clear();
  FuncByName.clear();
  RuntimeFuncs.clear();
  ExternByOrigin.clear();
  Classes_.clear();
  ClassByName.clear();
  return m;
}

void ModuleLowering::reportInternalError(const std::string &msg) {
  Errs << "internal compiler error: " << msg << "\n";
  HadInternalError = true;
}

// -- Scope

bool ModuleLowering::Scope::hasLocal(const std::string &name) const {
  if (Locals.count(name))
    return true;
  return Parent ? Parent->hasLocal(name) : false;
}

pir::LocalId ModuleLowering::Scope::lookup(const std::string &name) const {
  auto it = Locals.find(name);
  if (it != Locals.end())
    return it->second;
  assert(Parent && "Sema should have caught undeclared variable");
  return Parent->lookup(name);
}

bool ModuleLowering::Scope::isOwned(const std::string &name) const {
  if (SharedVars.count(name))
    return true;
  // Declared in THIS scope but not owned: stop here (a parent may own a
  // same-named variable).
  if (Locals.count(name))
    return false;
  return Parent ? Parent->isOwned(name) : false;
}

ast::Type *ModuleLowering::Scope::lookupASTType(const std::string &name) const {
  auto it = ASTTypeMap.find(name);
  if (it != ASTTypeMap.end())
    return it->second;
  return Parent ? Parent->lookupASTType(name) : nullptr;
}

void ModuleLowering::Scope::updateASTType(const std::string &name,
                                          ast::Type *newTy) {
  if (ASTTypeMap.count(name)) {
    ASTTypeMap[name] = newTy;
    return;
  }
  if (Parent)
    Parent->updateASTType(name, newTy);
}

void ModuleLowering::Scope::declare(const std::string &name, pir::LocalId local,
                                    ast::Type *astTy) {
  Locals[name] = local;
  if (astTy)
    ASTTypeMap[name] = astTy;
  if (astTy && ast::isRefType(astTy)) {
    SharedVars.insert(name);
    DeclOrder.push_back({local, astTy});
  }
}

void ModuleLowering::Scope::declareUnowned(const std::string &name,
                                           pir::LocalId local,
                                           ast::Type *astTy) {
  Locals[name] = local;
  if (astTy)
    ASTTypeMap[name] = astTy;
}

ModuleLowering::Scope *
ModuleLowering::Scope::findOwner(const std::string &name) {
  if (Locals.count(name))
    return this;
  return Parent ? Parent->findOwner(name) : nullptr;
}

ModuleLowering::ScopeGuard::ScopeGuard(ModuleLowering &l)
    : L(l), ScopeObj(l.CurrentScope) {
  L.CurrentScope = &ScopeObj;
}

ModuleLowering::ScopeGuard::~ScopeGuard() {
  // Only emit cleanup if the current block is not yet terminated.
  if (!L.B.isTerminated())
    L.emitScopeCleanup(ScopeObj);
  L.CurrentScope = ScopeObj.Parent;
}

// -- FunctionStateGuard

ModuleLowering::FunctionStateGuard::FunctionStateGuard(
    ModuleLowering &l, pir::Function *fn, ast::Type *retASTType,
    ast::ClassType *methodClassTy)
    : L(l), SavedBuilder(l.B.saveState()),
      SavedRetASTType(l.CurrentFuncReturnASTType),
      SavedMethodClassType(l.CurrentMethodClassType) {
  L.CurrentFuncReturnASTType = retASTType;
  L.CurrentMethodClassType = methodClassTy;
  // A body starts with an empty string-temp tracker and no enclosing loops.
  std::swap(SavedStringTemps, L.OwnedStringTemps);
  std::swap(SavedLoops, L.LoopStack);
  L.B.setFunction(fn);
}

ModuleLowering::FunctionStateGuard::~FunctionStateGuard() {
  std::swap(L.OwnedStringTemps, SavedStringTemps);
  std::swap(L.LoopStack, SavedLoops);
  L.CurrentFuncReturnASTType = SavedRetASTType;
  L.CurrentMethodClassType = SavedMethodClassType;
  L.B.restoreState(std::move(SavedBuilder));
}

// -- Type helpers

Type ModuleLowering::toPIRType(ast::Type *ty) {
  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(ty)) {
    switch (bt->getTypeKind()) {
    case ast::BuiltinType::Int:
      return Type::I64;
    case ast::BuiltinType::Float:
      return Type::F64;
    case ast::BuiltinType::Bool:
      return Type::Bool;
    case ast::BuiltinType::Char:
      return Type::Char;
    case ast::BuiltinType::Void:
      return Type::Void;
    }
  }
  // A parser ClassType stub may name an enum (resolved by Sema): enums are
  // i64 values, not boxes.
  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty))
    if (ASTCtx.lookupEnumType(ct->getName()))
      return Type::I64;
  if (ast::isa<ast::EnumType>(ty))
    return Type::I64;
  if (ast::isRefType(ty))
    return Type::Box;
  return Type::Void;
}

bool ModuleLowering::isObjectElementType(ast::Type *elemTy) const {
  if (!elemTy)
    return false;
  if (ast::isa<ast::EnumType>(elemTy))
    return false;
  if (auto *ct = ast::dyn_cast<ast::ClassType>(elemTy))
    return ASTCtx.lookupEnumType(ct->getName()) == nullptr;
  return ast::isa<ast::ArrayType>(elemTy) || ast::isa<ast::TupleType>(elemTy) ||
         ast::isa<ast::OptionalType>(elemTy);
}

ast::Type *ModuleLowering::canonicalizeDeclType(ast::Type *ty) {
  if (!ty)
    return ty;
  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty)) {
    if (auto *et = ASTCtx.lookupEnumType(ct->getName()))
      return et;
    if (auto *canonical = ASTCtx.lookupClassType(ct->getName()))
      return canonical;
  }
  if (auto *tt = ast::dyn_cast<ast::TupleType>(ty)) {
    std::vector<ast::Type *> elems;
    elems.reserve(tt->getArity());
    for (ast::Type *e : tt->getElementTypes()) {
      ast::Type *c = canonicalizeDeclType(e);
      if (auto *at = ast::dyn_cast<ast::ArrayType>(c))
        c = ASTCtx.getArrayType(canonicalizeDeclType(at->getElementType()));
      elems.push_back(c);
    }
    return ASTCtx.getTupleType(std::move(elems));
  }
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(ty)) {
    ast::Type *inner = canonicalizeDeclType(ot->getInnerType());
    if (auto *at = ast::dyn_cast<ast::ArrayType>(inner))
      inner = ASTCtx.getArrayType(canonicalizeDeclType(at->getElementType()));
    return ASTCtx.getOptionalType(inner);
  }
  return ty;
}

ast::ClassType *ModuleLowering::resolveExprClassType(ast::Expr *expr) {
  ast::Type *resolved = nullptr;
  if (auto *ce = ast::dyn_cast<ast::CallExpr>(expr))
    resolved = ce->getResolvedType();
  else if (auto *mce = ast::dyn_cast<ast::MethodCallExpr>(expr))
    resolved = mce->getResolvedType();
  else if (auto *mae = ast::dyn_cast<ast::MemberAccessExpr>(expr))
    resolved = mae->getResolvedType();
  else if (auto *se = ast::dyn_cast<ast::SubscriptExpr>(expr))
    resolved = se->getResolvedType();
  else if (auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(expr))
    resolved = ti->getResolvedType();
  else if (auto *id = ast::dyn_cast<ast::Identifier>(expr))
    resolved =
        CurrentScope ? CurrentScope->lookupASTType(id->getName()) : nullptr;

  auto *ct = ast::dyn_cast<ast::ClassType>(resolved); // null-safe
  if (!ct)
    return nullptr;
  if (auto *canonical = ASTCtx.lookupClassType(ct->getName()))
    return canonical;
  return ct;
}

pir::Signature ModuleLowering::functionSignature(ast::FuncDecl *node) {
  pir::Signature sig;
  ast::Type *retAstTy = canonicalizeDeclType(node->getReturnType());
  sig.Ret = retAstTy ? toPIRType(retAstTy) : Type::Void;
  for (auto &p : node->getParams())
    sig.Params.push_back(paramPIRType(p.ParamType, p.Mode));
  return sig;
}

pir::Function *ModuleLowering::declareFunctionPrototype(ast::FuncDecl *node) {
  if (auto it = FuncByName.find(node->getName()); it != FuncByName.end())
    return it->second;
  pir::Function *fn =
      getOrCreateFunction(node->getName(), functionSignature(node));
  size_t idx = 0;
  for (auto &p : fn->Params)
    p.Name = node->getParams()[idx++].getName();
  return fn;
}

// -- Declarations

pir::Function *ModuleLowering::getOrCreateFunction(const std::string &name,
                                                   const pir::Signature &sig) {
  if (auto it = FuncByName.find(name); it != FuncByName.end()) {
    assert(it->second->Sig == sig &&
           "function declared twice with different signatures");
    return it->second;
  }
  Funcs.emplace_back();
  pir::Function &fn = Funcs.back();
  fn.Name = name;
  fn.Sig = sig;
  for (Type t : sig.Params)
    fn.Params.push_back(pir::Value{fn.NextValueId++, t, ""});
  FuncByName[name] = &fn;
  return &fn;
}

const pir::Function &ModuleLowering::declareRuntime(const std::string &symbol) {
  // Runtime externs have their own table and their own PIR names
  // (`$rt.<symbol>`, PIR.h), so a program function spelled like a runtime
  // symbol (`fn PaykanString_new`) is never mistaken for it (#117).
  if (auto it = RuntimeFuncs.find(symbol); it != RuntimeFuncs.end())
    return *it->second;
  const RuntimeSig *rs = findRuntimeSig(symbol);
  assert(rs && "unknown runtime symbol");
  Funcs.emplace_back();
  pir::Function &fn = Funcs.back();
  fn.Name = pir::runtimeName(symbol);
  fn.Sig = rs->Sig;
  fn.IsExtern = true;
  RuntimeFuncs[symbol] = &fn;
  return fn;
}

Val ModuleLowering::callRuntime(const std::string &symbol,
                                const std::vector<Val> &args,
                                std::string resultName) {
  const pir::Function &fn = declareRuntime(symbol);
  assert(fn.Sig.Params.size() == args.size() && "runtime call arity mismatch");
  return B.call(fn.Name, fn.Sig, args, std::move(resultName));
}

Val ModuleLowering::externGlobal(const std::string &symbol,
                                 pir::ExternGlobal::Kind kind) {
  std::string name = pir::runtimeName(symbol);
  if (ExternGlobals.insert(name).second)
    Mod.Externs.push_back({name, kind});
  return Val::symbol(name,
                     kind == pir::ExternGlobal::Object ? Type::Obj : Type::Ptr);
}

Val ModuleLowering::externObject(const std::string &symbol) {
  return externGlobal(symbol, pir::ExternGlobal::Object);
}

Val ModuleLowering::externVTable(const std::string &symbol) {
  return externGlobal(symbol, pir::ExternGlobal::VTable);
}

pir::Function *ModuleLowering::declareExternFrom(const pir::Function &fn,
                                                 const std::string &modulePath,
                                                 const std::string &localName) {
  // One declaration per (defining module, symbol), however many qualifiers
  // (`y::tag`, `r::tag`, `path::to::y::tag`) reach it.
  std::string key = modulePath + '\n' + fn.Name;
  if (auto it = ExternByOrigin.find(key); it != ExternByOrigin.end())
    return it->second;
  // The local name is the one the call sites use: qualified for a function
  // (so it cannot clash with this module's own `tag` or another module's),
  // the plain symbol for a class's generated function (class names are
  // global).  Should it still be taken, any free name will do: calls refer
  // to the declaration, and backends link through Module + Symbol.
  std::string name = localName;
  for (unsigned n = 1; FuncByName.count(name); ++n)
    name = localName + kExternDupSep + std::to_string(n);
  pir::Function *ext = getOrCreateFunction(name, fn.Sig);
  ext->IsExtern = true;
  ext->Module = modulePath;
  if (name != fn.Name)
    ext->Symbol = fn.Name;
  ext->Params.clear();
  ExternByOrigin[key] = ext;
  return ext;
}

pir::Function *ModuleLowering::lookupFunction(const std::string &name) {
  if (auto it = FuncByName.find(name); it != FuncByName.end())
    return it->second;
  auto imp = ImportedFunctions.find(name);
  if (imp == ImportedFunctions.end())
    return nullptr;
  auto modIt = PL.ByName.find(imp->second.Module);
  if (modIt == PL.ByName.end())
    return nullptr;
  const pir::Function *def = modIt->second->findFunction(imp->second.Plain);
  if (!def || def->IsExtern)
    return nullptr;
  return declareExternFrom(*def, imp->second.Module, name);
}

pir::Function *ModuleLowering::lookupClassFunction(ast::ClassType *ct,
                                                   const std::string &symbol) {
  if (auto it = FuncByName.find(symbol); it != FuncByName.end())
    return it->second;
  auto origin = PL.ClassOrigins.find(ct->getName());
  if (origin == PL.ClassOrigins.end() || origin->second == Mod.Name)
    return nullptr;
  auto modIt = PL.ByName.find(origin->second);
  if (modIt == PL.ByName.end())
    return nullptr;
  const pir::Function *def = modIt->second->findFunction(symbol);
  if (!def || def->IsExtern)
    return nullptr;
  return declareExternFrom(*def, origin->second, symbol);
}

Val ModuleLowering::internString(const std::string &content) {
  auto it = InternedStrings.find(content);
  if (it != InternedStrings.end())
    return Val::symbol(it->second, Type::Ptr);
  std::string name =
      std::string(kStrGlobalName) + std::to_string(Mod.CStrs.size());
  Mod.CStrs.push_back({name, content});
  InternedStrings[content] = name;
  return Val::symbol(name, Type::Ptr);
}

Val ModuleLowering::wrapStringLiteral(const Val &rawStr, size_t len) {
  Val str = callRuntime(kPaykanStringNew,
                        {rawStr, Val::i64(static_cast<int64_t>(len))}, "str");
  // The freshly-built PaykanString* is an owned temporary until it is boxed
  // or explicitly consumed/destroyed.
  trackStringTemp(str);
  return str;
}

void ModuleLowering::bootstrapBuiltins() {
  auto reg = [&](const char *paykanName, const char *runtimeName) {
    const RuntimeSig *rs = findRuntimeSig(runtimeName);
    assert(rs);
    FunctionTable[paykanName] = {runtimeName, rs->Sig};
  };
  reg(kPrint, kPaykanPrint);
  reg(kPrintln, kPaykanPrintln);
  reg(kErrPrint, kPaykanErrPrint);
  reg(kErrPrintln, kPaykanErrPrintln);
  // Conversions with a runtime implementation, by the spelled callee Sema
  // rebinds them to (the numeric ones are inline, see emitConversion).
  reg(kConvStrInt, kPaykanStringFromInt);
  reg(kConvStrFloat, kPaykanStringFromFloat);
  reg(kConvStrBool, kPaykanStringFromBool);
  reg(kConvStrChar, kPaykanStringFromChar);
  reg(kConvIntStr, kPaykanIntFromStr);
  reg(kConvFloatStr, kPaykanFloatFromStr);
  reg(kConvBoolStr, kPaykanBoolFromStr);
  // The boxed forms (#88).  A boxed source is unboxed (emitBuiltinCall,
  // conversionUnboxer) and formatted like its primitive; a boxed target is
  // the same parse, whose `int?` & co. box already is the `Int?` & co.
  reg(kConvStrIntBox, kPaykanStringFromInt);
  reg(kConvStrFloatBox, kPaykanStringFromFloat);
  reg(kConvStrBoolBox, kPaykanStringFromBool);
  reg(kConvStrCharBox, kPaykanStringFromChar);
  reg(kConvIntBoxStr, kPaykanIntFromStr);
  reg(kConvFloatBoxStr, kPaykanFloatFromStr);
  reg(kConvBoolBoxStr, kPaykanBoolFromStr);
  reg(kOpen, kPaykanFileOpen);
  IdentityCtors.insert(kString);
}

// -- Entry point

bool ModuleLowering::run(ast::TranslationUnit *tu) {
  CurrentScope = nullptr;
  bootstrapBuiltins();
  processImports(tu);
  visit(tu);
  return !HadInternalError;
}

Val ModuleLowering::visitTranslationUnit(ast::TranslationUnit *node) {
  // Declare every free function and every class (struct layout, vtable,
  // methods, destructor, constructor) before emitting any body, so calls and
  // `__super__` resolve regardless of source order.
  for (auto *fn : node->getFuncDecls())
    declareFunctionPrototype(fn);
  for (auto *cls : node->getClassDecls())
    declareClass(cls);
  for (auto *cls : node->getClassDecls())
    visitClassDecl(cls);
  for (auto *fn : node->getFuncDecls())
    visitFuncDecl(fn);
  return Val();
}

Val ModuleLowering::visitEnumDecl(ast::EnumDecl *) { return Val(); }
Val ModuleLowering::visitMethodDecl(ast::MethodDecl *) { return Val(); }
Val ModuleLowering::visitImportDecl(ast::ImportDecl *) { return Val(); }
Val ModuleLowering::visitClassDecl(ast::ClassDecl *node) {
  return lowerClassDecl(node);
}
Val ModuleLowering::visitMemberAssignStmt(ast::MemberAssignStmt *node) {
  return lowerMemberAssignStmt(node);
}

Val ModuleLowering::emitExpr(ast::Expr *expr) { return Emitter.visit(expr); }

// -- Ownership predicates

bool ModuleLowering::exprAlreadyShared(ast::Expr *expr) const {
  // A primitive boxed for an optional primitive slot: a fresh +1 box.
  if (isPrimitiveBoxing(expr))
    return true;
  if (ast::isa<ast::ArrayLiteralExpr>(expr) ||
      ast::isa<ast::TupleLiteralExpr>(expr))
    return true;
  if (auto *e = ast::dyn_cast<ast::TupleIndexExpr>(expr))
    return e->getResolvedType() && ast::isRefType(e->getResolvedType());
  if (auto *se = ast::dyn_cast<ast::SubscriptExpr>(expr))
    return isObjectElementType(se->getResolvedType()) &&
           exprProducesFreshBox(se->getArray());
  if (auto *e = ast::dyn_cast<ast::MethodCallExpr>(expr))
    return e->getResolvedType() && ast::isRefType(e->getResolvedType());
  if (auto *e = ast::dyn_cast<ast::TernaryExpr>(expr))
    return e->getResolvedType() && ast::isRefType(e->getResolvedType());
  if (auto *e = ast::dyn_cast<ast::MemberAccessExpr>(expr))
    return e->getResolvedType() && ast::isRefType(e->getResolvedType());
  if (auto *ce = ast::dyn_cast<ast::CallExpr>(expr)) {
    if (!ce->getResolvedType() || !ast::isRefType(ce->getResolvedType()))
      return false;
    if (FunctionTable.count(ce->getCalleeName()) ||
        IdentityCtors.count(ce->getCalleeName())) {
      if (ce->getCalleeName() == kOpen ||
          isParseConversion(ce->getCalleeName()))
        return true;
      return false;
    }
    return true;
  }
  return false;
}

bool ModuleLowering::exprProducesFreshBox(ast::Expr *expr) const {
  if (isPrimitiveBoxing(expr))
    return true;
  if (auto *mae = ast::dyn_cast<ast::MemberAccessExpr>(expr))
    return mae->getResolvedType() && ast::isRefType(mae->getResolvedType()) &&
           exprProducesFreshBox(mae->getReceiver());
  if (auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(expr))
    return ti->getResolvedType() && ast::isRefType(ti->getResolvedType()) &&
           exprProducesFreshBox(ti->getTuple());
  if (ast::isa<ast::SubscriptExpr>(expr))
    return exprAlreadyShared(expr);
  if (auto *mce = ast::dyn_cast<ast::MethodCallExpr>(expr))
    return mce->getResolvedType() && ast::isRefType(mce->getResolvedType());
  if (auto *te = ast::dyn_cast<ast::TernaryExpr>(expr))
    return te->getResolvedType() && ast::isRefType(te->getResolvedType());
  if (ast::isa<ast::ArrayLiteralExpr>(expr) ||
      ast::isa<ast::TupleLiteralExpr>(expr))
    return true;
  if (ast::isa<ast::CallExpr>(expr))
    return exprAlreadyShared(expr);
  return false;
}

Val ModuleLowering::takeSharedOwnership(ast::Expr *expr, const Val &val) {
  // Fresh +1 boxes are owned as-is; a borrowed box (plain ref-typed field
  // read) is retained because its slot keeps its own reference.
  if (!exprProducesFreshBox(expr))
    emitRetain(val);
  return val;
}

Val ModuleLowering::emitSharedNew(const Val &raw, std::string name) {
  // Boxing transfers ownership of a raw string temporary to the box.
  untrackStringTemp(raw);
  // PaykanShared_new has create-OR-acquire semantics (unique-box invariant,
  // see Runtime.h), so boxing a raw value is always safe.
  return B.box(raw, std::move(name));
}

Val ModuleLowering::emitSharedGet(const Val &shared, std::string name) {
  return B.unbox(shared, std::move(name));
}

Val ModuleLowering::emitUnwrappedRef(ast::Expr *expr, std::string name) {
  Val val = emitExpr(expr);
  if (!val)
    return val;
  if (exprAlreadyShared(expr) && val.Ty == Type::Box)
    val = emitSharedGet(val, std::move(name));
  return val;
}

void ModuleLowering::trackStringTemp(const Val &v) {
  if (v && v.isValue())
    OwnedStringTemps.insert(v.id());
}

void ModuleLowering::untrackStringTemp(const Val &v) {
  if (v && v.isValue())
    OwnedStringTemps.erase(v.id());
}

bool ModuleLowering::isTrackedStringTemp(const Val &v) const {
  return v && v.isValue() && OwnedStringTemps.count(v.id());
}

void ModuleLowering::destroyStringTempIfOwned(const Val &v) {
  if (!isTrackedStringTemp(v))
    return;
  OwnedStringTemps.erase(v.id());
  if (B.isTerminated())
    return;
  callRuntime(kPaykanStringDestroy, {v});
}

ExprValue ModuleLowering::classifyExpr(ast::Expr *expr, const Val &val) const {
  if (!val)
    return ExprValue();
  if (isTrackedStringTemp(val))
    return ExprValue::owned(val);
  if (exprAlreadyShared(expr) && val.Ty == Type::Box &&
      exprProducesFreshBox(expr))
    return ExprValue::owned(val);
  return ExprValue::borrowed(val);
}

void ModuleLowering::releaseIfOwned(const ExprValue &ev) {
  if (!ev.isOwned() || !ev.V)
    return;
  if (isTrackedStringTemp(ev.V)) {
    destroyStringTempIfOwned(ev.V);
    return;
  }
  emitRelease(ev.V);
}

Val ModuleLowering::promoteIntToFloat(const Val &v, ast::Type *targetTy) {
  if (v && targetTy == ASTCtx.getFloatTy() && v.Ty == Type::I64) {
    if (auto *c = std::get_if<int64_t>(&v.Op.V))
      return Val::f64(static_cast<double>(*c));
    return B.itof(v, kInt2FPName);
  }
  return v;
}

Val ModuleLowering::coerceBoolToI64(const Val &v, Type dest) {
  if (v.Ty == Type::Bool && dest == Type::I64) {
    if (auto *c = std::get_if<bool>(&v.Op.V))
      return Val::i64(*c ? 1 : 0);
    return B.cast(v, Type::I64, "boolext");
  }
  return v;
}

Val ModuleLowering::coerceTo(const Val &v, Type dest) {
  Val out = v;
  if (dest == Type::F64 && v.Ty == Type::I64)
    out = promoteIntToFloat(out, ASTCtx.getFloatTy());
  return coerceBoolToI64(out, dest);
}

ModuleLowering::Receiver ModuleLowering::emitReceiver(ast::Expr *expr,
                                                      const std::string &name) {
  Receiver r;
  Val recv = emitExpr(expr);
  if (!recv)
    return r;
  r.Owned = classifyExpr(expr, recv);
  r.Raw = recv;
  if (exprAlreadyShared(expr) && recv.Ty == Type::Box)
    r.Raw = emitSharedGet(recv, name);
  return r;
}

// -- Scope cleanup

void ModuleLowering::emitScopeCleanup(Scope &scope) {
  // Reverse declaration order (LIFO).
  for (auto it = scope.DeclOrder.rbegin(); it != scope.DeclOrder.rend(); ++it) {
    assert(it->ASTType && ast::isRefType(it->ASTType) &&
           "DeclOrder must only contain ref-typed variables");
    emitRelease(B.load(it->Local));
  }
  for (auto it = scope.PendingReleases.rbegin();
       it != scope.PendingReleases.rend(); ++it)
    emitRelease(*it);
}

void ModuleLowering::emitAllScopesCleanup() {
  for (Scope *s = CurrentScope; s != nullptr; s = s->Parent)
    emitScopeCleanup(*s);
}

void ModuleLowering::emitLoopScopesCleanup() {
  if (LoopStack.empty())
    return;
  Scope *stop = LoopStack.back().EnclosingScope;
  for (Scope *s = CurrentScope; s != nullptr && s != stop; s = s->Parent)
    emitScopeCleanup(*s);
}

// -- Statements

Val ModuleLowering::visitCompoundStmt(ast::CompoundStmt *node) {
  ScopeGuard guard(*this);
  for (auto *stmt : node->getStatements())
    visit(stmt);
  return Val();
}

void ModuleLowering::emitBody(ast::CompoundStmt *body) {
  for (auto *stmt : body->getStatements())
    visit(stmt);
}

Val ModuleLowering::visitDeclStmt(ast::DeclStmt *node) {
  return visit(node->getDecl());
}

Val ModuleLowering::visitExprStmt(ast::ExprStmt *node) {
  Val val = emitExpr(node->getExpr());
  // An owned result bound to nothing (`A();`, a bare string temporary) is
  // torn down immediately.
  releaseIfOwned(classifyExpr(node->getExpr(), val));
  return val;
}

Val ModuleLowering::emitImplicitVarDecl(const std::string &name,
                                        ast::Expr *rhsExpr, const Val &valIn) {
  Val val = valIn;
  // Determine the AST type for the RHS (for isOwned / method dispatch).
  ast::Type *rhsAstTy = resolveExprClassType(rhsExpr);
  bool isPointer = val.Ty == Type::Box || val.Ty == Type::Obj;
  if (!rhsAstTy && isPointer) {
    // Not a class: an optional, array or tuple value keeps its precise Sema
    // type; fall back to Obj when nothing recorded a type.
    ast::Type *rt = rhsExpr->getResolvedType();
    if (!(rt && ast::isa<ast::OptionalType>(rt)))
      if (auto *id = ast::dyn_cast<ast::Identifier>(rhsExpr))
        rt = CurrentScope->lookupASTType(id->getName());
    rhsAstTy = rt && ast::isRefType(rt)
                   ? canonicalizeDeclType(rt)
                   : static_cast<ast::Type *>(ASTCtx.getObjTy());
  }

  if (isPointer && rhsAstTy) {
    if (exprAlreadyShared(rhsExpr)) {
      // Already a box: take ownership without re-emitting the expression.
      val = takeSharedOwnership(rhsExpr, val);
    } else if (auto *id = ast::dyn_cast<ast::Identifier>(rhsExpr);
               id && holdsBox(id->getName())) {
      // A variable holding a box: visitIdentifier already unwrapped it; load
      // the box itself and retain it.
      val = loadVarBox(id->getName());
      emitRetain(val);
    } else {
      // Raw value — box it.  A fresh temporary gets its first box; an alias
      // of an already-boxed object (`self`, a match binding, the immortal
      // `None` singleton of an untyped `x = None`) acquires the existing one.
      val = emitSharedNew(val, "shared");
    }
  }

  pir::LocalId local = B.addLocal(name, val.Ty);
  B.store(local, val);
  CurrentScope->declare(name, local, rhsAstTy);
  return val;
}

void ModuleLowering::emitClassVarRebind(const std::string &name,
                                        ast::Expr *rhsExpr, const Val &val) {
  Val newBox;
  // `x = None` for an optional variable: the null box.
  if (isNoneForOptional(rhsExpr))
    newBox = Val::null(Type::Box);
  // RHS is another owned variable — retain its box (share the reference).
  if (auto *ident = ast::dyn_cast<ast::Identifier>(rhsExpr)) {
    if (holdsBox(ident->getName())) {
      newBox = loadVarBox(ident->getName());
      emitRetain(newBox);
    }
  }
  if (!newBox) {
    if (exprAlreadyShared(rhsExpr))
      newBox = takeSharedOwnership(rhsExpr, val);
    else
      // A freshly-produced raw value already computed as `val`: box it (re-
      // emitting would evaluate the RHS twice).  A borrowed array element
      // never gets here: visitAssignStmt acquires it with emitAsShared.
      newBox = emitSharedNew(val, "new.box");
  }
  // `T?` into an `Obj` variable: never store a NULL box in an `Obj`.
  newBox = emitOptionalToObj(rhsExpr, newBox);

  // Release the old box (null-safe) and store the new one.
  storeVarBox(name, newBox);
}

Val ModuleLowering::visitAssignStmt(ast::AssignStmt *node) {
  // `x = arr[i]` into an owned ref variable: acquire the element's box
  // straight from the array.  Evaluating the value first and then again for
  // the ownership handling would run the array and index expressions twice.
  if (auto *owner = CurrentScope->findOwner(node->getVarName());
      owner && holdsBox(node->getVarName()) &&
      isBorrowedObjectElement(node->getValue())) {
    auto *astTy = CurrentScope->lookupASTType(node->getVarName());
    if (astTy && ast::isRefType(astTy)) {
      ast::Expr *rhsExpr = node->getValue();
      Val newBox = emitAsShared(rhsExpr);
      if (!newBox)
        return newBox;
      storeVarBox(node->getVarName(), newBox);
      ast::ClassType *rhsCT = resolveExprClassType(rhsExpr);
      if (rhsCT && !ast::isa<ast::OptionalType>(astTy))
        CurrentScope->updateASTType(node->getVarName(), rhsCT);
      return newBox;
    }
  }

  Val val = emitExpr(node->getValue());
  if (!val)
    return val;

  auto *owner = CurrentScope->findOwner(node->getVarName());
  if (!owner)
    return emitImplicitVarDecl(node->getVarName(), node->getValue(), val);

  pir::LocalId local = owner->lookup(node->getVarName());
  Type localTy = B.localType(local);

  // Implicit int -> float promotion.
  if (localTy == Type::F64 && val.Ty == Type::I64)
    val = promoteIntToFloat(val, ASTCtx.getFloatTy());

  auto *astTy = CurrentScope->lookupASTType(node->getVarName());
  if (astTy && ast::isRefType(astTy)) {
    ast::ClassType *rhsCT = resolveExprClassType(node->getValue());
    // Every assignable ref-typed variable owns its box (match-arm bindings
    // included) or is an `inout` parameter, a reference to the caller's;
    // only `self` is unowned, and Sema rejects assigning to it.
    if (!holdsBox(node->getVarName())) {
      reportInternalError("assignment to unowned variable '" +
                          node->getVarName() + "'");
      return val;
    }
    emitClassVarRebind(node->getVarName(), node->getValue(), val);
    // Narrow the scope type to the concrete RHS type (vtable dispatch through
    // base-typed variables).  An optional variable keeps its optional type.
    if (rhsCT && !ast::isa<ast::OptionalType>(astTy))
      CurrentScope->updateASTType(node->getVarName(), rhsCT);
    return val;
  }

  if (storeInout(node->getVarName(), local, val))
    return val;
  val = coerceBoolToI64(val, localTy);
  B.store(local, val);
  return val;
}

Val ModuleLowering::visitReturnStmt(ast::ReturnStmt *node) {
  if (node->getReturnValue()) {
    auto *retExpr = node->getReturnValue();
    Val val;
    if (CurrentFuncReturnASTType && ast::isRefType(CurrentFuncReturnASTType)) {
      val = emitAsShared(retExpr);
    } else {
      val = emitExpr(retExpr);
      if (B.function())
        val = coerceTo(val, B.function()->Sig.Ret);
    }
    emitAllScopesCleanup();
    B.emitRet(val);
    return Val();
  }
  emitAllScopesCleanup();
  B.emitRetVoid();
  return Val();
}

Val ModuleLowering::visitIfStmt(ast::IfStmt *node) {
  Val cond = emitExpr(node->getCondition());
  if (!cond)
    return Val();
  pir::If *s = B.openIf(cond, node->hasElse());
  B.enter(*s->Then);
  visit(node->getThenBranch());
  B.leave();
  if (node->hasElse()) {
    B.enter(*s->Else);
    visit(node->getElseBranch());
    B.leave();
  }
  return Val();
}

Val ModuleLowering::visitWhileStmt(ast::WhileStmt *node) {
  pir::While *w = B.openWhile();
  LoopStack.push_back({CurrentScope});
  B.enter(*w->CondBlock);
  Val cond = emitExpr(node->getCondition());
  B.setWhileCond(w, cond);
  B.leave();
  B.enterLoopBody(*w->Body);
  visit(node->getBody());
  B.leaveLoopBody();
  LoopStack.pop_back();
  return Val();
}

Val ModuleLowering::visitBreakStmt(ast::BreakStmt *) {
  assert(!LoopStack.empty() && "break outside loop");
  emitLoopScopesCleanup();
  B.emitBreak();
  return Val();
}

Val ModuleLowering::visitContinueStmt(ast::ContinueStmt *) {
  assert(!LoopStack.empty() && "continue outside loop");
  emitLoopScopesCleanup();
  B.emitContinue();
  return Val();
}

Val ModuleLowering::visitVarDecl(ast::VarDecl *node) {
  if (node->getMode() == ast::ParamMode::Inout)
    return emitInoutLocal(node);
  ast::Type *declTy = canonicalizeDeclType(node->getType());
  Type pirTy = declTy ? toPIRType(declTy) : Type::Void;

  Val initVal;
  if (node->getInitExpr()) {
    // Every ref-typed variable holds an owned box: a class/array/tuple value,
    // an optional (`None` is the null box), and `o: Obj = None`, which boxes
    // the immortal None singleton (acquire semantics; its destroy is a
    // no-op, so the box is the only allocation).
    bool isClassDecl = declTy && ast::isRefType(declTy);
    if (isClassDecl) {
      initVal = emitAsShared(node->getInitExpr());
    } else {
      initVal = emitExpr(node->getInitExpr());
      if (pirTy == Type::Void)
        pirTy = initVal.Ty;
      initVal = coerceTo(initVal, pirTy);
    }
  }
  if (pirTy == Type::Void)
    pirTy = Type::I64;

  pir::LocalId local = B.addLocal(node->getName(), pirTy);
  if (initVal)
    B.store(local, initVal);
  else {
    switch (pirTy) {
    case Type::F64:
      B.store(local, Val::f64(0.0));
      break;
    case Type::Bool:
      B.store(local, Val::boolean(false));
      break;
    case Type::Char:
      B.store(local, Val::chr(0));
      break;
    case Type::Box:
    case Type::Obj:
      B.store(local, Val::null(pirTy));
      break;
    default:
      B.store(local, Val::i64(0));
      break;
    }
  }

  ast::Type *scopeTy = declTy;
  // Narrow the scope type to the concrete RHS class (base-typed declarations
  // dispatch through the concrete vtable convention).
  if (node->getInitExpr() && scopeTy && ast::isa<ast::ClassType>(scopeTy)) {
    ast::ClassType *rhsCT = resolveExprClassType(node->getInitExpr());
    if (rhsCT && rhsCT != ASTCtx.getObjTy() && rhsCT != ASTCtx.getStrTy())
      scopeTy = rhsCT;
  }
  CurrentScope->declare(node->getName(), local, scopeTy);
  return Val();
}

void ModuleLowering::emitImplicitReturn(const pir::Signature &sig) {
  if (B.isTerminated())
    return;
  switch (sig.Ret) {
  case Type::Void:
    B.emitRetVoid();
    break;
  case Type::F64:
    B.emitRet(Val::f64(0.0));
    break;
  case Type::Bool:
    B.emitRet(Val::boolean(false));
    break;
  case Type::Char:
    B.emitRet(Val::chr(0));
    break;
  case Type::Box:
  case Type::Obj:
    B.emitRet(Val::null(sig.Ret));
    break;
  case Type::Ptr:
  case Type::I64:
    B.emitRet(Val::i64(0));
    break;
  }
}

Val ModuleLowering::visitFuncDecl(ast::FuncDecl *node) {
  pir::Function *fn = declareFunctionPrototype(node);
  ast::Type *retAstTy = canonicalizeDeclType(node->getReturnType());
  FunctionStateGuard fnState(*this, fn, retAstTy, /*methodClassTy=*/nullptr);
  {
    ScopeGuard guard(*this);
    for (size_t i = 0; i < fn->Params.size(); ++i)
      declareParam(node->getParams()[i].getName(), fn->Params[i],
                   canonicalizeDeclType(node->getParams()[i].ParamType));
    if (node->isNative())
      emitNativeBody(node, *fn);
    else
      emitBody(node->getBody());
  }
  emitImplicitReturn(fn->Sig);
  return Val();
}

// A `native fn` (#198) is an ordinary function whose body calls its C symbol
// with the runtime builtins' convention: a reference argument is borrowed as
// the raw object, a reference result is an owned box.  Its own parameters
// are released by the scope cleanup, as in any function.
void ModuleLowering::emitNativeBody(ast::FuncDecl *node,
                                    const pir::Function &fn) {
  pir::Signature csig;
  csig.Ret = fn.Sig.Ret;
  std::vector<Val> args;
  for (const ast::Param &p : node->getParams()) {
    Val v = B.load(CurrentScope->lookup(p.getName()), p.getName());
    if (v.Ty == Type::Box)
      v = emitSharedGet(v, p.getName() + ".obj");
    csig.Params.push_back(v.Ty);
    args.push_back(v);
  }
  const std::string name = pir::nativeName(node->getNativeSymbol());
  if (!FuncByName.count(name)) {
    Funcs.emplace_back();
    pir::Function &ext = Funcs.back();
    ext.Name = name;
    ext.Sig = csig;
    ext.IsExtern = true;
    FuncByName[name] = &ext;
  }
  Val result = B.call(name, csig, args, "r");
  emitAllScopesCleanup();
  if (csig.Ret == Type::Void)
    B.emitRetVoid();
  else
    B.emitRet(result);
}

// -- Optionals

bool ModuleLowering::isNoneForOptional(ast::Expr *expr) {
  return ast::isa<ast::NoneLiteral>(expr) && expr->getResolvedType() &&
         ast::isa<ast::OptionalType>(expr->getResolvedType());
}

bool ModuleLowering::isPrimitiveBoxing(const ast::Expr *expr) {
  return ast::needsPrimitiveBoxing(expr->getCoercedType(),
                                   expr->getResolvedType());
}

Val ModuleLowering::emitPrimitiveBox(const Val &v, ast::Type *optTy) {
  auto *inner = ast::cast<ast::BuiltinType>(ast::stripOptional(optTy));
  const char *ctor = nullptr;
  Val arg = v;
  switch (inner->getTypeKind()) {
  case ast::BuiltinType::Int:
    ctor = kPaykanIntNew;
    break;
  case ast::BuiltinType::Float:
    ctor = kPaykanFloatNew;
    arg = promoteIntToFloat(arg, ASTCtx.getFloatTy()); // `float? = 3`
    break;
  case ast::BuiltinType::Bool:
    ctor = kPaykanBoolNew;
    arg = coerceBoolToI64(arg, Type::I64);
    break;
  case ast::BuiltinType::Char:
    ctor = kPaykanCharNew;
    break;
  case ast::BuiltinType::Void:
    break;
  }
  if (!ctor) {
    reportInternalError("boxing a value of type '" + ast::typeName(inner) +
                        "'");
    return Val();
  }
  Val raw = callRuntime(ctor, {arg}, "opt.prim");
  return emitSharedNew(raw, "opt.prim.box");
}

Val ModuleLowering::emitPrimitiveUnbox(const Val &rawObj, ast::Type *innerTy,
                                       const std::string &name) {
  auto *inner = ast::cast<ast::BuiltinType>(innerTy);
  switch (inner->getTypeKind()) {
  case ast::BuiltinType::Int:
    return callRuntime(kPaykanIntValue, {rawObj}, name);
  case ast::BuiltinType::Float:
    return callRuntime(kPaykanFloatValue, {rawObj}, name);
  case ast::BuiltinType::Bool: {
    Val bits = callRuntime(kPaykanBoolValue, {rawObj}, name);
    return B.cmp(pir::CmpPred::Ne, bits, Val::i64(0), name);
  }
  case ast::BuiltinType::Char:
    return callRuntime(kPaykanCharValue, {rawObj}, name);
  case ast::BuiltinType::Void:
    break;
  }
  reportInternalError("unboxing a value of type '" + ast::typeName(inner) +
                      "'");
  return Val();
}

static bool isOptionalToObjCoercion(ast::Expr *expr) {
  ast::Type *rt = expr->getResolvedType();
  return expr->getCoercedType() && rt && ast::isa<ast::OptionalType>(rt) &&
         !ast::isa<ast::OptionalType>(expr->getCoercedType());
}

Val ModuleLowering::emitOptionalToObj(ast::Expr *expr, const Val &box) {
  if (!box || !isOptionalToObjCoercion(expr))
    return box;
  // None: hand the `Obj` slot a +1 box of the immortal None singleton.
  pir::LocalId tmp = B.addLocal("opt.box", Type::Box);
  B.store(tmp, box);
  Val isNone = B.cmp(pir::CmpPred::Eq, box, Val::null(Type::Box), "opt.isnone");
  pir::If *s = B.openIf(isNone, false);
  B.enter(*s->Then);
  Val noneBox = emitSharedNew(externObject(kPaykanObjectNone), "opt.none.box");
  B.store(tmp, noneBox);
  B.leave();
  return B.load(tmp, "opt.box");
}

Val ModuleLowering::emitOptionalToObjRaw(ast::Expr *expr, const Val &raw) {
  if (!raw || !isOptionalToObjCoercion(expr))
    return raw;
  Val isNone = B.cmp(pir::CmpPred::Eq, raw, Val::null(Type::Obj), "opt.isnone");
  return B.select(isNone, externObject(kPaykanObjectNone), raw, "opt.obj");
}

Val ModuleLowering::emitOptionalEquality(ast::BinaryExpr *node) {
  ast::Expr *lhs = node->getLHS();
  ast::Expr *rhs = node->getRHS();

  // `x == None` / `None == x`: a null check on whatever form the optional
  // takes (a box, or the raw object of an unwrapped variable / element).
  bool lhsNone = ast::isa<ast::NoneLiteral>(lhs);
  bool rhsNone = ast::isa<ast::NoneLiteral>(rhs);
  if (lhsNone || rhsNone) {
    ast::Expr *opt = lhsNone ? rhs : lhs;
    Val v = emitExpr(opt);
    if (!v)
      return v;
    ExprValue ev = classifyExpr(opt, v);
    Val isNone = B.cmp(pir::CmpPred::Eq, v, Val::null(v.Ty), "opt.isnone");
    releaseIfOwned(ev);
    return isNone;
  }

  // Two optionals: +1 boxes of both (null-safe); either None -> equal iff
  // both None; both present -> lhs.equals(rhs) through the vtable (the
  // callee consumes the rhs box), release lhs.
  Val a = emitAsShared(lhs);
  Val b = emitAsShared(rhs);
  if (!a || !b)
    return Val();
  Val aNone = B.cmp(pir::CmpPred::Eq, a, Val::null(Type::Box), "opt.isnone");
  Val bNone = B.cmp(pir::CmpPred::Eq, b, Val::null(Type::Box), "opt.isnone");
  Val anyNone = B.select(aNone, Val::boolean(true), bNone, "opt.anynone");
  Val bothNone = B.select(aNone, bNone, Val::boolean(false), "opt.bothnone");

  pir::LocalId result = B.addLocal("opt.eq", Type::Bool);
  pir::If *s = B.openIf(anyNone, true);
  B.enter(*s->Then);
  emitRelease(a);
  emitRelease(b);
  B.store(result, bothNone);
  B.leave();
  B.enter(*s->Else);
  Val rawA = B.unbox(a, "lhs.obj");
  int slot = ASTCtx.getObjTy()->getVTableIndex(kMethodEquals);
  assert(slot >= 0 && "Obj must have an equals slot");
  pir::Signature eqSig{{Type::Obj, Type::Box}, Type::I64};
  Val eqI64 =
      B.vcall(rawA, "", static_cast<uint32_t>(slot), eqSig, {b}, "mcall");
  Val eqSome = B.cmp(pir::CmpPred::Ne, eqI64, Val::i64(0), "eq");
  emitRelease(a);
  B.store(result, eqSome);
  B.leave();
  return B.load(result, "eq");
}

Val ModuleLowering::emitAsShared(ast::Expr *expr) {
  // `None` into a `T?` slot is the null box (no allocation at all).
  if (isNoneForOptional(expr))
    return Val::null(Type::Box);
  Val box = emitAsSharedRaw(expr);
  // `T?` into an `Obj` slot: never hand an `Obj` a NULL box.
  return emitOptionalToObj(expr, box);
}

bool ModuleLowering::isBorrowedObjectElement(ast::Expr *expr) const {
  auto *se = ast::dyn_cast<ast::SubscriptExpr>(expr);
  if (!se || !se->getResolvedType() || !ast::isRefType(se->getResolvedType()))
    return false;
  if (auto *id = ast::dyn_cast<ast::Identifier>(se->getArray()))
    if (CurrentScope &&
        CurrentScope->lookupASTType(id->getName()) == ASTCtx.getStrTy())
      return false;
  return !exprAlreadyShared(se);
}

Val ModuleLowering::emitAsSharedRaw(ast::Expr *expr) {
  // Owned identifier: retain + return the existing box.
  if (auto *id = ast::dyn_cast<ast::Identifier>(expr)) {
    bool declared = CurrentScope->hasLocal(id->getName());
    if (declared && holdsBox(id->getName())) {
      Val sharedPtr = loadVarBox(id->getName());
      emitRetain(sharedPtr);
      return sharedPtr;
    }
  }

  // Object array element (arr[i]) of ref type: the array owns one reference
  // per slot, so acquiring the element retains the stored box.  The index
  // and the array are each evaluated once, in the order visitSubscriptExpr
  // uses (index first).
  if (isBorrowedObjectElement(expr)) {
    auto *se = ast::cast<ast::SubscriptExpr>(expr);
    Val idx = emitExpr(se->getIndex());
    idx = coerceBoolToI64(idx, Type::I64);
    Val arrRaw = emitUnwrappedRef(se->getArray());
    Val bits = callRuntime(kPaykanArrayGet, {arrRaw, idx}, "elem.raw");
    Val box = B.cast(bits, Type::Box, "elem.shared");
    emitRetain(box);
    return box;
  }

  // Ref-typed field (obj.field) or tuple element (t.1): the slot owns its
  // box, so a new owner retains it (unless the read was call-rooted and
  // already handed us a +1).
  if (ast::isa<ast::MemberAccessExpr>(expr) ||
      ast::isa<ast::TupleIndexExpr>(expr)) {
    if (expr->getResolvedType() && ast::isRefType(expr->getResolvedType())) {
      Val box = emitExpr(expr);
      return takeSharedOwnership(expr, box);
    }
  }

  // Call/ternary that already returns a box — pass through.
  if (exprAlreadyShared(expr))
    return emitExpr(expr);

  // Everything else (a string literal is already a Str temporary): emit
  // raw, then box (acquire semantics cover aliases of already-boxed objects).
  Val val = emitExpr(expr);
  return emitSharedNew(val);
}

} // namespace paykan::lowering
