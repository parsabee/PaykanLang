// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
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

#include "ModuleName.h"
#include "ModuleUtils.h"
#include "Names.h"
#include "Runtime.h"
#include "Version.h"
#include "paykan/pir/Binary.h"
#include "paykan/pir/Verifier.h"
#include "paykan/pir/Version.h"
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
  std::vector<uint8_t> SourceBytes; ///< Source, Built: for the manifest
  pkm::Interface Iface;             ///< Built, Loaded
  std::vector<std::string> Deps;    ///< Built, Loaded: direct imports
  support::Hash256 IfaceHash{};     ///< Built, Loaded
  std::optional<pkm::File> File;    ///< Loaded
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
      [](const pir::Function &f) { return !f.IsExtern && f.Name == "main"; });
}

/// "<verb> <what>" with the manifest's toolchain numbers, the shape of the
/// §8.2 error.
std::string describeProducer(const pkm::File &f) {
  const pkm::Manifest &m = f.manifest();
  return "was compiled by paykan " + m.Core.Version + " (pkm " +
         std::to_string(f.formatMajor()) + "." +
         std::to_string(f.formatMinor()) + ", interface " +
         std::to_string(m.Formats.IfaceMajor) + "." +
         std::to_string(m.Formats.IfaceMinor) + ", PIR " +
         std::to_string(m.Formats.PirVersion) + ", runtime ABI " +
         std::to_string(m.RuntimeAbi) + ")";
}

} // namespace

ModuleResolver::ModuleResolver(ResolverOptions opts, pkm::HostIdentity host)
    : Opts(std::move(opts)), Host(std::move(host)) {}

ModuleResolver::~ModuleResolver() = default;

pkm::HostIdentity ModuleResolver::hostIdentity() {
  pkm::HostIdentity h;
  h.PirVersion = pir::kPIRVersion;
  h.RuntimeAbi = kRuntimeABIVersion;
  h.Target = {8, 8, 1, 64, 64};
  h.CoreVersion = kVersion;
  h.CompatibleVersions = {kVersion};
  return h;
}

void ModuleResolver::log(const std::string &line) const {
  if (Opts.Verbose)
    *Opts.Verbose << line << "\n";
}

std::string
ModuleResolver::relativeToRoot(const std::filesystem::path &path) const {
  std::error_code ec;
  auto root = std::filesystem::weakly_canonical(
      Opts.ProjectRoot.empty() ? std::filesystem::path(".")
                               : std::filesystem::path(Opts.ProjectRoot),
      ec);
  if (ec)
    return path.string();
  auto abs = std::filesystem::weakly_canonical(path, ec);
  if (ec)
    return path.string();
  auto rel = abs.lexically_relative(root);
  if (rel.empty() || *rel.begin() == "..")
    return path.string();
  return rel.generic_string();
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

Resolution ModuleResolver::resolveSource(const std::string &canonical,
                                         const std::filesystem::path &source) {
  auto fromSource = [&](const std::string &why) {
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
  };
  if (!Opts.ReadCache)
    return fromSource("cache not read");
  std::filesystem::path cache = cachePath(canonical);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(cache, ec))
    return fromSource("no cache entry");
  std::string shown = relativeToRoot(cache);
  std::vector<uint8_t> sourceBytes;
  if (auto it = Pendings.find(canonical); it == Pendings.end()) {
    StatusOr<std::vector<uint8_t>> bytes = pkm::readFileBytes(source);
    if (!bytes)
      return fromSource("cache " + shown + " unusable (" +
                        bytes.status().message() + ")");
    sourceBytes = std::move(*bytes);
  }
  Resolution r = load(canonical, cache, pkm::Policy::Cache, sourceBytes);
  if (r.Outcome == Resolution::Kind::Rejected)
    return fromSource("cache " + shown + " " + r.Message);
  if (r.Outcome == Resolution::Kind::Loaded)
    log("module " + canonical + ": cache " + shown + " usable");
  return r;
}

