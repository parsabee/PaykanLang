// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Semantic analysis: import resolution, module exports and their
// reconstruction in the importer.  A module is identified by its canonical
// name (ModuleName.h); with a resolver (paykan/modules/ModuleResolver.h) its
// exports may come from a .pkm file instead of its source.

#include "ASTClone.h"
#include "ModuleName.h"
#include "ModuleUtils.h"
#include "Names.h"
#include "ParserDriver.h"
#include "Sema.h"
#include "paykan/modules/ModuleResolver.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <utility>

namespace paykan {
namespace sema {

// -- Static helpers (file-scope, not part of the Sema class)

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
///   suffix := '[]'                            -- slice of the type so far
///           | '[' INT ']'+                    -- fixed-size array of it
///           | '?'                             -- optional of the type so far
///
/// Suffixes apply left to right, mirroring how ast::typeName spells nesting:
/// "Str?[]" is a slice of optional strings, "int[2]?" would be an optional
/// array, and "(Node?, int)[3]" an array of tuples.  A name runs up to the
/// next ',' or ')' at this nesting level or the next suffix.  A generic
/// instantiation's name carries its type arguments (`Pair<Str, int>`,
/// `Box<Node?>`): everything between its '<' and the matching '>' is part of
/// the name, which is registered under exactly that spelling.  Returns nullptr
/// (leaving @p pos wherever it stopped) if the text is malformed or a base
/// name is unknown in @p ctx.
static ast::Type *parseExportedType(ast::ASTContext &ctx,
                                    const std::string &text, size_t &pos) {
  ast::Type *ty = nullptr;
  if (pos < text.size() && text[pos] == '(') {
    // `(T1, T2)`, a tuple type, or `(view T1, T2) -> R`, a function type
    // (`() -> R` takes no parameter), as typeName spells them; a function
    // type in parentheses takes a suffix, `((int) -> int)?`.
    ++pos;
    std::vector<ast::Type *> elems;
    std::vector<ast::ParamMode> modes;
    bool anyMode = false;
    if (pos < text.size() && text[pos] == ')') {
      ++pos;
    } else {
      for (;;) {
        ast::ParamMode mode = ast::ParamMode::Value;
        if (text.compare(pos, 5, "view ") == 0) {
          mode = ast::ParamMode::View;
          pos += 5;
        } else if (text.compare(pos, 6, "inout ") == 0) {
          mode = ast::ParamMode::Inout;
          pos += 6;
        }
        anyMode |= mode != ast::ParamMode::Value;
        ast::Type *elem = parseExportedType(ctx, text, pos);
        if (!elem)
          return nullptr;
        elems.push_back(elem);
        modes.push_back(mode);
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
    }
    if (text.compare(pos, 4, " -> ") == 0) {
      pos += 4;
      ast::Type *ret = parseExportedType(ctx, text, pos);
      if (!ret)
        return nullptr;
      return ctx.getFunctionType(std::move(elems), std::move(modes), ret);
    }
    if (anyMode)
      return nullptr;
    if (elems.size() == 1 && ast::isa<ast::FunctionType>(elems[0]))
      ty = elems[0];
    else if (elems.size() < 2)
      return nullptr;
    else
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
      ty = ctx.getSliceType(ty);
    } else if (pos + 1 < text.size() && text[pos] == '[' &&
               std::isdigit(static_cast<unsigned char>(text[pos + 1]))) {
      // A run of lengths, first index first, as typeName writes it: two
      // `int[3]`s are `int[2][3]`.
      std::vector<int64_t> lengths;
      while (pos + 1 < text.size() && text[pos] == '[' &&
             std::isdigit(static_cast<unsigned char>(text[pos + 1]))) {
        size_t close = text.find(']', pos);
        if (close == std::string::npos)
          return nullptr;
        int64_t n = 0;
        for (size_t i = pos + 1; i < close; ++i) {
          if (!std::isdigit(static_cast<unsigned char>(text[i])) ||
              n > (INT64_MAX - 9) / 10)
            return nullptr;
          n = n * 10 + (text[i] - '0');
        }
        if (n < 1)
          return nullptr;
        lengths.push_back(n);
        pos = close + 1;
      }
      for (auto it = lengths.rbegin(); it != lengths.rend(); ++it)
        ty = ctx.getFixedArrayType(ty, *it);
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

/// The order exportModuleInfo and fromInterface hand declarations out in:
/// the module's own types first, then by name, so reconstruction (and any
/// diagnostics it emits) is deterministic and the same for a module built in
/// process and one loaded from a file.
static void sortModuleInfo(Sema::ModuleInfo &info) {
  auto byLocalThenName = [](const auto &a, const auto &b) {
    if (a.IsLocal != b.IsLocal)
      return a.IsLocal;
    return a.Name < b.Name;
  };
  std::sort(info.ExportedClasses.begin(), info.ExportedClasses.end(),
            byLocalThenName);
  std::sort(info.ExportedStructs.begin(), info.ExportedStructs.end(),
            byLocalThenName);
  std::sort(info.ExportedEnums.begin(), info.ExportedEnums.end(),
            byLocalThenName);
  std::sort(info.ExportedFunctions.begin(), info.ExportedFunctions.end(),
            [](const auto &a, const auto &b) { return a.Name < b.Name; });
}

// -- Failed imports and diagnostics

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

// -- resolveModulePath

std::string Sema::resolveModulePath(const std::string &modulePath,
                                    bool isSystem, ast::SourceLocation loc) {
  std::filesystem::path file =
      module_utils::moduleFile(ProjectRoot, isSystem, modulePath);
  // A project module is shown relative to the source root, like every
  // imported file.
  return checkModuleFile(file,
                         isSystem
                             ? displayPath(file)
                             : module_utils::modulePathToRelative(modulePath),
                         module_name::canonicalImportName(modulePath, isSystem),
                         isSystem ? "system module" : "module", loc);
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

// -- Exports and their .pkm form

Sema::ModuleInfo Sema::exportModuleInfo(ast::TranslationUnit *tu,
                                        const std::string &moduleName) const {
  // The module's type registry holds (a) the classes/enums it declares,
  // (b) the ones it reached through its own imports, and (c) compiler
  // builtins.  (a) is exported
  // under the importer's qualifier.  (b) is exported WITHOUT a qualifier —
  // it must exist in the importing context so the module's signatures that
  // mention it resolve (with type identity preserved), but names are never
  // re-exported: to spell such a type the importer imports its defining
  // module.  (c) is never exported.
  ModuleInfo info;
  // Defining module of a type the module itself reached through an import
  // ("" when unknown).
  auto lookupOrigin = [&](const std::string &name) {
    auto it = ImportedTypeOrigins.find(name);
    return it == ImportedTypeOrigins.end() ? std::string() : it->second;
  };
  StringSet localClasses, localEnums, localStructs;
  for (auto *cd : tu->getClassDecls())
    localClasses.insert(cd->getName());
  for (auto *ed : tu->getEnumDecls())
    localEnums.insert(ed->getName());
  for (auto *sd : tu->getStructDecls())
    localStructs.insert(sd->getName());
  auto methodInfo = [](const ast::MethodDecl *m) {
    ModuleInfo::ClassInfo::MethodInfo mi;
    mi.Name = m->getName();
    mi.ReturnTypeName = ast::typeName(m->getReturnType());
    for (auto *pty : m->getParamTypes())
      mi.ParamTypeNames.push_back(ast::typeName(pty));
    mi.Flags =
        static_cast<uint8_t>((m->isPrivate() ? ast::MethodDecl::Private : 0) |
                             (m->isView() ? ast::MethodDecl::View : 0) |
                             (m->isImpl() ? ast::MethodDecl::Impl : 0));
    mi.Modes = m->getParamModes();
    return mi;
  };

  for (auto &[name, ct] : Ctx.getClassTypes()) {
    // The registry holds each class under its canonical name and again
    // under any qualified import aliases; serialise only the canonical
    // entry so a class reached through several import paths keeps one
    // identity.
    if (name != ct->getName())
      continue;
    if (isBootstrapClassName(name))
      continue;
    ModuleInfo::ClassInfo ci;
    ci.Name = name;
    ci.IsLocal = localClasses.count(name) != 0;
    ci.OriginModule = ci.IsLocal ? moduleName : lookupOrigin(name);
    if (ct->getSuperClass()) {
      ci.SuperClassName = ct->getSuperClass()->getName();
      if (ci.SuperClassName == names::kObj)
        ci.SuperClassName = "";
    }
    for (auto &[fname, fty] : ct->getFields())
      ci.Fields.push_back({fname, ast::typeName(fty), ct->isWeakField(fname)});
    // __init__ and the `impl` methods have no vtable slot but are part of
    // the class's interface: a subclass in the importing module must call
    // __init__ through `__super__` with the right arguments, and importers
    // call the `impl` methods directly (addMethod files both back).
    std::vector<ast::MethodDecl *> methods = ct->getVTable();
    if (auto *init = ct->findMethod(names::kMethodInit))
      methods.push_back(init);
    methods.insert(methods.end(), ct->getImplMethods().begin(),
                   ct->getImplMethods().end());
    for (auto *m : methods)
      ci.Methods.push_back(methodInfo(m));
    info.ExportedClasses.push_back(std::move(ci));
  }

  // Struct types: fields in declaration order, then `__init__` and the
  // `impl` methods, which importers call directly (canonical entries only,
  // as for classes).
  for (auto &[name, st] : Ctx.getStructTypes()) {
    if (name != st->getName())
      continue; // alias key — skip
    ModuleInfo::StructInfo si;
    si.Name = name;
    si.IsLocal = localStructs.count(name) != 0;
    si.OriginModule = si.IsLocal ? moduleName : lookupOrigin(name);
    for (size_t i = 0; i < st->getNumFields(); ++i)
      si.Fields.push_back(
          {st->getFieldName(i), ast::typeName(st->getElementType(i))});
    if (auto *init = st->findMethod(names::kMethodInit))
      si.Methods.push_back(methodInfo(init));
    for (auto *m : st->getImplMethods())
      si.Methods.push_back(methodInfo(m));
    info.ExportedStructs.push_back(std::move(si));
  }

  // Enum types: name + variants in declaration order (canonical entries
  // only, as for classes).
  for (auto &[name, et] : Ctx.getEnumTypes()) {
    if (name != et->getName())
      continue; // alias key — skip
    ModuleInfo::EnumInfo ei;
    ei.Name = name;
    ei.IsLocal = localEnums.count(name) != 0;
    ei.OriginModule = ei.IsLocal ? moduleName : lookupOrigin(name);
    for (auto *v : et->getVariants())
      ei.Variants.push_back(*v);
    info.ExportedEnums.push_back(std::move(ei));
  }

  // Free functions and constructors declared by the module.  Functions the
  // module itself imported were injected as builtins and are not
  // re-exported.
  for (auto &[name, sig] : FunctionTable) {
    // Instantiations of other modules' templates (and their constructors)
    // are those modules'.
    if (sig.IsBuiltin || name.starts_with(names::kHiddenQualPrefix) ||
        ImportedTypeOrigins.count(name))
      continue;
    ModuleInfo::FunctionInfo fi;
    fi.Name = name;
    fi.ReturnTypeName = ast::typeName(sig.ReturnType);
    for (auto *pty : sig.ParamTypes)
      fi.ParamTypeNames.push_back(ast::typeName(pty));
    fi.Modes = sig.Modes;
    info.ExportedFunctions.push_back(std::move(fi));
  }
  sortModuleInfo(info);
  return info;
}

// The IFACE records' parameter modes (pkm::kMode*, a byte per parameter,
// with the names) are ast::ParamMode's values.
static_assert(static_cast<uint8_t>(ast::ParamMode::Value) == pkm::kModeValue &&
                  static_cast<uint8_t>(ast::ParamMode::View) ==
                      pkm::kModeView &&
                  static_cast<uint8_t>(ast::ParamMode::Inout) ==
                      pkm::kModeInout,
              "the IFACE parameter mode bytes are ast::ParamMode's values");

static pkm::ModeRecs toModeRecs(const ast::ParamModes &modes) {
  pkm::ModeRecs out;
  for (ast::ParamMode m : modes.Modes)
    out.Modes.push_back(static_cast<uint8_t>(m));
  out.Names = modes.Names;
  out.Result = static_cast<uint8_t>(modes.Result);
  return out;
}

static ast::ParamModes fromModeRecs(const pkm::ModeRecs &recs) {
  ast::ParamModes out;
  for (uint8_t m : recs.Modes)
    out.Modes.push_back(static_cast<ast::ParamMode>(m));
  out.Names = recs.Names;
  out.Result = static_cast<ast::ParamMode>(recs.Result);
  return out;
}

pkm::Interface Sema::toInterface(const ModuleInfo &info) {
  pkm::Interface iface;
  for (auto &fi : info.ExportedFunctions)
    iface.Functions.push_back(
        {fi.Name, fi.ReturnTypeName, fi.ParamTypeNames, toModeRecs(fi.Modes)});
  for (auto &ci : info.ExportedClasses) {
    pkm::ClassRec cr;
    cr.Name = ci.Name;
    cr.SuperClassName = ci.SuperClassName;
    cr.IsLocal = ci.IsLocal;
    cr.OriginModule = ci.OriginModule;
    for (auto &f : ci.Fields)
      cr.Fields.push_back({f.FieldName, f.TypeName, f.Weak});
    for (auto &m : ci.Methods)
      cr.Methods.push_back({m.Name, m.ReturnTypeName, m.ParamTypeNames, m.Flags,
                            toModeRecs(m.Modes)});
    iface.Classes.push_back(std::move(cr));
  }
  for (auto &si : info.ExportedStructs) {
    pkm::StructRec sr;
    sr.Name = si.Name;
    sr.IsLocal = si.IsLocal;
    sr.OriginModule = si.OriginModule;
    for (auto &f : si.Fields)
      sr.Fields.push_back({f.FieldName, f.TypeName});
    for (auto &m : si.Methods)
      sr.Methods.push_back({m.Name, m.ReturnTypeName, m.ParamTypeNames, m.Flags,
                            toModeRecs(m.Modes)});
    iface.Structs.push_back(std::move(sr));
  }
  for (auto &ei : info.ExportedEnums)
    iface.Enums.push_back({ei.Name, ei.Variants, ei.IsLocal, ei.OriginModule});
  return iface;
}

Sema::ModuleInfo Sema::fromInterface(const pkm::Interface &iface) {
  ModuleInfo info;
  for (auto &fr : iface.Functions)
    info.ExportedFunctions.push_back({fr.Name, fr.ReturnTypeName,
                                      fr.ParamTypeNames,
                                      fromModeRecs(fr.Modes)});
  for (auto &cr : iface.Classes) {
    ModuleInfo::ClassInfo ci;
    ci.Name = cr.Name;
    ci.SuperClassName = cr.SuperClassName;
    ci.IsLocal = cr.IsLocal;
    ci.OriginModule = cr.OriginModule;
    for (auto &f : cr.Fields)
      ci.Fields.push_back({f.FieldName, f.TypeName, f.Weak});
    for (auto &m : cr.Methods)
      ci.Methods.push_back({m.Name, m.ReturnTypeName, m.ParamTypeNames, m.Flags,
                            fromModeRecs(m.Modes)});
    info.ExportedClasses.push_back(std::move(ci));
  }
  for (auto &sr : iface.Structs) {
    ModuleInfo::StructInfo si;
    si.Name = sr.Name;
    si.IsLocal = sr.IsLocal;
    si.OriginModule = sr.OriginModule;
    for (auto &f : sr.Fields)
      si.Fields.push_back({f.FieldName, f.TypeName});
    for (auto &m : sr.Methods)
      si.Methods.push_back({m.Name, m.ReturnTypeName, m.ParamTypeNames, m.Flags,
                            fromModeRecs(m.Modes)});
    info.ExportedStructs.push_back(std::move(si));
  }
  for (auto &er : iface.Enums)
    info.ExportedEnums.push_back(
        {er.Name, er.Variants, er.IsLocal, er.OriginModule});
  sortModuleInfo(info);
  return info;
}

// -- Locating and loading a module

bool Sema::locateModule(const std::string &canonical, bool isSystem,
                        const std::string &modulePath, ast::SourceLocation loc,
                        LocatedModule &out) {
  const char *kind = isSystem ? "system module" : "module";
  if (!Resolver) {
    out.Path = resolveModulePath(modulePath, isSystem, loc);
    return !out.Path.empty();
  }
  for (;;) {
    modules::Resolution r = Resolver->resolve(canonical);
    using K = modules::Resolution::Kind;
    switch (r.Outcome) {
    case K::NeedDependency: {
      // A cache entry of this module is checked against the current
      // interface of each dependency: load (or build) the dependency first,
      // then ask again.  A dependency that fails is reported here; the
      // entry is then unusable and the module's own import of it adds the
      // note.
      const std::string &dep = r.Dependency;
      if (FailedModules && FailedModules->count(dep)) {
        Resolver->failed(dep);
        continue;
      }
      bool depSystem = dep.starts_with(names::kQualSep);
      LocatedModule depLocated;
      if (locateModule(dep, depSystem, depSystem ? dep.substr(2) : dep, loc,
                       depLocated))
        loadModule(dep, depLocated, loc);
      continue;
    }
    case K::Source:
      // The resolver only checks that something is there; a directory or
      // a special file is diagnosed as before.
      out.Path = checkModuleFile(
          r.Path,
          isSystem ? displayPath(r.Path)
                   : module_utils::modulePathToRelative(modulePath),
          canonical, kind, loc);
      if (out.Path.empty())
        Resolver->failed(canonical);
      return !out.Path.empty();
    case K::Loaded:
      out.Path = module_utils::realPath(r.Path);
      out.Iface = r.Iface;
      return true;
    case K::NotFound:
    case K::Rejected:
      error(loc, r.Outcome == K::NotFound
                     ? std::string(kind) + " '" + canonical + "' " + r.Message
                     : r.Message);
      // Reported once, here: a later import of the module adds a note.
      if (FailedModules)
        FailedModules->insert(canonical);
      return false;
    }
  }
}

const Sema::ModuleInfo *Sema::loadModule(const std::string &canonical,
                                         const LocatedModule &located,
                                         ast::SourceLocation loc) {
  // Already built or loaded in this process.  The cache outlives a
  // compilation, so it is keyed by the file (a source or a .pkm), never by
  // the name alone: another compilation's module of the same name is
  // another module.
  const std::string &path = located.Path;
  if (auto it = ModuleCache.find(path); it != ModuleCache.end())
    return &it->second;
  if (located.Iface)
    return &(ModuleCache[path] = fromInterface(*located.Iface));

  // Cycle detection.
  if (ImportStack && ImportStack->count(canonical)) {
    error(loc, "circular import of module '" + canonical + "'");
    return nullptr;
  }

  // Parse the imported file, with the parser wired to a DiagEngine that
  // carries the imported file's name and source lines — so a syntax error
  // inside a module prints the same rich source-located format
  // (file:line:col + snippet + caret) as main-file parse errors, instead of
  // a bare one-line fallback.  Mirrors the wiring in src/Driver/main.cpp:
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
  // The module is recorded as failed (see FailedModules): a later import
  // of it, on any path, adds no second report.
  auto failedModule = [&](unsigned moduleErrors,
                          bool importsFailedModule) -> const ModuleInfo * {
    if (FailedModules)
      FailedModules->insert(canonical);
    if (Resolver)
      Resolver->failed(canonical);
    if (moduleErrors == 0) {
      if (importsFailedModule) {
        // It failed only by importing a module whose failure was reported
        // already: that report covers it too.
        ++RepeatedImportFailures;
        Diags.note(loc, "in module '" + canonical + "' imported here");
        return nullptr;
      }
      // Nothing was reported (should not happen): keep the failure visible.
      error(loc, "errors in imported module '" + canonical + "'");
      return nullptr;
    }
    ImportedModuleErrors += moduleErrors;
    Diags.note(loc, "in module '" + canonical + "' imported here");
    return nullptr;
  };
  if (importDriverPtr->parseFile(path) != 0) {
    importDriverPtr->setDiagEngine(nullptr);
    return failedModule(importDiag.getErrorCount(), false);
  }
  // importDiag dies with this call frame but the driver (kept alive in the
  // returned SemaContext) does not — detach it now that parsing is done.
  importDriverPtr->setDiagEngine(nullptr);

  // Run Sema on the imported module, reusing the same engine.
  Sema importSema(importDriverPtr->getASTContext(), importDiag, ProjectRoot,
                  FrontendName);
  if (ImportStack)
    ImportStack->insert(canonical);
  importSema.ImportStack = ImportStack;
  importSema.FailedModules = FailedModules;
  importSema.Resolver = Resolver;

  auto *importRoot = importDriverPtr->getRoot();
  auto childCtx = importSema.run(importRoot);
  bool importOk = (bool)childCtx;
  if (ImportStack)
    ImportStack->erase(canonical);
  if (!importOk)
    return failedModule(childCtx.ErrorCount, childCtx.ImportsFailedModule);

  ModuleInfo &info = ModuleCache[path] =
      importSema.exportModuleInfo(importRoot, canonical);
  if (auto templates = importSema.exportTemplates(importRoot, canonical)) {
    templates->Uses.front().second = path;
    templates->File = displayPath(path);
    templates->Lines = &importDriverPtr->getSourceLines();
    templates->Owner = importDriverPtr;
    info.Templates = std::move(templates);
  }
  if (Resolver) {
    std::vector<std::string> deps;
    for (auto *imp : importRoot->getImports())
      for (auto &m : imp->getModules())
        deps.push_back(module_name::canonicalImportName(imp->modulePath(m),
                                                        imp->isSystem()));
    Resolver->built(canonical, toInterface(info), std::move(deps));
    // A .pkm holds neither templates nor instantiations of another
    // module's ones yet (#160): such a module is built from source each
    // time, so its importers instantiate from its bodies.
    if (info.Templates || !childCtx.ForeignClasses.empty() ||
        !childCtx.ForeignFunctions.empty() || !childCtx.ForeignStructs.empty())
      Resolver->keepUncached(canonical);
  }

  childCtx.OwnedDriver = importDriverPtr;
  childCtx.Root = importRoot;
  AccumulatedImportContexts[canonical] =
      std::make_shared<SemaContext>(std::move(childCtx));
  return &info;
}

// -- Reconstruction in the importer

bool Sema::injectModule(const ModuleInfo &info, const std::string &qualifier,
                        const std::string &fullModulePath,
                        const std::string &moduleName,
                        ast::SourceLocation loc) {
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
                         const std::string &originModule) -> bool {
    if (originModule.empty())
      return true; // compiler builtin — identical in every context
    auto it = ImportedTypeOrigins.find(name);
    if (it == ImportedTypeOrigins.end()) {
      ImportedTypeOrigins[name] = originModule;
      return true;
    }
    if (it->second == originModule)
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
  auto addStructAliases = [&](ast::StructType *st, const std::string &name) {
    Ctx.addStructTypeAlias(qualifier + names::kQualSep + name, st);
    if (qualifier != fullModulePath)
      Ctx.addStructTypeAlias(fullModulePath + names::kQualSep + name, st);
  };
  /// A method of an imported class or struct, as a MethodDecl; nullptr
  /// after reporting a type that does not resolve.
  auto importedMethod = [&](const ModuleInfo::ClassInfo::MethodInfo &m,
                            const std::string &owner) -> ast::MethodDecl * {
    const std::string decl = "method '" + owner + "." + m.Name + "'";
    auto *retTy = resolveOrReport(m.ReturnTypeName, "return type of " + decl);
    bool sigOk = retTy != nullptr;
    std::vector<ast::Type *> params;
    for (size_t i = 0; i < m.ParamTypeNames.size(); ++i) {
      auto *pty =
          resolveOrReport(m.ParamTypeNames[i],
                          "parameter " + std::to_string(i + 1) + " of " + decl);
      if (pty)
        params.push_back(pty);
      else
        sigOk = false;
    }
    if (!sigOk)
      return nullptr;
    auto *md =
        Ctx.make<ast::MethodDecl>(ast::SourceLocation(), Ctx.intern(m.Name),
                                  retTy, std::move(params), m.Flags);
    md->setParamModes(m.Modes);
    return md;
  };

  // Reconstruct every exported enum and class.  Returns false (with
  // diagnostics emitted) if anything failed to resolve.
  auto registerExportedTypes = [&]() -> bool {
    bool ok = true;

    // Enums first: class fields and method signatures may be enum-typed.
    for (auto &ei : info.ExportedEnums) {
      if (!checkOrigin("enum", ei.Name, ei.OriginModule)) {
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

    // Struct stubs next: a class field or a method signature may name a
    // struct, and a struct field a class, so every type exists before any
    // field is resolved.  Their fields and methods come after the classes.
    std::vector<const ModuleInfo::StructInfo *> createdStructs;
    for (auto &si : info.ExportedStructs) {
      if (!checkOrigin("struct", si.Name, si.OriginModule)) {
        ok = false;
        continue;
      }
      auto *st = Ctx.lookupStructType(si.Name);
      if (!st) {
        if (Ctx.lookupClassType(si.Name) || Ctx.lookupEnumType(si.Name)) {
          error(loc, diagPrefix + "struct '" + si.Name + "' conflicts with a " +
                         (Ctx.lookupClassType(si.Name) ? "class" : "an enum") +
                         " of the same name already in scope");
          ok = false;
          continue;
        }
        st = Ctx.registerStructType(si.Name, loc);
        createdStructs.push_back(&si);
      } // else: already present — pointer identity preserved
      if (si.IsLocal)
        addStructAliases(st, si.Name);
    }

    // Classes, phase 1: pre-register a stub for every class not already
    // present, superclass before subclass.  All stubs exist before any
    // signature is resolved, so a method or field may name any class of the
    // module regardless of the (unordered) serialisation order — this is
    // the same two-phase scheme checkClassDecls uses for a single file.
    std::vector<const ModuleInfo::ClassInfo *> pending, created;
    for (auto &ci : info.ExportedClasses) {
      if (!checkOrigin("class", ci.Name, ci.OriginModule)) {
        ok = false;
        continue;
      }
      if (auto *ct = Ctx.lookupClassType(ci.Name)) {
        // Already present — pointer identity preserved.
        if (ci.IsLocal)
          addClassAliases(ct, ci.Name);
        continue;
      }
      if (Ctx.lookupEnumType(ci.Name) || Ctx.lookupStructType(ci.Name)) {
        error(loc, diagPrefix + "class '" + ci.Name + "' conflicts with " +
                       (Ctx.lookupEnumType(ci.Name) ? "an enum" : "a struct") +
                       " of the same name already in scope");
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
        builder.field(f.FieldName, fty, f.Weak);
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
        builder.method(m.Name, retTy, std::move(params), m.Flags, m.Modes);
      }
      builder.build();
      if (ci->IsLocal)
        addClassAliases(ct, ci->Name);
    }

    // Structs, phase 2: their fields (in the exporter's order, which holds
    // every struct before the structs that hold it) and methods.
    for (auto *si : createdStructs) {
      auto *st = Ctx.lookupStructType(si->Name);
      for (auto &f : si->Fields) {
        auto *fty =
            resolveOrReport(f.TypeName, "field '" + f.FieldName +
                                            "' of struct '" + si->Name + "'");
        if (!fty) {
          ok = false;
          continue;
        }
        st->addField(f.FieldName, fty);
      }
      for (auto &m : si->Methods) {
        if (auto *md = importedMethod(m, si->Name))
          st->addMethod(md);
        else
          ok = false;
      }
    }
    return ok;
  };

  auto injectFnAs = [&](const ModuleInfo::FunctionInfo &fi,
                        const std::string &name) -> bool {
    if (lookupFunction(name))
      return true; // same module already imported under this qualifier
    const std::string decl = "function '" + fi.Name + "'";
    auto *retTy = resolveOrReport(fi.ReturnTypeName, "return type of " + decl);
    bool sigOk = retTy != nullptr;
    std::vector<ast::Type *> params;
    for (size_t i = 0; i < fi.ParamTypeNames.size(); ++i) {
      auto *pty =
          resolveOrReport(fi.ParamTypeNames[i],
                          "parameter " + std::to_string(i + 1) + " of " + decl);
      if (pty)
        params.push_back(pty);
      else
        sigOk = false;
    }
    if (!sigOk)
      return false;
    declareFunction(name, retTy, std::move(params), /*isBuiltin=*/true);
    FunctionTable[name].Modes = fi.Modes;
    return true;
  };

  // Types first (function signatures name them), then functions as
  // qualifier::name and fullModulePath::name, then templates.
  bool ok = registerExportedTypes();
  for (auto &fi : info.ExportedFunctions) {
    if (!injectFnAs(fi, qualifier + names::kQualSep + fi.Name))
      ok = false;
    if (qualifier != fullModulePath &&
        !injectFnAs(fi, fullModulePath + names::kQualSep + fi.Name))
      ok = false;
  }
  if (ok && info.Templates &&
      !injectTemplates(info, qualifier, fullModulePath, loc))
    ok = false;
  return ok;
}

// -- Templates of other modules (docs/design/pkm.md §4)

std::string Sema::hiddenQualifier(const std::string &canonical) {
  return names::kHiddenQualPrefix + canonical;
}

std::string Sema::displayName(const std::string &name) {
  const std::string_view prefix(names::kHiddenQualPrefix);
  return name.starts_with(prefix) ? name.substr(prefix.size()) : name;
}

bool Sema::parseInstantiationName(const std::string &name, std::string &base,
                                  std::vector<ast::Type *> &args) {
  const size_t open = name.find('<');
  if (open == std::string::npos || name.back() != '>')
    return false;
  base = name.substr(0, open);
  // The arguments as instantiationName spelled them, ", "-separated.
  args.clear();
  auto arg = [&](size_t from, size_t to) {
    auto *ty = resolveExportedType(Ctx, name.substr(from, to - from));
    args.push_back(ty);
    return ty != nullptr;
  };
  int depth = 0;
  size_t start = open + 1;
  for (size_t i = start; i + 1 < name.size(); ++i) {
    const char c = name[i];
    if (c == '<' || c == '(')
      ++depth;
    else if (c == '>' || c == ')')
      --depth;
    else if (c == ',' && depth == 0) {
      if (!arg(start, i))
        return false;
      start = i + 2;
    }
  }
  return arg(start, name.size() - 1);
}

const Sema::InstantiationInfo *Sema::instantiationInfo(ast::ClassType *ct) {
  if (auto it = ClassInstantiationInfo.find(ct);
      it != ClassInstantiationInfo.end())
    return &it->second;
  // `Tmpl<args>` from an import: an instantiation of a template of the
  // module the class comes from, when that module's templates are here.
  const std::string &name = ct->getName();
  auto origin = ImportedTypeOrigins.find(name);
  std::string base;
  std::vector<ast::Type *> args;
  if (origin == ImportedTypeOrigins.end() ||
      !parseInstantiationName(name, base, args))
    return nullptr;
  auto tmpl = ClassTemplates.find(hiddenQualifier(origin->second) +
                                  names::kQualSep + base);
  if (tmpl == ClassTemplates.end())
    return nullptr;
  return &(ClassInstantiationInfo[ct] =
               InstantiationInfo{tmpl->second, std::move(args)});
}

const Sema::StructInstantiationInfo *
Sema::structInstantiationInfo(ast::StructType *st) {
  if (auto it = StructInstantiationInfos.find(st);
      it != StructInstantiationInfos.end())
    return &it->second;
  const std::string &name = st->getName();
  auto origin = ImportedTypeOrigins.find(name);
  std::string base;
  std::vector<ast::Type *> args;
  if (origin == ImportedTypeOrigins.end() ||
      !parseInstantiationName(name, base, args))
    return nullptr;
  auto tmpl = StructTemplates.find(hiddenQualifier(origin->second) +
                                   names::kQualSep + base);
  if (tmpl == StructTemplates.end())
    return nullptr;
  return &(StructInstantiationInfos[st] =
               StructInstantiationInfo{tmpl->second, std::move(args)});
}

std::shared_ptr<Sema::ModuleTemplates>
Sema::exportTemplates(ast::TranslationUnit *tu, const std::string &canonical) {
  if (tu->getGenericClassDecls().empty() && tu->getGenericFuncDecls().empty() &&
      tu->getGenericStructDecls().empty())
    return nullptr;
  // What the module declares, by the names its bodies use for them.
  StringSet types, generics, callees;
  for (auto *cd : tu->getClassDecls()) {
    types.insert(cd->getName());
    callees.insert(cd->getName()); // its constructor
  }
  for (auto *sd : tu->getStructDecls()) {
    types.insert(sd->getName());
    callees.insert(sd->getName()); // its constructor
  }
  for (auto *ed : tu->getEnumDecls())
    types.insert(ed->getName());
  for (auto *cd : tu->getGenericClassDecls()) {
    generics.insert(cd->getName());
    callees.insert(cd->getName());
  }
  for (auto *sd : tu->getGenericStructDecls()) {
    generics.insert(sd->getName());
    callees.insert(sd->getName());
  }
  for (auto *fn : tu->getFuncDecls())
    callees.insert(fn->getName());
  for (auto *fn : tu->getGenericFuncDecls())
    callees.insert(fn->getName());

  auto mt = std::make_shared<ModuleTemplates>();
  mt->Module = canonical;
  mt->Uses.emplace_back(canonical, "");
  // A body names the module's own declarations bare and an import's as
  // `q::name`: both become `$<module>::name` (docs/design/pkm.md §4.2).
  class Names : public ast::ASTCloner::Renamer {
    Sema &S;
    ModuleTemplates &MT;
    const StringSet &Types, &Generics, &Callees;
    mutable StringSet Used;

    std::string imported(const std::string &name) const {
      size_t sep = name.rfind(names::kQualSep);
      if (sep == std::string::npos)
        return "";
      auto it = S.ImportQualifiers.find(name.substr(0, sep));
      if (it == S.ImportQualifiers.end())
        return "";
      const std::string &module = it->second.Module;
      if (Used.insert(module).second) {
        auto path = S.ModulePaths.find(module);
        MT.Uses.emplace_back(
            module, path == S.ModulePaths.end() ? std::string() : path->second);
      }
      return hiddenQualifier(module) + names::kQualSep +
             name.substr(sep + std::string_view(names::kQualSep).size());
    }
    std::string spell(const std::string &name, const StringSet &own) const {
      if (std::string q = imported(name); !q.empty())
        return q;
      return own.count(name)
                 ? hiddenQualifier(MT.Module) + names::kQualSep + name
                 : name;
    }

  public:
    Names(Sema &s, ModuleTemplates &mt, const StringSet &types,
          const StringSet &generics, const StringSet &callees)
        : S(s), MT(mt), Types(types), Generics(generics), Callees(callees),
          Used{mt.Module} {}
    std::string type(const std::string &name) const override {
      return spell(name, Types);
    }
    std::string generic(const std::string &name) const override {
      return spell(name, Generics);
    }
    std::string callee(const std::string &name) const override {
      return spell(name, Callees);
    }
  };
  Names names(*this, *mt, types, generics, callees);
  ast::ASTCloner cloner(Ctx, {}, &names);
  for (auto *cd : tu->getGenericClassDecls()) {
    if (!ClassTemplates.count(cd->getName())) // rejected
      continue;
    mt->Classes.push_back(cloner.cloneTemplate(cd));
    if (auto it = TemplateImpls.find(cd); it != TemplateImpls.end())
      for (auto *im : it->second)
        if (!im->getTrait())
          mt->Impls[mt->Classes.back()].push_back(cloner.cloneTemplate(im));
  }
  for (auto *fn : tu->getGenericFuncDecls())
    if (FuncTemplates.count(fn->getName()))
      mt->Functions.push_back(cloner.cloneTemplate(fn));
  for (auto *sd : tu->getGenericStructDecls()) {
    if (!StructTemplates.count(sd->getName())) // rejected
      continue;
    mt->Structs.push_back(cloner.cloneTemplate(sd));
    if (auto it = TemplateImpls.find(sd); it != TemplateImpls.end())
      for (auto *im : it->second)
        if (!im->getTrait())
          mt->Impls[mt->Structs.back()].push_back(cloner.cloneTemplate(im));
  }
  return mt;
}

bool Sema::injectHidden(const ModuleInfo &info, const std::string &canonical,
                        ast::SourceLocation loc) {
  if (!HiddenModules.insert(canonical).second)
    return true;
  const std::string q = hiddenQualifier(canonical);
  return injectModule(info, q, q, canonical, loc);
}

bool Sema::injectTemplates(const ModuleInfo &info, const std::string &qualifier,
                           const std::string &fullModulePath,
                           ast::SourceLocation loc) {
  const ModuleTemplates &mt = *info.Templates;
  bool ok = true;
  // The bodies name what the module and its imports declare by their
  // hidden qualifiers.
  for (const auto &[canonical, path] : mt.Uses) {
    const ModuleInfo *used = &info;
    if (canonical != mt.Module) {
      auto it = ModuleCache.find(path);
      if (it == ModuleCache.end()) {
        error(loc, "internal compiler error: module '" + canonical +
                       "', which the templates of module '" + mt.Module +
                       "' use, was not loaded");
        ok = false;
        continue;
      }
      used = &it->second;
    }
    if (!injectHidden(*used, canonical, loc))
      ok = false;
  }
  for (const std::string &q :
       {qualifier, fullModulePath, hiddenQualifier(mt.Module)}) {
    for (auto *cd : mt.Classes)
      ClassTemplates[q + names::kQualSep + cd->getName()] = cd;
    for (auto *fn : mt.Functions)
      FuncTemplates[q + names::kQualSep + fn->getName()] = fn;
    for (auto *sd : mt.Structs)
      StructTemplates[q + names::kQualSep + sd->getName()] = sd;
  }
  for (auto *cd : mt.Classes)
    ForeignTemplates[cd] = &mt;
  for (auto *sd : mt.Structs)
    ForeignTemplates[sd] = &mt;
  for (const auto &[cd, impls] : mt.Impls)
    TemplateImpls[cd] = impls;
  for (auto *fn : mt.Functions)
    ForeignTemplates[fn] = &mt;
  return ok;
}

// -- processImport

bool Sema::processImport(ast::ImportDecl *node) {
  const bool isSystem = node->isSystem();
  bool ok = true;
  for (auto &m : node->getModules()) {
    std::string fullPath = node->modulePath(m);
    const std::string &qualifier = m.qualifier();
    const std::string canonical =
        module_name::canonicalImportName(fullPath, isSystem);
    // The names a failed import binds are poisoned: their uses were reported
    // with the import (#119).
    // (A name that already names another module keeps it.)
    auto poison = [&] {
      for (const std::string *name : {&qualifier, &std::as_const(fullPath)}) {
        auto it = ImportQualifiers.find(*name);
        if (it == ImportQualifiers.end() || it->second.Module == canonical)
          FailedImportQualifiers.insert(*name);
      }
      ok = false;
    };
    // A module that already failed in this compilation was reported where
    // it failed; this import adds a note, not the same errors again (#132).
    if (FailedModules && FailedModules->count(canonical)) {
      Diags.note(node->getLocation(),
                 "module '" + canonical +
                     "' failed to load; its errors are reported above");
      ++RepeatedImportFailures;
      poison();
      continue;
    }
    LocatedModule located;
    if (!locateModule(canonical, isSystem, fullPath, node->getLocation(),
                      located)) {
      if (FailedModules)
        FailedModules->insert(canonical);
      poison();
      continue;
    }
    ModulePaths[canonical] = located.Path;
    // The names this import binds must not already name another module.
    bool clash = false;
    for (const std::string *name :
         {&qualifier, static_cast<const std::string *>(&fullPath)}) {
      auto [it, fresh] = ImportQualifiers.try_emplace(
          *name, ImportQualifier{canonical, fullPath});
      if (fresh || it->second.Module == canonical)
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
    const ModuleInfo *info =
        loadModule(canonical, located, node->getLocation());
    if (!info || !injectModule(*info, qualifier, fullPath, canonical,
                               node->getLocation()))
      poison();
  }
  return ok;
}

} // namespace sema
} // namespace paykan
