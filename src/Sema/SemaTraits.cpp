// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Sema: traits (#216).  A trait names the methods a type implementing it
// must have; `impl Trait for A` declares that class A implements it.  The
// block's methods join A like those of any `impl` block (populateImplMethods),
// and a requirement may also be met by a method A already has.  A class
// implements what its superclasses implement, and every class implements the
// builtin `Show` through `Obj`.  The bounds of a template's type parameters
// (`<T: Show>`) are checked when it is instantiated: once the module's
// `impl ... for` blocks are recorded, so an instantiation an annotation asks
// for before that is checked then.

#include "ASTClone.h"
#include "Names.h"
#include "Sema.h"

namespace paykan {
namespace sema {

namespace {

/// `view fn name(int, view Str) -> T`, as a requirement or a method reads.
std::string fnSignature(bool view, const std::string &name,
                        const std::vector<ast::Type *> &params,
                        const ast::ParamModes &modes, ast::Type *ret) {
  std::string s = view ? "view fn " : "fn ";
  s += name + "(";
  for (size_t i = 0; i < params.size(); ++i) {
    if (i)
      s += ", ";
    if (modes.mode(i) != ast::ParamMode::Value)
      s += std::string(ast::paramModeName(modes.mode(i))) + " ";
    s += typeName(params[i]);
  }
  s += ") -> ";
  if (modes.Result != ast::ParamMode::Value)
    s += std::string(ast::paramModeName(modes.Result)) + " ";
  return s + typeName(ret);
}

} // namespace

void Sema::registerBuiltinTraits() {
  // `trait <name> { view fn <method>() -> <ret>; }`, or `trait <name> {}`
  // without a method, implemented by @p impls.
  auto builtin = [&](const char *name, const char *method, ast::Type *ret,
                     const std::vector<ast::Type *> &impls) {
    std::vector<ast::FuncDecl *> methods;
    if (method) {
      auto *fn =
          Ctx.make<ast::FuncDecl>(ast::SourceLocation(), Ctx.intern(method),
                                  std::vector<ast::Param>{}, ret, nullptr);
      fn->setView(true);
      methods.push_back(fn);
    }
    auto *decl = Ctx.make<ast::TraitDecl>(
        ast::SourceLocation(), Ctx.intern(name),
        std::vector<const std::string *>{}, std::move(methods));
    TraitInfo &info = Traits[name];
    info.Decl = decl;
    info.IsBuiltin = true;
    for (ast::Type *t : impls)
      TraitImpls[t].push_back({&info, {}});
  };
  // The numeric types, `int` and `float` first.
  std::vector<ast::Type *> numerics;
  std::vector<ast::Type *> integers;
  for (unsigned k = 0; k < ast::BuiltinType::kNumKinds; ++k) {
    auto kind = static_cast<ast::BuiltinType::Kind>(k);
    if (ast::BuiltinType::isNumericKind(kind))
      numerics.push_back(Ctx.getBuiltinType(kind));
    if (ast::BuiltinType::isIntegerKind(kind))
      integers.push_back(Ctx.getBuiltinType(kind));
  }
  // Show is implemented by Obj, the scalars (whose `toString` is their `Str`
  // conversion) and the tuples (traitImplOwner).
  std::vector<ast::Type *> show{Ctx.getObjTy()};
  show.insert(show.end(), numerics.begin(), numerics.end());
  show.push_back(Ctx.getBoolTy());
  show.push_back(Ctx.getCharTy());
  builtin(names::kTraitShow, names::kMethodToString, Ctx.getStrTy(), show);
  // Hashable by the integer types, bool, char and Str (whose `hash` is the
  // runtime's PaykanInt_hash / PaykanString_hash); a class implements it
  // with an `impl Hashable for` block.  Not a float, whose `==` is not an
  // equivalence.
  std::vector<ast::Type *> hashable = integers;
  hashable.push_back(Ctx.getBoolTy());
  hashable.push_back(Ctx.getCharTy());
  hashable.push_back(Ctx.getStrTy());
  builtin(names::kTraitHashable, names::kMethodHash, Ctx.getIntTy(), hashable);
  // ValueType by the types held in place and copied whole: the numeric
  // types, bool and char here, and every enum, tuple, fixed-size array and
  // function type (traitImplOwner).  Not a class, Str or an optional.
  std::vector<ast::Type *> values = numerics;
  values.push_back(Ctx.getBoolTy());
  values.push_back(Ctx.getCharTy());
  builtin(names::kTraitValueType, nullptr, nullptr, values);
}

bool Sema::declareTraits(ast::TranslationUnit *tu) {
  bool ok = true;
  for (auto *td : tu->getTraitDecls()) {
    const std::string &name = td->getName();
    if (auto it = Traits.find(name); it != Traits.end()) {
      error(td->getLocation(),
            it->second.IsBuiltin
                ? "'" + name + "' is a builtin trait and cannot be redeclared"
                : "redefinition of trait '" + name + "'");
      ok = false;
      continue;
    }
    if (!checkDeclNameAvailable(name, td->getLocation(), DeclKind::Trait)) {
      ok = false;
      continue;
    }
    StringSet seen;
    for (auto *p : td->getTypeParams())
      if (!seen.insert(*p).second || *p == names::kSelfType ||
          Ctx.lookupType(*p)) {
        error(td->getLocation(), "type parameter '" + *p + "' of trait '" +
                                     name +
                                     "' repeats a name or shadows a type");
        ok = false;
      }
    Traits[name].Decl = td;
  }
  return ok;
}

bool Sema::checkTraitDecls(ast::TranslationUnit *tu) {
  bool ok = true;
  for (auto *td : tu->getTraitDecls()) {
    auto it = Traits.find(td->getName());
    if (it == Traits.end() || it->second.Decl != td)
      continue; // rejected by declareTraits
    // The signatures resolve with `Self` and the type parameters standing
    // for any class.
    Subst placeholders{{std::string(names::kSelfType), Ctx.getObjTy()}};
    for (auto *p : td->getTypeParams())
      placeholders[*p] = Ctx.getObjTy();
    StringSet seen;
    for (auto *fn : td->getMethods()) {
      if (!seen.insert(fn->getName()).second) {
        error(fn->getLocation(), "duplicate method '" + fn->getName() +
                                     "' in trait '" + td->getName() + "'");
        ok = false;
      } else if (fn->getName() == names::kMethodInit ||
                 fn->getName() == names::kMethodDestroy) {
        error(fn->getLocation(),
              "a trait cannot require '" + fn->getName() + "'");
        ok = false;
      }
      RequiredSig sig;
      if (!resolveRequirement(fn, placeholders, td->getName(), sig)) {
        it->second.Erroneous = true;
        ok = false;
      }
    }
  }
  return ok;
}

bool Sema::resolveRequirement(const ast::FuncDecl *fn, const Subst &subst,
                              const std::string &traitName, RequiredSig &out) {
  ast::ASTCloner cloner(Ctx, subst);
  const std::string what =
      "method '" + fn->getName() + "' of trait '" + traitName + "'";
  out.Ret = Ctx.getVoidTy();
  if (fn->getReturnType()) {
    out.Ret = resolveType(cloner.cloneType(fn->getReturnType()),
                          fn->getLocation(), what);
    if (!out.Ret)
      return false;
  }
  for (const auto &p : fn->getParams()) {
    if (p.getName() == names::kSelf) {
      error(fn->getLocation(), "'self' cannot be used as a parameter name of " +
                                   what + "; it is the implicit receiver");
      return false;
    }
    auto *t = resolveType(cloner.cloneType(p.ParamType), fn->getLocation(),
                          "parameter '" + p.getName() + "' of " + what);
    if (!t)
      return false;
    out.Params.push_back(t);
  }
  out.Modes = ast::paramModes(fn->getParams());
  out.Modes.Result = fn->getResultMode();
  return true;
}

std::optional<Sema::TraitRef> Sema::resolveTraitRef(ast::Type *ref,
                                                    const Subst &subst,
                                                    ast::SourceLocation loc) {
  std::string name;
  std::vector<ast::Type *> argAnns;
  if (auto *ct = ast::dyn_cast<ast::ClassType>(ref)) {
    name = ct->getName();
  } else if (auto *gt = ast::dyn_cast<ast::GenericType>(ref)) {
    name = gt->getName();
    argAnns = gt->getArgs();
  } else {
    error(loc, "'" + typeName(ref) + "' is not a trait");
    return std::nullopt;
  }
  auto it = Traits.find(name);
  if (it == Traits.end()) {
    if (isFailedImportUse(name))
      return std::nullopt;
    if (Ctx.lookupType(name) || ClassTemplates.count(name) ||
        StructTemplates.count(name))
      error(loc, "'" + name + "' is not a trait");
    else if (!reportPrivateImport(name, loc))
      error(loc, "unknown trait '" + name + "'");
    return std::nullopt;
  }
  const auto &params = it->second.Decl->getTypeParams();
  if (argAnns.size() != params.size()) {
    error(loc, "trait '" + name + "' expects " + std::to_string(params.size()) +
                   " type argument(s), got " + std::to_string(argAnns.size()));
    return std::nullopt;
  }
  TraitRef tr{&it->second, {}};
  ast::ASTCloner cloner(Ctx, subst);
  for (size_t i = 0; i < argAnns.size(); ++i) {
    auto *t = resolveType(cloner.cloneType(argAnns[i]), loc,
                          "type argument " + std::to_string(i + 1) +
                              " of trait '" + name + "'");
    if (!t)
      return std::nullopt;
    tr.Args.push_back(t);
  }
  return tr;
}

std::string Sema::traitRefName(const TraitRef &tr) {
  std::string s = tr.Trait->Decl->getName();
  if (tr.Args.empty())
    return s;
  s += "<";
  for (size_t i = 0; i < tr.Args.size(); ++i)
    s += (i ? ", " : "") + typeName(tr.Args[i]);
  return s + ">";
}

const ast::Type *Sema::traitImplOwner(ast::Type *ty, const TraitRef &tr) {
  auto has = [&](const ast::Type *t) {
    auto it = TraitImpls.find(t);
    if (it == TraitImpls.end())
      return false;
    for (const auto &impl : it->second) {
      bool same = impl.Trait == tr.Trait && impl.Args.size() == tr.Args.size();
      for (size_t i = 0; same && i < impl.Args.size(); ++i)
        same = typesEqual(impl.Args[i], tr.Args[i]);
      if (same)
        return true;
    }
    return false;
  };
  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty)) {
    for (const ast::ClassType *c = ct; c; c = c->getSuperClass())
      if (has(c))
        return c;
    return nullptr;
  }
  auto isBuiltinTrait = [&](const char *name) {
    return tr.Trait->IsBuiltin && tr.Trait->Decl->getName() == name;
  };
  // An optional shows itself when its value does: "None", or the value's
  // text (the builtin `Show`).
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    return isBuiltinTrait(names::kTraitShow) &&
                   traitImplOwner(ot->getInnerType(), tr)
               ? ty
               : nullptr;
  // A struct is a ValueType and shows its fields (the builtin `Show`);
  // anything else, an `impl ... for` block gives it.  Before the tuple
  // cases, which a struct is too.
  if (ast::isa<ast::StructType>(ty)) {
    if (isBuiltinTrait(names::kTraitValueType) ||
        isBuiltinTrait(names::kTraitShow))
      return ty;
    return has(ty) ? ty : nullptr;
  }
  // Every enum, tuple, fixed-size array and function type is a ValueType.
  if ((ast::isa<ast::EnumType>(ty) || ast::isa<ast::TupleType>(ty) ||
       ast::isa<ast::FixedArrayType>(ty) || ast::isa<ast::FunctionType>(ty)) &&
      isBuiltinTrait(names::kTraitValueType))
    return ty;
  // A tuple shows its elements (the builtin `Show`).
  if (ast::isa<ast::TupleType>(ty))
    return isBuiltinTrait(names::kTraitShow) ? ty : nullptr;
  return has(ty) ? ty : nullptr;
}

