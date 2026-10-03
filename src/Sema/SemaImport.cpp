// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Semantic analysis: import resolution and module caching.

#include "ModuleName.h"
#include "ModuleUtils.h"
#include "Names.h"
#include "ParserDriver.h"
#include "Sema.h"

#include <algorithm>
#include <filesystem>
#include <utility>

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
///   suffix := '[]'                            -- array of the type so far
///           | '?'                             -- optional of the type so far
///
/// Suffixes apply left to right, mirroring how ast::typeName spells nesting:
/// "Str?[]" is an array of optional strings, "int[]?" an optional array, and
/// "(Node?, int)[]?" an optional array of tuples.  A name runs up to the next
/// ',' or ')' at this nesting level or the next suffix.  A generic
/// instantiation's name carries its type arguments (`Pair<Str, int>`,
/// `Box<Node?[]>`): everything between its '<' and the matching '>' is part of
/// the name, which is registered under exactly that spelling.  Returns nullptr
/// (leaving @p pos wherever it stopped) if the text is malformed or a base
/// name is unknown in @p ctx.
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
           text[pos] != '[' && text[pos] != '?') {
      if (text[pos] == '<') {
        // Instantiation name: skip to the matching '>'.
        int depth = 0;
        do {
          if (text[pos] == '<')
            ++depth;
          else if (text[pos] == '>')
            --depth;
          ++pos;
        } while (pos < text.size() && depth > 0);
        if (depth != 0)
          return nullptr;
        continue;
      }
      ++pos;
    }
    if (pos == start)
      return nullptr;
    ty = ctx.lookupType(text.substr(start, pos - start));
    if (!ty)
      return nullptr;
  }
  for (;;) {
    if (text.compare(pos, 2, "[]") == 0) {
      pos += 2;
      ty = ctx.getArrayType(ty);
    } else if (pos < text.size() && text[pos] == '?') {
      ++pos;
      ty = ctx.getOptionalType(ty);
    } else {
      break;
    }
  }
  return ty;
}

/// Resolve a serialised type name — a builtin/class/enum name, optionally
/// wrapped in "[]" array / "?" optional markers and/or "(T1, T2)" tuple
/// parentheses, nested arbitrarily — to the canonical Type* in @p ctx.
/// Returns nullptr if any base name is unknown or the text is malformed.
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
// Failed imports and diagnostics
// ---------------------------------------------------------------------------

bool Sema::isFailedImportUse(std::string_view name) {
  if (FailedImportQualifiers.empty())
    return false;
  // The qualifier is everything before the last "::" of the name proper (a
  // type argument list may hold qualified names of its own).
  std::string_view base = name.substr(0, name.find('<'));
  size_t sep = base.rfind(names::kQualSep);
  if (sep == std::string_view::npos ||
      !FailedImportQualifiers.count(base.substr(0, sep)))
    return false;
  ++SuppressedFollowOns;
  return true;
}

std::string Sema::displayPath(const std::filesystem::path &path) const {
  if (path.is_relative())
    return path.string();
  // An empty root is the current directory (see module_utils::appendPath).
  std::error_code ec;
  auto root = std::filesystem::weakly_canonical(
      ProjectRoot.empty() ? std::filesystem::path(".")
                          : std::filesystem::path(ProjectRoot),
      ec);
  if (ec)
    return path.string();
  auto rel = path.lexically_relative(root);
  if (rel.empty() || *rel.begin() == "..")
    return path.string();
  return rel.string();
}

// ---------------------------------------------------------------------------
// resolveModulePath
// ---------------------------------------------------------------------------

