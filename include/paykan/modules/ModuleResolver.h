// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The module resolver: where an imported module comes from (docs/design/pkm.md
// §8, phase A6-lite).  The driver creates one per compilation and hands it to
// Sema and the lowering.  For every canonical module name it locates the
// source and the `.pkm` candidates (§8.3), decides whether a cache entry is
// fresh or a prebuilt file usable (§8.2), loads the interface Sema needs and
// the CODE the lowering needs from the file, and writes the `.pkm` of every
// module built from source once the whole program verified.  Sema still
// parses and type-checks a module built from source; the resolver only
// decides and records.  A Sema without a resolver behaves as before.

#pragma once

#include "paykan/Status.h"
#include "paykan/pir/PIR.h"
#include "paykan/pkm/File.h"
#include "paykan/pkm/Interface.h"
#include "paykan/pkm/Manifest.h"

#include <filesystem>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace paykan::modules {

struct ResolverOptions {
  /// The source root imports are resolved against ("" = the current
  /// directory); the cache lives under it (names::kCacheDir).
  std::string ProjectRoot;
  /// The frontend name recorded in the manifests ("" = the default).
  std::string FrontendName;
  /// Where prebuilt modules are looked for after "next to the source":
  /// the --module-path directories, then $PAYKAN_MODULE_PATH.
  std::vector<std::string> ModulePath;
  /// Read cache entries (--rebuild-modules clears it).
  bool ReadCache = true;
  /// Write cache entries (--no-module-cache clears both).
  bool WriteCache = true;
  /// --verbose: one line per import (file, verdict, reason), and one per
  /// file written.
  std::ostream *Verbose = nullptr;
};

/// The answer to resolve() (§8.1 DECIDE).
struct Resolution {
  enum class Kind {
    /// Build it from the source at Path (Sema parses and checks it, then
    /// calls built()).  Path may not be a regular file: Sema diagnoses that
    /// as before.
    Source,
    /// Loaded from the `.pkm` at Path: Iface is the interface, the CODE is
    /// available through loadCode().
    Loaded,
    /// A cache or prebuilt entry depends on module Dependency, which must be
    /// resolved (and built if need be) first; then ask again.
    NeedDependency,
    /// Neither a source nor a usable prebuilt file: Message says what was
    /// tried, without the "module 'x'" prefix.
    NotFound,
    /// A prebuilt file exists but is unusable (§8.2): Message is the whole
    /// diagnostic text.
    Rejected,
  };
  Kind Outcome = Kind::NotFound;
  std::string Path;
  std::string Dependency;
  std::string Message;
  const pkm::Interface *Iface = nullptr;
};

class ModuleResolver {
public:
  ModuleResolver(ResolverOptions opts, pkm::HostIdentity host);
  ~ModuleResolver();
  ModuleResolver(const ModuleResolver &) = delete;
  ModuleResolver &operator=(const ModuleResolver &) = delete;

  /// What this toolchain is: Version.h, pir::kPIRVersion, the runtime ABI
  /// and the LP64 little-endian target.  core.build is left empty in this
  /// prototype (no build id yet).
  static pkm::HostIdentity hostIdentity();

  const ResolverOptions &options() const { return Opts; }

  /// Where module @p canonical comes from.  Memoised: once a module is
  /// Loaded, built() or failed(), the answer does not change.
  Resolution resolve(const std::string &canonical);

  /// Sema built @p canonical from its source: @p iface holds its exports
  /// (Mods, System and DisplayFile are filled here), @p deps its direct
  /// imports (canonical names, any order).  Its interface hash is known from
  /// here on, so importers' cache entries can be checked against it.
  void built(const std::string &canonical, pkm::Interface iface,
             std::vector<std::string> deps);
  /// Sema could not build @p canonical (not found, not a file, errors).
  void failed(const std::string &canonical);

  /// True when @p canonical was Loaded from a `.pkm`.
  bool isLoaded(const std::string &canonical) const;
  /// The direct dependencies of a Loaded module (manifest order).
  const std::vector<std::string> &
  dependencies(const std::string &canonical) const;
  /// The PIR of a Loaded module: CODE decoded and verified (§8.4 step 5).
  StatusOr<pir::Module> loadCode(const std::string &canonical) const;
  /// The file a diagnostic names for a Loaded module (project-relative).
  std::string displayPath(const std::string &canonical) const;

  /// After the program verified: write the cache entry of every module
  /// built from source whose pir::Module is in @p program.  A failure to
  /// write is reported on Verbose and otherwise ignored (the cache is only a
  /// cache).
  void writeCache(const pir::Program &program);

  /// --emit-pkm: the file of module @p canonical (the main module) at
  /// @p out, from its exports, its direct imports, its source and its
  /// lowered module.  Portable: MANIFEST, IFACE, CODE and SYMIDX.
  Status writeModuleFile(const std::filesystem::path &out,
                         const std::string &canonical, pkm::Interface iface,
                         std::vector<std::string> deps,
                         const std::filesystem::path &source,
                         const pir::Module &module);

  /// @p path relative to the project root when it lies under it, as given
  /// otherwise (the same rule as Sema's diagnostics).
  std::string relativeToRoot(const std::filesystem::path &path) const;

private:
  struct Entry;
  struct Pending;
  struct Candidate;

  Resolution resolveSource(const std::string &canonical,
                           const std::filesystem::path &source);
  Resolution resolvePrebuilt(const std::string &canonical,
                             const std::filesystem::path &source);
  /// Loads the file at @p path for @p canonical under @p policy up to the
  /// dependency check; the outcome is Loaded, NeedDependency, or a Source /
  /// Rejected / NotFound answer with the reason in Message.
  Resolution load(const std::string &canonical,
                  const std::filesystem::path &path, pkm::Policy policy,
                  std::span<const uint8_t> sourceBytes);
  StatusOr<std::vector<uint8_t>>
  encodeFile(const std::string &canonical, const pkm::Interface &iface,
             const std::vector<std::string> &deps,
             std::span<const uint8_t> sourceBytes,
             const pir::Module &module) const;
  std::filesystem::path cachePath(const std::string &canonical) const;
  void log(const std::string &line) const;

  ResolverOptions Opts;
  pkm::HostIdentity Host;
  std::map<std::string, std::unique_ptr<Entry>> Entries;
  /// Files read for a module whose dependencies are still being resolved.
  std::map<std::string, std::unique_ptr<Pending>> Pendings;
  /// Modules whose resolution is waiting on a dependency (a cycle through
  /// hand-edited cache entries falls back to building from source).
  std::set<std::string> Awaiting;
};

} // namespace paykan::modules