bool Sema::checkTraitImpl(ast::ImplDecl *im, ast::Type *ty) {
  auto tr = resolveTraitRef(im->getTrait(), {}, im->getLocation());
  if (!tr)
    return false;
  const std::string trait = traitRefName(*tr);
  const std::string kind = typeKindWord(ty);
  const std::string name = typeName(ty);
  // A class is an object, shared by reference: never a ValueType.  (A
  // struct is one already: reported below as implemented.)
  if (ast::isa<ast::ClassType>(ty) && tr->Trait->IsBuiltin &&
      tr->Trait->Decl->getName() == names::kTraitValueType) {
    error(im->getLocation(),
          "class '" + name + "' cannot implement 'ValueType': a " +
              "class is shared by reference, not held in place");
    return false;
  }
  if (const ast::Type *owner = traitImplOwner(ty, *tr)) {
    error(im->getLocation(),
          kind + " '" + name + "' already implements '" + trait + "'" +
              (owner == ty
                   ? ""
                   : " (through '" + typeName(const_cast<ast::Type *>(owner)) +
                         "')"));
    return false;
  }
  // Recorded even if a requirement is not met, which is reported here, so
  // the bounds it satisfies are not reported again.
  TraitImpls[ty].push_back(*tr);
  if (tr->Trait->Erroneous) {
    ++SuppressedFollowOns; // its requirements were reported with it
    return false;
  }

  Subst subst{{std::string(names::kSelfType), ty}};
  const auto &params = tr->Trait->Decl->getTypeParams();
  for (size_t i = 0; i < params.size(); ++i)
    subst[*params[i]] = tr->Args[i];
  bool ok = true;
  StringSet required;
  for (auto *req : tr->Trait->Decl->getMethods()) {
    required.insert(req->getName());
    RequiredSig sig;
    if (!resolveRequirement(req, subst, trait, sig)) {
      ok = false;
      continue;
    }
    const std::string want = fnSignature(req->isView(), req->getName(),
                                         sig.Params, sig.Modes, sig.Ret);
    ast::MethodDecl *m = methodOf(ty, req->getName());
    if (!m) {
      std::string msg = kind;
      msg += " '";
      msg += name;
      msg += "' does not implement '";
      msg += want;
      msg += "' of trait '";
      msg += trait;
      msg += "'";
      error(im->getLocation(), msg);
      ok = false;
      continue;
    }
    bool same = m->isView() == req->isView() &&
                typesEqual(m->getReturnType(), sig.Ret) &&
                m->getParamTypes().size() == sig.Params.size() &&
                m->getParamModes().Result == sig.Modes.Result;
    for (size_t i = 0; same && i < sig.Params.size(); ++i)
      same = typesEqual(m->getParamTypes()[i], sig.Params[i]) &&
             m->getParamModes().mode(i) == sig.Modes.mode(i);
    if (!same) {
      std::string msg = "'";
      msg += fnSignature(m->isView(), m->getName(), m->getParamTypes(),
                         m->getParamModes(), m->getReturnType());
      msg += "' of " + kind + " '";
      msg += name;
      msg += "' does not match '";
      msg += want;
      msg += "' of trait '";
      msg += trait;
      msg += "'";
      error(m->getLocation().isValid() ? m->getLocation() : im->getLocation(),
            msg);
      ok = false;
    }
  }
  for (auto *fn : im->getMethods())
    if (!required.count(fn->getName())) {
      error(fn->getLocation(),
            "'" + fn->getName() + "' is not a method of trait '" + trait + "'");
      ok = false;
    }
  return ok;
}

