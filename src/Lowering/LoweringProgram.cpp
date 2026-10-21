// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// AST -> PIR lowering: the program driver.  Imports are lowered before their
// importer (each module once; one the resolver loaded from a `.pkm` is
// decoded instead), and the importer declares what it uses from them as
// `extern ... module "<name>"`.  Modules are named by their canonical
// module name (ModuleName.h), never by a file path, so the PIR -- and every
// backend's output -- is the same wherever the sources live (#102).

#include "LoweringInternal.h"
#include "ModuleName.h"
#include "Names.h"
#include "paykan/lowering/Lowering.h"

#include <string_view>
#include <variant>

namespace paykan::lowering {

using namespace names;

const sema::SemaContext *
ProgramLowering::lookupImportContext(const std::string &canonical) {
  if (!Indexed) {
    Indexed = true;
    // Depth-first over the SemaContext tree; the first context seen for a
    // module wins (there is normally exactly one).
    std::vector<const sema::SemaContext *> work{&Top};
    while (!work.empty()) {
      const auto *ctx = work.back();
      work.pop_back();
      for (auto &[name, child] : ctx->ImportedContexts) {
        if (!child)
          continue;
        if (Contexts.try_emplace(std::string(name), child.get()).second)
          work.push_back(child.get());
      }
    }
  }
  auto it = Contexts.find(canonical);
  return it == Contexts.end() ? nullptr : it->second;
}

std::string ProgramLowering::claimName(const std::string &name) {
  std::string unique = name;
  for (unsigned n = 2; !UsedNames.insert(unique).second; ++n)
    unique = name + kNameSep + std::to_string(n);
  return unique;
}

pir::Module *ProgramLowering::lowerImport(const std::string &name) {
  if (auto it = ByCanonical.find(name); it != ByCanonical.end())
    return it->second;
  // Reserve the registry slot first: Sema rejects import cycles, but a
  // cycle that slipped through must not recurse forever.
  ByCanonical[name] = nullptr;
  if (Resolver && Resolver->isLoaded(name)) {
    // A module loaded from a `.pkm`: its dependencies first (they are in
    // the program whether they were loaded or built), then its own CODE,
    // verified (docs/design/pkm.md §8.4).  The decoded module carries the
    // canonical name; a clash with a module already named so would be two
    // modules of one identity.
    for (const std::string &dep : Resolver->dependencies(name))
      if (!lowerImport(dep))
        return nullptr;
    StatusOr<pir::Module> mod = Resolver->loadCode(name);
    if (!mod) {
      Errs << "error: " << mod.status().message() << "\n";
      return nullptr;
    }
    if (!UsedNames.insert((*mod).Name).second) {
      Errs << "internal compiler error: two modules named '" << (*mod).Name
           << "'\n";
      return nullptr;
    }
    Modules.push_back(std::move(*mod));
    pir::Module &m = Modules.back();
    for (const pir::Class &cls : m.Classes)
      if (m.defines(cls))
        ClassOrigins.try_emplace(cls.Name, m.Name);
    // A struct has no PIR item: its methods and constructor are the module's
    // functions `S.m` and `S`, so the struct `S` is defined where a
    // function `S.<name>` is (names::kNameSep cannot occur in a Paykan
    // identifier).
    for (const pir::Function &fn : m.Functions) {
      size_t sep = m.defines(fn) ? fn.Name.find(kNameSep) : std::string::npos;
      if (sep != std::string::npos && sep > 0)
        ClassOrigins.try_emplace(fn.Name.substr(0, sep), m.Name);
    }
    ByCanonical[name] = &m;
    ByName[m.Name] = &m;
    ImportsOf[name] = Resolver->dependencies(name);
    return &m;
  }
  const sema::SemaContext *modCtx = lookupImportContext(name);
  if (!modCtx || !modCtx->Root)
    return nullptr;
  // One module per canonical name.  claimName() keeps the main module's
  // name apart should an import share it.
  ModuleLowering ml(*this, *modCtx, claimName(name), name);
  if (!ml.run(modCtx->Root)) {
    Errs << "internal compiler error: lowering failed for imported module '"
         << name << "'\n";
    return nullptr;
  }
  Modules.push_back(ml.takeModule());
  ByCanonical[name] = &Modules.back();
  ByName[Modules.back().Name] = &Modules.back();
  return &Modules.back();
}

void ModuleLowering::processImports(ast::TranslationUnit *tu) {
  // Record where this module's own classes and structs live before anything
  // references them (instantiations count as local declarations).
  for (auto *cls : tu->getClassDecls()) {
    PL.ClassOrigins.try_emplace(cls->getName(), Mod.Name);
    OwnTypes.insert(cls->getName());
  }
  for (auto *sd : tu->getStructDecls()) {
    PL.ClassOrigins.try_emplace(sd->getName(), Mod.Name);
    OwnTypes.insert(sd->getName());
  }

  for (auto *imp : tu->getImports()) {
    for (auto &m : imp->getModules()) {
      std::string modulePath = imp->modulePath(m);
      const std::string &qualifier = m.qualifier();
      std::string canonical =
          module_name::canonicalImportName(modulePath, imp->isSystem());
      DirectImports.push_back(canonical);
      pir::Module *defMod = PL.lowerImport(canonical);
      if (!defMod) {
        reportInternalError("lowering failed for imported module '" +
                            canonical + "'");
        continue;
      }
      // Every function the module defines is reachable under this import
      // site's qualifier and under the full module path.
      for (const auto &fn : defMod->Functions) {
        if (!defMod->defines(fn))
          continue;
        ImportedFunctions[qualifier + kQualSep + fn.Name] = {defMod->Name,
                                                             fn.Name};
        if (qualifier != modulePath)
          ImportedFunctions[modulePath + kQualSep + fn.Name] = {defMod->Name,
                                                                fn.Name};
      }
    }
  }

  if (!Canonical.empty())
    PL.ImportsOf[Canonical] = DirectImports;

  // Instantiations of other modules' templates: every module that uses one
  // lowers its own copy, so its PIR is the same in every program (and its
  // CODE can be cached); shareInstantiations keeps one per program.  The
  // origins record the first copy for a use that is not one of this
  // module's (noteExternUse).
  for (auto *cls : SemaCtx.ForeignClasses) {
    PL.ClassOrigins.try_emplace(cls->getName(), Mod.Name);
    OwnTypes.insert(cls->getName());
    PL.ForeignInstantiations.insert(cls->getName());
    ForeignClasses.push_back(cls);
  }
  for (auto *fn : SemaCtx.ForeignFunctions) {
    PL.FuncOrigins.try_emplace(fn->getName(), Mod.Name);
    PL.ForeignInstantiations.insert(fn->getName());
    ForeignFuncs.push_back(fn);
  }
  for (auto *sd : SemaCtx.ForeignStructs) {
    PL.ClassOrigins.try_emplace(sd->getName(), Mod.Name);
    OwnTypes.insert(sd->getName());
    PL.ForeignInstantiations.insert(sd->getName());
    ForeignStructs.push_back(sd);
  }
}

// -- One copy of each instantiation per program

namespace {

/// @p name up to its first '.' outside `<...>` and `(...)` (`Box<int>.get`
/// -> `Box<int>`), or all of it.  A function type's `->` closes nothing.
std::string_view typePart(std::string_view name) {
  int depth = 0;
  for (size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    if (c == '<' || c == '(')
      ++depth;
    else if (c == '-' && i + 1 < name.size() && name[i + 1] == '>')
      ++i;
    else if (c == '>' || c == ')')
      --depth;
    else if (c == '.' && depth == 0)
      return name.substr(0, i);
  }
  return name;
}

/// The type a function of a type is named after: @p name up to its first
/// '.' or "::" outside `<...>` and `(...)` (`Box<int>.get`, `Box<int>::make`
/// -> `Box<int>`), or all of it (a constructor, a free function).
std::string_view ownerPart(std::string_view name) {
  std::string_view type = typePart(name);
  int depth = 0;
  for (size_t i = 0; i + 1 < type.size(); ++i) {
    const char c = type[i];
    if (c == '<' || c == '(')
      ++depth;
    else if (c == '-' && type[i + 1] == '>')
      ++i;
    else if (c == '>' || c == ')')
      --depth;
    else if (c == ':' && type[i + 1] == ':' && depth == 0)
      return type.substr(0, i);
  }
  return type;
}

} // namespace

void ModuleLowering::markShared() {
  for (pir::Class &c : Classes_)
    c.Shared = c.Name.find('<') != std::string::npos;
  for (pir::Function &f : Funcs) {
    if (f.IsExtern)
      continue; // an extern is as shared as its definition (declareExternFrom)
    const std::string owner(ownerPart(f.Name));
    f.Shared = (f.Name.starts_with(kHiddenQualPrefix) &&
                f.Name.find('<') != std::string::npos) ||
               (owner.find('<') != std::string::npos && OwnTypes.count(owner));
  }
}

void shareInstantiations(pir::Program &program) {
  // Who keeps each shared class and function: the first module that
  // defines it, imports first (the order they were lowered in), the main
  // module last.  Each symbol has its own keeper: a module keeps only the
  // part of an instantiation it uses (a constructor without the class), and
  // shared items link by a name of their own, so the parts need not be kept
  // together.
  std::vector<pir::Module *> order;
  for (size_t i = 1; i < program.Modules.size(); ++i)
    order.push_back(&program.Modules[i]);
  if (!program.Modules.empty())
    order.push_back(&program.Modules.front());
  std::unordered_map<std::string, const pir::Module *> classKeeper, fnKeeper;
  for (const pir::Module *m : order) {
    for (const pir::Class &c : m->Classes)
      if (c.Shared && !c.IsExtern)
        classKeeper.try_emplace(c.Name, m);
    for (const pir::Function &f : m->Functions)
      if (f.Shared && !f.IsExtern)
        fnKeeper.try_emplace(f.Name, m);
  }
  if (classKeeper.empty() && fnKeeper.empty())
    return;
  auto keeper = [](const auto &map, const std::string &name) {
    auto it = map.find(name);
    return it == map.end() ? nullptr : it->second;
  };

  for (pir::Module *m : order) {
    // A vcall names the class whose vtable it reads; once that class is an
    // extern (whose vtable is not described) it carries the slot's
    // signature instead, as a vcall on an imported class does.
    std::unordered_map<std::string, std::vector<pir::VTableEntry>> vtables;
    for (pir::Class &c : m->Classes) {
      const pir::Module *k = c.Shared ? keeper(classKeeper, c.Name) : nullptr;
      if (!k || k == m)
        continue;
      if (!c.IsExtern)
        vtables[c.Name] = std::move(c.VTable);
      c.IsExtern = true;
      c.Module = k->Name;
      c.VTable.clear();
    }
    for (pir::Function &f : m->Functions) {
      const pir::Module *k =
          f.Shared ? keeper(fnKeeper, f.linkName()) : nullptr;
      if (!k || k == m)
        continue;
      if (f.IsExtern) {
        f.Module = k->Name;
        continue;
      }
      f.IsExtern = true;
      f.Module = k->Name;
      f.Symbol.clear();
      f.Params.clear();
      f.Locals.clear();
      f.Body = pir::Block();
      f.NextValueId = 1;
    }
    if (vtables.empty())
      continue;
    std::function<void(pir::Block &)> visit = [&](pir::Block &b) {
      for (pir::Stmt &s : b.Stmts) {
        if (auto *in = std::get_if<pir::Instr>(&s)) {
          if (in->Op != pir::Opcode::VCall || in->ClassName.empty())
            continue;
          auto it = vtables.find(in->ClassName);
          if (it == vtables.end() || in->Slot >= it->second.size())
            continue;
          in->Sig = it->second[in->Slot].Sig;
          in->ClassName.clear();
        } else if (auto *i = std::get_if<pir::If>(&s)) {
          visit(*i->Then);
          if (i->Else)
            visit(*i->Else);
        } else if (auto *w = std::get_if<pir::While>(&s)) {
          visit(*w->CondBlock);
          visit(*w->Body);
        } else if (auto *sw = std::get_if<pir::Switch>(&s)) {
          for (auto &c : sw->Cases)
            visit(*c.Body);
          if (sw->Default)
            visit(*sw->Default);
        }
      }
    };
    for (pir::Function &f : m->Functions)
      if (!f.IsExtern)
        visit(f.Body);
  }
}

bool lowerProgram(const sema::SemaContext &ctx, ast::TranslationUnit *tu,
                  const std::string &mainName, const std::string &projectRoot,
                  pir::Program &out, std::ostream &errs,
                  modules::ModuleResolver *resolver, Copies copies) {
  if (!resolver)
    resolver = ctx.Resolver.get(); // the one Sema made
  ProgramLowering pl(ctx, projectRoot, errs, resolver);
  // The main module is lowered last (its imports first) but listed first.
  ModuleLowering ml(pl, ctx, pl.claimName(mainName));
  bool ok = ml.run(tu);
  pir::Module mainMod = ml.takeModule();
  out.Modules.clear();
  out.Modules.push_back(std::move(mainMod));
  for (auto &m : pl.Modules)
    out.Modules.push_back(std::move(m));
  if (copies == Copies::Shared)
    shareInstantiations(out);
  return ok;
}

} // namespace paykan::lowering