Resolution
ModuleResolver::resolvePrebuilt(const std::string &canonical,
                                const std::filesystem::path &source) {
  std::vector<std::filesystem::path> candidates;
  candidates.push_back(std::filesystem::path(source).replace_extension(".pkm"));
  std::filesystem::path rel =
      module_utils::modulePathToRelative(modulePathOf(canonical));
  rel.replace_extension(".pkm");
  for (const std::string &dir : Opts.ModulePath)
    candidates.push_back(module_utils::appendPath(dir, rel));

  std::string tried = relativeToRoot(source);
  for (const auto &c : candidates) {
    tried += ", " + relativeToRoot(c);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(c, ec))
      continue;
    Resolution r = load(canonical, c, pkm::Policy::Prebuilt, {});
    std::string shown = relativeToRoot(c);
    if (r.Outcome == Resolution::Kind::Loaded) {
      log("module " + canonical + ": prebuilt " + shown + " usable");
      return r;
    }
    if (r.Outcome == Resolution::Kind::NeedDependency)
      return r;
    // The first prebuilt file found decides: a rejected one is an error,
    // not a reason to look further (§8.2).
    log("module " + canonical + ": prebuilt " + shown + " " + r.Message);
    failed(canonical);
    r.Message = "module '" + canonical + "' (" + shown + ") " + r.Message +
                ". Rebuild it from source.";
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
    StatusOr<std::vector<uint8_t>> bytes = pkm::readFileBytes(path);
    if (!bytes)
      return unusable("cannot be read (" + bytes.status().message() + ")");
    pkm::Error err;
    StatusOr<pkm::File> file = pkm::File::read(*bytes, {}, &err);
    if (!file)
      return unusable("is corrupt: " + err.str());
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
  if (!(m.Contents & pkm::kContentsHasCode) ||
      file.section(pkm::Kind::Code).empty())
    return unusable("has no CODE section");
  // Every direct dependency must be resolved first, so its current
  // interface hash is known; a dependency that is itself waiting on this
  // module can only come from hand-edited files.
  for (const pkm::Manifest::Dep &d : m.Deps) {
    auto it = Entries.find(d.Name);
    if (it == Entries.end()) {
      if (Awaiting.count(d.Name))
        return unusable("depends on module '" + d.Name +
                        "', whose cache entry depends on it");
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
  }
  StatusOr<pkm::Interface> iface =
      pkm::readInterface(file.section(pkm::Kind::Iface), canonical);
  if (!iface)
    return unusable("has a bad IFACE section: " + iface.status().message());
  auto e = std::make_unique<Entry>();
  e->State = Entry::State::Loaded;
  e->Path = path;
  e->Iface = std::move(*iface);
  for (const pkm::Manifest::Dep &d : m.Deps)
    e->Deps.push_back(d.Name);
  e->IfaceHash = file.ifaceHash();
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

// -- Sema's reports

void ModuleResolver::built(const std::string &canonical, pkm::Interface iface,
                           std::vector<std::string> deps) {
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
    if (auto dit = Entries.find(d); dit != Entries.end())
      ref.IfaceHash = dit->second->IfaceHash;
    iface.Mods.push_back(std::move(ref));
  }
  if (StatusOr<std::vector<uint8_t>> bytes = pkm::readFileBytes(e.Path))
    e.SourceBytes = std::move(*bytes);
  e.IfaceHash = support::sha256(pkm::writeInterface(iface));
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

StatusOr<pir::Module>
ModuleResolver::loadCode(const std::string &canonical) const {
  auto it = Entries.find(canonical);
  if (it == Entries.end() || it->second->State != Entry::State::Loaded)
    return Status::error("module '" + canonical +
                         "' was not loaded from a "
                         ".pkm file");
  const Entry &e = *it->second;
  std::string shown =
      "module '" + canonical + "' (" + displayPath(canonical) + ")";
  StatusOr<pir::Module> mod =
      pir::binary::decode(e.File->section(pkm::Kind::Code));
  if (!mod)
    return Status::error(shown +
                         ": CODE does not decode: " + mod.status().message());
  if ((*mod).Name != canonical)
    return Status::error(shown + ": CODE names module '" + (*mod).Name + "'");
  if (auto errors = pir::verify(*mod); !errors.empty())
    return Status::error(shown + ": CODE does not verify:\n" +
                         pir::formatErrors(errors));
  return mod;
}

std::string ModuleResolver::displayPath(const std::string &canonical) const {
  auto it = Entries.find(canonical);
  return it == Entries.end() ? std::string() : relativeToRoot(it->second->Path);
}

// -- Writing

StatusOr<std::vector<uint8_t>> ModuleResolver::encodeFile(
    const std::string &canonical, const pkm::Interface &iface,
    const std::vector<std::string> &deps, std::span<const uint8_t> sourceBytes,
    const pir::Module &module) const {
  pkm::Manifest m;
  m.Module = canonical;
  m.Contents = pkm::kContentsHasCode;
  if (definesMain(module))
    m.Contents |= pkm::kContentsHasMain;
  if (isSystemName(canonical))
    m.Contents |= pkm::kContentsSystem;
  m.Core = {kVersion,
            static_cast<uint32_t>(kVersionMajor),
            static_cast<uint32_t>(kVersionMinor),
            static_cast<uint32_t>(kVersionPatch),
            kVersionPrerelease,
            ""};
  m.Formats = {pkm::kIfaceMajor, pkm::kIfaceMinor,     0,
               pir::kPIRVersion, pkm::kCodeBinaryPir1, 0};
  m.RuntimeAbi = Host.RuntimeAbi;
  m.Target = Host.Target;
  for (const std::string &d : deps) {
    pkm::Manifest::Dep dep;
    dep.Name = d;
    dep.Flags = isSystemName(d) ? pkm::kDepSystem : 0;
    if (auto it = Entries.find(d); it != Entries.end())
      dep.IfaceHash = it->second->IfaceHash;
    dep.CoreVersion = kVersion;
    m.Deps.push_back(std::move(dep));
  }
  m.Source = pkm::Manifest::SourceInfo{support::sha256(sourceBytes),
                                       sourceBytes.size()};
  m.Libraries.push_back({static_cast<uint8_t>(pkm::LibKind::Runtime),
                         "paykan_runtime", kVersion, Host.RuntimeAbi,
                         pkm::kLibRequiredAtLink});
  m.Frontend = {Opts.FrontendName, "", ""};
  m.Producer = {"paykan", kVersion};
  m.PluginApi = PAYKAN_PLUGIN_API_VERSION;

  std::vector<uint8_t> code = pir::binary::encode(module);
  std::vector<uint8_t> symidx =
      pir::binary::encodeSymbolIndex(pir::binary::index(code));
  pkm::Writer w;
  w.add(pkm::Kind::Manifest, pkm::kFlagRequired, pkm::encodeManifest(m));
  w.add(pkm::Kind::Iface, pkm::kFlagRequired, pkm::writeInterface(iface));
  w.add(pkm::Kind::Code, pkm::kFlagRequired, std::move(code));
  w.add(pkm::Kind::SymIdx, 0, std::move(symidx));
  return w.finish();
}

void ModuleResolver::writeCache(const pir::Program &program) {
  if (!Opts.WriteCache)
    return;
  for (auto &[name, entry] : Entries) {
    if (entry->State != Entry::State::Built)
      continue;
    auto mod =
        std::find_if(program.Modules.begin(), program.Modules.end(),
                     [&](const pir::Module &m) { return m.Name == name; });
    if (mod == program.Modules.end())
      continue;
    std::filesystem::path path = cachePath(name);
    StatusOr<std::vector<uint8_t>> bytes =
        encodeFile(name, entry->Iface, entry->Deps, entry->SourceBytes, *mod);
    Status s = bytes ? pkm::writeFileAtomically(path, *bytes) : bytes.status();
    log("module " + name + ": " +
        (s ? "wrote " + relativeToRoot(path)
           : "cannot write " + relativeToRoot(path) + ": " + s.message()));
  }
}

Status ModuleResolver::writeModuleFile(const std::filesystem::path &out,
                                       const std::string &canonical,
                                       pkm::Interface iface,
                                       std::vector<std::string> deps,
                                       const std::filesystem::path &source,
                                       const pir::Module &module) {
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
    if (auto it = Entries.find(d); it != Entries.end())
      ref.IfaceHash = it->second->IfaceHash;
    iface.Mods.push_back(std::move(ref));
  }
  StatusOr<std::vector<uint8_t>> sourceBytes = pkm::readFileBytes(source);
  if (!sourceBytes)
    return sourceBytes.status();
  StatusOr<std::vector<uint8_t>> bytes =
      encodeFile(canonical, iface, deps, *sourceBytes, module);
  if (!bytes)
    return bytes.status();
  return pkm::writeFileAtomically(out, *bytes);
}

} // namespace paykan::modules
