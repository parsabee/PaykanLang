// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Semantic analysis: struct declarations and their method bodies
// (docs/language/14-structs.md).  A struct is a value type with named,
// mutable fields and the methods of its `impl` blocks, which are called
// directly: it has no vtable, no superclass and no subclasses.  Its type is
// an ast::StructType, which is a TupleType of its field types by its name,
// so the lowering treats it as a tuple; Sema keeps it nominal.
//
// A struct's declaration goes through the phases a class's does, in step
// with the classes: its name and a stub come first (declareStructs), so a
// class field or a signature can name it; its fields, `impl` methods,
// `__init__` and constructor function come after the classes
// (populateStructs), so a field can name a class; its method bodies last
// (checkStructBodies), when every function of the module is declared.

#include "ASTClone.h"
#include "Names.h"
#include "Sema.h"
#include "SemaInternal.h"

#include <functional>

namespace paykan {
namespace sema {

const char *Sema::typeKindWord(const ast::Type *ty) {
  return ast::isa<ast::StructType>(ty) ? "struct" : "class";
}

ast::MethodDecl *Sema::methodOf(ast::Type *ty, const std::string &name) {
  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty))
    return ct->findMethod(name);
  if (auto *st = ast::dyn_cast<ast::StructType>(ty))
    return st->findMethod(name);
  return nullptr;
}

bool Sema::declareStructs(ast::TranslationUnit *tu) {
  bool ok = true;
  // A generic struct is a template (registerGenericTemplates): it is
  // instantiated where it is used, never registered as a type itself.
  // A struct whose name is taken is dropped (its name keeps resolving to
  // whatever owns it, and the calls by it are follow-ons); every other one
  // is registered as a stub, fields to come.
  StringSet seen;
  for (auto *sd : tu->getStructDecls()) {
    const std::string &name = sd->getName();
    if (!seen.insert(name).second) {
      error(sd->getLocation(), "redefinition of struct '" + name + "'");
      ok = false;
      continue;
    }
    if (!checkDeclNameAvailable(name, sd->getLocation(), DeclKind::Struct)) {
      ErroneousNames.insert(name);
      ok = false;
      continue;
    }
    Ctx.registerStructType(name, sd->getLocation());
    DeclaredStructs.push_back(sd);
    LocalStructNames.insert(name);
  }
  return ok;
}

namespace {

/// The names of the structs a value of the annotation @p ty holds in place:
/// a struct named outright, or as an element of a tuple or a fixed-size
/// array (an optional, a slice, a class and a type argument hold nothing in
/// place).  @p ty is a parser annotation: a struct is a ClassType stub.
void heldStructNames(const ast::Type *ty, const StringSet &structs,
                     std::vector<std::string> &out) {
  if (const auto *ct = ast::dyn_cast<ast::ClassType>(ty)) {
    if (structs.count(ct->getName()))
      out.push_back(ct->getName());
  } else if (const auto *st = ast::dyn_cast<ast::StructType>(ty)) {
    out.push_back(st->getName());
  } else if (const auto *tt = ast::dyn_cast<ast::TupleType>(ty)) {
    for (const ast::Type *e : tt->getElementTypes())
      heldStructNames(e, structs, out);
  } else if (const auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty)) {
    heldStructNames(ft->getElementType(), structs, out);
  }
}

} // namespace

