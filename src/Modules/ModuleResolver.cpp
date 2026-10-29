// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// The module resolver (paykan/modules/ModuleResolver.h).  The freshness rule
// is docs/design/pkm.md §8.2: a cache entry is used only when the toolchain
// numbers, the source bytes and the interface hash of every direct
// dependency are what it was written against; a prebuilt file skips the
// source check and is an error when unusable.  Dependencies are checked
// through the memo, so an importer's entry is judged after its dependencies
// were resolved (and rebuilt when stale): a body edit rebuilds one module, a
// signature edit its importers too.

#include "paykan/modules/ModuleResolver.h"

#include "BuildId.h"
#include "ModuleName.h"
#include "ModuleUtils.h"
#include "Names.h"
#include "Runtime.h"
#include "Version.h"
#include "paykan/modules/ModuleCode.h"
#include "paykan/pir/Binary.h"
#include "paykan/pir/Verifier.h"
#include "paykan/pir/Version.h"
#include "paykan/pkm/Templates.h"

// The runtime ABI version is spelled out in Runtime.h, for C; it is the
// release's, like every format's.
static_assert(paykan::kRuntimeABIVersion == paykan::kVersionNumber,
              "PAYKAN_RUNTIME_ABI_VERSION in src/Runtime/Runtime.h must be "
              "the release version, major * 10000 + minor * 100 + patch");
#include "paykan/plugin_api.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <ostream>
#include <utility>

