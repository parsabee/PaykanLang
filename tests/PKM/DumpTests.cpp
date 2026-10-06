// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// `pkm dump` goldens (docs/design/pkm.md §8.5): the text is stable, so a
// change here is a deliberate format change.

#include "PKMTestUtils.h"

#include "paykan/pkm/Dump.h"

#include <sstream>

using namespace pkmtest;

namespace {

std::string dumpOf(const std::vector<uint8_t> &b, DumpOptions opts = {}) {
  StatusOr<File> f = File::read(b);
  EXPECT_TRUE(f) << f.status().message();
  std::ostringstream os;
  if (f)
    dump(*f, os, opts);
  return os.str();
}

const char *const kGolden =
    R"(pkm 1.0, 6 sections, table sha256 d5d88a6dd8fca423
iface_hash b941837c1ce30969fc0b3b62d51ce1420309a6d68958c0d6cb69982b448d2cdc
tmpl_hash 0000000000000000000000000000000000000000000000000000000000000000
code_hash 31635559fa224a2c41eac6c6a0ecf6e9b08ee0747deaf38cbf2f08365f54f4c6
module_hash 265b6785a489d1a82dd6ce1d22a3e22a42a48b7dd31e875ff473ec935aa2b705
sections:
  #0 MANIFEST flags REQUIRED offset 0x1c0 size 430 sha256 cd1968da24a98197
  #1 IFACE flags REQUIRED offset 0x370 size 584 sha256 b941837c1ce30969
  #2 CODE flags REQUIRED offset 0x5b8 size 21 sha256 31635559fa224a2c
  #3 SYMIDX flags - offset 0x5d0 size 6 sha256 12643e6ba7fdc1f1
  #4 PAYLOAD flags INSTANCED offset 0x5d8 size 11 sha256 18e9d2416680ac3c
  #5 PAYLOAD flags INSTANCED offset 0x5e8 size 19 sha256 6d8555986ae05822
manifest:
  module geometry::shapes
  contents 0x1 (HAS_CODE)
  core "0.2.0-rc1" 0.2.0 prerelease "rc1" build "abc123"
  format_versions iface 1.0 tmpl 1 pir 2 code_encoding 2 debug 1
  runtime_abi 7
  target pointer 8 slot 8 endianness 1 int 64 float 64
  deps 2
    ::io flags 0x1 iface d15e8a8021cbf6f9 tmpl 0000000000000000 core "0.2.0-rc1"
    geometry::vec flags 0x2 iface 204c017c008cfc0e tmpl 9656a97f47735794 core "0.2.0-rc1"
  source sha256 41cf6794ba4200b839c53531555f0f3998df4cbb01a4d5cb0b94e3ca5e23947d size 1234
  libraries 1
    RUNTIME "paykan_runtime" version "0.2.0" abi 7 flags 0x1
  frontend "recursive-descent" "0.2.0" plugin ""
  producer "paykan" "0.2.0-rc1"
  plugin_api 3
  opt_pipeline ""
  attributes 2 "@inline" "@cold"
iface:
  module geometry::shapes
  display_file "geometry/shapes.pkn"
  deps 2
    ::io system iface d15e8a8021cbf6f9 tmpl 0000000000000000
    geometry::vec iface 204c017c008cfc0e tmpl 0000000000000000
  enum Color [local from "geometry::shapes"] { Red, Green }
  class Shape super "" [local from "geometry::shapes"]
    field w: int
    field h: int
    method area() -> float flags 0x0
    method __scale(float) -> void flags 0x1
    method __init__(int, int) -> void flags 0x0
  class Vec super "Shape" [imported from "geometry::vec"]
  func Shape(int, int) -> Shape
  func area(Shape) -> float
tmpl: absent
code: 21 B, sha256 31635559fa224a2c41eac6c6a0ecf6e9b08ee0747deaf38cbf2f08365f54f4c6
symidx: 6 B, sha256 12643e6ba7fdc1f1e76eb68141e5d02904f35ea9e64b4094bcd2ada3e44e990f
debug: absent
payloads 2
  kind 0x0210 11 B (not decoded)
  kind 0x0210 19 B (not decoded)
)";

TEST(Dump, Golden) {
  std::string text = dumpOf(sampleFileBytes());
  EXPECT_EQ(text, kGolden);
  EXPECT_EQ(dumpOf(sampleFileBytes()), text);
}

TEST(Dump, Sections) {
  std::vector<uint8_t> b = sampleFileBytes();
  std::string all = dumpOf(b);
  for (DumpOptions::Section s :
       {DumpOptions::Section::Manifest, DumpOptions::Section::Sections,
        DumpOptions::Section::Iface, DumpOptions::Section::SymIdx,
        DumpOptions::Section::Payloads}) {
    DumpOptions o;
    o.Which = s;
    std::string part = dumpOf(b, o);
    EXPECT_FALSE(part.empty());
    EXPECT_NE(all.find(part), std::string::npos) << part;
  }
  DumpOptions o;
  o.Which = DumpOptions::Section::Payloads;
  EXPECT_EQ(dumpOf(b, o), "payloads 2\n"
                          "  kind 0x0210 11 B (not decoded)\n"
                          "  kind 0x0210 19 B (not decoded)\n");
}

TEST(Dump, CorruptInterfaceIsReported) {
  Writer w;
  w.add(Kind::Manifest, kFlagRequired, encodeManifest(minimalManifest()));
  w.add(Kind::Iface, kFlagRequired, bytesOf("PKMI garbage"));
  DumpOptions o;
  o.Which = DumpOptions::Section::Iface;
  EXPECT_EQ(dumpOf(*w.finish(), o).substr(0, 15), "iface: corrupt:");
}

} // namespace
