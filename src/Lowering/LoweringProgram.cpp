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
      if (!cls.IsExtern)
        ClassOrigins.try_emplace(cls.Name, m.Name);
    // A struct has no PIR item: its methods and constructor are the module's
    // functions `S.m` and `S`, so the struct `S` is defined where a
    // function `S.<name>` is (names::kNameSep cannot occur in a Paykan
    // identifier).
    for (const pir::Function &fn : m.Functions) {
      size_t sep = fn.IsExtern ? std::string::npos : fn.Name.find(kNameSep);
      if (sep != std::string::npos && sep > 0)
        ClassOrigins.try_emplace(fn.Name.substr(0, sep), m.Name);
    }
    ByCanonical[name] = &m;
    ByName[m.Name] = &m;
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
  for (auto *cls : tu->getClassDecls())
    PL.ClassOrigins.try_emplace(cls->getName(), Mod.Name);
  for (auto *sd : tu->getStructDecls())
    PL.ClassOrigins.try_emplace(sd->getName(), Mod.Name);

  for (auto *imp : tu->getImports()) {
    for (auto &m : imp->getModules()) {
      std::string modulePath = imp->modulePath(m);
      const std::string &qualifier = m.qualifier();
      std::string canonical =
          module_name::canonicalImportName(modulePath, imp->isSystem());
      pir::Module *defMod = PL.lowerImport(canonical);
      if (!defMod) {
        reportInternalError("lowering failed for imported module '" +
                            canonical + "'");
        continue;
      }
      // Every function the module defines is reachable under this import
      // site's qualifier and under the full module path.
      for (const auto &fn : defMod->Functions) {
        if (fn.IsExtern)
          continue;
        ImportedFunctions[qualifier + kQualSep + fn.Name] = {defMod->Name,
                                                             fn.Name};
        if (qualifier != modulePath)
          ImportedFunctions[modulePath + kQualSep + fn.Name] = {defMod->Name,
                                                                fn.Name};
      }
    }
  }

  // Instantiations of other modules' templates, once the imports (which may
  // need the same ones) are lowered: the first module to need one emits it,
  // and the later ones declare it extern.
  for (auto *cls : SemaCtx.ForeignClasses)
    if (PL.ClassOrigins.try_emplace(cls->getName(), Mod.Name).second) {
      PL.ForeignInstantiations.insert(cls->getName());
      ForeignClasses.push_back(cls);
    }
  for (auto *fn : SemaCtx.ForeignFunctions)
    if (PL.FuncOrigins.try_emplace(fn->getName(), Mod.Name).second) {
      PL.ForeignInstantiations.insert(fn->getName());
      ForeignFuncs.push_back(fn);
    }
  for (auto *sd : SemaCtx.ForeignStructs)
    if (PL.ClassOrigins.try_emplace(sd->getName(), Mod.Name).second) {
      PL.ForeignInstantiations.insert(sd->getName());
      ForeignStructs.push_back(sd);
    }
}

bool lowerProgram(const sema::SemaContext &ctx, ast::TranslationUnit *tu,
                  const std::string &mainName, const std::string &projectRoot,
                  pir::Program &out, std::ostream &errs,
                  modules::ModuleResolver *resolver) {
  ProgramLowering pl(ctx, projectRoot, errs, resolver);
  // The main module is lowered last (its imports first) but listed first.
  ModuleLowering ml(pl, ctx, pl.claimName(mainName));
  bool ok = ml.run(tu);
  pir::Module mainMod = ml.takeModule();
  out.Modules.clear();
  out.Modules.push_back(std::move(mainMod));
  for (auto &m : pl.Modules)
    out.Modules.push_back(std::move(m));
  return ok;
}

} // namespace paykan::lowering
