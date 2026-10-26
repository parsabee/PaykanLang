// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// AST -> PIR lowering: scopes, ownership classification, ARC placement,
// statements.  Expressions are in LoweringExpr.cpp, `match` in
// LoweringMatch.cpp, classes in LoweringClass.cpp, and the module/import
// driver in LoweringProgram.cpp.

#include "LoweringInternal.h"
#include "Names.h"
#include "NativeABI.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>

namespace paykan::lowering {

using namespace names;
using pir::Type;

// -- Constructor

ModuleLowering::ModuleLowering(ProgramLowering &program,
                               const sema::SemaContext &semaCtx,
                               std::string moduleName, std::string canonical)
    : PL(program), Canonical(std::move(canonical)), ASTCtx(*semaCtx.ASTCtx),
      SemaCtx(semaCtx), Errs(program.Errs) {
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

bool ModuleLowering::Scope::isWeak(const std::string &name) const {
  if (WeakVars.count(name))
    return true;
  if (Locals.count(name))
    return false;
  return Parent ? Parent->isWeak(name) : false;
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
  if (memHoldsRefs(astTy))
    MemVars.push_back({local, astTy});
}

void ModuleLowering::Scope::declareWeak(const std::string &name,
                                        pir::LocalId local, ast::Type *astTy) {
  Locals[name] = local;
  ASTTypeMap[name] = astTy;
  WeakVars.insert(name);
  DeclOrder.push_back({local, astTy, true});
}

void ModuleLowering::Scope::declareUnowned(const std::string &name,
                                           pir::LocalId local,
                                           ast::Type *astTy) {
  Locals[name] = local;
  if (astTy)
    ASTTypeMap[name] = astTy;
}

bool ModuleLowering::Scope::isStable(const std::string &name) const {
  if (Locals.count(name))
    return StableVars.count(name) > 0;
  return Parent ? Parent->isStable(name) : false;
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
  L.emitParamPrologue(*fn);
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
  return sema::pirType(ASTCtx, ty);
}

// static
bool ModuleLowering::isFunctionValueExpr(const ast::Expr *expr) {
  if (ast::isa<ast::LambdaExpr>(expr))
    return true;
  if (auto *id = ast::dyn_cast<ast::Identifier>(expr))
    return id->isFunctionRef();
  if (auto *ev = ast::dyn_cast<ast::EnumValueExpr>(expr))
    return ev->getFunctionRef() != nullptr;
  return false;
}

bool ModuleLowering::isObjectElementType(ast::Type *elemTy) const {
  if (!elemTy || ast::isFunctionValue(elemTy) || ast::isMemOptional(elemTy))
    return false;
  if (ast::isa<ast::EnumType>(elemTy) || ast::isa<ast::StructType>(elemTy))
    return false;
  if (auto *ct = ast::dyn_cast<ast::ClassType>(elemTy))
    return ASTCtx.lookupEnumType(ct->getName()) == nullptr &&
           ASTCtx.lookupStructType(ct->getName()) == nullptr;
  // An optional value is stored inline (#96), the optional of a reference
  // as its box.
  return ast::isa<ast::OptionalType>(elemTy) && !ast::isValueOptional(elemTy);
}

ast::Type *ModuleLowering::canonicalizeDeclType(ast::Type *ty) {
  if (!ty)
    return ty;
  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty)) {
    if (auto *et = ASTCtx.lookupEnumType(ct->getName()))
      return et;
    if (auto *st = ASTCtx.lookupStructType(ct->getName()))
      return st;
    if (auto *canonical = ASTCtx.lookupClassType(ct->getName()))
      return canonical;
  }
  // A struct by its name, never rebuilt from its fields (before the tuple
  // case, which it is too).
  if (auto *st = ast::dyn_cast<ast::StructType>(ty)) {
    if (auto *canonical = ASTCtx.lookupStructType(st->getName()))
      return canonical;
    return st;
  }
  if (auto *tt = ast::dyn_cast<ast::TupleType>(ty)) {
    std::vector<ast::Type *> elems;
    elems.reserve(tt->getArity());
    for (ast::Type *e : tt->getElementTypes())
      elems.push_back(canonicalizeDeclType(e));
    return ASTCtx.getTupleType(std::move(elems));
  }
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(ty)) {
    return ASTCtx.getOptionalType(canonicalizeDeclType(ot->getInnerType()));
  }
  return ty;
}

ast::ClassType *ModuleLowering::resolveExprClassType(ast::Expr *expr) {
  ast::Type *resolved = nullptr;
  if (auto *ce = ast::dyn_cast<ast::CallExpr>(expr))
    resolved = ce->getResolvedType();
  else if (auto *mce = ast::dyn_cast<ast::MethodCallExpr>(expr))
    resolved = mce->getResolvedType();
  else if (auto *cv = ast::dyn_cast<ast::CallValueExpr>(expr))
    resolved = cv->getResolvedType();
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
  const bool inoutResult = node->getResultMode() == ast::ParamMode::Inout;
  if (inoutResult)
    sig.Ret = Type::Ptr; // the address of the storage it names
  for (auto &p : node->getParams())
    appendParamTypes(sig.Params, p.ParamType, p.Mode);
  // A result held in memory is written where a last parameter points (but
  // an `inout` one is its storage's address, like any other).
  if (!inoutResult && ast::isMemType(retAstTy)) {
    sig.Params.push_back(Type::Ptr);
    sig.Ret = Type::Void;
  }
  return sig;
}

pir::Function *ModuleLowering::declareFunctionPrototype(ast::FuncDecl *node) {
  if (auto it = FuncByName.find(node->getName()); it != FuncByName.end())
    return it->second;
  pir::Function *fn =
      getOrCreateFunction(node->getName(), functionSignature(node));
  nameParams(*fn, 0, node->getParams());
  if (fn->Sig.Ret != Type::Ptr && ast::isMemType(node->getReturnType()))
    fn->Params.back().Name = "result"; // where the result is written
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
  const pir::Signature *sig = pir::runtimeSignature(symbol);
  assert(sig && "unknown runtime symbol");
  Funcs.emplace_back();
  pir::Function &fn = Funcs.back();
  fn.Name = pir::runtimeName(symbol);
  fn.Sig = *sig;
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
  ext->Shared = fn.Shared;
  if (name != fn.Name)
    ext->Symbol = fn.Name;
  ext->Params.clear();
  ExternByOrigin[key] = ext;
  return ext;
}