bool Sema::checkBoundDecls(ast::TranslationUnit *tu) {
  bool ok = true;
  auto check = [&](const ast::Decl *tmpl,
                   const std::vector<const std::string *> &params,
                   const ast::TypeParamBounds &bounds) {
    Subst placeholders;
    for (auto *p : params)
      placeholders[*p] = Ctx.getObjTy();
    for (const auto &traits : bounds)
      for (ast::Type *b : traits)
        if (!resolveTraitRef(b, placeholders, tmpl->getLocation())) {
          BadBounds.insert(tmpl);
          ok = false;
        }
  };
  for (auto *cd : tu->getGenericClassDecls())
    check(cd, cd->getTypeParams(), cd->getTypeParamBounds());
  for (auto *sd : tu->getGenericStructDecls())
    check(sd, sd->getTypeParams(), sd->getTypeParamBounds());
  for (auto *fn : tu->getGenericFuncDecls())
    check(fn, fn->getTypeParams(), fn->getTypeParamBounds());
  return ok;
}

bool Sema::checkBounds(const ast::Decl *tmpl, const std::string &name,
                       const std::vector<const std::string *> &params,
                       const ast::TypeParamBounds &bounds,
                       const std::vector<ast::Type *> &args,
                       ast::SourceLocation loc) {
  if (bounds.empty())
    return true;
  if (!TraitImplsKnown) {
    PendingBoundChecks.push_back({tmpl, name, args, loc});
    return true;
  }
  if (BadBounds.count(tmpl)) {
    ++SuppressedFollowOns; // its bounds were reported with the template
    return false;
  }
  Subst subst;
  for (size_t i = 0; i < params.size() && i < args.size(); ++i)
    subst[*params[i]] = args[i];
  bool ok = true;
  for (size_t i = 0; i < bounds.size() && i < args.size(); ++i)
    for (ast::Type *b : bounds[i]) {
      auto tr = resolveTraitRef(b, subst, loc);
      if (!tr) {
        ok = false;
        continue;
      }
      if (!traitImplOwner(args[i], *tr)) {
        error(loc, "'" + typeName(args[i]) + "' does not implement trait '" +
                       traitRefName(*tr) + "', which '" + displayName(name) +
                       "' requires of '" + *params[i] + "'");
        ok = false;
      }
    }
  return ok;
}

bool Sema::checkPendingBounds() {
  TraitImplsKnown = true;
  bool ok = true;
  for (const auto &pc : PendingBoundChecks) {
    const std::vector<const std::string *> *params;
    const ast::TypeParamBounds *bounds;
    if (auto *cd = ast::dyn_cast<ast::ClassDecl>(pc.Template)) {
      params = &cd->getTypeParams();
      bounds = &cd->getTypeParamBounds();
    } else if (auto *sd = ast::dyn_cast<ast::StructDecl>(pc.Template)) {
      params = &sd->getTypeParams();
      bounds = &sd->getTypeParamBounds();
    } else {
      auto *fn = ast::cast<ast::FuncDecl>(pc.Template);
      params = &fn->getTypeParams();
      bounds = &fn->getTypeParamBounds();
    }
    if (!checkBounds(pc.Template, pc.Name, *params, *bounds, pc.Args, pc.Loc))
      ok = false;
  }
  PendingBoundChecks.clear();
  return ok;
}

} // namespace sema
} // namespace paykan
