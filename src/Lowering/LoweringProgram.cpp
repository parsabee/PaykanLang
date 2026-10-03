// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// AST -> PIR lowering: the program driver.  Imports are lowered before their
// importer (each module once), and the importer declares what it uses from
// them as `extern ... module "<name>"`.  Modules are named by their canonical
// module name (ModuleName.h), never by a file path, so the PIR -- and every
// backend's output -- is the same wherever the sources live (#102).

#include "LoweringInternal.h"
#include "ModuleName.h"
#include "Names.h"
#include "paykan/lowering/Lowering.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace paykan::lowering {

using namespace names;

std::string resolveImportFile(const std::string &projectRoot, bool isSystem,
                              const std::string &modulePath) {
  namespace fs = std::filesystem;
  fs::path full;
  if (isSystem) {
    const char *env = std::getenv(kPaykanStdlibEnv);
    if (env && env[0])
      full = env;
    else
      full = fs::path(projectRoot) / kStdlibDir;
  } else {
    full = projectRoot;
  }
  // "a::b::c" -> a/b/c.pkn
  std::string rel;
  size_t start = 0;
  while (start <= modulePath.size()) {
    size_t sep = modulePath.find(kQualSep, start);
    std::string part = modulePath.substr(
        start, sep == std::string::npos ? std::string::npos : sep - start);
    if (!part.empty())
      full /= part;
    if (sep == std::string::npos)
      break;
    start = sep + 2;
  }
  full += ".pkn";
  std::error_code ec;
  fs::path canon = fs::canonical(full, ec);
  if (ec)
    return "";
  return canon.string();
}

const sema::SemaContext *
ProgramLowering::lookupImportContext(const std::string &resolved) {
  if (!Indexed) {
    Indexed = true;
    // Depth-first over the SemaContext tree; the first context seen for a
    // path wins (there is normally exactly one).
    std::vector<const sema::SemaContext *> work{&Top};
    while (!work.empty()) {
      const auto *ctx = work.back();
      work.pop_back();
      for (auto &[path, child] : ctx->ImportedContexts) {
        if (!child)
          continue;
        if (Contexts.try_emplace(std::string(path), child.get()).second)
          work.push_back(child.get());
      }
    }
  }
  auto it = Contexts.find(resolved);
  return it == Contexts.end() ? nullptr : it->second;
}

std::string ProgramLowering::claimName(const std::string &name) {
  std::string unique = name;
  for (unsigned n = 2; !UsedNames.insert(unique).second; ++n)
    unique = name + kNameSep + std::to_string(n);
  return unique;
}

pir::Module *ProgramLowering::lowerImport(const std::string &resolved,
                                          const std::string &name) {
  if (auto it = ByPath.find(resolved); it != ByPath.end())
    return it->second;
  const sema::SemaContext *modCtx = lookupImportContext(resolved);
  if (!modCtx || !modCtx->Root)
    return nullptr;
  // Reserve the registry slot first: Sema rejects import cycles, but a
  // cycle that slipped through must not recurse forever.
  ByPath[resolved] = nullptr;
  // One file is one module, named after the first import that reaches it.
  // Two different files only share a name in contrived set-ups (a symlinked
  // directory, `import stdlib::io` next to `import ::io` with the default
  // standard library); claimName() keeps the names apart regardless.
  ModuleLowering ml(*this, *modCtx, claimName(name));
  if (!ml.run(modCtx->Root)) {
    Errs << "internal compiler error: lowering failed for imported module '"
         << resolved << "'\n";
    return nullptr;
  }
  Modules.push_back(ml.takeModule());
  ByPath[resolved] = &Modules.back();
  ByName[Modules.back().Name] = &Modules.back();
  return &Modules.back();
}

void ModuleLowering::processImports(ast::TranslationUnit *tu) {
  // Record where this module's own classes live before anything references
  // them (instantiations count as local declarations).
  for (auto *cls : tu->getClassDecls())
    PL.ClassOrigins.try_emplace(cls->getName(), Mod.Name);

  for (auto *imp : tu->getImports()) {
    for (auto &m : imp->getModules()) {
      std::string modulePath = imp->modulePath(m);
      const std::string &qualifier = m.qualifier();
      std::string resolved =
          resolveImportFile(PL.ProjectRoot, imp->isSystem(), modulePath);
      if (resolved.empty())
        continue; // Sema already reported the error.
      pir::Module *defMod = PL.lowerImport(
          resolved,
          module_name::canonicalImportName(modulePath, imp->isSystem()));
      if (!defMod) {
        reportInternalError("lowering failed for imported module '" + resolved +
                            "'");
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
}

bool lowerProgram(const sema::SemaContext &ctx, ast::TranslationUnit *tu,
                  const std::string &mainFile, const std::string &projectRoot,
                  pir::Program &out, std::ostream &errs) {
  ProgramLowering pl(ctx, projectRoot, errs);
  // The main module is lowered last (its imports first) but listed first.
  ModuleLowering ml(pl, ctx,
                    pl.claimName(module_name::mainModuleName(mainFile)));
  bool ok = ml.run(tu);
  pir::Module mainMod = ml.takeModule();
  out.Modules.clear();
  out.Modules.push_back(std::move(mainMod));
  for (auto &m : pl.Modules)
    out.Modules.push_back(std::move(m));
  return ok;
}

} // namespace paykan::lowering