namespace paykan::modules {

using namespace std::string_literals;

/// What the resolver knows about one module, by canonical name.
struct ModuleResolver::Entry {
  enum class State { Source, Built, Loaded, Failed };
  State State = State::Failed;
  std::filesystem::path Path; ///< the source (Source, Built) or .pkm (Loaded)
  /// Loaded from a cache entry: the source it was checked against.
  std::filesystem::path Source;
  std::vector<uint8_t> SourceBytes; ///< Source, Built: for the manifest
  pkm::Interface Iface;             ///< Built, Loaded
  /// Loaded: its CODE, decoded and verified, until loadCode() takes it.
  std::optional<pir::Module> Code;
  std::vector<std::string> Deps; ///< Built, Loaded: direct imports
  support::Hash256 IfaceHash{};  ///< Built, Loaded
  /// Built, Loaded: the TMPL section's hash (zero without one).  The
  /// interface hash covers the dependencies' (IFACE's MODS), so an edit of
  /// a template body reaches every module that may hold a copy of an
  /// instantiation of it.
  support::Hash256 TmplHash{};
  std::vector<uint8_t> Tmpl;     ///< Built: the TMPL section
  std::optional<pkm::File> File; ///< Loaded
  /// Loaded: its compiled code, when CODE declares it.
  std::optional<pkm::Object> Object;
  /// Loaded: what ReadFrontend made of the file, until
  /// takeFrontendData() takes it.
  std::shared_ptr<void> Frontend;
  bool KeepUncached = false; ///< Built: not cacheable()
  /// Built: the cache entry writeCacheEntry() wrote ("" before, or when it
  /// could not be written).
  std::filesystem::path Written;
};

/// A .pkm read for a module whose dependencies are still being resolved,
/// kept so the second resolve() does not read it again.
struct ModuleResolver::Pending {
  pkm::File File;
  std::vector<uint8_t> SourceBytes;
};

namespace {

bool isSystemName(const std::string &canonical) {
  return canonical.starts_with(module_name::kSep);
}

/// The module path an import spells for @p canonical (`::io` -> `io`).
std::string modulePathOf(const std::string &canonical) {
  return isSystemName(canonical) ? canonical.substr(module_name::kSep.size())
                                 : canonical;
}

bool definesMain(const pir::Module &m) {
  return std::any_of(
      m.Functions.begin(), m.Functions.end(),
      [&](const pir::Function &f) { return m.defines(f) && f.Name == "main"; });
}

/// "<verb> <what>" with the manifest's toolchain numbers, the shape of the
/// §8.2 error.
std::string describeProducer(const pkm::File &f) {
  const pkm::Manifest &m = f.manifest();
  return "was compiled by paykan " + m.Core.Version + " (pkm " +
         versionString(f.formatVersion()) + ", interface " +
         versionString(m.Formats.IfaceVersion) + ", PIR " +
         versionString(m.Formats.PirVersion) + ", runtime ABI " +
         versionString(m.RuntimeAbi) + ")";
}

} // namespace

StatusOr<ModuleContents> readModuleContents(const pkm::File &file,
                                            const std::string &canonical,
                                            const ObjectCheck &checkObject,
                                            const FrontendRead &readFrontend) {
  ModuleContents out;
  StatusOr<pkm::Interface> iface =
      pkm::readInterface(file.section(pkm::Kind::Iface), canonical);
  if (!iface)
    return Status::error("has a bad IFACE section: " +
                         iface.status().message());
  if (Status s = checkInterface(*iface); !s)
    return Status::error("has a bad IFACE section: " + s.message());
  out.Iface = std::move(*iface);
  // CODE is read now, not when the program is lowered: a file whose code
  // is bad is unusable (a cache entry is rebuilt, a prebuilt file rejected
  // where it is imported, --check-only too), and the module decoded here
  // is the one the lowering gets.
  std::span<const uint8_t> codeBytes = file.section(pkm::Kind::Code);
  if (!(file.manifest().Contents & pkm::kContentsHasCode) || codeBytes.empty())
    return Status::error("has no CODE section");
  StatusOr<pir::Module> code = pir::binary::decode(codeBytes);
  if (!code)
    return Status::error("has a bad CODE section: " + code.status().message());
  if ((*code).Name != canonical)
    return Status::error("has a bad CODE section: it names module '" +
                         (*code).Name + "'");
  if (auto errors = pir::verify(*code); !errors.empty())
    return Status::error("has a bad CODE section: it does not verify:\n" +
                         pir::formatErrors(errors));
  if (Status s = checkCodeMatchesInterface(*code, out.Iface); !s)
    return Status::error("has an IFACE section that does not match its CODE: " +
                         s.message());
  out.Code = std::move(*code);
  // The type names and the templates are Sema's to read.
  if (readFrontend) {
    StatusOr<std::shared_ptr<void>> frontend =
        readFrontend(out.Iface, file.section(pkm::Kind::Tmpl), canonical);
    if (!frontend)
      return frontend.status();
    out.Frontend = std::move(*frontend);
  }
  if (file.manifest().Contents & pkm::kContentsHasObject) {
    StatusOr<pkm::Object> object =
        pkm::readObject(file.section(pkm::Kind::Object));
    if (!object)
      return Status::error("has a bad OBJ section: " +
                           object.status().message());
    if (checkObject)
      if (Status s = checkObject(*object); !s)
        return Status::error("has a bad OBJ section: " + s.message());
    out.Object = std::move(*object);
  }
  return out;
}

ModuleResolver::ModuleResolver(ResolverOptions opts, pkm::HostIdentity host)
    : Opts(std::move(opts)), Host(std::move(host)) {}

ModuleResolver::~ModuleResolver() = default;

pkm::HostIdentity ModuleResolver::hostIdentity() {
  pkm::HostIdentity h;
  h.PirVersion = pir::kPIRVersion;
  h.RuntimeAbi = kRuntimeABIVersion;
  h.Target = {8, 8, 1, 64, 64};
  h.CoreVersion = kVersion;
  h.CoreBuild = kBuildId;
  h.CompatibleVersions = {kVersion};
  return h;
}

void ModuleResolver::log(const std::string &line) const {
  if (Opts.Verbose)
    *Opts.Verbose << line << "\n";
}

void ModuleResolver::noteRead(const std::filesystem::path &path) {
  if (std::find(FilesRead.begin(), FilesRead.end(), path) == FilesRead.end())
    FilesRead.push_back(path);
}

std::string
ModuleResolver::relativeToRoot(const std::filesystem::path &path) const {
  return module_utils::relativeToRoot(Opts.ProjectRoot, path);
}

std::string ModuleResolver::shownPath(const std::filesystem::path &path) const {
  return module_utils::displayPath(Opts.ProjectRoot, path);
}

std::filesystem::path
ModuleResolver::cachePath(const std::string &canonical) const {
  std::filesystem::path p = module_utils::appendPath(
      Opts.ProjectRoot, std::filesystem::path(names::kCacheDir) /
                            module_name::cacheRelativePath(canonical));
  p += ".pkm";
  return p;
}

// -- resolve

Resolution ModuleResolver::resolve(const std::string &canonical) {
  if (auto it = Entries.find(canonical); it != Entries.end()) {
    const Entry &e = *it->second;
    Resolution r;
    r.Path = e.Path.string();
    if (e.State == Entry::State::Loaded) {
      r.Outcome = Resolution::Kind::Loaded;
      r.Iface = &e.Iface;
    } else {
      r.Outcome = Resolution::Kind::Source;
    }
    return r;
  }
  std::filesystem::path source = module_utils::moduleFile(
      Opts.ProjectRoot, isSystemName(canonical), modulePathOf(canonical));
  std::error_code ec;
  if (std::filesystem::exists(source, ec))
    return resolveSource(canonical, source);
  return resolvePrebuilt(canonical, source);
}

Resolution ModuleResolver::fromSource(const std::string &canonical,
                                      const std::filesystem::path &source,
                                      const std::string &why) {
  auto e = std::make_unique<Entry>();
  e->State = Entry::State::Source;
  e->Path = source;
  Entries[canonical] = std::move(e);
  Awaiting.erase(canonical);
  Pendings.erase(canonical);
  log("module " + canonical + ": " + why + "; building from source");
  Resolution r;
  r.Outcome = Resolution::Kind::Source;
  r.Path = source.string();
  return r;
}

Resolution ModuleResolver::resolveSource(const std::string &canonical,
                                         const std::filesystem::path &source) {
  auto fromSource = [&](const std::string &why) {
    return this->fromSource(canonical, source, why);
  };
  if (!Opts.ReadCache)
    return fromSource("cache not read");
  std::filesystem::path cache = cachePath(canonical);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(cache, ec))
    return fromSource("no cache entry");
  std::string shown = shownPath(cache);
  std::vector<uint8_t> sourceBytes;
  if (auto it = Pendings.find(canonical); it == Pendings.end()) {
    noteRead(source);
    StatusOr<std::vector<uint8_t>> bytes = pkm::readFileBytes(source);
    if (!bytes)
      return fromSource("cache " + shown + " unusable (" +
                        bytes.status().message() + ")");
    sourceBytes = std::move(*bytes);
  }
  Resolution r = load(canonical, cache, pkm::Policy::Cache, sourceBytes);
  if (r.Outcome == Resolution::Kind::Rejected)
    return fromSource("cache " + shown + " " + r.Message);
  if (r.Outcome == Resolution::Kind::Loaded) {
    Entries[canonical]->Source = source;
    log("module " + canonical + ": cache " + shown + " usable");
  }
  return r;
}