bool Sema::populateStructs() {
  bool ok = true;
  StringMap<ast::StructDecl *> byName;
  StringSet names;
  for (auto *sd : DeclaredStructs) {
    byName[sd->getName()] = sd;
    names.insert(sd->getName());
  }
  // The structs in an order where every struct a field holds in place comes
  // first, so its size is known when the holder's is computed.  A struct
  // that holds itself, through other structs or not, has no size: it is
  // reported once, where the cycle closes, and left without fields.
  StringMap<uint8_t> color; // 0 = white, 1 = in progress, 2 = done
  std::vector<std::string> path;
  std::function<void(ast::StructDecl *)> visit = [&](ast::StructDecl *sd) {
    const std::string &name = sd->getName();
    if (color[name] == 2)
      return;
    if (color[name] == 1) {
      // The edge that closes the cycle: path holds the structs from the
      // first visit of `name` to the holder.
      size_t start = path.size();
      while (start > 0 && path[start - 1] != name)
        --start;
      std::string through;
      for (size_t i = start; i < path.size(); ++i) {
        if (!through.empty())
          through += ", ";
        through += "'";
        through += path[i];
        through += "'";
      }
      error(sd->getLocation(),
            "struct '" + name + "' contains itself" +
                (through.empty()
                     ? std::string(": a field holds a '" + name + "'")
                     : " (through " + through + ")") +
                "; a struct is held in place, so it cannot hold one of its "
                "own (hold it in a class instead)");
      ErroneousClasses.insert(name);
      ok = false;
      return;
    }
    color[name] = 1;
    path.push_back(name);
    for (auto *field : sd->getFields()) {
      std::vector<std::string> held;
      heldStructNames(field->getType(), names, held);
      for (const std::string &h : held)
        if (auto it = byName.find(h); it != byName.end())
          visit(it->second);
    }
    path.pop_back();
    color[name] = 2;
    SortedStructs.push_back(sd);
  };
  for (auto *sd : DeclaredStructs)
    visit(sd);

  for (auto *sd : SortedStructs) {
    auto *st = Ctx.lookupStructType(sd->getName());
    assert(st && "StructType stub must exist after declareStructs");
    if (ErroneousClasses.count(sd->getName()))
      continue; // it contains itself: reported, no fields
    if (!populateStructType(sd, st)) {
      ok = false;
      ErroneousClasses.insert(sd->getName());
      continue;
    }
    declareConstructor(sd, st);
  }
  return ok;
}

bool Sema::populateStructType(ast::StructDecl *sd, ast::StructType *st) {
  bool ok = true;
  const std::string &name = sd->getName();
  StringSet fieldNames;
  for (auto *field : sd->getFields()) {
    const std::string context =
        "field '" + field->getName() + "' in struct '" + name + "'";
    if (!fieldNames.insert(field->getName()).second) {
      error(field->getLocation(), "duplicate field '" + field->getName() +
                                      "' in struct '" + name + "'");
      ok = false;
      continue;
    }
    // Resolved as a class field is (so a fixed-size array parses), then
    // the array is reported: a struct cannot hold one yet, like a tuple.
    FixedArrayAllowed = true;
    auto *fty = resolveType(field->getType(), field->getLocation(), context);
    if (!fty) {
      ok = false;
      continue;
    }
    if (fty == Ctx.getVoidTy()) {
      error(field->getLocation(), context + " cannot have type 'void'");
      ok = false;
      continue;
    }
    if (holdsFixedArray(fty)) {
      error(field->getLocation(),
            context + " has type '" + typeName(fty) +
                "': a struct field cannot be a fixed-size array yet");
      ok = false;
      continue;
    }
    // A struct it holds in place was populated first (populateStructs);
    // one that failed is reported with its declaration.
    if (auto *held = ast::dyn_cast<ast::StructType>(fty);
        held && ErroneousClasses.count(held->getName())) {
      ++SuppressedFollowOns;
      ok = false;
      continue;
    }
    // A struct held in place cannot hold itself.  The module's own structs
    // are ordered so that a cycle is found before any of them is populated
    // (populateStructs); an instantiation's fields are resolved as they
    // come (its own instantiation resolving to the stub), so the field is
    // checked here, and left out, so that no type holds itself.
    if (holdsStruct(fty, st)) {
      std::string msg = "struct '";
      msg += name;
      msg += "' contains itself: field '";
      msg += field->getName();
      msg += "' holds a '";
      msg += name;
      msg += "'; a struct is held in place, so it cannot hold one of its "
             "own (hold it in a class instead)";
      error(field->getLocation(), msg);
      ok = false;
      continue;
    }
    field->setType(fty); // canonical write-back for the lowering
    st->addField(field->getName(), fty);
    if (!names::isPrivateName(field->getName()) &&
        !isModuleOwnName(sd->getName()) &&
        sd->getName().find('<') == std::string::npos &&
        !checkPublicSurface("field '" + field->getName() + "' of '" +
                                sd->getName() + "' is public but it",
                            "field", fty, field->getLocation()))
      ok = false;
  }
  if (!populateImplMethods(sd, st))
    ok = false;
  if (!st->findMethod(names::kMethodInit) && !declareDefaultInit(sd, st))
    ok = false;
  return ok;
}

