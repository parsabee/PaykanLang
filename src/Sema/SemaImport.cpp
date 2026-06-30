// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Semantic analysis: import resolution and module caching.

#include "ModuleUtils.h"
#include "Names.h"
#include "ParserDriver.h"
#include "Sema.h"

namespace paykan {
namespace sema {

// ---------------------------------------------------------------------------
// Static helpers (file-scope, not part of the Sema class)
// ---------------------------------------------------------------------------

/// Return the canonical string name for a type (used when serialising
/// ClassInfo).
static std::string typeToName(ast::Type *ty) {
  if (!ty)
    return names::kTypeVoid;
  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(ty)) {
    switch (bt->getTypeKind()) {
    case ast::BuiltinType::Int:
      return names::kTypeInt;
    case ast::BuiltinType::Float:
      return names::kTypeFloat;
    case ast::BuiltinType::Bool:
      return names::kTypeBool;
    case ast::BuiltinType::Char:
      return names::kTypeChar;
    case ast::BuiltinType::Void:
      return names::kTypeVoid;
    }
  }
  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty))
    return ct->getName();
  if (auto *et = ast::dyn_cast<ast::EnumType>(ty))
    return et->getName();
  return names::kTypeVoid;
}

/// Reconstruct a single ClassInfo into a context, guaranteeing type identity.
static void registerClassInfoInto(
    const std::string &name, const std::string &superName,
    const std::vector<std::pair<std::string, std::string>> &fields,
    const std::vector<std::tuple<std::string, std::string,
                                 std::vector<std::string>, uint8_t>> &methods,
    ast::ASTContext &ctx) {
  if (ctx.lookupClassType(name))
    return; // already present — pointer identity guaranteed

  ast::ClassType *superClass = nullptr;
  if (!superName.empty())
    superClass = ctx.lookupClassType(superName);
  if (!superClass)
    superClass = ctx.getObjTy();

  auto builder = ctx.buildClassType(name, superClass);
  for (auto &[fname, ftname] : fields) {
    ast::Type *fty = ctx.lookupType(ftname);
    if (!fty)
      fty = ctx.getObjTy();
    builder.field(fname, fty);
  }
  for (auto &[mname, retName, paramNames, flags] : methods) {
    ast::Type *retTy = ctx.lookupType(retName);
    if (!retTy)
      retTy = ctx.getVoidTy();
    std::vector<ast::Type *> params;
    for (auto &pn : paramNames) {
      ast::Type *pty = ctx.lookupType(pn);
      if (!pty)
        pty = ctx.getObjTy();
      params.push_back(pty);
    }
    builder.method(mname, retTy, std::move(params), flags);
  }
  builder.build();
}