std::vector<std::filesystem::path>
ModuleResolver::prebuiltCandidates(const std::string &canonical,
                                   const std::filesystem::path &source) const {
  std::vector<std::filesystem::path> candidates;
  candidates.push_back(std::filesystem::path(source).replace_extension(".pkm"));
  std::filesystem::path rel =
      module_utils::modulePathToRelative(modulePathOf(canonical));
  rel.replace_extension(".pkm");
  for (const std::string &dir : Opts.ModulePath)
    candidates.push_back(module_utils::appendPath(dir, rel));
  return candidates;
}

bool ModuleResolver::exists(const std::string &canonical) const {
  std::filesystem::path source = module_utils::moduleFile(
      Opts.ProjectRoot, isSystemName(canonical), modulePathOf(canonical));
  std::error_code ec;
  if (std::filesystem::exists(source, ec))
    return true;
  const std::vector<std::filesystem::path> candidates =
      prebuiltCandidates(canonical, source);
  return std::any_of(candidates.begin(), candidates.end(),
                     [](const std::filesystem::path &c) {
                       std::error_code ec;
                       return std::filesystem::is_regular_file(c, ec);
                     });
}

Resolution
ModuleResolver::resolvePrebuilt(const std::string &canonical,
                                const std::filesystem::path &source) {
  const std::vector<std::filesystem::path> candidates =
      prebuiltCandidates(canonical, source);
  std::string tried = shownPath(source);
  for (const auto &c : candidates) {
    tried += ", " + shownPath(c);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(c, ec))
      continue;
    Resolution r = load(canonical, c, pkm::Policy::Prebuilt, {});
    std::string shown = shownPath(c);
    std::string line = "module ";
    line += canonical;
    line += ": prebuilt ";
    line += shown;
    line += ' ';
    if (r.Outcome == Resolution::Kind::Loaded) {
      log(line + "usable");
      return r;
    }
    if (r.Outcome == Resolution::Kind::NeedDependency)
      return r;
    // The first prebuilt file found decides: a rejected one is an error,
    // not a reason to look further (§8.2).
    log(line + r.Message);
    failed(canonical);
    std::string msg = "module '";
    msg += canonical;
    msg += "' (";
    msg += shown;
    msg += ") ";
    msg += r.Message;
    msg += ". Rebuild it from source.";
    r.Message = std::move(msg);
    return r;
  }
  failed(canonical);
  Resolution r;
  r.Outcome = Resolution::Kind::NotFound;
  r.Message = "not found (tried " + tried + ")";
  return r;
}