bool Sema::populateImplMethods(ast::StructDecl *sd, ast::StructType *st) {
  auto it = ImplsByClass.find(sd->getName());
  if (it == ImplsByClass.end())
    return true;
  bool ok = true;
  for (auto *im : it->second)
    for (auto *fn : im->getMethods()) {
      if (fn->isStatic())
        continue; // the module's function `S::f` (checkImplDecls)
      const std::string &name = fn->getName();
      // `toString` is every struct's (the builtin `Show`): `Point(x: 1,
      // y: 2)`, which nested structs and tuples print too.
      if (name == names::kMethodToString || st->findMethod(name)) {
        error(fn->getLocation(), "struct '" + sd->getName() +
                                     "' already has a method '" + name + "'");
        ok = false;
        continue;
      }
      if (auto *md =
              declareMethod(fn, st, nullptr,
                            name == names::kMethodInit ? ast::MethodDecl::None
                                                       : ast::MethodDecl::Impl,
                            ok)) {
        st->addMethod(md);
        sd->addMethod(fn); // checked and lowered with the struct
      } else {
        ok = false;
      }
    }
  return ok;
}

bool Sema::declareDefaultInit(ast::StructDecl *sd, ast::StructType *st) {
  // `fn __init__(x: int, y: int) { self.x = x; self.y = y; }`
  const ast::SourceLocation loc = sd->getLocation();
  std::vector<ast::Param> params;
  std::vector<ast::Stmt *> body;
  for (size_t i = 0; i < st->getNumFields(); ++i) {
    const std::string &interned = Ctx.intern(st->getFieldName(i));
    params.push_back({&interned, st->getElementType(i)});
    body.push_back(Ctx.make<ast::MemberAssignStmt>(
        loc, Ctx.make<ast::Identifier>(loc, Ctx.intern(names::kSelf)), interned,
        Ctx.make<ast::Identifier>(loc, interned)));
  }
  auto *fn = Ctx.make<ast::FuncDecl>(
      loc, Ctx.intern(names::kMethodInit), std::move(params), nullptr,
      Ctx.make<ast::CompoundStmt>(loc, std::move(body)));
  bool ok = true;
  DeclaringDefaultInit = true; // its parameters are the fields, checked
  auto *md = declareMethod(fn, st, nullptr, ast::MethodDecl::None, ok);
  DeclaringDefaultInit = false;
  if (!md)
    return false;
  st->addMethod(md);
  sd->addMethod(fn); // checked and lowered like a written one
  DefaultInits[sd->getName()] = fn;
  return ok;
}

void Sema::declareConstructor(ast::StructDecl *sd, ast::StructType *st) {
  auto *initDecl = st->findMethod(names::kMethodInit);
  std::vector<ast::Type *> ctorParams;
  if (initDecl)
    ctorParams = initDecl->getParamTypes();
  // The struct's name was checked to be free of any function.
  assert(!lookupFunction(sd->getName()) &&
         "constructor would overwrite a registered function");
  declareFunction(sd->getName(), st, ctorParams);
  if (initDecl)
    FunctionTable[sd->getName()].Modes = initDecl->getParamModes();
}

// -- Generic structs

// static
bool Sema::holdsStruct(const ast::Type *ty, const ast::StructType *target) {
  if (ty == target)
    return true;
  if (const auto *tt = ast::dyn_cast<ast::TupleType>(ty)) {
    for (const ast::Type *e : tt->getElementTypes())
      if (holdsStruct(e, target))
        return true;
    return false;
  }
  if (const auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty))
    return holdsStruct(ft->getElementType(), target);
  return false;
}