std::string Sema::resolveModulePath(const std::string &modulePath,
                                    bool isSystem, ast::SourceLocation loc) {
  auto relPath = module_utils::modulePathToRelative(modulePath);

  if (isSystem) {
    std::filesystem::path base;
    const char *stdlibEnv = std::getenv(names::kPaykanStdlibEnv);
    if (stdlibEnv && stdlibEnv[0])
      base = stdlibEnv;
    else
      base = module_utils::appendPath(ProjectRoot, names::kStdlibDir);
    base = module_utils::appendPath(base, relPath);
    return checkModuleFile(
        base, displayPath(base),
        module_name::canonicalImportName(modulePath, /*isSystem=*/true),
        "system module", loc);
  }

  std::filesystem::path full = module_utils::appendPath(ProjectRoot, relPath);
  // Shown relative to the source root, like every imported file.
  return checkModuleFile(
      full, relPath,
      module_name::canonicalImportName(modulePath, /*isSystem=*/false),
      "module", loc);
}

std::string Sema::checkModuleFile(const std::filesystem::path &file,
                                  const std::string &shown,
                                  const std::string &module, const char *kind,
                                  ast::SourceLocation loc) {
  std::string resolved = module_utils::realPath(file);
  if (resolved.empty()) {
    error(loc, std::string(kind) + " '" + module + "' not found (tried " +
                   shown + ")");
    return "";
  }
  // `lib/m.pkn/` -- a directory where the module's file should be (#120).
  std::error_code ec;
  if (!std::filesystem::is_regular_file(resolved, ec)) {
    error(loc, std::string(kind) + " '" + module + "': '" + shown + "' is " +
                   (std::filesystem::is_directory(resolved, ec)
                        ? "a directory"
                        : "not a regular file") +
                   ", not a source file");
    return "";
  }
  return resolved;
}

// ---------------------------------------------------------------------------
// processImport
// ---------------------------------------------------------------------------

