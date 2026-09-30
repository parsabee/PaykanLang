// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Semantic analysis: import resolution and module caching.

#include "ModuleUtils.h"
#include "Names.h"
#include "ParserDriver.h"
#include "Sema.h"

#include <algorithm>

namespace paykan {
namespace sema {

// ---------------------------------------------------------------------------
// Static helpers (file-scope, not part of the Sema class)
// ---------------------------------------------------------------------------

// Types are serialised by name via the shared ast::typeName (ASTContext.cpp).
// A name it cannot express ("unknown", never registered) fails
// resolveExportedType below and is diagnosed by the caller as an unknown type
// — export reconstruction never silently substitutes Obj or void.

/// Parse one serialised type starting at @p pos in @p text, advancing @p pos
/// past it.  Grammar (the output language of ast::typeName):
///
///   type  := base suffix*
///   base  := '(' type (', ' type)+ ')'        -- tuple (arity >= 2)
///          | name                             -- builtin / class / enum
///   suffix := '[]'
///
/// A name runs up to the next ',' or ')' at this nesting level or the next
/// "[]" suffix.  Returns nullptr (leaving @p pos wherever it stopped) if the
/// text is malformed or a base name is unknown in @p ctx.
static ast::Type *parseExportedType(ast::ASTContext &ctx,
                                    const std::string &text, size_t &pos) {
  ast::Type *ty = nullptr;
  if (pos < text.size() && text[pos] == '(') {
    ++pos;
    std::vector<ast::Type *> elems;
    for (;;) {
      ast::Type *elem = parseExportedType(ctx, text, pos);
      if (!elem)
        return nullptr;
      elems.push_back(elem);
      if (text.compare(pos, 2, ", ") == 0) {
        pos += 2;
        continue;
      }
      if (pos < text.size() && text[pos] == ')') {
        ++pos;
        break;
      }
      return nullptr;
    }
    if (elems.size() < 2)
      return nullptr;
    ty = ctx.getTupleType(std::move(elems));
  } else {
    size_t start = pos;
    while (pos < text.size() && text[pos] != ',' && text[pos] != ')' &&
           text[pos] != '[')
      ++pos;
    if (pos == start)
      return nullptr;
    ty = ctx.lookupType(text.substr(start, pos - start));
    if (!ty)
      return nullptr;
  }
  while (text.compare(pos, 2, "[]") == 0) {
    pos += 2;
    ty = ctx.getArrayType(ty);
  }
  return ty;
}

/// Resolve a serialised type name — a builtin/class/enum name, optionally
/// wrapped in "[]" array markers and/or "(T1, T2)" tuple parentheses, nested
/// arbitrarily — to the canonical Type* in @p ctx.  Returns nullptr if any
/// base name is unknown or the text is malformed.
static ast::Type *resolveExportedType(ast::ASTContext &ctx,
                                      const std::string &name) {
  size_t pos = 0;
  ast::Type *ty = parseExportedType(ctx, name, pos);
  return (ty && pos == name.size()) ? ty : nullptr;
}

/// True for the class names every ASTContext registers at construction (Obj,
/// Str, Array, File, ...).  They are never exported: each context bootstraps
/// its own copy, so they are always present on the importing side.
static bool isBootstrapClassName(const std::string &name) {
  static const ast::ASTContext Probe;
  return Probe.lookupClassType(name) != nullptr;
}

// ---------------------------------------------------------------------------
// resolveModulePath
// ---------------------------------------------------------------------------

std::string Sema::resolveModulePath(const std::string &modulePath,
                                    bool isSystem, ast::SourceLocation loc) {
  auto relPath = module_utils::modulePathToRelative(modulePath);

  if (isSystem) {
    llvm::SmallString<256> base;
    const char *stdlibEnv = std::getenv(names::kPaykanStdlibEnv);
    if (stdlibEnv && stdlibEnv[0])
      base = stdlibEnv;
    else {
      base = ProjectRoot;
      llvm::sys::path::append(base, names::kStdlibDir);
    }
    llvm::sys::path::append(base, relPath);
    if (auto resolved = module_utils::realPath(base); !resolved.empty())
      return resolved;
    error(loc, "system module '" + modulePath + "' not found (tried " +
                   std::string(base) + ")");
    return "";
  }

  llvm::SmallString<256> full(ProjectRoot);
  llvm::sys::path::append(full, relPath);
  if (auto resolved = module_utils::realPath(full); !resolved.empty())
    return resolved;
  error(loc, "module '" + modulePath + "' not found (tried " +
                 std::string(full) + ")");
  return "";
}

// ---------------------------------------------------------------------------
// processImport
// ---------------------------------------------------------------------------

bool Sema::processImport(ast::ImportDecl *node) {
  const bool isSystem = node->isSystem();

  // Helper: load one module from a resolved path, cache it, inject exports.
  auto loadModule = [&](const std::string &path, const std::string &qualifier,
                        const std::string &fullModulePath,
                        ast::SourceLocation loc) -> bool {
    // Cycle detection.
    if (ImportStack && ImportStack->count(path)) {
      error(loc, "circular import detected for '" + path + "'");
      return false;
    }

    // -- Reconstruction helpers ---------------------------------------------
    //
    // Shared by the cache-hit and fresh-load paths.  They rebuild a module's
    // exported entities in the importing context (Ctx) and register the
    // qualified names (short qualifier and full module path) of the entities
    // the module itself declares.
    //
    // Every type name an export mentions must resolve here: the exporting
    // module type-checked, so each such type is either a builtin, declared by
    // the module, or reached through the module's own imports — and all of
    // those are serialised alongside the export.  A name that still fails to
    // resolve is a compiler bug or a corrupt cache entry, and is reported
    // against the import site rather than degraded to Obj/void, which would
    // only surface later as a baffling type mismatch at a call site.
    const std::string diagPrefix =
        "import of module '" + fullModulePath + "': ";

    auto resolveOrReport = [&](const std::string &tyName,
                               const std::string &what) -> ast::Type * {
      if (auto *ty = resolveExportedType(Ctx, tyName))
        return ty;
      error(loc, diagPrefix + what + " has unknown type '" + tyName + "'");
      return nullptr;
    };

    // Type names are global across the import graph (a type is reconstructed
    // once, by canonical name, so that it keeps a single identity however many
    // import paths reach it).  Binding a name to a type declared by a
    // DIFFERENT module would silently merge two unrelated types; reject it.
    auto checkOrigin = [&](const char *kind, const std::string &name,
                           const std::string &origin) -> bool {
      if (origin.empty())
        return true; // compiler builtin — identical in every context
      auto it = ImportedTypeOrigins.find(name);
      if (it == ImportedTypeOrigins.end()) {
        ImportedTypeOrigins[name] = origin;
        return true;
      }
      if (it->second == origin)
        return true;
      error(loc, diagPrefix + kind + " '" + name +
                     "' conflicts with a type of the same name declared by "
                     "module '" +
                     it->second + "' (type names are global across imports)");
      return false;
    };

    auto addClassAliases = [&](ast::ClassType *ct, const std::string &name) {
      Ctx.addClassTypeAlias(qualifier + names::kQualSep + name, ct);
      if (qualifier != fullModulePath)
        Ctx.addClassTypeAlias(fullModulePath + names::kQualSep + name, ct);
    };
    auto addEnumAliases = [&](ast::EnumType *et, const std::string &name) {
      Ctx.addEnumTypeAlias(qualifier + names::kQualSep + name, et);
      if (qualifier != fullModulePath)
        Ctx.addEnumTypeAlias(fullModulePath + names::kQualSep + name, et);
    };

    // Reconstruct every exported enum and class.  Returns false (with
    // diagnostics emitted) if anything failed to resolve.
    auto registerExportedTypes = [&](const ModuleInfo &info) -> bool {
      bool ok = true;

      // Enums first: class fields and method signatures may be enum-typed.
      for (auto &ei : info.ExportedEnums) {
        if (!checkOrigin("enum", ei.Name, ei.OriginPath)) {
          ok = false;
          continue;
        }
        auto *et = Ctx.lookupEnumType(ei.Name);
        if (!et) {
          if (Ctx.lookupClassType(ei.Name)) {
            error(loc, diagPrefix + "enum '" + ei.Name +
                           "' conflicts with a class of the same name "
                           "already in scope");
            ok = false;
            continue;
          }
          et = Ctx.registerEnumType(ei.Name, loc);
          for (auto &v : ei.Variants)
            et->addVariant(Ctx.intern(v));
        } // else: already present — pointer identity preserved
        if (ei.IsLocal)
          addEnumAliases(et, ei.Name);
      }

      // Classes, phase 1: pre-register a stub for every class not already
      // present, superclass before subclass.  All stubs exist before any
      // signature is resolved, so a method or field may name any class of the
      // module regardless of the (unordered) serialisation order — this is
      // the same two-phase scheme checkClassDecls uses for a single file.
      std::vector<const ModuleInfo::ClassInfo *> pending, created;
      for (auto &ci : info.ExportedClasses) {
        if (!checkOrigin("class", ci.Name, ci.OriginPath)) {
          ok = false;
          continue;
        }
        if (auto *ct = Ctx.lookupClassType(ci.Name)) {
          // Already present — pointer identity preserved.
          if (ci.IsLocal)
            addClassAliases(ct, ci.Name);
          continue;
        }
        if (Ctx.lookupEnumType(ci.Name)) {
          error(loc, diagPrefix + "class '" + ci.Name +
                         "' conflicts with an enum of the same name already "
                         "in scope");
          ok = false;
          continue;
        }
        pending.push_back(&ci);
      }
      for (bool progress = true; progress && !pending.empty();) {
        progress = false;
        for (auto it = pending.begin(); it != pending.end();) {
          const auto &ci = **it;
          ast::ClassType *super = ci.SuperClassName.empty()
                                      ? Ctx.getObjTy()
                                      : Ctx.lookupClassType(ci.SuperClassName);
          if (!super) {
            ++it; // superclass not registered yet — retry next round
            continue;
          }
          Ctx.preRegisterClassType(ci.Name, super);
          created.push_back(&ci);
          it = pending.erase(it);
          progress = true;
        }
      }
      for (auto *ci : pending) {
        error(loc, diagPrefix + "superclass '" + ci->SuperClassName +
                       "' of class '" + ci->Name + "' is unknown");
        ok = false;
      }

      // Classes, phase 2: populate fields and method signatures in creation
      // order (a superclass is always created — hence populated — before its
      // subclasses, so the inherited vtable prefix is complete).
      for (auto *ci : created) {
        auto *ct = Ctx.lookupClassType(ci->Name);
        ct->reinheritVTable(ct->getSuperClass());
        ast::ASTContext::ClassTypeBuilder builder(Ctx, ct);
        for (auto &f : ci->Fields) {
          auto *fty =
              resolveOrReport(f.TypeName, "field '" + f.FieldName +
                                              "' of class '" + ci->Name + "'");
          if (!fty) {
            ok = false;
            continue;
          }
          builder.field(f.FieldName, fty);
        }
        for (auto &m : ci->Methods) {
          const std::string decl = "method '" + ci->Name + "." + m.Name + "'";
          auto *retTy =
              resolveOrReport(m.ReturnTypeName, "return type of " + decl);
          bool sigOk = retTy != nullptr;
          std::vector<ast::Type *> params;
          for (size_t i = 0; i < m.ParamTypeNames.size(); ++i) {
            auto *pty = resolveOrReport(m.ParamTypeNames[i],
                                        "parameter " + std::to_string(i + 1) +
                                            " of " + decl);
            if (pty)
              params.push_back(pty);
            else
              sigOk = false;
          }
          if (!sigOk) {
            ok = false;
            continue;
          }
          builder.method(m.Name, retTy, std::move(params), m.Flags);
        }
        builder.build();
        if (ci->IsLocal)
          addClassAliases(ct, ci->Name);
      }
      return ok;
    };

    auto injectFnAs = [&](const ModuleInfo::FunctionInfo &fi,
                          const std::string &name) -> bool {
      if (lookupFunction(name))
        return true; // same module already imported under this qualifier
      const std::string decl = "function '" + fi.Name + "'";
      auto *retTy =
          resolveOrReport(fi.ReturnTypeName, "return type of " + decl);
      bool sigOk = retTy != nullptr;
      std::vector<ast::Type *> params;
      for (size_t i = 0; i < fi.ParamTypeNames.size(); ++i) {
        auto *pty = resolveOrReport(fi.ParamTypeNames[i],
                                    "parameter " + std::to_string(i + 1) +
                                        " of " + decl);
        if (pty)
          params.push_back(pty);
        else
          sigOk = false;
      }
      if (!sigOk)
        return false;
      declareFunction(name, retTy, std::move(params), /*isBuiltin=*/true);
      return true;
    };

    // Inject everything a cached ModuleInfo exports: types first (function
    // signatures name them), then functions as qualifier::name and
    // fullModulePath::name.
    auto injectExports = [&](const ModuleInfo &info) -> bool {
      bool ok = registerExportedTypes(info);
      for (auto &fi : info.ExportedFunctions) {
        if (!injectFnAs(fi, qualifier + names::kQualSep + fi.Name))
          ok = false;
        if (qualifier != fullModulePath &&
            !injectFnAs(fi, fullModulePath + names::kQualSep + fi.Name))
          ok = false;
      }
      return ok;
    };

    // Check cache first.
    auto cacheIt = ModuleCache.find(path);
    if (cacheIt != ModuleCache.end())
      return injectExports(cacheIt->second);

    // Parse the imported file, with the parser wired to a DiagEngine that
    // carries the imported file's name and source lines — so a syntax error
    // inside a module prints the same rich source-located format
    // (file:line:col + snippet + caret) as main-file parse errors, instead of
    // the yacc-style fallback.  Mirrors the wiring in src/Driver/main.cpp:
    // SourceLines lives in the driver and is filled by parseFile before the
    // parser runs, so handing its address over now is safe.
    auto importDriverPtr = std::make_shared<parser::ParserDriver>();
    DiagEngine importDiag(Diags.getOS());
    importDiag.setSourceInfo(path, &importDriverPtr->getSourceLines());
    importDriverPtr->setDiagEngine(&importDiag);
    if (importDriverPtr->parseFile(path) != 0) {
      importDriverPtr->setDiagEngine(nullptr);
      error(loc, "failed to parse module '" + path + "'");
      return false;
    }
    // importDiag dies with this call frame but the driver (kept alive in the
    // returned SemaContext) does not — detach it now that parsing is done.
    importDriverPtr->setDiagEngine(nullptr);

    // Run Sema on the imported module, reusing the same engine (re-pointed at
    // the file name the parse recorded).
    importDiag.setSourceInfo(importDriverPtr->getCurrentFile(),
                             &importDriverPtr->getSourceLines());
    Sema importSema(importDriverPtr->getASTContext(), importDiag, ProjectRoot);
    if (ImportStack)
      ImportStack->insert(path);
    importSema.ImportStack = ImportStack;

    auto *importRoot = importDriverPtr->getRoot();
    auto childCtx = importSema.run(importRoot);
    bool importOk = (bool)childCtx;
    if (ImportStack)
      ImportStack->erase(path);
    if (!importOk) {
      error(loc, "errors in imported module '" + path + "'");
      return false;
    }

    // -- Serialise the module's exports --------------------------------------
    //
    // The module's type registry holds (a) the classes/enums it declares,
    // (b) the ones it reached through its own imports, and (c) compiler
    // builtins plus lazily-created Array<T> specialisations.  (a) is exported
    // under the importer's qualifier.  (b) is exported WITHOUT a qualifier —
    // it must exist in the importing context so the module's signatures that
    // mention it resolve (with type identity preserved), but names are never
    // re-exported: to spell such a type the importer imports its defining
    // module.  (c) is never exported.
    ModuleInfo info;
    auto &modCtx = importDriverPtr->getASTContext();
    llvm::StringSet<> localClasses, localEnums;
    for (auto *cd : importRoot->getClassDecls())
      localClasses.insert(cd->getName());
    for (auto *ed : importRoot->getEnumDecls())
      localEnums.insert(ed->getName());

    for (auto &[name, ct] : modCtx.getClassTypes()) {
      // The registry holds each class under its canonical name and again
      // under any qualified import aliases; serialise only the canonical
      // entry so a class reached through several import paths keeps one
      // identity.
      if (name != ct->getName())
        continue;
      // Lazily-created Array<T> / Tuple<T1, T2> specialisations are never
      // exported either: the importing context rebuilds its own from the
      // `T[]` / `(T1, T2)` spellings in the signatures that use them.
      if (isBootstrapClassName(name) ||
          modCtx.getSpecializedArrayElemType(ct) ||
          modCtx.getSpecializedTupleElemType(ct))
        continue;
      ModuleInfo::ClassInfo ci;
      ci.Name = name;
      ci.IsLocal = localClasses.count(name) != 0;
      ci.OriginPath =
          ci.IsLocal ? path : importSema.ImportedTypeOrigins.lookup(name);
      if (ct->getSuperClass()) {
        ci.SuperClassName = ct->getSuperClass()->getName();
        if (ci.SuperClassName == names::kObj)
          ci.SuperClassName = "";
      }
      for (auto &[fname, fty] : ct->getFields())
        ci.Fields.push_back({fname, ast::typeName(fty)});
      for (auto *m : ct->getVTable()) {
        ModuleInfo::ClassInfo::MethodInfo mi;
        mi.Name = m->getName();
        mi.ReturnTypeName = ast::typeName(m->getReturnType());
        for (auto *pty : m->getParamTypes())
          mi.ParamTypeNames.push_back(ast::typeName(pty));
        mi.Flags =
            static_cast<uint8_t>(m->isPrivate() ? ast::MethodDecl::Private : 0);
        ci.Methods.push_back(std::move(mi));
      }
      info.ExportedClasses.push_back(std::move(ci));
    }

    // Enum types: name + variants in declaration order (canonical entries
    // only, as for classes).
    for (auto &[name, et] : modCtx.getEnumTypes()) {
      if (name != et->getName())
        continue; // alias key — skip
      ModuleInfo::EnumInfo ei;
      ei.Name = name;
      ei.IsLocal = localEnums.count(name) != 0;
      ei.OriginPath =
          ei.IsLocal ? path : importSema.ImportedTypeOrigins.lookup(name);
      for (auto *v : et->getVariants())
        ei.Variants.push_back(*v);
      info.ExportedEnums.push_back(std::move(ei));
    }

    // The registries are unordered maps; sort so reconstruction (and any
    // diagnostics it emits) is deterministic.
    auto byLocalThenName = [](const auto &a, const auto &b) {
      if (a.IsLocal != b.IsLocal)
        return a.IsLocal;
      return a.Name < b.Name;
    };
    std::sort(info.ExportedClasses.begin(), info.ExportedClasses.end(),
              byLocalThenName);
    std::sort(info.ExportedEnums.begin(), info.ExportedEnums.end(),
              byLocalThenName);

    // Free functions and constructors declared by the module.  Functions the
    // module itself imported were injected as builtins and are not
    // re-exported.
    for (auto &[name, sig] : importSema.FunctionTable) {
      if (sig.IsBuiltin)
        continue;
      ModuleInfo::FunctionInfo fi;
      fi.Name = name.str();
      fi.ReturnTypeName = ast::typeName(sig.ReturnType);
      for (auto *pty : sig.ParamTypes)
        fi.ParamTypeNames.push_back(ast::typeName(pty));
      info.ExportedFunctions.push_back(std::move(fi));
    }
    std::sort(info.ExportedFunctions.begin(), info.ExportedFunctions.end(),
              [](const auto &a, const auto &b) { return a.Name < b.Name; });

    ModuleCache[path] = std::move(info);
    bool ok = injectExports(ModuleCache[path]);

    childCtx.OwnedDriver = importDriverPtr;
    childCtx.Root = importRoot;
    AccumulatedImportContexts[path] =
        std::make_shared<SemaContext>(std::move(childCtx));
    return ok;
  };

  bool ok = true;
  for (auto &m : node->getModules()) {
    std::string fullPath = node->modulePath(m);
    const std::string &qualifier = m.qualifier();
    std::string resolved =
        resolveModulePath(fullPath, isSystem, node->getLocation());
    if (resolved.empty()) {
      ok = false;
      continue;
    }
    if (!loadModule(resolved, qualifier, fullPath, node->getLocation()))
      ok = false;
  }
  return ok;
}

} // namespace sema
} // namespace paykan
