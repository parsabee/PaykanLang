// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The archetype check of templates (#368; docs/design/concurrency.md,
// "Copyable"; docs/language/13-generics.md, "The archetype check").  A
// template is checked per instance, so one used only with `int` would hide
// a copy of a `T` that needs `T: Copyable`.  Once the module's bodies are
// checked, each of its templates is instantiated once more with an
// archetype for each type parameter without bounds: a class of its own,
// `%T`, which no source can spell and which, as every class, is move-only.
// A copy of one in the template's own code is reported as the missing
// bound; everything else the check finds is dropped, and nothing it makes
// reaches the lowering or the module's interface.

#include "Names.h"
#include "Sema.h"
#include "SemaInternal.h"

#include <algorithm>
#include <functional>

namespace paykan::sema {

namespace {

const std::vector<const std::string *> *typeParamsOf(const ast::Decl *d) {
  if (const auto *f = ast::dyn_cast<ast::FuncDecl>(d))
    return &f->getTypeParams();
  if (const auto *c = ast::dyn_cast<ast::ClassDecl>(d))
    return &c->getTypeParams();
  if (const auto *s = ast::dyn_cast<ast::StructDecl>(d))
    return &s->getTypeParams();
  if (const auto *e = ast::dyn_cast<ast::EnumDecl>(d))
    return &e->getTypeParams();
  return nullptr;
}

const ast::TypeParamBounds *boundsOf(const ast::Decl *d) {
  if (const auto *f = ast::dyn_cast<ast::FuncDecl>(d))
    return &f->getTypeParamBounds();
  if (const auto *c = ast::dyn_cast<ast::ClassDecl>(d))
    return &c->getTypeParamBounds();
  if (const auto *s = ast::dyn_cast<ast::StructDecl>(d))
    return &s->getTypeParamBounds();
  if (const auto *e = ast::dyn_cast<ast::EnumDecl>(d))
    return &e->getTypeParamBounds();
  return nullptr;
}

/// Whether the type spelled @p spelling names the archetype @p name (`%T`):
/// it is the archetype, or holds it as a part or a type argument
/// (`Pair<%T, int>`, `%T?`, `(%T, Str)`).  An instance is known by its
/// name, and `%` is in no other.
bool spellsArchetype(const std::string &spelling, const std::string &name) {
  for (size_t at = spelling.find(name); at != std::string::npos;
       at = spelling.find(name, at + 1)) {
    const size_t end = at + name.size();
    if (end == spelling.size() ||
        !(std::isalnum(static_cast<unsigned char>(spelling[end])) ||
          spelling[end] == '_'))
      return true;
  }
  return false;
}

} // namespace

void Sema::noteArchetypeCopy(ast::SourceLocation loc, const ast::Type *ty,
                             const std::string &what, bool view) {
  if (!Archetypes || !ty || InstantiatingTemplates.empty())
    return;
  // Only in the code of a template's own instance, one of the module's:
  // another instance (one its code asks for, `Pair<%T, %T>`) is checked by
  // its template's own run, in its own names.
  const ast::Decl *tmpl = InstantiatingTemplates.back().Template;
  auto own = Archetypes->Own.find(tmpl);
  if (own == Archetypes->Own.end())
    return;
  // The instance being made (its fields, its signature), or the one whose
  // body is checked.
  const std::vector<ast::Type *> *args = CurrentInstanceArgs;
  if (!MakingInstances.empty() && MakingInstances.back().first == tmpl)
    args = MakingInstances.back().second;
  if (!args || std::find(own->second.begin(), own->second.end(), *args) ==
                   own->second.end())
    return;
  const std::vector<const std::string *> *params = typeParamsOf(tmpl);
  // A generic method's code sees its block's names and its own (#353).
  if (auto m = MethodTemplateParams.find(tmpl); m != MethodTemplateParams.end())
    params = &m->second.Params;
  if (!params)
    return;
  std::unordered_set<const ast::Type *> mine;
  for (const ast::Type *a : *args)
    if (Archetypes->Classes.count(a))
      mine.insert(a);
  // The copy is the archetypes': a value of @p ty copies, or shares nothing,
  // once they do (`Box<%T>` is move-only for every T, a function value
  // shares its closure).
  std::function<bool(const ast::Type *, ast::Qualifier)> copies =
      [&](const ast::Type *t, ast::Qualifier q) -> bool {
    auto *tt = const_cast<ast::Type *>(t);
    if (!t || mine.count(t) || ast::isa<ast::PoisonType>(t) ||
        ast::isa<ast::BuiltinType>(t) || ast::isa<ast::AtomicType>(t) ||
        ast::isa<ast::FunctionType>(t))
      return true;
    if (ast::isRefType(tt) && q.Form != ast::QualForm::Unique)
      return true; // a `shared` reference, a frozen object, a cell
    if (const auto *qt = ast::dyn_cast<ast::QualifiedType>(t))
      return copies(qt->getInnerType(), qt->getQualifier());
    if (const auto *ot = ast::dyn_cast<ast::OptionalType>(t))
      return copies(ot->getInnerType(), q);
    if (const auto *ft = ast::dyn_cast<ast::FixedArrayType>(t))
      return copies(ft->getElementType(), ft->getElemQual());
    if (bufferElementType(tt))
      return false;
    if (const auto *st = ast::dyn_cast<ast::StructType>(t)) {
      const auto &types = st->getElementTypes();
      for (size_t i = 0; i < types.size() && i < st->getNumFields(); ++i)
        if (!copies(types[i], st->findFieldQualifier(st->getFieldName(i))))
          return false;
      return true;
    }
    if (const auto *tu = ast::dyn_cast<ast::TupleType>(t)) {
      const auto &types = tu->getElementTypes();
      for (size_t i = 0; i < types.size(); ++i)
        if (!copies(types[i], tu->getElemQual(i)))
          return false;
      return true;
    }
    if (const auto *et = ast::dyn_cast<ast::EnumType>(t)) {
      for (size_t i = 0; i < et->getNumVariants(); ++i)
        if (const ast::StructType *pl = et->getPayload(i);
            pl && !copies(pl, {}))
          return false;
      return true;
    }
    return false; // `Str`, a class, a task
  };
  std::function<bool(const ast::Type *)> shares =
      [&](const ast::Type *t) -> bool {
    if (!t || mine.count(t) || ast::isa<ast::PoisonType>(t) ||
        ast::isa<ast::BuiltinType>(t))
      return false;
    if (const auto *et = ast::dyn_cast<ast::EnumType>(t)) {
      for (size_t i = 0; i < et->getNumVariants(); ++i)
        if (const ast::StructType *pl = et->getPayload(i); pl && shares(pl))
          return true;
      return false;
    }
    if (const auto *ot = ast::dyn_cast<ast::OptionalType>(t))
      return shares(ot->getInnerType());
    if (const auto *qt = ast::dyn_cast<ast::QualifiedType>(t))
      return shares(qt->getInnerType());
    if (const auto *ft = ast::dyn_cast<ast::FixedArrayType>(t))
      return shares(ft->getElementType());
    if (const auto *tu = ast::dyn_cast<ast::TupleType>(t)) {
      for (const ast::Type *e : tu->getElementTypes())
        if (shares(e))
          return true;
      return false;
    }
    return true;
  };
  if (view ? shares(ty) : !copies(ty, {}))
    return;
  // The code's own names: a method's, its signature's and the lambdas'
  // in it, are its `impl` block's (`impl R<U>`), whose names are open.
  const ast::FuncDecl *method = ImplScopeMethod;
  if (!method)
    method = CurrentMethodUse ? CurrentMethodUse->Decl : CurrentResult.Fn;
  // A lambda's body is in the function it is written in.
  for (auto it = LambdaStack.rbegin(); !method && it != LambdaStack.rend();
       ++it)
    method = it->OuterUse ? it->OuterUse->Decl : it->OuterFn;
  const ast::ImplDecl *block = nullptr;
  if (method)
    if (auto it = ImplMethodBlocks.find(method); it != ImplMethodBlocks.end())
      block = it->second;
  if (block && block->getTypeParams().size() == params->size())
    params = &block->getTypeParams();
  const std::string spelling = typeName(const_cast<ast::Type *>(ty));
  for (size_t i = 0; i < params->size() && i < args->size(); ++i) {
    const ast::Type *arg = (*args)[i];
    if (!mine.count(arg) ||
        !spellsArchetype(spelling, typeName(const_cast<ast::Type *>(arg))))
      continue;
    const std::string &p = *(*params)[i];
    std::string msg;
    // An untyped parameter's type is a parameter no source can name: one
    // to write is a name the template's parameters leave free, another
    // for each untyped parameter.
    if (const std::string_view untyped = names::untypedParamOf(p);
        !untyped.empty()) {
      size_t nth = 0;
      for (size_t j = 0; j < i; ++j)
        nth += !names::untypedParamOf(*(*params)[j]).empty();
      std::string fresh;
      for (const char *c : {"T", "U", "V", "W", "X", "Y", "Z"}) {
        bool taken = false;
        for (const std::string *q : *params)
          taken = taken || *q == c;
        if (!taken && nth-- == 0) {
          fresh = c;
          break;
        }
      }
      if (fresh.empty())
        fresh = "T" + std::to_string(i);
      msg = "parameter '";
      msg += untyped;
      msg += "' takes any type, which may not be 'Copyable': write its type, "
             "'";
      msg += untyped;
      msg += ": ";
      msg += fresh;
      msg += "' with '";
      msg += fresh;
      msg += ": Copyable', ";
      msg += what;
    } else {
      msg = "'";
      msg += p;
      msg += "' is not 'Copyable': declare '";
      msg += p;
      msg += ": Copyable' ";
      msg += what;
    }
    // A method's copy may instead go in a conditional `impl` block, whose
    // methods exist only on the instances meeting its bounds; not a trait
    // method's, which every instance has.
    const std::string *name = nullptr;
    if (const auto *cd = ast::dyn_cast<ast::ClassDecl>(tmpl))
      name = &cd->getName();
    else if (const auto *sd = ast::dyn_cast<ast::StructDecl>(tmpl))
      name = &sd->getName();
    if (name && method && !(block && block->getTrait())) {
      // The block's own bounds stay (`impl Pair<A: Hashable, B: Copyable>`).
      const ast::TypeParamBounds *had =
          block ? &block->getTypeParamBounds() : nullptr;
      msg += ", or put the method in 'impl " + *name + "<";
      for (size_t j = 0; j < params->size(); ++j) {
        std::string bs;
        if (had && j < had->size())
          for (ast::Type *b : (*had)[j])
            if (j != i || traitAnnotationName(b) != names::kTraitCopyable)
              bs += (bs.empty() ? "" : " + ") + typeName(b);
        if (j == i)
          bs += (bs.empty() ? "" : " + ") + std::string(names::kTraitCopyable);
        msg += (j ? ", " : "") + *(*params)[j] + (bs.empty() ? "" : ": " + bs);
      }
      msg += ">'";
    }
    const std::string key = std::to_string(loc.getLineStart()) + ":" +
                            std::to_string(loc.getColumnStart()) + ":" + msg;
    if (Archetypes->Seen.insert(key).second)
      Archetypes->Copies.push_back({loc, msg});
  }
}

const std::string *Sema::unsendableArchetype(const ast::ClassType *archetype) {
  if (!Archetypes || InstantiatingTemplates.empty())
    return nullptr;
  // Only in the code of a template's own instance (noteArchetypeCopy).
  const ast::Decl *tmpl = InstantiatingTemplates.back().Template;
  auto own = Archetypes->Own.find(tmpl);
  if (own == Archetypes->Own.end())
    return nullptr;
  const std::vector<ast::Type *> *args = CurrentInstanceArgs;
  if (!MakingInstances.empty() && MakingInstances.back().first == tmpl)
    args = MakingInstances.back().second;
  if (!args || std::find(own->second.begin(), own->second.end(), *args) ==
                   own->second.end())
    return nullptr;
  const std::vector<const std::string *> *params = typeParamsOf(tmpl);
  const ast::TypeParamBounds *bounds = boundsOf(tmpl);
  if (auto m = MethodTemplateParams.find(tmpl);
      m != MethodTemplateParams.end()) {
    params = &m->second.Params;
    bounds = &m->second.Bounds;
  }
  if (!params)
    return nullptr;
  // A method's names and bounds are also its `impl` block's.
  const ast::FuncDecl *method = ImplScopeMethod;
  if (!method)
    method = CurrentMethodUse ? CurrentMethodUse->Decl : CurrentResult.Fn;
  for (auto it = LambdaStack.rbegin(); !method && it != LambdaStack.rend();
       ++it)
    method = it->OuterUse ? it->OuterUse->Decl : it->OuterFn;
  const ast::ImplDecl *block = nullptr;
  if (method)
    if (auto it = ImplMethodBlocks.find(method); it != ImplMethodBlocks.end())
      block = it->second;
  auto sendable = [&](const ast::TypeParamBounds *bs, size_t i) {
    if (bs && i < bs->size())
      for (ast::Type *b : (*bs)[i])
        if (namesBuiltinTrait(b) &&
            traitAnnotationName(b) == names::kTraitSendable)
          return true;
    return false;
  };
  for (size_t i = 0; i < args->size() && i < params->size(); ++i) {
    if ((*args)[i] != archetype)
      continue;
    if (sendable(bounds, i) ||
        (block && sendable(&block->getTypeParamBounds(), i)))
      return nullptr;
    if (block && block->getTypeParams().size() == params->size())
      return block->getTypeParams()[i];
    return (*params)[i];
  }
  return nullptr;
}

std::string Sema::unsent(ast::Type *ty, ast::Qualifier q,
                         ast::SourceLocation loc, const std::string &what,
                         bool sent) {
  UnsentArchetype = nullptr;
  const bool saved = std::exchange(RecordCrossings, sent);
  std::string why = uncrossable(ty, q, "");
  RecordCrossings = saved;
  if (!why.empty() && UnsentArchetype)
    if (const std::string *param = unsendableArchetype(UnsentArchetype)) {
      std::string msg;
      // An untyped parameter's type is a parameter no source can name.
      if (const std::string_view untyped = names::untypedParamOf(*param);
          !untyped.empty()) {
        msg = "parameter '";
        msg += untyped;
        msg += "' takes any type, which may not be 'Sendable': write its "
               "type with a type parameter bounded 'Sendable', ";
      } else {
        msg = "'" + *param + "' is not 'Sendable': declare '" + *param +
              ": Sendable' ";
      }
      msg += what;
      const std::string key = std::to_string(loc.getLineStart()) + ":" +
                              std::to_string(loc.getColumnStart()) + ":" + msg;
      if (Archetypes->Seen.insert(key).second)
        Archetypes->Copies.push_back({loc, msg});
    }
  UnsentArchetype = nullptr;
  return why;
}

void Sema::noteArchetypeSend(ast::SourceLocation loc, ast::Type *ty,
                             const std::string &what) {
  if (Archetypes)
    static_cast<void>(unsent(ty, {}, loc, what, /*sent=*/false));
}

bool Sema::checkArchetypes() {
  // The module's own templates, in a stable order.
  std::vector<std::pair<std::string, const ast::Decl *>> templates;
  for (auto &[name, fd] : FuncTemplates)
    if (!foreignOrigin(fd))
      templates.push_back({name, fd});
  for (auto &[name, cd] : ClassTemplates)
    if (!foreignOrigin(cd))
      templates.push_back({name, cd});
  for (auto &[name, sd] : StructTemplates)
    if (!foreignOrigin(sd))
      templates.push_back({name, sd});
  for (auto &[name, ed] : EnumTemplates)
    if (!foreignOrigin(ed))
      templates.push_back({name, ed});
  // And its generic methods and generic `static fn`s (#353), which no
  // instance of their owner holds: each with its owner's name and, for a
  // generic owner, its template.
  struct Member {
    std::string Owner;
    const ast::Decl *OwnerTmpl;
    MethodTemplate T;
  };
  std::vector<Member> members;
  // (The table holds each instance's too; those are their template's,
  // below.)
  for (const auto &[owner, of] : MethodTemplates)
    for (const auto &[name, mt] : of)
      if (!foreignOrigin(mt.Fn) && mt.Impl->getTypeParams().empty())
        members.push_back({owner, nullptr, mt});
  for (const auto &[name, tmpl] : templates)
    if (auto of = TemplateMethodTemplates.find(tmpl);
        of != TemplateMethodTemplates.end())
      for (const MethodTemplate &mt : of->second)
        members.push_back({name, tmpl, mt});
  if (templates.empty() && members.empty())
    return true;
  std::sort(members.begin(), members.end(),
            [](const Member &a, const Member &b) {
              const ast::SourceLocation la = a.T.Fn->getLocation(),
                                        lb = b.T.Fn->getLocation();
              if (la.getLineStart() != lb.getLineStart())
                return la.getLineStart() < lb.getLineStart();
              if (la.getColumnStart() != lb.getColumnStart())
                return la.getColumnStart() < lb.getColumnStart();
              return a.Owner < b.Owner;
            });
  std::sort(templates.begin(), templates.end(),
            [](const auto &a, const auto &b) {
              const ast::SourceLocation la = a.second->getLocation(),
                                        lb = b.second->getLocation();
              if (la.getLineStart() != lb.getLineStart())
                return la.getLineStart() < lb.getLineStart();
              if (la.getColumnStart() != lb.getColumnStart())
                return la.getColumnStart() < lb.getColumnStart();
              return a.first < b.first;
            });

  // What the check makes is dropped at its end: the diagnostics it reports,
  // the instantiations it adds for the lowering (another module's copied
  // too), the method records the `view fn` suggestions read, and the
  // functions and types it registers, which the module's interface leaves
  // out.
  StringSet before;
  for (const auto &[name, sig] : FunctionTable)
    before.insert(name);
  for (const auto &[name, ct] : Ctx.getClassTypes())
    before.insert(name);
  for (const auto &[name, st] : Ctx.getStructTypes())
    before.insert(name);
  for (const auto &[name, et] : Ctx.getEnumTypes())
    before.insert(name);
  const DiagEngine::Mark mark = Diags.mute();
  const unsigned suppressed = SuppressedFollowOns;
  const unsigned poisoned = PoisonedBindings;
  const unsigned repeated = RepeatedImportFailures;
  const size_t structs = InstantiatedStructDecls.size(),
               enums = InstantiatedEnumDecls.size(),
               classes = InstantiatedClassDecls.size(),
               funcs = InstantiatedFuncDecls.size(),
               foreignClasses = ForeignInstantiatedClassDecls.size(),
               foreignFuncs = ForeignInstantiatedFuncDecls.size(),
               foreignStructs = ForeignInstantiatedStructDecls.size(),
               importedClasses = ImportedInstanceClassDecls.size(),
               importedStructs = ImportedInstanceStructDecls.size(),
               methodUses = MethodUses.size(),
               madeMethods = InstantiatedMethods.size();
  const StringSet copied = CopiedInstances;

  ArchetypeCheck check;
  ArchetypeCheck *const saved = std::exchange(Archetypes, &check);
  // One archetype per parameter name: `%T`, a class of its own.
  StringMap<ast::ClassType *> archetypes;
  auto archetype = [&](const std::string &param) {
    auto [it, fresh] = archetypes.try_emplace(param, nullptr);
    if (fresh) {
      const std::string name = std::string(names::kArchetypePrefix) + param;
      it->second = Ctx.lookupClassType(name);
      if (!it->second) {
        // A `clone` gives another archetype (`T: Cloneable`'s copy), whose
        // copies are then the archetype's too.
        it->second = Ctx.buildClassType(name, Ctx.getObjTy()).build();
        it->second->addMethod(Ctx.make<ast::MethodDecl>(
            ast::SourceLocation(), Ctx.intern(names::kMethodClone), it->second,
            std::vector<ast::Type *>{}, ast::MethodDecl::View));
      }
      check.Classes.insert(it->second);
    }
    return it->second;
  };
  // A parameter bounded `Copyable` takes the first of these that meets its
  // bounds; any other is an archetype, which meets every other bound.  A
  // template whose signature takes no value there (`shared K`) is tried
  // again with a reference.
  const std::vector<ast::Type *> values = {Ctx.getIntTy(), Ctx.getFloatTy(),
                                           Ctx.getBoolTy(), Ctx.getStrTy()};
  const std::vector<ast::Type *> references = {
      Ctx.getQualifiedType({ast::QualForm::Shared, false}, Ctx.getStrTy())};
  auto namesCopyable = [&](const std::vector<ast::Type *> &bs) {
    for (ast::Type *b : bs)
      if (namesBuiltinTrait(b) &&
          traitAnnotationName(b) == names::kTraitCopyable)
        return true;
    return false;
  };
  auto argsFor = [&](const ast::Decl *tmpl,
                     const std::vector<const std::string *> &params,
                     const ast::TypeParamBounds &bounds,
                     const std::vector<ast::Type *> &standIns,
                     std::vector<ast::Type *> &args) {
    bool any = false;
    for (size_t i = 0; i < params.size(); ++i) {
      if (i >= bounds.size() || !namesCopyable(bounds[i])) {
        args.push_back(archetype(*params[i]));
        any = true;
        continue;
      }
      Subst subst;
      ast::Type *pick = nullptr;
      for (ast::Type *c : standIns) {
        bool all = true;
        for (ast::Type *b : bounds[i]) {
          auto tr = resolveTraitRef(b, subst, tmpl->getLocation());
          all = all && tr && traitImplOwner(c, *tr);
        }
        if (all) {
          pick = c;
          break;
        }
      }
      if (!pick)
        return false;
      args.push_back(pick);
    }
    return any;
  };
  struct Plan {
    std::string Name;
    const ast::Decl *Tmpl;
    std::vector<ast::Type *> Args, Again;
    std::vector<std::vector<ast::Type *>>
        Blocks; // impl blocks bounded Copyable
  };
  std::vector<Plan> plans;
  for (const auto &[name, tmpl] : templates) {
    const std::vector<const std::string *> &params = *typeParamsOf(tmpl);
    const ast::TypeParamBounds &bounds = *boundsOf(tmpl);
    Plan plan{name, tmpl, {}, {}, {}};
    if (!argsFor(tmpl, params, bounds, values, plan.Args))
      continue;
    std::vector<ast::Type *> again;
    if (argsFor(tmpl, params, bounds, references, again) && again != plan.Args)
      plan.Again = std::move(again);
    // A class's or a struct's block bounded `Copyable` on a parameter
    // (`impl Pair<A: Copyable, B>`) joins an instance with a stand-in there.
    if (auto it = TemplateImpls.find(tmpl); it != TemplateImpls.end())
      for (const ast::ImplDecl *im : it->second) {
        const ast::TypeParamBounds &ib = im->getTypeParamBounds();
        std::vector<ast::Type *> args = plan.Args;
        bool changed = false;
        for (size_t i = 0; i < ib.size() && i < args.size(); ++i) {
          if (!namesCopyable(ib[i]) || !check.Classes.count(args[i]))
            continue;
          std::vector<ast::Type *> all = ib[i];
          if (i < bounds.size())
            all.insert(all.end(), bounds[i].begin(), bounds[i].end());
          std::vector<ast::Type *> one;
          // The stand-in, if any, is in `one`.
          static_cast<void>(argsFor(tmpl, {params[i]}, {all}, values, one));
          if (one.size() == 1 && !check.Classes.count(one[0])) {
            args[i] = one[0];
            changed = true;
          }
        }
        if (changed && std::find(plan.Blocks.begin(), plan.Blocks.end(),
                                 args) == plan.Blocks.end())
          plan.Blocks.push_back(std::move(args));
      }
    // Every template's own instances are known before any is made: one's
    // signature may make another's first.
    check.Own[tmpl] = {plan.Args};
    if (!plan.Again.empty())
      check.Own[tmpl].push_back(plan.Again);
    for (const auto &b : plan.Blocks)
      check.Own[tmpl].push_back(b);
    plans.push_back(std::move(plan));
  }
  auto instantiate = [&](const Plan &plan,
                         const std::vector<ast::Type *> &args) {
    const ast::Decl *tmpl = plan.Tmpl;
    if (ast::isa<ast::FuncDecl>(tmpl))
      return !instantiateFunction(plan.Name, args, tmpl->getLocation()).empty();
    if (ast::isa<ast::ClassDecl>(tmpl))
      return instantiateClass(plan.Name, args, tmpl->getLocation()) != nullptr;
    if (ast::isa<ast::StructDecl>(tmpl))
      return instantiateStruct(plan.Name, args, tmpl->getLocation()) != nullptr;
    return instantiateEnum(plan.Name, args, tmpl->getLocation()) != nullptr;
  };
  // A generic member's own instance: on its owner's, the owner's
  // parameters standing as its block bounds them.
  struct MemberPlan {
    const Member *M;
    std::vector<ast::Type *> OwnerArgs, Args;
  };
  std::vector<MemberPlan> memberPlans;
  for (const Member &m : members) {
    const MethodTemplateNames &names = MethodTemplateParams.at(m.T.Fn);
    const size_t owners = names.Params.size() - m.T.Fn->getTypeParams().size();
    MemberPlan mp{&m, {}, {}};
    std::vector<ast::Type *> all;
    static_cast<void>(argsFor(m.T.Fn, names.Params, names.Bounds, values, all));
    if (all.size() != names.Params.size())
      continue; // no stand-in meets a bound
    mp.OwnerArgs.assign(all.begin(), all.begin() + owners);
    mp.Args.assign(all.begin() + owners, all.end());
    check.Own[m.T.Fn] = {std::move(all)};
    memberPlans.push_back(std::move(mp));
  }
  for (const Plan &plan : plans) {
    if (!instantiate(plan, plan.Args) && !plan.Again.empty())
      instantiate(plan, plan.Again);
    for (const auto &args : plan.Blocks)
      instantiate(plan, args);
  }
  for (const MemberPlan &mp : memberPlans) {
    const Member &m = *mp.M;
    const ast::SourceLocation at = m.T.Fn->getLocation();
    FoundMethodTemplate found;
    found.T = m.T;
    found.OwnerArgs = mp.OwnerArgs;
    if (!m.OwnerTmpl) {
      found.Class = Ctx.lookupClassType(m.Owner);
      found.Struct = found.Class ? nullptr : Ctx.lookupStructType(m.Owner);
    } else if (ast::isa<ast::ClassDecl>(m.OwnerTmpl)) {
      found.Class = instantiateClass(m.Owner, mp.OwnerArgs, at);
    } else {
      found.Struct = instantiateStruct(m.Owner, mp.OwnerArgs, at);
    }
    if (found.Class || found.Struct)
      static_cast<void>(instantiateMember(found, mp.Args, at, nullptr));
  }
  checkPendingInstantiations();
  Archetypes = saved;

  // Nothing made here is the module's: back to where the check began.
  auto made = [&](const auto &table) {
    for (const auto &[name, entry] : table)
      if (!before.count(name))
        ArchetypeMade.insert(name);
  };
  made(FunctionTable);
  made(Ctx.getClassTypes());
  made(Ctx.getStructTypes());
  made(Ctx.getEnumTypes());
  Diags.unmute(mark);
  SuppressedFollowOns = suppressed;
  PoisonedBindings = poisoned;
  RepeatedImportFailures = repeated;
  auto forget = [&](auto &decls, size_t keep) {
    for (size_t i = keep; i < decls.size(); ++i)
      ForeignInstantiationSources.erase(decls[i]);
    decls.resize(keep);
  };
  InstantiatedStructDecls.resize(structs);
  InstantiatedEnumDecls.resize(enums);
  InstantiatedClassDecls.resize(classes);
  InstantiatedFuncDecls.resize(funcs);
  forget(ForeignInstantiatedClassDecls, foreignClasses);
  forget(ForeignInstantiatedFuncDecls, foreignFuncs);
  forget(ForeignInstantiatedStructDecls, foreignStructs);
  forget(ImportedInstanceClassDecls, importedClasses);
  forget(ImportedInstanceStructDecls, importedStructs);
  CopiedInstances = copied;
  MethodUses.resize(methodUses);
  // A generic method's instance was added to its owner, which may be one
  // of the module's types.
  for (size_t i = madeMethods; i < InstantiatedMethods.size(); ++i) {
    const MadeMethod &m = InstantiatedMethods[i];
    if (auto *ct = ast::dyn_cast<ast::ClassType>(m.Type))
      ct->dropImplMethod(m.Method);
    else
      ast::cast<ast::StructType>(m.Type)->dropImplMethod(m.Method);
    MemberInstantiations.erase(m.Owner + names::kNameSep + m.Fn->getName());
  }
  InstantiatedMethods.resize(madeMethods);
  PendingInstantiations.clear();

  std::stable_sort(check.Copies.begin(), check.Copies.end(),
                   [](const auto &a, const auto &b) {
                     if (a.first.getLineStart() != b.first.getLineStart())
                       return a.first.getLineStart() < b.first.getLineStart();
                     return a.first.getColumnStart() < b.first.getColumnStart();
                   });
  for (const auto &[loc, msg] : check.Copies)
    Diags.error(loc, msg);
  return check.Copies.empty();
}

} // namespace paykan::sema