Resolution ModuleResolver::load(const std::string &canonical,
                                const std::filesystem::path &path,
                                pkm::Policy policy,
                                std::span<const uint8_t> sourceBytes) {
  auto unusable = [&](const std::string &why) {
    Awaiting.erase(canonical);
    Pendings.erase(canonical);
    Resolution r;
    r.Outcome = Resolution::Kind::Rejected;
    r.Message = why;
    return r;
  };
  Pending *p = nullptr;
  if (auto it = Pendings.find(canonical); it != Pendings.end()) {
    p = it->second.get();
  } else {
    noteRead(path);
    StatusOr<std::vector<uint8_t>> bytes = pkm::readFileBytes(path);
    if (!bytes)
      return unusable("cannot be read (" + bytes.status().message() + ")");
    pkm::Error err;
    StatusOr<pkm::File> file = pkm::File::read(*bytes, {}, &err);
    if (!file)
      // Every format is the release's: a file of another release is not
      // corrupt, only not this paykan's.
      return unusable(err.Code == pkm::ErrorCode::UnsupportedFormat
                          ? "was written by another paykan release: " +
                                err.str()
                          : "is corrupt: " + err.str());
    auto pending = std::make_unique<Pending>();
    pending->File = std::move(*file);
    pending->SourceBytes.assign(sourceBytes.begin(), sourceBytes.end());
    p = pending.get();
    Pendings[canonical] = std::move(pending);
  }
  const pkm::File &file = p->File;
  const pkm::Manifest &m = file.manifest();
  if (m.Module != canonical)
    return unusable("names module '" + m.Module + "'");
  pkm::Verdict v = pkm::checkCompatibility(m, Host, policy);
  if (v.Outcome != pkm::Verdict::Kind::Usable)
    return unusable(policy == pkm::Policy::Cache
                        ? (v.Outcome == pkm::Verdict::Kind::Stale
                               ? "is stale ("s
                               : "is rejected ("s) +
                              v.Message + ")"
                        : describeProducer(file) + "; " + v.Message);
  if (policy == pkm::Policy::Cache && !pkm::sourceMatches(m, p->SourceBytes))
    return unusable("is stale (the source changed)");
  // Every direct dependency must be resolved first, so its current
  // interface hash is known; a dependency that is itself waiting on this
  // module can only come from hand-edited files.
  for (const pkm::Manifest::Dep &d : m.Deps) {
    auto it = Entries.find(d.Name);
    if (it == Entries.end()) {
      if (Awaiting.count(d.Name))
        return unusable("depends on module '" + d.Name +
                        "', whose cache entry depends on it");
      // A cache entry's: its source's own import of it says why, where it
      // is (a prebuilt file has no source, and its import is the place).
      if (policy == pkm::Policy::Cache && !exists(d.Name))
        return unusable("depends on module '" + d.Name +
                        "', which is not found");
      Awaiting.insert(canonical);
      Resolution r;
      r.Outcome = Resolution::Kind::NeedDependency;
      r.Dependency = d.Name;
      return r;
    }
    const Entry &dep = *it->second;
    if (dep.State == Entry::State::Failed)
      return unusable("depends on module '" + d.Name +
                      "', which failed to load");
    if (dep.IfaceHash != d.IfaceHash)
      return unusable(policy == pkm::Policy::Cache
                          ? "is stale (the interface of module '" + d.Name +
                                "' changed)"
                          : "was built against another interface of module '" +
                                d.Name + "'");
    // Its code holds copies of the instantiations it uses, made from the
    // templates' bodies.
    if (dep.TmplHash != d.TmplHash)
      return unusable(
          policy == pkm::Policy::Cache
              ? "is stale (the templates of module '" + d.Name + "' changed)"
              : "was built against other templates of module '" + d.Name + "'");
  }
  // Code the active backend links must read and verify (docs/design/
  // pkm.md §8.4): a backend's reader is not made for damaged input.
  auto checkObject = [this](const pkm::Object &o) {
    return Opts.CheckObject && Opts.CompiledBy && o.sameKind(*Opts.CompiledBy)
               ? Opts.CheckObject(o)
               : Status::ok();
  };
  StatusOr<ModuleContents> contents =
      readModuleContents(file, canonical, checkObject, Opts.ReadFrontend);
  if (!contents)
    return unusable(contents.status().message());
  if (Status s = checkImportedTypes((*contents).Iface); !s)
    return unusable("has an IFACE section that does not match " + s.message());
  if (Status s = checkLinks((*contents).Code); !s)
    return unusable("has a bad CODE section: " + s.message());
  // A cache entry's code is the active backend's, or it is built again
  // (a prebuilt file's is checked by the backend that links it).
  if (const std::optional<pkm::Object> &o = (*contents).Object;
      o && policy == pkm::Policy::Cache && Opts.CompiledBy &&
      !o->sameKind(*Opts.CompiledBy))
    return unusable("is stale (its code was compiled by backend '" +
                    o->Backend + "' (" + o->Producer + " for " + o->Target +
                    "), not by this one)");
  auto e = std::make_unique<Entry>();
  e->State = Entry::State::Loaded;
  e->Path = path;
  e->Iface = std::move((*contents).Iface);
  e->Code = std::move((*contents).Code);
  for (const pkm::Manifest::Dep &d : m.Deps)
    e->Deps.push_back(d.Name);
  e->IfaceHash = file.ifaceHash();
  e->TmplHash = file.tmplHash();
  e->Object = std::move((*contents).Object);
  e->Frontend = std::move((*contents).Frontend);
  e->File = std::move(p->File);
  Pendings.erase(canonical);
  Awaiting.erase(canonical);
  Resolution r;
  r.Outcome = Resolution::Kind::Loaded;
  r.Path = path.string();
  r.Iface = &e->Iface;
  Entries[canonical] = std::move(e);
  return r;
}

