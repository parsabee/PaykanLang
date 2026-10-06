// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The MANIFEST codec (docs/design/pkm.md §2) and the compatibility verdict
// (§8.2).  Each record is decoded by a sub-reader bounded to its length that
// must consume it exactly, so a record can never read into its neighbour
// and an extended value is caught rather than silently misread.

#include "paykan/pkm/Manifest.h"

#include "Internal.h"
#include "ModuleName.h"
#include "paykan/support/Bytes.h"

#include <algorithm>
#include <cstring>

namespace paykan::pkm {

using support::ByteReader;
using support::ByteWriter;

namespace {

enum Tag : uint64_t {
  TModule = 0x01,
  TContents = 0x03,
  TCore = 0x05,
  TFormats = 0x07,
  TRuntimeAbi = 0x09,
  TTarget = 0x0B,
  TDeps = 0x0D,
  TSource = 0x0F,
  TLibraries = 0x11,
  TFrontend = 0x20,
  TProducer = 0x22,
  TPluginApi = 0x24,
  TOptPipeline = 0x26,
  TAttributes = 0x28,
};

constexpr uint64_t kRequired[] = {TModule,     TContents, TCore, TFormats,
                                  TRuntimeAbi, TTarget,   TDeps};

const char *tagName(uint64_t tag) {
  switch (tag) {
  case TModule:
    return "module";
  case TContents:
    return "contents";
  case TCore:
    return "core";
  case TFormats:
    return "format_versions";
  case TRuntimeAbi:
    return "runtime_abi";
  case TTarget:
    return "target";
  case TDeps:
    return "deps";
  case TSource:
    return "source";
  case TLibraries:
    return "libraries";
  case TFrontend:
    return "frontend";
  case TProducer:
    return "producer";
  case TPluginApi:
    return "plugin_api";
  case TOptPipeline:
    return "opt_pipeline";
  case TAttributes:
    return "attributes";
  default:
    return nullptr;
  }
}

/// Appends one record: tag, length, the bytes @p body wrote.
template <typename F> void record(ByteWriter &w, uint64_t tag, F body) {
  ByteWriter v;
  body(v);
  w.uleb(tag);
  w.uleb(v.size());
  w.bytes(v.data());
}

// Each decoder reads the fields it knows from @p r; the caller requires
// that nothing is left.
bool decodeCore(ByteReader &r, Manifest::CoreInfo &c) {
  return detail::readStr(r, c.Version) && r.u32(c.Major) && r.u32(c.Minor) &&
         r.u32(c.Patch) && detail::readStr(r, c.Prerelease) &&
         detail::readStr(r, c.Build);
}

bool decodeFormats(ByteReader &r, Manifest::FormatVersions &f) {
  return r.u32(f.IfaceMajor) && r.u32(f.IfaceMinor) && r.u32(f.TmplVersion) &&
         r.u32(f.PirVersion) && r.u32(f.CodeEncoding) && r.u32(f.DebugVersion);
}

bool decodeTarget(ByteReader &r, Manifest::TargetInfo &t) {
  return r.u8(t.PointerSize) && r.u8(t.SlotSize) && r.u8(t.Endianness) &&
         r.u8(t.IntWidth) && r.u8(t.FloatWidth);
}

bool decodeDeps(ByteReader &r, std::vector<Manifest::Dep> &deps) {
  uint64_t n;
  // name(≥1) + flags(4) + two hashes(64) + core_version(≥1)
  if (!detail::readCount(r, n, 70))
    return false;
  deps.resize(static_cast<size_t>(n));
  for (Manifest::Dep &d : deps)
    if (!detail::readStr(r, d.Name) || !r.u32(d.Flags) ||
        !detail::readHash(r, d.IfaceHash) || !detail::readHash(r, d.TmplHash) ||
        !detail::readStr(r, d.CoreVersion))
      return false;
  return true;
}

bool decodeLibs(ByteReader &r, std::vector<Manifest::Lib> &libs) {
  uint64_t n;
  if (!detail::readCount(r, n, 11))
    return false;
  libs.resize(static_cast<size_t>(n));
  for (Manifest::Lib &l : libs)
    if (!r.u8(l.Kind) || !detail::readStr(r, l.Name) ||
        !detail::readStr(r, l.Version) || !r.u32(l.Abi) || !r.u32(l.Flags))
      return false;
  return true;
}

bool decodeStrList(ByteReader &r, std::vector<std::string> &out) {
  uint64_t n;
  if (!detail::readCount(r, n, 1))
    return false;
  out.resize(static_cast<size_t>(n));
  for (std::string &s : out)
    if (!detail::readStr(r, s))
      return false;
  return true;
}

/// A canonical module name (ModuleName.h): what canonicalImportName gives
/// back unchanged, with at least one component.
bool wellFormedModule(const std::string &name) {
  bool system = name.starts_with(module_name::kSep);
  return !module_name::components(name).empty() &&
         module_name::canonicalImportName(name, system) == name;
}

std::string describe(const Manifest::TargetInfo &t) {
  return std::to_string(t.PointerSize) + "/" + std::to_string(t.SlotSize) +
         "/" + std::to_string(t.Endianness) + "/" + std::to_string(t.IntWidth) +
         "/" + std::to_string(t.FloatWidth);
}

} // namespace

std::vector<uint8_t> encodeManifest(const Manifest &m) {
  ByteWriter w;
  w.bytes("PKMM", 4);
  w.u16(kManifestMajor);
  w.u16(kManifestMinor);
  record(w, TModule, [&](ByteWriter &v) { v.str(m.Module); });
  record(w, TContents, [&](ByteWriter &v) { v.u32(m.Contents); });
  record(w, TCore, [&](ByteWriter &v) {
    v.str(m.Core.Version);
    v.u32(m.Core.Major);
    v.u32(m.Core.Minor);
    v.u32(m.Core.Patch);
    v.str(m.Core.Prerelease);
    v.str(m.Core.Build);
  });
  record(w, TFormats, [&](ByteWriter &v) {
    v.u32(m.Formats.IfaceMajor);
    v.u32(m.Formats.IfaceMinor);
    v.u32(m.Formats.TmplVersion);
    v.u32(m.Formats.PirVersion);
    v.u32(m.Formats.CodeEncoding);
    v.u32(m.Formats.DebugVersion);
  });
  record(w, TRuntimeAbi, [&](ByteWriter &v) { v.u32(m.RuntimeAbi); });
  record(w, TTarget, [&](ByteWriter &v) {
    v.u8(m.Target.PointerSize);
    v.u8(m.Target.SlotSize);
    v.u8(m.Target.Endianness);
    v.u8(m.Target.IntWidth);
    v.u8(m.Target.FloatWidth);
  });
  record(w, TDeps, [&](ByteWriter &v) {
    v.uleb(m.Deps.size());
    for (const Manifest::Dep &d : m.Deps) {
      v.str(d.Name);
      v.u32(d.Flags);
      detail::writeHash(v, d.IfaceHash);
      detail::writeHash(v, d.TmplHash);
      v.str(d.CoreVersion);
    }
  });
  if (m.Source)
    record(w, TSource, [&](ByteWriter &v) {
      detail::writeHash(v, m.Source->Sha256);
      v.u64(m.Source->Size);
    });
  if (!m.Libraries.empty())
    record(w, TLibraries, [&](ByteWriter &v) {
      v.uleb(m.Libraries.size());
      for (const Manifest::Lib &l : m.Libraries) {
        v.u8(l.Kind);
        v.str(l.Name);
        v.str(l.Version);
        v.u32(l.Abi);
        v.u32(l.Flags);
      }
    });
  if (m.Frontend != Manifest::FrontendInfo{})
    record(w, TFrontend, [&](ByteWriter &v) {
      v.str(m.Frontend.Name);
      v.str(m.Frontend.Version);
      v.str(m.Frontend.Plugin);
    });
  if (m.Producer != Manifest::ProducerInfo{})
    record(w, TProducer, [&](ByteWriter &v) {
      v.str(m.Producer.Tool);
      v.str(m.Producer.Version);
    });
  if (m.PluginApi)
    record(w, TPluginApi, [&](ByteWriter &v) { v.u32(m.PluginApi); });
  if (!m.OptPipeline.empty())
    record(w, TOptPipeline, [&](ByteWriter &v) { v.str(m.OptPipeline); });
  if (!m.Attributes.empty())
    record(w, TAttributes, [&](ByteWriter &v) {
      v.uleb(m.Attributes.size());
      for (const std::string &a : m.Attributes)
        v.str(a);
    });
  return w.take();
}

StatusOr<Manifest> decodeManifest(std::span<const uint8_t> bytes) {
  if (bytes.size() > kMaxManifestSize)
    return Status::error("manifest larger than 1 MiB");
  ByteReader r(bytes);
  std::span<const uint8_t> magic;
  uint16_t major, minor;
  if (!r.bytes(4, magic) || !r.u16(major) || !r.u16(minor))
    return Status::error("truncated manifest header");
  if (std::memcmp(magic.data(), "PKMM", 4) != 0)
    return Status::error("bad manifest magic");
  if (major != kManifestMajor)
    return Status::error("manifest format " + std::to_string(major) + "." +
                         std::to_string(minor) + "; this reader knows " +
                         std::to_string(kManifestMajor) + ".x");
  Manifest m;
  std::vector<uint64_t> seen;
  uint64_t last = 0;
  while (!r.atEnd()) {
    size_t at = r.offset();
    uint64_t tag, len;
    if (!r.uleb(tag) || !r.uleb(len))
      return Status::error("bad record header: " + detail::readerError(r));
    if (!seen.empty() && tag <= last)
      return Status::error("record " + detail::hex(tag) + " at byte " +
                           std::to_string(at) + " is out of order or repeated");
    last = tag;
    seen.push_back(tag);
    if (len > r.remaining())
      return Status::error("record " + detail::hex(tag) + " at byte " +
                           std::to_string(at) + " is longer than the manifest");
    ByteReader v = r.sub(static_cast<size_t>(len));
    bool ok = true;
    bool knownTag = true;
    switch (tag) {
    case TModule:
      ok = detail::readStr(v, m.Module);
      break;
    case TContents:
      ok = v.u32(m.Contents);
      break;
    case TCore:
      ok = decodeCore(v, m.Core);
      break;
    case TFormats:
      ok = decodeFormats(v, m.Formats);
      break;
    case TRuntimeAbi:
      ok = v.u32(m.RuntimeAbi);
      break;
    case TTarget:
      ok = decodeTarget(v, m.Target);
      break;
    case TDeps:
      ok = decodeDeps(v, m.Deps);
      break;
    case TSource: {
      Manifest::SourceInfo s;
      ok = detail::readHash(v, s.Sha256) && v.u64(s.Size);
      m.Source = s;
      break;
    }
    case TLibraries:
      ok = decodeLibs(v, m.Libraries);
      break;
    case TFrontend:
      ok = detail::readStr(v, m.Frontend.Name) &&
           detail::readStr(v, m.Frontend.Version) &&
           detail::readStr(v, m.Frontend.Plugin);
      break;
    case TProducer:
      ok = detail::readStr(v, m.Producer.Tool) &&
           detail::readStr(v, m.Producer.Version);
      break;
    case TPluginApi:
      ok = v.u32(m.PluginApi);
      break;
    case TOptPipeline:
      ok = detail::readStr(v, m.OptPipeline);
      break;
    case TAttributes:
      ok = decodeStrList(v, m.Attributes);
      break;
    default:
      knownTag = false;
      if (tag & 1)
        return Status::error("unknown critical record " + detail::hex(tag) +
                             " at byte " + std::to_string(at));
      break; // non-critical: skipped
    }
    if (!ok)
      return Status::error(std::string("bad ") + tagName(tag) +
                           " record: " + detail::readerError(v));
    if (knownTag && !v.atEnd())
      return Status::error(std::string("record ") + tagName(tag) + " has " +
                           std::to_string(v.remaining()) + " trailing bytes");
  }
  for (uint64_t tag : kRequired)
    if (std::find(seen.begin(), seen.end(), tag) == seen.end())
      return Status::error(std::string("missing required record ") +
                           tagName(tag));
  if (!wellFormedModule(m.Module))
    return Status::error("'" + m.Module + "' is not a canonical module name");
  for (size_t i = 0; i < m.Deps.size(); ++i) {
    const std::string &name = m.Deps[i].Name;
    if (!wellFormedModule(name))
      return Status::error("dependency '" + name +
                           "' is not a canonical module name");
    if (name == m.Module)
      return Status::error("module '" + name + "' depends on itself");
    if (i > 0 && !(m.Deps[i - 1].Name < name))
      return Status::error("deps are not sorted and unique ('" +
                           m.Deps[i - 1].Name + "' then '" + name + "')");
    if (bool(m.Deps[i].Flags & kDepSystem) != name.starts_with("::"))
      return Status::error("dependency '" + name +
                           "': SYSTEM flag disagrees with the name");
  }
  size_t runtimes = 0;
  for (const Manifest::Lib &l : m.Libraries) {
    if (l.Kind < 1 || l.Kind > 4)
      return Status::error("library '" + l.Name + "' has unknown kind " +
                           std::to_string(l.Kind));
    if (l.Kind == static_cast<uint8_t>(LibKind::Runtime)) {
      ++runtimes;
      if (l.Abi != m.RuntimeAbi)
        return Status::error("RUNTIME library ABI " + std::to_string(l.Abi) +
                             " differs from runtime_abi " +
                             std::to_string(m.RuntimeAbi));
    }
  }
  if (runtimes != 1)
    return Status::error("libraries must hold exactly one RUNTIME entry, "
                         "not " +
                         std::to_string(runtimes));
  return m;
}

Verdict checkCompatibility(const Manifest &m, const HostIdentity &host,
                           Policy policy) {
  using K = Verdict::Kind;
  using R = Verdict::Reason;
  auto verdict = [](K k, R r, std::string msg) {
    return Verdict{k, r, std::move(msg)};
  };
  const Manifest::FormatVersions &f = m.Formats;
  if (f.IfaceMajor != host.IfaceMajor)
    return verdict(K::Rejected, R::Format,
                   "interface format " + std::to_string(f.IfaceMajor) + "." +
                       std::to_string(f.IfaceMinor) + "; this paykan reads " +
                       std::to_string(host.IfaceMajor) + ".x");
  if (f.IfaceMinor > host.IfaceMinor)
    return verdict(
        policy == Policy::Cache ? K::Stale : K::Rejected, R::Interface,
        "interface format " + std::to_string(f.IfaceMajor) + "." +
            std::to_string(f.IfaceMinor) + " is newer than this paykan's " +
            std::to_string(host.IfaceMajor) + "." +
            std::to_string(host.IfaceMinor));
  if (f.PirVersion != host.PirVersion)
    return verdict(K::Rejected, R::PIR,
                   "PIR " + std::to_string(f.PirVersion) +
                       "; this paykan needs PIR " +
                       std::to_string(host.PirVersion));
  if (m.RuntimeAbi != host.RuntimeAbi)
    return verdict(K::Rejected, R::ABI,
                   "runtime ABI " + std::to_string(m.RuntimeAbi) +
                       "; this paykan needs runtime ABI " +
                       std::to_string(host.RuntimeAbi));
  if (m.Target != host.Target)
    return verdict(K::Rejected, R::Target,
                   "target " + describe(m.Target) + "; this paykan targets " +
                       describe(host.Target));
  if (policy == Policy::Cache) {
    if (m.Core.Version != host.CoreVersion || m.Core.Build != host.CoreBuild)
      return verdict(
          K::Stale, R::Toolchain,
          "compiled by paykan " + m.Core.Version +
              (m.Core.Build.empty() ? "" : " (" + m.Core.Build + ")") +
              "; this is paykan " + host.CoreVersion +
              (host.CoreBuild.empty() ? "" : " (" + host.CoreBuild + ")"));
  } else if (std::find(host.CompatibleVersions.begin(),
                       host.CompatibleVersions.end(),
                       m.Core.Version) == host.CompatibleVersions.end()) {
    return verdict(K::Rejected, R::Toolchain,
                   "compiled by paykan " + m.Core.Version +
                       ", which this paykan " + host.CoreVersion +
                       " does not accept");
  }
  return Verdict{};
}

bool sourceMatches(const Manifest &m, std::span<const uint8_t> sourceBytes) {
  return m.Source && m.Source->Size == sourceBytes.size() &&
         m.Source->Sha256 == support::sha256(sourceBytes);
}

} // namespace paykan::pkm