/// Reconstruct a single enum into a context, guaranteeing type identity.
static void registerEnumInfoInto(const std::string &name,
                                 const std::vector<std::string> &variants,
                                 ast::ASTContext &ctx) {
  if (ctx.lookupEnumType(name))
    return; // already present — pointer identity guaranteed
  auto *et = ctx.registerEnumType(name, ast::SourceLocation());
  if (!et)
    return;
  for (auto &v : variants)
    et->addVariant(ctx.intern(v));
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

  // Helper: register a cached ClassInfo into an ASTContext.
  auto applyClassInfo = [&](const ModuleInfo::ClassInfo &ci,
                            ast::ASTContext &ctx) {
    std::vector<std::pair<std::string, std::string>> fields;
    fields.reserve(ci.Fields.size());
    for (auto &f : ci.Fields)
      fields.emplace_back(f.FieldName, f.TypeName);
    std::vector<
        std::tuple<std::string, std::string, std::vector<std::string>, uint8_t>>
        methods;
    methods.reserve(ci.Methods.size());
    for (auto &m : ci.Methods)
      methods.emplace_back(m.Name, m.ReturnTypeName, m.ParamTypeNames, m.Flags);
    registerClassInfoInto(ci.Name, ci.SuperClassName, fields, methods, ctx);
  };

  // Helper: load one module from a resolved path, cache it, inject exports.
  auto loadModule = [&](const std::string &path, const std::string &qualifier,
                        const std::string &fullModulePath,
                        ast::SourceLocation loc) -> bool {
    // Cycle detection.
    if (ImportStack && ImportStack->count(path)) {
      error(loc, "circular import detected for '" + path + "'");
      return false;
    }

    // Check cache first.
    auto cacheIt = ModuleCache.find(path);
    if (cacheIt != ModuleCache.end()) {
      for (auto &ei : cacheIt->second.ExportedEnums) {
        registerEnumInfoInto(ei.Name, ei.Variants, Ctx);
        if (auto *et = Ctx.lookupEnumType(ei.Name)) {
          Ctx.addEnumTypeAlias(qualifier + names::kQualSep + ei.Name, et);
          if (qualifier != fullModulePath)
            Ctx.addEnumTypeAlias(fullModulePath + names::kQualSep + ei.Name,
                                 et);
        }
      }
      for (auto &ci : cacheIt->second.ExportedClasses) {
        applyClassInfo(ci, Ctx);
        if (auto *ct = Ctx.lookupClassType(ci.Name)) {
          Ctx.addClassTypeAlias(qualifier + names::kQualSep + ci.Name, ct);
          if (qualifier != fullModulePath)
            Ctx.addClassTypeAlias(fullModulePath + names::kQualSep + ci.Name,
                                  ct);
        }
      }
      auto injectFn = [&](const ModuleInfo::FunctionInfo &fi,
                          const std::string &name) {
        if (lookupFunction(name))
          return;
        ast::Type *retTy = Ctx.lookupType(fi.ReturnTypeName);
        if (!retTy)
          retTy = Ctx.getVoidTy();
        std::vector<ast::Type *> params;
        for (auto &pn : fi.ParamTypeNames) {
          ast::Type *pty = Ctx.lookupType(pn);
          if (!pty)
            pty = Ctx.getObjTy();
          params.push_back(pty);
        }
        declareFunction(name, retTy, params, fi.IsVariadic, /*isBuiltin=*/true);
      };
      for (auto &fi : cacheIt->second.ExportedFunctions) {
        injectFn(fi, qualifier + names::kQualSep + fi.Name);
        if (qualifier != fullModulePath)
          injectFn(fi, fullModulePath + names::kQualSep + fi.Name);
      }
      return true;
    }

    // Parse the imported file.
    auto importDriverPtr = std::make_shared<parser::ParserDriver>();
    if (importDriverPtr->parseFile(path) != 0) {
      error(loc, "failed to parse module '" + path + "'");
      return false;
    }

    // Run Sema on the imported module.
    DiagEngine importDiag(Diags.getOS());
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

    // Serialise exported class types.
    ModuleInfo info;
    {
      for (auto &[name, ct] :
           importDriverPtr->getASTContext().getClassTypes()) {
        if (name == names::kObj || name == names::kString)
          continue;
        ModuleInfo::ClassInfo ci;
        ci.Name = name;
        if (ct->getSuperClass()) {
          ci.SuperClassName = ct->getSuperClass()->getName();
          if (ci.SuperClassName == names::kObj)
            ci.SuperClassName = "";
        }
        for (auto &[fname, fty] : ct->getFields())
          ci.Fields.push_back({fname, typeToName(fty)});
        for (auto *m : ct->getVTable()) {
          ModuleInfo::ClassInfo::MethodInfo mi;
          mi.Name = m->getName();
          mi.ReturnTypeName = typeToName(m->getReturnType());
          for (auto *pty : m->getParamTypes())
            mi.ParamTypeNames.push_back(typeToName(pty));
          mi.Flags = static_cast<uint8_t>(
              (m->isStatic() ? ast::MethodDecl::Static : 0) |
              (m->isPrivate() ? ast::MethodDecl::Private : 0));
          ci.Methods.push_back(std::move(mi));
        }
        info.ExportedClasses.push_back(std::move(ci));
      }

      // Serialise exported enum types (name + variants in declaration order).
      // The registry holds each enum under its canonical name and again under
      // any qualified import aliases; serialise only the canonical entry so the
      // type is reconstructed once and keeps a single identity.
      for (auto &[name, et] : importDriverPtr->getASTContext().getEnumTypes()) {
        if (name != et->getName())
          continue; // alias key — skip
        ModuleInfo::EnumInfo ei;
        ei.Name = name;
        for (auto *v : et->getVariants())
          ei.Variants.push_back(*v);
        info.ExportedEnums.push_back(std::move(ei));
      }

      // Reconstruct enums first: a class field or method may be enum-typed,
      // and registerClassInfoInto resolves those names against this context.
      for (auto &ei : info.ExportedEnums) {
        registerEnumInfoInto(ei.Name, ei.Variants, Ctx);
        if (auto *et = Ctx.lookupEnumType(ei.Name)) {
          Ctx.addEnumTypeAlias(qualifier + names::kQualSep + ei.Name, et);
          if (qualifier != fullModulePath)
            Ctx.addEnumTypeAlias(fullModulePath + names::kQualSep + ei.Name,
                                 et);
        }
      }
      for (auto &ci : info.ExportedClasses) {
        applyClassInfo(ci, Ctx);
        if (auto *ct = Ctx.lookupClassType(ci.Name)) {
          Ctx.addClassTypeAlias(qualifier + names::kQualSep + ci.Name, ct);
          if (qualifier != fullModulePath)
            Ctx.addClassTypeAlias(fullModulePath + names::kQualSep + ci.Name,
                                  ct);
        }
      }
    }

    // Serialise exported functions.
    for (auto &[name, sig] : importSema.FunctionTable) {
      if (sig.IsBuiltin)
        continue;
      ModuleInfo::FunctionInfo fi;
      fi.Name = name.str();
      fi.ReturnTypeName = typeToName(sig.ReturnType);
      for (auto *pty : sig.ParamTypes)
        fi.ParamTypeNames.push_back(typeToName(pty));
      fi.IsVariadic = sig.IsVariadic;
      info.ExportedFunctions.push_back(std::move(fi));
    }
    ModuleCache[path] = std::move(info);

    // Inject functions as qualifier::name and fullModulePath::name.
    auto injectFn2 = [&](const ModuleInfo::FunctionInfo &fi,
                         const std::string &name) {
      if (lookupFunction(name))
        return;
      ast::Type *retTy = Ctx.lookupType(fi.ReturnTypeName);
      if (!retTy)
        retTy = Ctx.getVoidTy();
      std::vector<ast::Type *> params;
      for (auto &pn : fi.ParamTypeNames) {
        ast::Type *pty = Ctx.lookupType(pn);
        if (!pty)
          pty = Ctx.getObjTy();
        params.push_back(pty);
      }
      declareFunction(name, retTy, params, fi.IsVariadic, /*isBuiltin=*/true);
    };
    for (auto &fi : ModuleCache[path].ExportedFunctions) {
      injectFn2(fi, qualifier + names::kQualSep + fi.Name);
      if (qualifier != fullModulePath)
        injectFn2(fi, fullModulePath + names::kQualSep + fi.Name);
    }

    childCtx.OwnedDriver = importDriverPtr;
    childCtx.Root = importRoot;
    AccumulatedImportContexts[path] =
        std::make_shared<SemaContext>(std::move(childCtx));
    return true;
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