Status ModuleResolver::checkImportedTypes(const pkm::Interface &iface) const {
  // A record of another module's type, against the one that module
  // describes (as it was built or loaded): the same but for whose it is.
  // An instantiation of the module's template made elsewhere (in this
  // module, or another it imports) may be one it does not describe.
  auto check = [&](const char *kind, const auto &records, auto theirs,
                   auto same) -> Status {
    for (const auto &r : records) {
      if (r.IsLocal || r.OriginModule.empty())
        continue;
      auto it = Entries.find(r.OriginModule);
      std::string why = "module '" + r.OriginModule + "': ";
      if (it == Entries.end() || (it->second->State != Entry::State::Built &&
                                  it->second->State != Entry::State::Loaded)) {
        why += "it records ";
        why += kind;
        why += " '" + r.Name + "' of that module, which it does not depend on";
        return Status::error(why);
      }
      // Sorted by name (readInterface, built()).
      const auto &defs = theirs(it->second->Iface);
      auto def = std::lower_bound(
          defs.begin(), defs.end(), r.Name,
          [](const auto &d, const std::string &n) { return d.Name < n; });
      const bool found = def != defs.end() && def->Name == r.Name;
      if (!found && r.Name.find('<') != std::string::npos)
        continue;
      if (!found || !same(r, *def)) {
        why += "its ";
        why += kind;
        why += " '" + r.Name + "' is not the one that module describes";
        return Status::error(why);
      }
    }
    return Status::ok();
  };
  if (Status s = check(
          "class", iface.Classes,
          [](const pkm::Interface &i) -> const auto & { return i.Classes; },
          [](const pkm::ClassRec &a, const pkm::ClassRec &b) {
            return a.SuperClassName == b.SuperClassName &&
                   a.Fields == b.Fields && a.Methods == b.Methods;
          });
      !s)
    return s;
  if (Status s = check(
          "struct", iface.Structs,
          [](const pkm::Interface &i) -> const auto & { return i.Structs; },
          [](const pkm::StructRec &a, const pkm::StructRec &b) {
            return a.Fields == b.Fields && a.Methods == b.Methods;
          });
      !s)
    return s;
  return check(
      "enum", iface.Enums,
      [](const pkm::Interface &i) -> const auto & { return i.Enums; },
      [](const pkm::EnumRec &a, const pkm::EnumRec &b) {
        return a.Variants == b.Variants &&
               a.PayloadVariants == b.PayloadVariants;
      });
}