pir::Function *ModuleLowering::lookupFunction(const std::string &name) {
  if (auto it = FuncByName.find(name); it != FuncByName.end())
    return it->second;
  // The module that defines it, and its symbol there.
  const pir::Module *def = nullptr;
  std::string symbol = name;
  auto byName = [&](const std::string &module) {
    auto it = PL.ByName.find(module);
    return it == PL.ByName.end() ? nullptr : it->second;
  };
  if (auto imp = ImportedFunctions.find(name); imp != ImportedFunctions.end()) {
    def = byName(imp->second.Module);
    symbol = imp->second.Plain;
  } else if (auto fo = PL.FuncOrigins.find(name); fo != PL.FuncOrigins.end()) {
    def = byName(fo->second); // another module emits this instantiation
  } else if (const std::string *co = typeOrigin(name)) {
    def = byName(*co); // the constructor of an instantiation there
  } else if (name.starts_with(kHiddenQualPrefix)) {
    // `$<module>::f`, named in the body of that module's template, or
    // `$<module>::T::f` for a `static fn` of one of its types: the longest
    // prefix that is a module.
    std::string_view prefix(kHiddenQualPrefix);
    for (size_t sep = name.rfind(kQualSep, name.find('<'));
         sep != std::string::npos && sep > prefix.size() && !def;
         sep = name.rfind(kQualSep, sep - 1)) {
      auto it =
          PL.ByCanonical.find(name.substr(prefix.size(), sep - prefix.size()));
      if (it == PL.ByCanonical.end())
        continue;
      def = it->second;
      symbol = name.substr(sep + std::string_view(kQualSep).size());
    }
  }
  if (!def || def->Name == Mod.Name)
    return nullptr;
  const pir::Function *fn = def->findFunction(symbol);
  if (!fn || !def->defines(*fn))
    return nullptr;
  noteExternUse(name);
  return declareExternFrom(*fn, def->Name, name);
}

void ModuleLowering::noteExternUse(const std::string &name) {
  if (!Canonical.empty() && PL.Resolver &&
      PL.ForeignInstantiations.count(name) && !DependencyOrigins.count(name))
    PL.Resolver->keepUncached(Canonical);
}

const std::string *ModuleLowering::typeOrigin(const std::string &type) {
  if (auto it = TypeOrigins.find(type); it != TypeOrigins.end())
    return it->second;
  if (type.find('<') != std::string::npos) {
    // Breadth first from the direct imports: each module once.
    std::vector<std::string> queue = DirectImports;
    std::unordered_set<std::string> seen(queue.begin(), queue.end());
    for (size_t i = 0; i < queue.size(); ++i) {
      auto mod = PL.ByCanonical.find(queue[i]);
      if (mod != PL.ByCanonical.end() && mod->second) {
        const pir::Module &m = *mod->second;
        const pir::Class *c = m.findClass(type);
        const pir::Function *ctor = m.findFunction(type);
        if ((c && m.defines(*c)) || (!c && ctor && m.defines(*ctor))) {
          DependencyOrigins.insert(type);
          return TypeOrigins[type] = &m.Name;
        }
      }
      if (auto deps = PL.ImportsOf.find(queue[i]); deps != PL.ImportsOf.end())
        for (const std::string &d : deps->second)
          if (seen.insert(d).second)
            queue.push_back(d);
    }
  }
  // Not memoised: the type may yet be defined by a module lowered later.
  auto origin = PL.ClassOrigins.find(type);
  return origin == PL.ClassOrigins.end() ? nullptr : &origin->second;
}

pir::Function *ModuleLowering::lookupClassFunction(ast::ClassType *ct,
                                                   const std::string &symbol) {
  if (auto it = FuncByName.find(symbol); it != FuncByName.end())
    return it->second;
  const std::string *origin = typeOrigin(ct->getName());
  if (!origin || *origin == Mod.Name || OwnTypes.count(ct->getName()))
    return nullptr;
  auto modIt = PL.ByName.find(*origin);
  if (modIt == PL.ByName.end())
    return nullptr;
  const pir::Function *def = modIt->second->findFunction(symbol);
  if (!def || !modIt->second->defines(*def))
    return nullptr;
  noteExternUse(ct->getName());
  return declareExternFrom(*def, *origin, symbol);
}

pir::Function *ModuleLowering::lookupTypeFunction(const std::string &typeName,
                                                  const std::string &symbol) {
  if (auto it = FuncByName.find(symbol); it != FuncByName.end())
    return it->second;
  const std::string *origin = typeOrigin(typeName);
  if (!origin || *origin == Mod.Name || OwnTypes.count(typeName))
    return nullptr;
  auto modIt = PL.ByName.find(*origin);
  if (modIt == PL.ByName.end())
    return nullptr;
  const pir::Function *def = modIt->second->findFunction(symbol);
  if (!def || !modIt->second->defines(*def))
    return nullptr;
  noteExternUse(typeName);
  return declareExternFrom(*def, *origin, symbol);
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
    const pir::Signature *sig = pir::runtimeSignature(runtimeName);
    assert(sig);
    FunctionTable[paykanName] = {runtimeName, *sig};
  };
  // Conversions with a runtime implementation, by the spelled callee Sema
  // rebinds them to (the numeric ones are inline, see emitConversion):
  // `Str<T>` for every numeric T, bool and char.
  for (unsigned k = 0; k < ast::BuiltinType::kNumKinds; ++k) {
    const auto kind = static_cast<ast::BuiltinType::Kind>(k);
    if (!ast::BuiltinType::isNumericKind(kind))
      continue;
    const std::string spelled =
        std::string(kString) + "<" + ast::builtinKindName(kind) + ">";
    reg(spelled.c_str(), toStringRuntime(kind));
  }
  reg(kConvStrBool, kPaykanStringFromBool);
  reg(kConvStrChar, kPaykanStringFromChar);
  IdentityCtors.insert(kString);
}

// -- Entry point

bool ModuleLowering::run(ast::TranslationUnit *tu) {
  CurrentScope = nullptr;
  bootstrapBuiltins();
  processImports(tu);
  visit(tu);
  markShared();
  return !HadInternalError;
}

Val ModuleLowering::visitTranslationUnit(ast::TranslationUnit *node) {
  // Declare every free function, every struct (methods, constructor) and
  // every class (struct layout, vtable, methods, destructor, constructor)
  // before emitting any body, so calls and `__super__` resolve regardless
  // of source order, and read where the bodies store into class fields
  // before any is emitted (scanFieldStores).
  std::vector<ast::FuncDecl *> funcs = node->getFuncDecls();
  funcs.insert(funcs.end(), ForeignFuncs.begin(), ForeignFuncs.end());
  std::vector<ast::ClassDecl *> classes = node->getClassDecls();
  classes.insert(classes.end(), ForeignClasses.begin(), ForeignClasses.end());
  std::vector<ast::StructDecl *> structs = node->getStructDecls();
  structs.insert(structs.end(), ForeignStructs.begin(), ForeignStructs.end());
  indexSubclasses(classes);
  for (auto *fn : funcs) {
    declareFunctionPrototype(fn);
    if (fn->isNative())
      NativeDecls[fn->getName()] = fn;
    else
      Bodies[fn->getName()] = fn;
  }
  for (auto *sd : structs)
    declareStruct(sd);
  for (auto *cls : classes)
    declareClass(cls);
  scanFieldStores(node);
  for (auto *sd : structs)
    visitStructDecl(sd);
  for (auto *cls : classes)
    visitClassDecl(cls);
  for (auto *fn : funcs)
    visitFuncDecl(fn);
  return Val();
}