bool Sema::processImport(ast::ImportDecl *node) {
  const bool isSystem = node->isSystem();

  // Helper: load one module from a resolved path, cache it, inject exports.
  auto loadModule = [&](const std::string &path, const std::string &qualifier,
                        const std::string &fullModulePath,
                        const std::string &moduleName,
                        ast::SourceLocation loc) -> bool {
    // Cycle detection.
    if (ImportStack && ImportStack->count(path)) {
      error(loc, "circular import of module '" + moduleName + "'");
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
    const std::string diagPrefix = "import of module '" + moduleName + "': ";

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
                           const std::string &originPath,
                           const std::string &originModule) -> bool {
      if (originPath.empty())
        return true; // compiler builtin — identical in every context
      auto it = ImportedTypeOrigins.find(name);
      if (it == ImportedTypeOrigins.end()) {
        ImportedTypeOrigins[name] = {originPath, originModule};
        return true;
      }
      if (it->second.Path == originPath)
        return true;
      const std::string &other = it->second.Module.empty()
                                     ? displayPath(it->second.Path)
                                     : it->second.Module;
      error(loc, diagPrefix + kind + " '" + name +
                     "' conflicts with a type of the same name declared by "
                     "module '" +
                     other + "' (type names are global across imports)");
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
        if (!checkOrigin("enum", ei.Name, ei.OriginPath, ei.OriginModule)) {
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
        if (!checkOrigin("class", ci.Name, ci.OriginPath, ci.OriginModule)) {
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
    auto importDriverPtr = std::make_shared<parser::ParserDriver>(FrontendName);
    // Diagnostics name the module's file by its path from the source root.
    DiagEngine importDiag(Diags.getOS());
    importDiag.setSourceInfo(displayPath(path),
                             &importDriverPtr->getSourceLines());
    importDriverPtr->setDiagEngine(&importDiag);
    // An imported module's own errors are reported through importDiag, with
    // its file name and source lines; the import site adds no error of its
    // own (it would only repeat them, once per importing module on the way
    // up), just a note pointing at the import that led there.  They still
    // fail this module (ImportedModuleErrors).
    auto failedModule = [&](unsigned moduleErrors) {
      if (moduleErrors == 0) {
        // Nothing was reported (should not happen): keep the failure visible.
        error(loc, "errors in imported module '" + moduleName + "'");
        return false;
      }
      ImportedModuleErrors += moduleErrors;
      Diags.note(loc, "in module '" + moduleName + "' imported here");
      return false;
    };
    if (importDriverPtr->parseFile(path) != 0) {
      importDriverPtr->setDiagEngine(nullptr);
      return failedModule(importDiag.getErrorCount());
    }
    // importDiag dies with this call frame but the driver (kept alive in the
    // returned SemaContext) does not — detach it now that parsing is done.
    importDriverPtr->setDiagEngine(nullptr);

    // Run Sema on the imported module, reusing the same engine.
    Sema importSema(importDriverPtr->getASTContext(), importDiag, ProjectRoot,
                    FrontendName);
    if (ImportStack)
      ImportStack->insert(path);
    importSema.ImportStack = ImportStack;

    auto *importRoot = importDriverPtr->getRoot();
    auto childCtx = importSema.run(importRoot);
    bool importOk = (bool)childCtx;
    if (ImportStack)
      ImportStack->erase(path);
    if (!importOk)
      return failedModule(childCtx.ErrorCount);

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
    // Defining module of a type the imported module itself reached through
    // an import ("" when unknown).
    auto lookupOrigin = [](const Sema &s, const std::string &name) {
      auto it = s.ImportedTypeOrigins.find(name);
      return it == s.ImportedTypeOrigins.end() ? TypeOrigin() : it->second;
    };
    StringSet localClasses, localEnums;
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
      TypeOrigin origin = ci.IsLocal ? TypeOrigin{path, moduleName}
                                     : lookupOrigin(importSema, name);
      ci.OriginPath = std::move(origin.Path);
      ci.OriginModule = std::move(origin.Module);
      if (ct->getSuperClass()) {
        ci.SuperClassName = ct->getSuperClass()->getName();
        if (ci.SuperClassName == names::kObj)
          ci.SuperClassName = "";
      }
      for (auto &[fname, fty] : ct->getFields())
        ci.Fields.push_back({fname, ast::typeName(fty)});
      // __init__ has no vtable slot but is part of the class's interface: a
      // subclass in the importing module must call it through `__super__`
      // with the right arguments (addMethod files it back as the init).
      std::vector<ast::MethodDecl *> methods = ct->getVTable();
      if (auto *init = ct->findMethod(names::kMethodInit))
        methods.push_back(init);
      for (auto *m : methods) {
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
      TypeOrigin origin = ei.IsLocal ? TypeOrigin{path, moduleName}
                                     : lookupOrigin(importSema, name);
      ei.OriginPath = std::move(origin.Path);
      ei.OriginModule = std::move(origin.Module);
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
      fi.Name = name;
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
    // The names a failed import binds are poisoned: their uses were reported
    // with the import (#119).
    // (A name that already names another module keeps it.)
    std::string resolved;
    auto poison = [&] {
      for (const std::string *name : {&qualifier, &std::as_const(fullPath)}) {
        auto it = ImportQualifiers.find(*name);
        if (it == ImportQualifiers.end() || it->second.Resolved == resolved)
          FailedImportQualifiers.insert(*name);
      }
      ok = false;
    };
    resolved = resolveModulePath(fullPath, isSystem, node->getLocation());
    if (resolved.empty()) {
      poison();
      continue;
    }
    // The names this import binds must not already name another module.
    bool clash = false;
    for (const std::string *name :
         {&qualifier, static_cast<const std::string *>(&fullPath)}) {
      auto [it, fresh] = ImportQualifiers.try_emplace(
          *name, ImportQualifier{resolved, fullPath});
      if (fresh || it->second.Resolved == resolved)
        continue;
      error(node->getLocation(),
            "import of module '" + fullPath + "': qualifier '" + *name +
                "' already names module '" + it->second.ModulePath +
                "' (give one of them an alias with 'as')");
      clash = true;
      break;
    }
    if (clash) {
      ok = false;
      continue;
    }
    if (!loadModule(resolved, qualifier, fullPath,
                    module_name::canonicalImportName(fullPath, isSystem),
                    node->getLocation()))
      poison();
  }
  return ok;
}

} // namespace sema
} // namespace paykan
