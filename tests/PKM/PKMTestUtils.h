// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Shared fixtures for the .pkm tests: a representative manifest, interface
// and container, and the byte patching helpers the corruption tests use.

#pragma once

#include "paykan/pkm/File.h"
#include "paykan/pkm/Interface.h"
#include "paykan/pkm/Manifest.h"
#include "paykan/support/Sha256.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace pkmtest {

using namespace paykan;
using namespace paykan::pkm;

inline support::Hash256 hashOf(const char *s) { return support::sha256(s); }

/// A manifest with every record populated.
inline Manifest sampleManifest() {
  Manifest m;
  m.Module = "geometry::shapes";
  m.Contents = kContentsHasCode;
  m.Core = {"0.2.0-rc1", 0, 2, 0, "rc1", "abc123"};
  m.Formats = {1, 0, 1, 2, kCodeBinaryPir1, 1};
  m.RuntimeAbi = 7;
  m.Target = {8, 8, 1, 64, 64};
  m.Deps.push_back({"::io", kDepSystem, hashOf("io iface"), {}, "0.2.0-rc1"});
  m.Deps.push_back({"geometry::vec", 0, hashOf("vec iface"), hashOf("vec tmpl"),
                    "0.2.0-rc1"});
  m.Deps.back().Flags = kDepInstantiated;
  m.Source = Manifest::SourceInfo{hashOf("source"), 1234};
  m.Libraries.push_back({static_cast<uint8_t>(LibKind::Runtime),
                         "paykan_runtime", "0.2.0", 7, kLibRequiredAtLink});
  m.Frontend = {"recursive-descent", "0.2.0", ""};
  m.Producer = {"paykan", "0.2.0-rc1"};
  m.PluginApi = 3;
  m.OptPipeline = "";
  m.Attributes = {"@inline", "@cold"};
  return m;
}

/// The smallest manifest decodeManifest accepts, for module @p name.
inline Manifest minimalManifest(const std::string &name = "m") {
  Manifest m;
  m.Module = name;
  if (name.starts_with("::"))
    m.Contents = kContentsSystem;
  m.Core.Version = "0.2.0";
  m.Formats = {1, 0, 0, 2, kCodeNone, 0};
  m.RuntimeAbi = 7;
  m.Target = {8, 8, 1, 64, 64};
  m.Libraries.push_back(
      {static_cast<uint8_t>(LibKind::Runtime), "paykan_runtime", "", 7, 0});
  return m;
}

inline Interface sampleInterface() {
  Interface i;
  i.Module = "geometry::shapes";
  i.Mods.push_back({"geometry::vec", false, hashOf("vec iface"), {}});
  i.Mods.push_back({"::io", true, hashOf("io iface"), {}});
  i.Functions.push_back({"area", "float", {"Shape"}, {}});
  // The constructor's and __init__'s second parameter is `h: view int`.
  ModeRecs viewH{{kModeValue, kModeView}, {"w", "h"}};
  i.Functions.push_back({"Shape", "Shape", {"int", "int"}, viewH});
  ClassRec shape;
  shape.Name = "Shape";
  shape.SuperClassName = "";
  shape.OriginModule = "geometry::shapes";
  shape.Fields = {{"w", "int"}, {"h", "int"}};
  shape.Methods = {{"area", "float", {}, 0, {}},
                   {"__scale", "void", {"float"}, 1, {{kModeInout}, {"by"}}},
                   {"__init__", "void", {"int", "int"}, 0, viewH}};
  ClassRec vec;
  vec.Name = "Vec";
  vec.SuperClassName = "Shape";
  vec.IsLocal = false;
  vec.OriginModule = "geometry::vec";
  i.Classes = {vec, shape};
  i.Enums.push_back({"Color", {"Red", "Green"}, true, "geometry::shapes"});
  i.DisplayFile = "geometry/shapes.pkn";
  return i;
}

inline std::vector<uint8_t> bytesOf(const char *s) {
  return std::vector<uint8_t>(s, s + std::strlen(s));
}

/// A container with MANIFEST, IFACE, CODE, SYMIDX and two payloads.
inline std::vector<uint8_t> sampleFileBytes() {
  Writer w;
  Manifest m = sampleManifest();
  Interface i = sampleInterface();
  w.add(Kind::Payload, kFlagInstanced, bytesOf("payload one"));
  w.add(Kind::Code, kFlagRequired, bytesOf("binary pir bytes here"));
  w.add(Kind::Manifest, kFlagRequired, encodeManifest(m));
  w.add(Kind::Iface, kFlagRequired, writeInterface(i));
  w.add(Kind::SymIdx, 0, bytesOf("symidx"));
  w.add(Kind::Payload, kFlagInstanced, bytesOf("payload two, longer"));
  StatusOr<std::vector<uint8_t>> out = w.finish();
  EXPECT_TRUE(out) << out.status().message();
  return out ? *out : std::vector<uint8_t>();
}

inline std::vector<uint8_t> minimalFileBytes(const std::string &name = "m") {
  Writer w;
  w.add(Kind::Manifest, kFlagRequired, encodeManifest(minimalManifest(name)));
  Interface i;
  i.Module = name;
  i.System = name.starts_with("::");
  w.add(Kind::Iface, kFlagRequired, writeInterface(i));
  StatusOr<std::vector<uint8_t>> out = w.finish();
  EXPECT_TRUE(out) << out.status().message();
  return out ? *out : std::vector<uint8_t>();
}

// Little-endian patching of the header and table (docs/design/pkm.md §1.2,
// §1.3); the table hash is recomputed by rehash() unless the test wants it
// wrong.
inline void putU32(std::vector<uint8_t> &b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
inline void putU64(std::vector<uint8_t> &b, size_t at, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
inline uint32_t getU32(const std::vector<uint8_t> &b, size_t at) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i)
    v |= uint32_t(b[at + i]) << (8 * i);
  return v;
}
inline uint64_t getU64(const std::vector<uint8_t> &b, size_t at) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v |= uint64_t(b[at + i]) << (8 * i);
  return v;
}
inline size_t entryAt(size_t index) { return 64 + 64 * index; }
inline std::vector<uint8_t> vec(std::span<const uint8_t> s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

/// Recomputes the table hash after a table edit.
inline void rehash(std::vector<uint8_t> &b) {
  uint32_t n = getU32(b, 16);
  support::Hash256 h =
      support::sha256(std::span<const uint8_t>(b.data() + 64, size_t(n) * 64));
  std::memcpy(b.data() + 32, h.data(), 32);
}

/// The structured error of a failed read; fails the test when the read
/// succeeds.
inline Error readError(const std::vector<uint8_t> &b,
                       const ReadOptions &opts = {}) {
  Error e;
  StatusOr<File> f = File::read(b, opts, &e);
  EXPECT_FALSE(f);
  return e;
}

} // namespace pkmtest