Status ModuleResolver::checkLinks(const pir::Module &code) const {
  // Every module the code may name was resolved before it (a dependency's
  // own dependencies first); the code of one loaded from a file is at hand
  // until the lowering takes it, one built from source has none yet.
  const pir::LinkedModules resolved =
      [&](const std::string &name) -> std::optional<const pir::Module *> {
    auto it = Entries.find(name);
    if (it == Entries.end() || it->second->State == Entry::State::Failed)
      return std::nullopt;
    const Entry &e = *it->second;
    return e.State == Entry::State::Loaded && e.Code ? &*e.Code : nullptr;
  };
  std::vector<pir::VerifyError> errors = pir::verifyLinks(code, resolved);
  if (errors.empty())
    return Status::ok();
  return Status::error("it does not match the modules it names:\n" +
                       pir::formatErrors(errors));
}

// -- Sema's reports

void ModuleResolver::built(const std::string &canonical, pkm::Interface iface,
                           std::vector<std::string> deps,
                           std::vector<uint8_t> templates) {
  auto it = Entries.find(canonical);
  if (it == Entries.end() || it->second->State != Entry::State::Source)
    return; // not a module this resolver handed out
  Entry &e = *it->second;
  std::sort(deps.begin(), deps.end());
  deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
  iface.Module = canonical;
  iface.System = isSystemName(canonical);
  iface.DisplayFile = relativeToRoot(e.Path);
  iface.Mods.clear();
  for (const std::string &d : deps) {
    pkm::ModuleRef ref;
    ref.Canonical = d;
    ref.System = isSystemName(d);
    if (auto dit = Entries.find(d); dit != Entries.end()) {
      ref.IfaceHash = dit->second->IfaceHash;
      ref.TmplHash = dit->second->TmplHash;
    }
    iface.Mods.push_back(std::move(ref));
  }
  noteRead(e.Path);
  if (StatusOr<std::vector<uint8_t>> bytes = pkm::readFileBytes(e.Path))
    e.SourceBytes = std::move(*bytes);
  e.IfaceHash = support::sha256(pkm::writeInterface(iface));
  // By name, as a file's interface reads (checkImportedTypes).
  auto byName = [](auto &records) {
    std::sort(records.begin(), records.end(),
              [](const auto &a, const auto &b) { return a.Name < b.Name; });
  };
  byName(iface.Classes);
  byName(iface.Structs);
  byName(iface.Enums);
  e.TmplHash =
      templates.empty() ? support::Hash256{} : support::sha256(templates);
  e.Tmpl = std::move(templates);
  e.Iface = std::move(iface);
  e.Deps = std::move(deps);
  e.State = Entry::State::Built;
}

void ModuleResolver::failed(const std::string &canonical) {
  auto e = std::make_unique<Entry>();
  e->State = Entry::State::Failed;
  if (auto it = Entries.find(canonical); it != Entries.end())
    e->Path = it->second->Path;
  Entries[canonical] = std::move(e);
  Awaiting.erase(canonical);
  Pendings.erase(canonical);
}

bool ModuleResolver::buildFromSource(const std::string &canonical,
                                     const std::string &why) {
  // Only a cache entry is pending with a source next to it: a prebuilt
  // file is looked for when there is none.
  if (!Pendings.count(canonical) || Entries.count(canonical))
    return false;
  std::filesystem::path source = module_utils::moduleFile(
      Opts.ProjectRoot, isSystemName(canonical), modulePathOf(canonical));
  std::error_code ec;
  if (!std::filesystem::exists(source, ec))
    return false;
  fromSource(canonical, source,
             "cache " + shownPath(cachePath(canonical)) + " " + why);
  return true;
}

// -- What the lowering reads

bool ModuleResolver::isLoaded(const std::string &canonical) const {
  auto it = Entries.find(canonical);
  return it != Entries.end() && it->second->State == Entry::State::Loaded;
}

const std::vector<std::string> &
ModuleResolver::dependencies(const std::string &canonical) const {
  static const std::vector<std::string> none;
  auto it = Entries.find(canonical);
  return it == Entries.end() ? none : it->second->Deps;
}