ast::StructType *Sema::instantiateStruct(const std::string &name,
                                         const std::vector<ast::Type *> &args,
                                         ast::SourceLocation loc) {
  auto tIt = StructTemplates.find(name);
  if (tIt == StructTemplates.end()) {
    // Only resolveType and resolveGenericCall come here, with a template.
    if (!reportPrivateImport(name, loc))
      error(loc, "unknown generic struct '" + name + "'");
    return nullptr;
  }
  ast::StructDecl *tmpl = tIt->second;
  for (size_t i = 0; i < args.size(); ++i)
    if (holdsFixedArray(args[i]))
      return errorFixedArrayHere(loc, args[i],
                                 "type argument " + std::to_string(i + 1) +
                                     " of '" + name + "'"),
             nullptr;
  if (args.size() != tmpl->getTypeParams().size()) {
    error(loc, "generic struct '" + name + "' expects " +
                   std::to_string(tmpl->getTypeParams().size()) +
                   " type argument(s), got " + std::to_string(args.size()));
    return nullptr;
  }
  if (!checkBounds(tmpl, name, tmpl->getTypeParams(),
                   tmpl->getTypeParamBounds(), args, loc))
    return nullptr;

  // Type names are global across imports (as for classes): an
  // instantiation is named by its template's own name.
  const ModuleTemplates *origin = foreignOrigin(tmpl);
  const std::string instName = instantiationName(tmpl->getName(), args);
  if (auto cIt = StructInstantiations.find(instName);
      cIt != StructInstantiations.end()) {
    auto info = cIt->second ? StructInstantiationInfos.find(cIt->second)
                            : StructInstantiationInfos.end();
    if (info == StructInstantiationInfos.end() || info->second.Template == tmpl)
      return cIt->second;
    error(loc, "instantiation '" + instName + "' of '" + displayName(name) +
                   "' conflicts with another template's instantiation of the "
                   "same name (type names are global across imports)");
    return nullptr;
  }

  if (auto *existing = Ctx.lookupStructType(instName)) {
    // An imported module exported `Pair<int>` as a concrete struct: the
    // very instantiation, when the template is that module's.
    auto o = ImportedTypeOrigins.find(instName);
    if (origin && o != ImportedTypeOrigins.end() &&
        o->second == origin->Module) {
      if (!lookupFunction(instName)) { // its constructor, by its name
        auto *init = existing->findMethod(names::kMethodInit);
        declareFunction(instName, existing,
                        init ? init->getParamTypes()
                             : std::vector<ast::Type *>{},
                        /*isBuiltin=*/true);
        if (init)
          FunctionTable[instName].Modes = init->getParamModes();
      }
      StructInstantiationInfos[existing] = StructInstantiationInfo{tmpl, args};
      return StructInstantiations[instName] = existing;
    }
    error(loc, "instantiation '" + instName +
                   "' conflicts with an imported struct of the same name "
                   "(type names are global across imports)");
    StructInstantiations[instName] = nullptr;
    return nullptr;
  }
  if (Ctx.lookupType(instName)) {
    error(loc, "instantiation '" + instName +
                   "' conflicts with an imported type of the same name "
                   "(type names are global across imports)");
    StructInstantiations[instName] = nullptr;
    return nullptr;
  }
  if (instantiationDepth() >= kMaxInstantiationDepth) {
    error(loc, "instantiating '" + instName +
                   "' exceeds the maximum instantiation depth (" +
                   std::to_string(kMaxInstantiationDepth) +
                   "); a generic struct may not instantiate itself with an "
                   "ever-growing type argument");
    StructInstantiations[instName] = nullptr;
    return nullptr;
  }

  // Clone the template with the type parameters substituted, and its
  // `impl` blocks, each by its own names for the type parameters.
  Subst subst;
  for (size_t i = 0; i < args.size(); ++i)
    subst[*tmpl->getTypeParams()[i]] = args[i];
  ast::ASTCloner cloner(Ctx, std::move(subst), nullptr, origin != nullptr);
  ast::StructDecl *clone = cloner.cloneStructDecl(tmpl, instName);
  std::vector<ast::ImplDecl *> &impls = ImplsByClass[instName];
  if (auto it = TemplateImpls.find(tmpl); it != TemplateImpls.end())
    for (auto *im : it->second) {
      Subst s;
      for (size_t i = 0; i < args.size(); ++i)
        s[*im->getTypeParams()[i]] = args[i];
      ast::ASTCloner c(Ctx, std::move(s), nullptr, origin != nullptr);
      impls.push_back(c.cloneImplDecl(im, instName));
    }

  const InstantiationFrame request = frameAt(instName, loc);
  InstantiationStack.push_back(request);
  OriginSource inOrigin(*this, origin);

  ast::StructType *st = Ctx.registerStructType(instName, loc);
  // Cached before the fields are resolved: a field naming this very
  // instantiation resolves to the stub, and is then reported below.
  StructInstantiations[instName] = st;
  StructInstantiationInfos[st] = StructInstantiationInfo{tmpl, args};
  if (origin)
    ImportedTypeOrigins[instName] = origin->Module;

  bool ok = populateStructType(clone, st);
  if (ok)
    declareConstructor(clone, st);
  for (auto *im : impls) {
    if (!ok || !im->getTrait())
      continue;
    if (!ImplsChecked)
      PendingTraitImpls.push_back({im, st, request, origin});
    else if (!checkTraitImpl(im, st))
      ok = false;
  }
  InstantiationStack.pop_back();

  if (!ok) {
    ErroneousClasses.insert(instName);
    StructInstantiations[instName] = nullptr;
    return nullptr;
  }

  PendingInstantiation pending;
  pending.Struct = clone;
  pending.Name = instName;
  pending.Depth = instantiationDepth() + 1;
  pending.RequestLoc = loc;
  pending.Origin = origin;
  pending.RequestFile = request.File;
  pending.RequestLines = request.Lines;
  for (auto *p : tmpl->getTypeParams())
    pending.TypeParams.insert(*p);
  PendingInstantiations.push_back(std::move(pending));
  (origin ? ForeignInstantiatedStructDecls : InstantiatedStructDecls)
      .push_back(clone);
  return st;
}

