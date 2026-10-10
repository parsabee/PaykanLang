// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// `paykan pkm dump` text (docs/design/pkm.md §8.5).  Golden-tested: the
// format changes only with a deliberate test update.  Nothing here depends
// on the host (no paths, no timestamps), so dumps compare across machines.

#include "paykan/pkm/Dump.h"

#include "Internal.h"
#include "paykan/pkm/Interface.h"

#include <cstdio>
#include <ostream>

namespace paykan::pkm {

namespace {

using Section = DumpOptions::Section;

std::string hex(const support::Hash256 &h) { return support::hex(h); }
std::string prefix(const support::Hash256 &h) {
  return support::hex(h).substr(0, 16);
}

std::string flagNames(uint32_t flags) {
  std::string out;
  if (flags & kFlagRequired)
    out += "REQUIRED";
  if (flags & kFlagInstanced)
    out += out.empty() ? "INSTANCED" : ",INSTANCED";
  return out.empty() ? "-" : out;
}

std::string contentsNames(uint32_t c) {
  static const char *const names[] = {"HAS_CODE",        "HAS_TEMPLATES",
                                      "HAS_MAIN",        "SYSTEM",
                                      "HAS_STATIC_INIT", "EXPORTS_C"};
  std::string out;
  for (int i = 0; i < 6; ++i)
    if (c & (1u << i))
      out += (out.empty() ? "" : ",") + std::string(names[i]);
  return out.empty() ? "-" : out;
}

const char *libKindName(uint8_t kind) {
  switch (static_cast<LibKind>(kind)) {
  case LibKind::Runtime:
    return "RUNTIME";
  case LibKind::Allocator:
    return "ALLOCATOR";
  case LibKind::CLibrary:
    return "C_LIBRARY";
  case LibKind::PaykanLibrary:
    return "PAYKAN_LIBRARY";
  }
  return "?";
}

std::string quoted(const std::string &s) { return "\"" + s + "\""; }

/// A section kind as four hex digits, the width of §1.4's table.
std::string kindHex(uint32_t kind) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%04x", kind);
  return buf;
}

std::string list(const std::vector<std::string> &v) {
  std::string out;
  for (const std::string &s : v)
    out += (out.empty() ? "" : ", ") + s;
  return out;
}

/// A signature's parameter types, each after its mode when it has one
/// (`inout int`).
std::string params(const std::vector<std::string> &types, const ModeRecs &m) {
  std::vector<std::string> out = types;
  for (size_t i = 0; i < out.size() && i < m.Modes.size(); ++i)
    if (m.Modes[i] != kModeValue)
      out[i] = (m.Modes[i] == kModeView ? "view " : "inout ") + out[i];
  return list(out);
}

/// A borrowed result's mode, before its type (`view `); "" for a copy.
std::string result(const ModeRecs &m) {
  if (m.Result == kModeValue)
    return "";
  return m.Result == kModeView ? "view " : "inout ";
}

void dumpHeader(const File &f, std::ostream &os) {
  os << "pkm " << f.formatMajor() << "." << f.formatMinor() << ", "
     << f.sections().size() << " sections, table sha256 "
     << prefix(f.tableHash()) << "\n";
  os << "iface_hash " << hex(f.ifaceHash()) << "\n";
  os << "tmpl_hash " << hex(f.tmplHash()) << "\n";
  os << "code_hash " << hex(f.codeHash()) << "\n";
  os << "module_hash " << hex(f.moduleHash()) << "\n";
}

void dumpSections(const File &f, std::ostream &os) {
  os << "sections:\n";
  size_t i = 0;
  for (const SectionEntry &e : f.sections()) {
    const char *name = kindName(e.Kind);
    os << "  #" << i++ << " " << (name ? name : kindHex(e.Kind)) << " flags "
       << flagNames(e.Flags) << " offset " << detail::hex(e.Offset) << " size "
       << e.Size << " sha256 " << prefix(e.Hash) << "\n";
  }
}

void dumpManifest(const Manifest &m, std::ostream &os) {
  os << "manifest:\n";
  os << "  module " << m.Module << "\n";
  os << "  contents " << detail::hex(m.Contents) << " ("
     << contentsNames(m.Contents) << ")\n";
  os << "  core " << quoted(m.Core.Version) << " " << m.Core.Major << "."
     << m.Core.Minor << "." << m.Core.Patch << " prerelease "
     << quoted(m.Core.Prerelease) << " build " << quoted(m.Core.Build) << "\n";
  os << "  format_versions iface " << m.Formats.IfaceMajor << "."
     << m.Formats.IfaceMinor << " tmpl " << m.Formats.TmplVersion << " pir "
     << m.Formats.PirVersion << " code_encoding " << m.Formats.CodeEncoding
     << " debug " << m.Formats.DebugVersion << "\n";
  os << "  runtime_abi " << m.RuntimeAbi << "\n";
  os << "  target pointer " << unsigned(m.Target.PointerSize) << " slot "
     << unsigned(m.Target.SlotSize) << " endianness "
     << unsigned(m.Target.Endianness) << " int " << unsigned(m.Target.IntWidth)
     << " float " << unsigned(m.Target.FloatWidth) << "\n";
  os << "  deps " << m.Deps.size() << "\n";
  for (const Manifest::Dep &d : m.Deps)
    os << "    " << d.Name << " flags " << detail::hex(d.Flags) << " iface "
       << prefix(d.IfaceHash) << " tmpl " << prefix(d.TmplHash) << " core "
       << quoted(d.CoreVersion) << "\n";
  if (m.Source)
    os << "  source sha256 " << hex(m.Source->Sha256) << " size "
       << m.Source->Size << "\n";
  else
    os << "  source -\n";
  os << "  libraries " << m.Libraries.size() << "\n";
  for (const Manifest::Lib &l : m.Libraries)
    os << "    " << libKindName(l.Kind) << " " << quoted(l.Name) << " version "
       << quoted(l.Version) << " abi " << l.Abi << " flags "
       << detail::hex(l.Flags) << "\n";
  os << "  frontend " << quoted(m.Frontend.Name) << " "
     << quoted(m.Frontend.Version) << " plugin " << quoted(m.Frontend.Plugin)
     << "\n";
  os << "  producer " << quoted(m.Producer.Tool) << " "
     << quoted(m.Producer.Version) << "\n";
  os << "  plugin_api " << m.PluginApi << "\n";
  os << "  opt_pipeline " << quoted(m.OptPipeline) << "\n";
  os << "  attributes " << m.Attributes.size();
  for (const std::string &a : m.Attributes)
    os << " " << quoted(a);
  os << "\n";
}

void dumpIface(const File &f, std::ostream &os) {
  std::span<const uint8_t> bytes = f.section(Kind::Iface);
  if (bytes.empty()) {
    os << "iface: absent\n";
    return;
  }
  StatusOr<Interface> r = readInterface(bytes, f.manifest().Module);
  if (!r) {
    os << "iface: corrupt: " << r.status().message() << "\n";
    return;
  }
  const Interface &i = *r;
  os << "iface:\n";
  os << "  module " << i.Module << (i.System ? " system" : "") << "\n";
  os << "  display_file " << quoted(i.DisplayFile) << "\n";
  os << "  deps " << i.Mods.size() << "\n";
  for (const ModuleRef &m : i.Mods)
    os << "    " << m.Canonical << (m.System ? " system" : "") << " iface "
       << prefix(m.IfaceHash) << " tmpl " << prefix(m.TmplHash) << "\n";
  auto origin = [](bool local, const std::string &module) {
    return std::string(local ? "local" : "imported") + " from " +
           quoted(module);
  };
  for (const EnumRec &e : i.Enums)
    os << "  enum " << e.Name << " [" << origin(e.IsLocal, e.OriginModule)
       << "] { " << list(e.Variants) << " }\n";
  for (const ClassRec &c : i.Classes) {
    os << "  class " << c.Name << " super " << quoted(c.SuperClassName) << " ["
       << origin(c.IsLocal, c.OriginModule) << "]\n";
    for (const ClassRec::Field &fl : c.Fields)
      os << "    field " << fl.FieldName << ": " << fl.TypeName << "\n";
    for (const ClassRec::Method &m : c.Methods)
      os << "    method " << m.Name << "(" << params(m.ParamTypeNames, m.Modes)
         << ") -> " << result(m.Modes) << m.ReturnTypeName << " flags "
         << detail::hex(m.Flags) << "\n";
  }
  for (const FuncRec &fn : i.Functions)
    os << "  func " << fn.Name << "(" << params(fn.ParamTypeNames, fn.Modes)
       << ") -> " << result(fn.Modes) << fn.ReturnTypeName << "\n";
}

void dumpBinary(const File &f, std::ostream &os, Kind k, const char *name) {
  std::span<const uint8_t> bytes = f.section(k);
  if (bytes.empty())
    os << name << ": absent\n";
  else
    os << name << ": " << bytes.size() << " B, sha256 "
       << hex(support::sha256(bytes)) << "\n";
}

void dumpPayloads(const File &f, std::ostream &os) {
  os << "payloads " << f.count(Kind::Payload) << "\n";
  for (const SectionEntry &e : f.sections())
    if (e.Kind == static_cast<uint32_t>(Kind::Payload))
      os << "  kind " << kindHex(e.Kind) << " " << e.Size
         << " B (not decoded)\n";
}

} // namespace

void dump(const File &f, std::ostream &os, const DumpOptions &opts) {
  Section w = opts.Which;
  bool all = w == Section::All;
  if (all)
    dumpHeader(f, os);
  if (all || w == Section::Sections)
    dumpSections(f, os);
  if (all || w == Section::Manifest)
    dumpManifest(f.manifest(), os);
  if (all || w == Section::Iface)
    dumpIface(f, os);
  if (all) {
    dumpBinary(f, os, Kind::Tmpl, "tmpl");
    dumpBinary(f, os, Kind::Code, "code");
  }
  if (all || w == Section::SymIdx)
    dumpBinary(f, os, Kind::SymIdx, "symidx");
  if (all)
    dumpBinary(f, os, Kind::Debug, "debug");
  if (all || w == Section::Payloads)
    dumpPayloads(f, os);
}

} // namespace paykan::pkm