StatusOr<pir::Module> ModuleResolver::loadCode(const std::string &canonical) {
  auto it = Entries.find(canonical);
  if (it == Entries.end() || it->second->State != Entry::State::Loaded)
    return Status::error("module '" + canonical +
                         "' was not loaded from a "
                         ".pkm file");
  Entry &e = *it->second;
  if (e.Code) {
    pir::Module code = std::move(*e.Code);
    e.Code.reset();
    return code;
  }
  // Handed over already: the bytes decoded and verified when they were
  // loaded.
  std::string shown =
      "module '" + canonical + "' (" + displayPath(canonical) + ")";
  if (!e.File)
    return Status::error(shown + ": no module file is open");
  StatusOr<pir::Module> mod =
      pir::binary::decode(e.File->section(pkm::Kind::Code));
  if (!mod)
    return Status::error(shown +
                         " has a bad CODE section: " + mod.status().message());
  return mod;
}

std::shared_ptr<void>
ModuleResolver::takeFrontendData(const std::string &canonical) {
  auto it = Entries.find(canonical);
  if (it == Entries.end() || it->second->State != Entry::State::Loaded)
    return nullptr;
  return std::move(it->second->Frontend);
}

const pkm::Object *ModuleResolver::object(const std::string &canonical) const {
  auto it = Entries.find(canonical);
  if (it == Entries.end() || it->second->State != Entry::State::Loaded)
    return nullptr;
  const std::optional<pkm::Object> &object = it->second->Object;
  return object ? &*object : nullptr;
}

std::span<const uint8_t>
ModuleResolver::templates(const std::string &canonical) const {
  auto it = Entries.find(canonical);
  if (it == Entries.end())
    return {};
  const Entry &e = *it->second;
  if (e.State != Entry::State::Loaded || !e.File)
    return {};
  return e.File->section(pkm::Kind::Tmpl);
}

std::string ModuleResolver::displayPath(const std::string &canonical) const {
  auto it = Entries.find(canonical);
  return it == Entries.end() ? std::string() : shownPath(it->second->Path);
}

std::string ModuleResolver::cacheSource(const std::string &canonical) const {
  auto it = Entries.find(canonical);
  return it == Entries.end() || it->second->Source.empty()
             ? std::string()
             : shownPath(it->second->Source);
}

std::string ModuleResolver::moduleFile(const std::string &canonical) const {
  auto it = Entries.find(canonical);
  if (it == Entries.end())
    return "";
  const Entry &e = *it->second;
  if (e.State == Entry::State::Loaded)
    return e.Path.string();
  return e.State == Entry::State::Built ? e.Written.string() : "";
}

// -- Writing

StatusOr<std::vector<uint8_t>> ModuleResolver::encodeFile(
    const std::string &canonical, const pkm::Interface &iface,
    const std::vector<std::string> &deps, std::span<const uint8_t> sourceBytes,
    const pir::Module &module, std::span<const uint8_t> templates,
    const pkm::Object *object) const {
  pkm::Manifest m;
  m.Module = canonical;
  m.Contents = pkm::kContentsHasCode;
  if (!templates.empty())
    m.Contents |= pkm::kContentsHasTemplates;
  if (object)
    m.Contents |= pkm::kContentsHasObject;
  if (definesMain(module))
    m.Contents |= pkm::kContentsHasMain;
  if (isSystemName(canonical))
    m.Contents |= pkm::kContentsSystem;
  m.Core = {kVersion,
            static_cast<uint32_t>(kVersionMajor),
            static_cast<uint32_t>(kVersionMinor),
            static_cast<uint32_t>(kVersionPatch),
            kVersionPrerelease,
            Host.CoreBuild};
  m.Formats = {pkm::kIfaceVersion, templates.empty() ? 0 : pkm::kTmplVersion,
               pir::kPIRVersion, pkm::kCodeBinaryPir1, 0};
  m.RuntimeAbi = Host.RuntimeAbi;
  m.Target = Host.Target;
  for (const std::string &d : deps) {
    pkm::Manifest::Dep dep;
    dep.Name = d;
    dep.Flags = isSystemName(d) ? pkm::kDepSystem : 0;
    if (auto it = Entries.find(d); it != Entries.end()) {
      dep.IfaceHash = it->second->IfaceHash;
      dep.TmplHash = it->second->TmplHash;
    }
    dep.CoreVersion = kVersion;
    m.Deps.push_back(std::move(dep));
  }
  m.Source = pkm::Manifest::SourceInfo{support::sha256(sourceBytes),
                                       sourceBytes.size()};
  m.Libraries.push_back({static_cast<uint8_t>(pkm::LibKind::Runtime),
                         "paykan_runtime", kVersion, Host.RuntimeAbi,
                         pkm::kLibRequiredAtLink});
  m.Frontend = {Opts.FrontendName, Opts.FrontendVersion, Opts.FrontendPlugin};
  m.Producer = {"paykan", kVersion};
  m.PluginApi = PAYKAN_PLUGIN_API_VERSION;

  std::vector<uint8_t> code = pir::binary::encode(module);
  std::vector<uint8_t> symidx =
      pir::binary::encodeSymbolIndex(pir::binary::index(code));
  pkm::Writer w;
  w.add(pkm::Kind::Manifest, pkm::kFlagRequired, pkm::encodeManifest(m));
  w.add(pkm::Kind::Iface, pkm::kFlagRequired, pkm::writeInterface(iface));
  if (!templates.empty())
    w.add(pkm::Kind::Tmpl, pkm::kFlagRequired,
          std::vector<uint8_t>(templates.begin(), templates.end()));
  w.add(pkm::Kind::Code, pkm::kFlagRequired, std::move(code));
  w.add(pkm::Kind::SymIdx, 0, std::move(symidx));
  if (object)
    w.add(pkm::Kind::Object, pkm::kFlagRequired, pkm::writeObject(*object));
  return w.finish();
}