bool Sema::checkStructBodies() {
  bool ok = true;
  for (auto *sd : SortedStructs) {
    // An erroneous struct is incomplete: its bodies would only produce
    // follow-on errors.
    if (ErroneousClasses.count(sd->getName()))
      continue;
    if (!visitStructDecl(sd))
      ok = false;
  }
  return ok;
}

bool Sema::visitStructDecl(ast::StructDecl *node) {
  auto *st = Ctx.lookupStructType(node->getName());
  assert(st && "StructType must exist before its bodies are checked");

  ClassContext ctx;
  ctx.Struct = st;
  auto *savedCtx = CurrentClassCtx;
  CurrentClassCtx = &ctx;
  bool ok = true;

  for (auto *method : node->getMethods()) {
    ctx.MethodName = method->getName();
    ctx.ViewMethod = method->isView();

    // The signature was resolved when the method joined the struct
    // (populateImplMethods); resolveType is idempotent.
    ast::Type *retTy = Ctx.getVoidTy();
    if (method->getReturnType()) {
      FixedArrayAllowed = true;
      retTy = resolveType(method->getReturnType(), method->getLocation(),
                          "method '" + method->getName() + "'");
    }
    std::vector<ast::Type *> paramTys;
    for (auto &p : method->getParams()) {
      FixedArrayAllowed = true;
      auto *pty = resolveType(p.ParamType, method->getLocation(),
                              "parameter '" + p.getName() + "'");
      paramTys.push_back(pty ? pty : Ctx.getPoisonTy());
    }
    auto *savedRetTy = CurrentReturnType;
    const ResultContext savedResult = CurrentResult;
    CurrentReturnType = retTy;
    {
      ScopeGuard guard(*this);
      // `self` is the receiver's storage: read-only in a `view fn`
      // (frozenPlace), the caller's variable, field or element to change
      // in any other method, as an `inout` parameter is.
      CurrentScope->declare(names::kSelf, st);
      for (size_t i = 0; i < method->getParams().size(); ++i)
        CurrentScope->declare(method->getParams()[i].getName(), paramTys[i]);
      declareParamKinds(method);

      if (method->getName() != names::kMethodInit) {
        CurrentMethodUse = &MethodUses.emplace_back();
        CurrentMethodUse->Struct = st;
        CurrentMethodUse->Decl = method;
      }
      bool bodyOk = visitBody(method->getBody()->getStatements());
      CurrentMethodUse = nullptr;

      if (!bodyOk) {
        ok = false;
      } else {
        if (retTy != Ctx.getVoidTy() &&
            !detail::blockAlwaysReturns(method->getBody()->getStatements())) {
          error(method->getLocation(), "non-void method '" + method->getName() +
                                           "' in struct '" + node->getName() +
                                           "' does not always return a value");
          ok = false;
        }
        // Every field is set on every path out of `__init__`.
        if (method->getName() == names::kMethodInit) {
          std::vector<std::pair<std::string, ast::Type *>> fields;
          for (size_t i = 0; i < st->getNumFields(); ++i)
            fields.emplace_back(st->getFieldName(i), st->getElementType(i));
          if (!checkInitFieldsAssigned(fields, method->getBody(),
                                       method->getLocation(), "struct",
                                       node->getName()))
            ok = false;
        }
      }
    }
    CurrentReturnType = savedRetTy;
    CurrentResult = savedResult;
  }

  CurrentClassCtx = savedCtx;
  return ok;
}

} // namespace sema
} // namespace paykan