Val ModuleLowering::visitEnumDecl(ast::EnumDecl *) { return Val(); }
Val ModuleLowering::visitImplDecl(ast::ImplDecl *) { return Val(); }
Val ModuleLowering::visitStructDecl(ast::StructDecl *node) {
  return lowerStructDecl(node);
}
Val ModuleLowering::visitTraitDecl(ast::TraitDecl *) { return Val(); }
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
  // An optional of a value type is a value, never a box.
  if (valueOptionalType(expr))
    return false;
  // A `weak` variable reads as a fresh +1 box (or null): its slot holds a
  // weak reference, never a box to borrow; so does a held read of a
  // reference (noteHeldReads).
  if (auto *id = ast::dyn_cast<ast::Identifier>(expr))
    return (CurrentScope && CurrentScope->isWeak(id->getName())) ||
           (isHeldRead(id) && ast::isRefType(expr->getResolvedType()));
  // An array's (or a slice's or a tuple's) element slot holds the box
  // itself.
  if (ast::isa<ast::SubscriptExpr>(expr) || ast::isa<ast::TupleIndexExpr>(expr))
    return ast::isRefType(expr->getResolvedType());
  if (auto *e = ast::dyn_cast<ast::MethodCallExpr>(expr))
    return e->getResolvedType() && ast::isRefType(e->getResolvedType());
  // A call through a function value returns a +1 box, as a function does.
  if (auto *e = ast::dyn_cast<ast::CallValueExpr>(expr))
    return e->getResolvedType() && ast::isRefType(e->getResolvedType());
  if (auto *e = ast::dyn_cast<ast::TernaryExpr>(expr))
    return e->getResolvedType() && ast::isRefType(e->getResolvedType());
  if (auto *e = ast::dyn_cast<ast::MemberAccessExpr>(expr))
    return e->getResolvedType() && ast::isRefType(e->getResolvedType());
  if (auto *ce = ast::dyn_cast<ast::CallExpr>(expr)) {
    if (!ce->getResolvedType() || !ast::isRefType(ce->getResolvedType()))
      return false;
    // `Str(s)` is its argument (emitIdentityCtor): a box when `s` is one.
    if (IdentityCtors.count(ce->getCalleeName()) && ce->getNumArguments() == 1)
      return exprAlreadyShared(ce->getArguments()[0]);
    if (FunctionTable.count(ce->getCalleeName()))
      return false;
    // A function's result, and `Str(x)` of a value with a text, which is
    // `x.toString()`'s (showCallOf): a +1 box.
    return true;
  }
  return false;
}

bool ModuleLowering::exprProducesFreshBox(ast::Expr *expr) const {
  if (valueOptionalType(expr))
    return false;
  // A held read of a reference is retained (noteHeldReads,
  // noteBorrowedReads), and a `weak` variable or field is loaded as a fresh
  // +1 box (or null).
  if (isHeldRead(expr) && ast::isRefType(expr->getResolvedType()))
    return true;
  if (auto *id = ast::dyn_cast<ast::Identifier>(expr))
    return CurrentScope && CurrentScope->isWeak(id->getName());
  if (isWeakFieldAccess(expr))
    return true;
  // A struct's field is an element of a value held in memory, like a
  // tuple's; a class's is in its object.
  auto structField = [](const ast::Expr *e) {
    auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(e);
    return ma && ast::isa<ast::StructType>(
                     ma->getReceiver()->getResolvedType()); // null-safe
  };
  if (auto *mae = ast::dyn_cast<ast::MemberAccessExpr>(expr);
      mae && !structField(expr))
    return mae->getResolvedType() && ast::isRefType(mae->getResolvedType()) &&
           exprProducesFreshBox(mae->getReceiver());
  if (ast::isa<ast::SubscriptExpr>(expr) ||
      ast::isa<ast::TupleIndexExpr>(expr) || structField(expr)) {
    // An element of a temporary (`mk()[0]`, `mk().1`, `mk().x` of a
    // struct, or a held read's copy, noteHeldReads), or of a fresh object's
    // field (`mk().xs[0]`), is read as a +1 box: the temporary dies with the
    // read.  A call with an `inout` result is no temporary: its element is
    // borrowed from the storage the call names.
    ast::Expr *root = expr;
    for (;;) {
      if (auto *se = ast::dyn_cast<ast::SubscriptExpr>(root))
        root = se->getArray();
      else if (auto *ti = ast::dyn_cast<ast::TupleIndexExpr>(root))
        root = ti->getTuple();
      else if (structField(root))
        root = ast::cast<ast::MemberAccessExpr>(root)->getReceiver();
      else
        break;
    }
    if (auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(root))
      return exprAlreadyShared(expr) && exprProducesFreshBox(ma->getReceiver());
    if (root->isInoutResult())
      return false;
    return exprAlreadyShared(expr) &&
           (ast::isa<ast::CallExpr>(root) ||
            ast::isa<ast::MethodCallExpr>(root) ||
            ast::isa<ast::CallValueExpr>(root) ||
            ast::isa<ast::ArrayLiteralExpr>(root) ||
            ast::isa<ast::TupleLiteralExpr>(root) ||
            ast::isa<ast::TernaryExpr>(root) || isHeldRead(root));
  }
  if (auto *mce = ast::dyn_cast<ast::MethodCallExpr>(expr))
    return mce->getResolvedType() && ast::isRefType(mce->getResolvedType());
  if (auto *cv = ast::dyn_cast<ast::CallValueExpr>(expr))
    return cv->getResolvedType() && ast::isRefType(cv->getResolvedType());
  if (auto *te = ast::dyn_cast<ast::TernaryExpr>(expr))
    return te->getResolvedType() && ast::isRefType(te->getResolvedType());
  if (auto *ce = ast::dyn_cast<ast::CallExpr>(expr)) {
    // `Str(s)` is its argument, fresh when the argument is.
    if (IdentityCtors.count(ce->getCalleeName()) && ce->getNumArguments() == 1)
      return exprProducesFreshBox(ce->getArguments()[0]);
    return exprAlreadyShared(expr);
  }
  return false;
}