void ModuleResolver::keepUncached(const std::string &canonical) {
  if (auto it = Entries.find(canonical); it != Entries.end())
    it->second->KeepUncached = true;
}

std::vector<std::string> ModuleResolver::cacheable() const {
  std::vector<std::string> names;
  if (!Opts.WriteCache)
    return names;
  for (const auto &[name, entry] : Entries)
    if (entry->State == Entry::State::Built && !entry->KeepUncached)
      names.push_back(name);
  return names;
}

const pkm::Interface *
ModuleResolver::builtInterface(const std::string &canonical) const {
  auto it = Entries.find(canonical);
  return it == Entries.end() || it->second->State != Entry::State::Built
             ? nullptr
             : &it->second->Iface;
}

void ModuleResolver::writeCacheEntry(const std::string &canonical,
                                     const pir::Module &code,
                                     const pkm::Object *object) {
  auto it = Entries.find(canonical);
  if (!Opts.WriteCache || it == Entries.end() ||
      it->second->State != Entry::State::Built || it->second->KeepUncached)
    return;
  Entry &entry = *it->second;
  std::filesystem::path path = cachePath(canonical);
  StatusOr<std::vector<uint8_t>> bytes =
      encodeFile(canonical, entry.Iface, entry.Deps, entry.SourceBytes, code,
                 entry.Tmpl, object);
  Status s = bytes.status();
  if (bytes) {
    // The cache makes its own directories (an entry of `a::b` is a/b.pkm).
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    s = ec ? Status::error("cannot create directory '" +
                           shownPath(path.parent_path()) + "': " + ec.message())
           : pkm::writeFileAtomically(path, *bytes);
  }
  if (s)
    entry.Written = path;
  log("module " + canonical + ": " +
      (s ? "wrote " + shownPath(path)
         : "cannot write " + shownPath(path) + ": " + s.message()));
}

Status ModuleResolver::writeModuleFile(
    const std::filesystem::path &out, const std::string &canonical,
    pkm::Interface iface, std::vector<std::string> deps,
    const std::filesystem::path &source, const pir::Module &code,
    std::span<const uint8_t> templates, const pkm::Object *object) {
  std::sort(deps.begin(), deps.end());
  deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
  iface.Module = canonical;
  iface.System = isSystemName(canonical);
  iface.DisplayFile = relativeToRoot(source);
  iface.Mods.clear();
  for (const std::string &d : deps) {
    pkm::ModuleRef ref;
    ref.Canonical = d;
    ref.System = isSystemName(d);
    if (auto it = Entries.find(d); it != Entries.end()) {
      ref.IfaceHash = it->second->IfaceHash;
      ref.TmplHash = it->second->TmplHash;
    }
    iface.Mods.push_back(std::move(ref));
  }
  StatusOr<std::vector<uint8_t>> sourceBytes = pkm::readFileBytes(source);
  if (!sourceBytes)
    return sourceBytes.status();
  StatusOr<std::vector<uint8_t>> bytes =
      encodeFile(canonical, iface, deps, *sourceBytes, code, templates, object);
  if (!bytes)
    return bytes.status();
  return pkm::writeFileAtomically(out, *bytes);
}

} // namespace paykan::modules