Val ModuleLowering::takeSharedOwnership(ast::Expr *expr, const Val &val,
                                        bool lend) {
  // Fresh +1 boxes are owned as-is; a borrowed box (plain ref-typed field
  // read) is retained because its slot keeps its own reference.
  if (!exprProducesFreshBox(expr))
    emitRetain(val, lend && isLentRead(expr));
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
  callRuntime(kPaykanDispose, {v});
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

ast::BuiltinType::Kind ModuleLowering::builtinKind(ast::Type *ty) {
  if (auto *bt = ty ? ast::dyn_cast<ast::BuiltinType>(ty) : nullptr)
    return bt->getTypeKind();
  return ast::BuiltinType::Void;
}

const char *ModuleLowering::toStringRuntime(ast::BuiltinType::Kind k) {
  if (k == ast::BuiltinType::U64)
    return kPaykanStringFromUint;
  if (ast::BuiltinType::isFloatKind(k))
    return kPaykanStringFromFloat;
  if (k == ast::BuiltinType::Bool)
    return kPaykanStringFromBool;
  if (k == ast::BuiltinType::Char)
    return kPaykanStringFromChar;
  return kPaykanStringFromInt;
}

bool ModuleLowering::conversionTypes(const std::string &callee,
                                     ast::Type *&target,
                                     ast::Type *&source) const {
  const size_t open = callee.find('<');
  if (open == std::string::npos || callee.back() != '>')
    return false;
  target = ASTCtx.lookupType(callee.substr(0, open));
  source = ASTCtx.lookupType(callee.substr(open + 1, callee.size() - open - 2));
  auto scalar = [&](ast::Type *t) {
    return t && (ast::isa<ast::BuiltinType>(t) || t == ASTCtx.getStrTy());
  };
  return scalar(target) && scalar(source);
}

Val ModuleLowering::canonicalize(const Val &v, ast::Type *ty) {
  using ast::BuiltinType;
  if (!v)
    return v;
  const BuiltinType::Kind k = builtinKind(ty);
  const unsigned bits = BuiltinType::integerBits(k);
  if (BuiltinType::isIntegerKind(k) && bits < 64) {
    const bool isUnsigned = BuiltinType::isUnsignedKind(k);
    if (auto *c = std::get_if<int64_t>(&v.Op.V)) {
      const uint64_t lowMask = (1ULL << bits) - 1;
      const uint64_t low = static_cast<uint64_t>(*c) & lowMask;
      if (!isUnsigned && ((low >> (bits - 1)) & 1))
        return Val::i64(static_cast<int64_t>(low | ~lowMask));
      return Val::i64(static_cast<int64_t>(low));
    }
    return isUnsigned ? B.mask(v, bits, "mask") : B.wrap(v, bits, "wrap");
  }
  if (k == BuiltinType::F32) {
    if (auto *c = std::get_if<double>(&v.Op.V)) {
      // Beyond the f32 maximum plus half an ulp a value rounds to infinity
      // (the C++ conversion of such a value is undefined).
      if (std::fabs(*c) >= 3.4028235677973366e+38)
        return Val::f64(
            std::copysign(std::numeric_limits<double>::infinity(), *c));
      return Val::f64(static_cast<double>(static_cast<float>(*c)));
    }
    return B.fround32(v, "f32");
  }
  if (k == BuiltinType::F16)
    return callRuntime(kPaykanRoundF16, {v}, "f16");
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
  // An `inout` arm's binding goes back to its subject first: what it holds
  // and the object of a field it names are released below.
  for (auto it = scope.WriteBacks.rbegin(); it != scope.WriteBacks.rend(); ++it)
    emitWriteBack(*it);
  // Reverse declaration order (LIFO).
  for (auto it = scope.DeclOrder.rbegin(); it != scope.DeclOrder.rend(); ++it) {
    assert(it->ASTType && ast::isRefType(it->ASTType) &&
           "DeclOrder must only contain ref-typed variables");
    if (it->Weak)
      B.weakRelease(B.load(it->Local));
    else
      emitRelease(B.load(it->Local));
  }
  for (auto it = scope.MemVars.rbegin(); it != scope.MemVars.rend(); ++it)
    emitMemRelease(B.localAddr(it->Local, "mem.addr"), it->ASTType);
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

Val ModuleLowering::visit(ast::ASTNode *node) {
  if (auto *s = ast::dyn_cast<ast::Stmt>(node)) {
    noteHeldReads(s);
    noteBorrowedReads(s);
  }
  return ASTVisitor::visit(node);
}

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
  // A result held in memory bound to nothing: its references go now.
  if (ast::isMemType(node->getExpr()->getResolvedType())) {
    size_t mark = MemTemps.size();
    emitMemAddress(node->getExpr());
    releaseMemTemps(mark);
    return Val();
  }
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
    // Not a class: an optional keeps its precise Sema type; fall back to Obj
    // when nothing recorded a type.
    ast::Type *rt = rhsExpr->getResolvedType();
    // A function used as a value (`g = square;`) is no variable: its
    // resolved function type stands.
    if (!(rt && ast::isa<ast::OptionalType>(rt)))
      if (auto *id = ast::dyn_cast<ast::Identifier>(rhsExpr);
          id && !id->isFunctionRef())
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

  // An optional value keeps its type (a value, not an owned variable).
  if (!rhsAstTy && pir::isOptional(val.Ty))
    rhsAstTy = canonicalizeDeclType(valueOptionalType(rhsExpr));

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

  // Release the old box (null-safe) and store the new one; a `weak`
  // variable takes a weak reference to what the new box names.
  if (CurrentScope->isWeak(name))
    storeWeakVar(name, newBox);
  else
    storeVarBox(name, newBox);
}

Val ModuleLowering::visitAssignStmt(ast::AssignStmt *node) {
  // A value held in memory: a new variable holding a copy, or its bytes
  // replaced.
  if (ast::Type *ty = node->getValue()->getResolvedType(); ast::isMemType(ty)) {
    const std::string &name = node->getVarName();
    if (!CurrentScope->findOwner(name)) {
      declareMemVar(name, ty, node->getValue());
      return Val();
    }
    pir::LocalId local = CurrentScope->lookup(name);
    Val dst = CurrentScope->inoutType(name) != Type::Void
                  ? B.load(local, name + ".addr")
                  : B.localAddr(local, name + ".addr");
    emitMemAssign(dst, node->getValue());
    return Val();
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
    // included), holds a weak reference, or is an `inout` parameter, a
    // reference to the caller's; only `self` is unowned, and Sema rejects
    // assigning to it.
    if (!holdsBox(node->getVarName()) &&
        !CurrentScope->isWeak(node->getVarName())) {
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
  // A result held in memory: written where the last parameter points (a
  // temporary of the caller's, which nothing here can reach otherwise).
  if (node->getReturnValue() && ast::isMemType(CurrentFuncReturnASTType) &&
      B.function()->Sig.Ret != Type::Ptr) {
    const pir::Value &dst = B.function()->Params.back();
    emitMemInit(node->getReturnValue(),
                Val(pir::Operand::value(dst), Type::Ptr));
    emitAllScopesCleanup();
    B.emitRetVoid();
    return Val();
  }
  if (node->getReturnValue()) {
    auto *retExpr = node->getReturnValue();
    Val val;
    if (B.function() && B.function()->Sig.Ret == Type::Ptr) {
      val = emitPlaceAddress(retExpr); // an `inout` result
    } else if (CurrentFuncReturnASTType &&
               ast::isRefType(CurrentFuncReturnASTType)) {
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

// A range loop (docs/language/02-basics.md, "for") is a while loop over the
// values the range fixes before it starts:
//
//   first = START; end = END; step = STEP         ; once, in order
//   step == 0: panic                              ; a step not a constant
//   more = step > 0 ? first < end : first > end   ; <= / >= for `..=`
//   last = first + (dist - 1) / |step| * step     ; dist - 0 for `..=`
//   cur = first
//   while more { i = cur; more = cur != last; cur = cur + step; BODY }
//
// dist is the distance from first to end, which fits a u64 whenever the
// range is not empty, so the count is exact even where a hand-written
// `i <= end` would wrap forever (`for b: u8 in 0..=255`).  The variable is
// set before the body, so `continue` (to the condition) moves on, and the
// loop ends on its last value without stepping past it.
Val ModuleLowering::visitForStmt(ast::ForStmt *node) {
  using pir::CmpPred;
  using pir::Opcode;
  ScopeGuard guard(*this);
  ast::Type *ty = node->getVar()->getType();
  const bool isUnsigned = ast::BuiltinType::isUnsignedKind(builtinKind(ty));
  auto compare = [&](CmpPred pred, const Val &a, const Val &b) {
    return isUnsigned ? B.ucmp(pred, a, b, "for.has")
                      : B.cmp(pred, a, b, "for.has");
  };
  Val first = emitExpr(node->getStart());
  Val end = first ? emitExpr(node->getEnd()) : Val();
  if (!end)
    return Val();
  const std::optional<int64_t> constant = node->constantStep();
  Val step = constant ? Val::i64(*constant) : emitExpr(node->getStep());
  if (!step)
    return Val();
  if (!constant) {
    Val zero = B.cmp(CmpPred::Eq, step, Val::i64(0), "for.zero");
    pir::If *z = B.openIf(zero, false);
    B.enter(*z->Then);
    callRuntime(kPaykanPanicForStep, {});
    B.emitUnreachable();
    B.leave();
  }

  // Whether the range has a value, the distance to its end and the step's
  // magnitude, for the direction the step runs in.
  const bool inclusive = node->isInclusive();
  struct Direction {
    Val More, Dist, Magnitude;
  };
  auto upward = [&] {
    return Direction{compare(inclusive ? CmpPred::Le : CmpPred::Lt, first, end),
                     B.binary(Opcode::Sub, end, first, "for.dist"), step};
  };
  auto downward = [&] {
    return Direction{compare(inclusive ? CmpPred::Ge : CmpPred::Gt, first, end),
                     B.binary(Opcode::Sub, first, end, "for.dist"),
                     B.unary(Opcode::Neg, step, "for.mag")};
  };
  Direction d;
  if (constant) {
    d = *constant > 0 ? upward() : downward();
  } else {
    Direction up = upward();
    Direction down = downward();
    Val isUp = B.cmp(CmpPred::Gt, step, Val::i64(0), "for.up");
    d = {B.select(isUp, up.More, down.More, "for.has"),
         B.select(isUp, up.Dist, down.Dist, "for.dist"),
         B.select(isUp, up.Magnitude, down.Magnitude, "for.mag")};
  }
  // The index of the last value (the distance over the magnitude, without
  // the end for `..`), and the value.
  Val lastIndex =
      inclusive ? d.Dist : B.binary(Opcode::Sub, d.Dist, Val::i64(1));
  const bool unit = constant && (*constant == 1 || *constant == -1);
  if (!unit)
    lastIndex = B.binary(Opcode::UDiv, lastIndex, d.Magnitude, "for.lastidx");
  Val last = unit
                 ? B.binary(*constant == 1 ? Opcode::Add : Opcode::Sub, first,
                            lastIndex, "for.last")
                 : B.binary(Opcode::Add, first,
                            B.binary(Opcode::Mul, lastIndex, step), "for.last");

  pir::LocalId more = B.addLocal("for.more", Type::Bool);
  B.store(more, d.More);
  pir::LocalId cur = B.addLocal("for.cur", Type::I64);
  B.store(cur, first);
  // `for _` has no variable: only the counter.
  const std::string &name = node->getVar()->getName();
  std::optional<pir::LocalId> var;
  if (!name.empty()) {
    var = B.addLocal(name, Type::I64);
    B.store(*var, first);
    CurrentScope->declare(name, *var, ty);
  }

  pir::While *w = B.openWhile();
  LoopStack.push_back({CurrentScope});
  B.enter(*w->CondBlock);
  B.setWhileCond(w, B.load(more, "for.go"));
  B.leave();
  B.enterLoopBody(*w->Body);
  Val value = B.load(cur, var ? name : "for.value");
  if (var)
    B.store(*var, value);
  B.store(more, B.cmp(CmpPred::Ne, value, last, "for.again"));
  B.store(cur, B.binary(Opcode::Add, value, step, "for.next"));
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
  ast::Type *varTy = node->getType();
  if (!varTy && node->getInitExpr())
    varTy = node->getInitExpr()->getResolvedType();
  if (ast::isMemType(varTy)) {
    pir::LocalId local = declareMemVar(
        node->getName(), canonicalizeDeclType(varTy), node->getInitExpr());
    if (node->getMode() == ast::ParamMode::View)
      B.markView(local);
    return Val();
  }
  ast::Type *declTy = canonicalizeDeclType(node->getType());
  Type pirTy = declTy ? toPIRType(declTy) : Type::Void;

  // `weak y: T? = e;`: a weak reference to what the initializer names
  // (none for a missing initializer or `None`), in a `weak` local.
  if (node->isWeak()) {
    Val box = node->getInitExpr() ? emitAsShared(node->getInitExpr())
                                  : Val::null(Type::Box);
    if (!box)
      return Val();
    Val weak = B.weakNew(box, node->getName() + ".weak");
    if (!box.isNull())
      emitRelease(box); // the +1 the initializer handed over
    pir::LocalId local = B.addLocal(node->getName(), Type::Weak);
    B.store(local, weak);
    CurrentScope->declareWeak(node->getName(), local, declTy);
    return Val();
  }

  Val initVal;
  if (node->getInitExpr()) {
    // Every ref-typed variable holds an owned box: a class value,
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
  if (node->getMode() == ast::ParamMode::View)
    B.markView(local);
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
    case Type::OptI64:
    case Type::OptF64:
    case Type::OptBool:
    case Type::OptChar:
      B.store(local, B.none(pirTy, "none"));
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
  case Type::OptI64:
  case Type::OptF64:
  case Type::OptBool:
  case Type::OptChar:
    B.emitRet(B.none(sig.Ret));
    break;
  case Type::Mem:  // never a result: the verifier rejects one
  case Type::Weak: // never a result either (a weak is loaded as a box)
    break;
  }
}

Val ModuleLowering::visitFuncDecl(ast::FuncDecl *node) {
  pir::Function *fn = declareFunctionPrototype(node);
  ast::Type *retAstTy = canonicalizeDeclType(node->getReturnType());
  FunctionStateGuard fnState(*this, fn, retAstTy, /*methodClassTy=*/nullptr);
  {
    ScopeGuard guard(*this);
    size_t at = 0;
    for (const ast::Param &p : node->getParams())
      at += declareParam(p.getName(), fn->Params, at,
                         canonicalizeDeclType(p.ParamType), p.Mode);
    if (node->isNative())
      emitNativeBody(node, *fn);
    else
      emitBody(node->getBody());
  }
  emitImplicitReturn(fn->Sig);
  return Val();
}

// A `native fn` (#198) calls its C symbol with the runtime builtins'
// convention: C borrows a reference argument as the raw object and gets an
// `inout` one's address; a reference result is an owned box.  An instance
// of a generic one passes a PaykanType for each type argument first, and
// what its template typed `T` by address: an argument as the address of its
// value (an `inout` one's own), a `-> T` result as the address of a local C
// writes, a `-> view T` result as the address C returns, which is copied.  A
// call in the module that declares it calls C directly, with the caller's
// own references (visitCallExpr); its body serves other modules' callers and
// passes on its parameters, which the scope cleanup releases.
void ModuleLowering::emitNativeBody(ast::FuncDecl *node,
                                    const pir::Function &fn) {
  std::vector<Val> args;
  for (const ast::Param &p : node->getParams()) {
    pir::LocalId local = CurrentScope->lookup(p.getName());
    // A value held in memory: its address (the caller's, for a borrow).
    if (ast::isMemType(p.ParamType) &&
        CurrentScope->inoutType(p.getName()) == Type::Void)
      args.push_back(B.localAddr(local, p.getName() + ".addr"));
    else
      args.push_back(B.load(local, p.getName()));
  }
  Val memResult;
  if (ast::isMemType(node->getReturnType()))
    memResult = Val(pir::Operand::value(fn.Params.back()), Type::Ptr);
  Val result = emitNativeCall(*node, args, memResult);
  emitAllScopesCleanup();
  if (fn.Sig.Ret == Type::Void)
    B.emitRetVoid();
  else
    B.emitRet(result);
}

Val ModuleLowering::emitNativeCall(const ast::FuncDecl &node,
                                   const std::vector<Val> &args,
                                   const Val &memResult) {
  ast::Type *retTy = canonicalizeDeclType(node.getReturnType());
  const Type ret = retTy ? toPIRType(retTy) : Type::Void;
  const bool memRet = ast::isMemType(retTy);
  const bool resultByAddress = node.resultByAddress() && ret != Type::Void;
  const pir::Signature csig = sema::nativeSignature(ASTCtx, node);
  std::vector<Val> cargs;
  for (ast::Type *arg : node.getTypeArgs())
    cargs.push_back(emitTypeDescriptor(canonicalizeDeclType(arg)));
  std::optional<pir::LocalId> out;
  const bool viewResult = node.getResultMode() == ast::ParamMode::View;
  if (resultByAddress && !viewResult) {
    if (memRet) {
      cargs.push_back(memResult);
    } else {
      out = B.addLocal("out", ret);
      cargs.push_back(B.localAddr(*out, "out.addr"));
    }
  }
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string &pname = node.getParams()[i].getName();
    Val v = args[i];
    if (node.getParams()[i].Mode == ast::ParamMode::Inout ||
        ast::isMemType(node.getParams()[i].ParamType)) {
      // Already the address of the caller's storage, or of the value.
    } else if (node.paramByAddress(i)) {
      pir::LocalId value = B.addLocal(pname + ".val", v.Ty);
      B.store(value, v);
      v = B.localAddr(value, pname + ".addr");
    } else if (v.Ty == Type::Box) {
      v = emitSharedGet(v, pname + ".obj");
    }
    cargs.push_back(v);
  }
  bool argsMatch = cargs.size() == csig.Params.size();
  for (size_t i = 0; argsMatch && i < cargs.size(); ++i)
    argsMatch = cargs[i].Ty == csig.Params[i];
  if (!argsMatch) {
    reportInternalError("the arguments of native function '" + node.getName() +
                        "' do not match its C signature");
    return Val();
  }
  const std::string name = pir::nativeName(node.getNativeSymbol());
  auto [it, fresh] = FuncByName.try_emplace(name, nullptr);
  if (fresh) {
    Funcs.emplace_back();
    pir::Function &ext = Funcs.back();
    ext.Name = name;
    ext.Sig = csig;
    ext.IsExtern = true;
    it->second = &ext;
  } else if (it->second->Sig != csig) {
    // Sema gives a C symbol one signature in a module (checkNativeSymbol).
    reportInternalError("native function '" + node.getName() +
                        "' binds C symbol '" + node.getNativeSymbol() +
                        "', which this module declares with another "
                        "signature");
    return Val();
  }
  Val result = B.call(name, csig, cargs, "r");
  if (memRet) {
    // A view result: the value it names, copied.
    if (viewResult) {
      B.memCopy(memResult, result, Val::i64(memLayout(retTy).Size));
      emitMemRetain(memResult, retTy);
    }
    return memResult;
  }
  if (out) {
    result = B.load(*out, "out");
  } else if (resultByAddress) {
    result = B.ptrLoad(result, ret, "r.val");
    if (result.Ty == Type::Box)
      emitRetain(result);
  }
  return result;
}

const ast::FuncDecl *
ModuleLowering::localNative(const std::string &name) const {
  auto it = NativeDecls.find(name);
  return it == NativeDecls.end() ? nullptr : it->second;
}

// A value of every type but a tuple is stored as one PIR slot: a reference
// as its box, None as a null one, an inline optional as its presence flag
// and payload (docs/pir.md §1).  A tuple is its elements in place, each
// reference among them at its offset.  After the references come the words
// that tell storage never set from a value (unsetWords).
Val ModuleLowering::emitTypeDescriptor(ast::Type *ty) {
  // A function value, an optional tuple or struct, or a tagged union is
  // described as its shape, a tuple; under an optional's shape (the presence
  // flag, then the value) a reference is null for None, and in a tagged
  // union's for every variant but the current one: nullable.  What tells a
  // value from storage never set is the type's, not its shape's: an
  // optional function value's shape is a function value's.
  const bool optional =
      ast::isa<ast::OptionalType>(ty) || ast::isPayloadEnum(ty);
  std::vector<int64_t> unset = unsetWords(ty, 0);
  ty = memShapeOf(ty);
  const Type slot = toPIRType(ty);
  std::vector<int64_t> words = {pir::storeSize(slot), pir::storeAlign(slot),
                                nativeTypeKind(ty), 0, 0};
  if (auto *tt = ast::dyn_cast<ast::TupleType>(ty)) {
    MemLayout l = memLayout(tt);
    words = {l.Size, l.Align, kNativeKindTuple, 0, 0};
    std::function<void(ast::TupleType *, int64_t, bool)> refs =
        [&](ast::TupleType *t, int64_t base, bool nullable) {
          for (size_t i = 0; i < t->getArity(); ++i) {
            ast::Type *e = t->getElementType(i);
            int64_t off = base + tupleElementOffset(t, i);
            const bool maybeNull = nullable || ast::isa<ast::OptionalType>(e) ||
                                   ast::isPayloadEnum(e);
            if (auto *nested = ast::dyn_cast<ast::TupleType>(memShapeOf(e))) {
              refs(nested, off, maybeNull);
            } else if (ast::isRefType(e)) {
              ++words[3];
              words.push_back(off << 1 | (maybeNull ? 1 : 0));
            }
          }
        };
    refs(tt, 0, optional);
  } else if (slot == Type::Box) {
    words[3] = 1;
    words.push_back(ast::isa<ast::OptionalType>(ty) ? 1 : 0); // offset 0
  }
  words[4] = static_cast<int64_t>(unset.size());
  words.insert(words.end(), unset.begin(), unset.end());
  std::string key;
  for (int64_t w : words) {
    key += std::to_string(w);
    key += '|';
  }
  auto [it, added] = InternedTypeDescriptors.try_emplace(key);
  if (added) {
    it->second = ".type" + std::to_string(InternedTypeDescriptors.size() - 1);
    Mod.Datas.push_back({it->second, std::move(words)});
  }
  return Val::symbol(it->second, Type::Ptr);
}

// All zero bytes, a buffer's new slot, is a value of every type but a
// reference that may not be None, a function value (its code word is 0 only
// in None) and what holds one where zeros put it.  A tuple or a struct
// holds one in any element: one element's words tell, the fewest.  A tagged
// union holds one when its first variant does, as zeros are that variant:
// its tag and then that variant's words, since a value of another variant
// has a nonzero tag.  Under an optional nothing does: zeros are None.
std::vector<int64_t> ModuleLowering::unsetWords(ast::Type *ty, int64_t base) {
  if (ast::isa<ast::OptionalType>(ty))
    return {};
  if (ast::isFunctionValue(ty))
    return {base + functionCodeOffset()};
  if (ast::isPayloadEnum(ty)) {
    auto *et = ast::cast<ast::EnumType>(ty);
    ast::StructType *first = et->getPayload(0);
    if (!first)
      return {};
    std::vector<int64_t> words =
        unsetWords(first, base + variantPayloadOffset(et, 0));
    if (!words.empty())
      words.insert(words.begin(),
                   base + tupleElementOffset(enumCarrier(et), 0)); // the tag
    return words;
  }
  if (auto *tt = ast::dyn_cast<ast::TupleType>(ty)) {
    std::vector<int64_t> fewest;
    for (size_t i = 0; i < tt->getArity(); ++i) {
      std::vector<int64_t> words =
          unsetWords(tt->getElementType(i), base + tupleElementOffset(tt, i));
      if (!words.empty() && (fewest.empty() || words.size() < fewest.size()))
        fewest = std::move(words);
    }
    return fewest;
  }
  if (ast::isRefType(ty))
    return {base};
  return {};
}

unsigned char ModuleLowering::nativeTypeKind(ast::Type *ty) const {
  if (ast::isa<ast::OptionalType>(ty))
    return kNativeKindOptional;
  if (isObjectElementType(ty))
    return kNativeKindObject;
  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(ty)) {
    switch (bt->getTypeKind()) {
    case ast::BuiltinType::Float:
    case ast::BuiltinType::F16:
    case ast::BuiltinType::F32:
      return kNativeKindFloat;
    case ast::BuiltinType::Bool:
      return kNativeKindBool;
    case ast::BuiltinType::Char:
      return kNativeKindChar;
    case ast::BuiltinType::U64:
      return kNativeKindUInt; // printed unsigned
    case ast::BuiltinType::Int:
    case ast::BuiltinType::I8:
    case ast::BuiltinType::I16:
    case ast::BuiltinType::I32:
    case ast::BuiltinType::U8:
    case ast::BuiltinType::U16:
    case ast::BuiltinType::U32:
    case ast::BuiltinType::Void:
      break;
    }
  }
  return kNativeKindInt; // an int or an enum
}

// -- Optionals

bool ModuleLowering::isNoneForOptional(ast::Expr *expr) {
  return ast::isa<ast::NoneLiteral>(expr) && expr->getResolvedType() &&
         ast::isa<ast::OptionalType>(expr->getResolvedType());
}

bool ModuleLowering::isOptionalWrap(const ast::Expr *expr) {
  return ast::isValueOptional(expr->getCoercedType()) &&
         ast::wrapsInOptional(expr->getCoercedType(), expr->getResolvedType());
}

bool ModuleLowering::isMemOptionalWrap(const ast::Expr *expr) {
  return ast::isMemOptional(expr->getCoercedType()) &&
         ast::wrapsInOptional(expr->getCoercedType(), expr->getResolvedType());
}

ast::Type *ModuleLowering::valueOptionalType(ast::Expr *expr) {
  if (isOptionalWrap(expr))
    return expr->getCoercedType();
  if (ast::isValueOptional(expr->getResolvedType()))
    return expr->getResolvedType();
  return nullptr;
}

Val ModuleLowering::emitSome(const Val &v, ast::Type *optTy) {
  // `f: float? = 3`: the int is promoted first.
  return B.some(promoteIntToFloat(v, ast::stripOptional(optTy)), "some");
}

Val ModuleLowering::emitValueOptionalEquals(const Val &a, const Val &b) {
  // The payload of a None is zero, so comparing it is harmless.
  Val ha = B.isSome(a, "lhs.has");
  Val hb = B.isSome(b, "rhs.has");
  Val same = B.cmp(pir::CmpPred::Eq, ha, hb, "opt.same");
  Val eq = B.cmp(pir::CmpPred::Eq, B.unwrap(a, "lhs.value"),
                 B.unwrap(b, "rhs.value"), "opt.eq");
  Val present = B.select(ha, eq, Val::boolean(true), "opt.eq");
  return B.select(same, present, Val::boolean(false), "eq");
}

Val ModuleLowering::emitOptionalToString(const Val &opt, ast::Type *astTy) {
  // A branch, not a select: only one of the two strings is made.
  pir::LocalId tmp = B.addLocal("opt.str", Type::Obj);
  pir::If *s = B.openIf(B.isSome(opt, "opt.has"), true);
  B.enter(*s->Then);
  Val v = B.unwrap(opt, "opt.value");
  ast::Type *inner = astTy ? ast::stripOptional(astTy) : nullptr;
  if (auto *et = inner ? ast::dyn_cast<ast::EnumType>(inner) : nullptr) {
    // An enum's: its variant's name.
    pir::LocalId tag = B.addLocal("enum", Type::I64);
    B.store(tag, v);
    B.store(tmp, emitEnumToStringAt(B.localAddr(tag, "enum.addr"), et));
  } else {
    const char *format = kPaykanStringFromInt;
    if (inner && builtinKind(inner) == ast::BuiltinType::U64)
      format = kPaykanStringFromUint;
    switch (v.Ty) {
    case Type::F64:
      format = kPaykanStringFromFloat;
      break;
    case Type::Bool:
      format = kPaykanStringFromBool;
      v = coerceBoolToI64(v, Type::I64);
      break;
    case Type::Char:
      format = kPaykanStringFromChar;
      break;
    default:
      break;
    }
    B.store(tmp, callRuntime(format, {v}, "opt.str"));
  }
  B.leave();
  B.enter(*s->Else);
  B.store(tmp,
          callRuntime(kPaykanStringNew,
                      {internString(kNoneText),
                       Val::i64(static_cast<int64_t>(std::strlen(kNoneText)))},
                      "opt.str"));
  B.leave();
  Val str = B.load(tmp, "opt.str");
  trackStringTemp(str);
  return str;
}

Val ModuleLowering::emitNoneObject(const Val &raw) {
  Val isNone = B.cmp(pir::CmpPred::Eq, raw, Val::null(Type::Obj), "opt.isnone");
  return B.select(isNone, externObject(kPaykanObjectNone), raw, "opt.obj");
}

Val ModuleLowering::emitOptionalEquality(ast::BinaryExpr *node) {
  ast::Expr *lhs = node->getLHS();
  ast::Expr *rhs = node->getRHS();

  // `x == None` / `None == x`: a null check on whatever form the optional
  // takes (a box, or the raw object of an unwrapped variable / element), or
  // the presence flag of an optional value.
  bool lhsNone = ast::isa<ast::NoneLiteral>(lhs);
  bool rhsNone = ast::isa<ast::NoneLiteral>(rhs);
  if (lhsNone || rhsNone) {
    ast::Expr *opt = lhsNone ? rhs : lhs;
    // An optional held in memory: no code, or no presence flag.
    if (ast::Type *t = opt->getResolvedType();
        ast::isFunctionValue(t) || ast::isMemOptional(t)) {
      size_t mark = MemTemps.size();
      Val addr = emitMemAddress(opt);
      if (!addr)
        return addr;
      Val isNone = emitMemOptionalIsNone(addr, t);
      releaseMemTemps(mark);
      return isNone;
    }
    Val v = emitExpr(opt);
    if (!v)
      return v;
    if (pir::isOptional(v.Ty))
      return B.unary(pir::Opcode::Not, B.isSome(v, "opt.has"), "opt.isnone");
    ExprValue ev = classifyExpr(opt, v);
    Val isNone = B.cmp(pir::CmpPred::Eq, v, Val::null(v.Ty), "opt.isnone");
    releaseIfOwned(ev);
    return isNone;
  }

  // Two function values, either optional: the same code and closure.
  if (ast::isFunctionValue(lhs->getResolvedType()) ||
      ast::isMemOptional(lhs->getResolvedType()))
    return emitTupleEquals(lhs, rhs);

  // Two optional values: their flags and payloads.
  if (valueOptionalType(lhs) && valueOptionalType(rhs)) {
    Val a = emitExpr(lhs);
    Val b = emitExpr(rhs);
    if (!a || !b)
      return Val();
    if (a.Ty != b.Ty) {
      reportInternalError("comparing optionals of different value types");
      return Val();
    }
    return emitValueOptionalEquals(a, b);
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

Val ModuleLowering::emitAsShared(ast::Expr *expr, bool lend) {
  // `None` into a `T?` slot is the null box (no allocation at all).
  if (isNoneForOptional(expr))
    return Val::null(Type::Box);
  // An optional value is never boxed (Sema lets none reach a box slot).
  if (valueOptionalType(expr)) {
    reportInternalError("an optional value where a box is due");
    return Val();
  }
  return emitAsSharedRaw(expr, lend);
}

bool ModuleLowering::hasOwnershipBit(const pir::Signature &sig, bool lends,
                                     size_t param) {
  return lends && param < sig.Params.size() && sig.Params[param] == Type::Box &&
         pir::boxIndex(sig, param) < pir::kOwnershipBits;
}

Val ModuleLowering::emitBorrowedBox(ast::Expr *arg, std::vector<Val> &keep) {
  if (isNoneForOptional(arg))
    return Val::null(Type::Box);
  // A variable's or a field's own box, when the argument is that reference
  // as it is: it outlives the call, unless a later argument may replace it
  // (noteHeldReads).
  ast::Type *ty = arg->getResolvedType();
  if (ty && ast::isRefType(ty)) {
    auto *id = ast::dyn_cast<ast::Identifier>(arg);
    if (id && CurrentScope->hasLocal(id->getName()) &&
        holdsBox(id->getName()) && !isHeldRead(id))
      return loadVarBox(id->getName(), isStableRead(id));
    auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(arg);
    if (ma && ast::isa<ast::Identifier>(ma->getReceiver())) {
      Val v = emitExpr(arg);
      if (v && classifyExpr(arg, v).isOwned())
        keep.push_back(v);
      return v;
    }
  }
  // A temporary or a conversion: a reference of the caller's own.
  Val box = emitAsShared(arg);
  if (box)
    keep.push_back(box);
  return box;
}

Val ModuleLowering::emitAsSharedRaw(ast::Expr *expr, bool lend) {
  // Owned identifier: retain + return the existing box.  The copy of an
  // argument may be lent (docs/pir.md §6) unless a later part of the
  // statement may replace the variable (noteHeldReads); what an `inout`
  // parameter names, the caller's storage, only as a read of LentReads.
  if (auto *id = ast::dyn_cast<ast::Identifier>(expr)) {
    bool declared = CurrentScope->hasLocal(id->getName());
    if (declared && holdsBox(id->getName())) {
      Val sharedPtr = loadVarBox(id->getName());
      const bool inout = CurrentScope->inoutType(id->getName()) == Type::Box;
      emitRetain(sharedPtr, lend && (inout ? isLentRead(id) : !isHeldRead(id)));
      return sharedPtr;
    }
  }

  // Ref-typed field (obj.field), tuple element (t.1) or array element: the
  // slot owns its box, so a new owner retains it (unless the read was
  // call-rooted and already handed us a +1).
  if (ast::isa<ast::MemberAccessExpr>(expr) ||
      ast::isa<ast::TupleIndexExpr>(expr) ||
      ast::isa<ast::SubscriptExpr>(expr)) {
    if (expr->getResolvedType() && ast::isRefType(expr->getResolvedType())) {
      Val box = emitExpr(expr);
      return takeSharedOwnership(expr, box, lend);
    }
  }

  // A call or a ternary returns a +1 box, passed through; `Str(s)` is its
  // argument's box, retained unless that is a +1 one (`Str(self.v)` is the
  // field's).
  if (exprAlreadyShared(expr)) {
    Val box = emitExpr(expr);
    return box ? takeSharedOwnership(expr, box, lend) : box;
  }

  // Everything else (a string literal is already a Str temporary): emit
  // raw, then box (acquire semantics cover aliases of already-boxed objects).
  Val val = emitExpr(expr);
  return emitSharedNew(val);
}

} // namespace paykan::lowering
