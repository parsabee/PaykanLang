// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema::ModuleInfo <-> pkm::Interface: the exports of a module and their
// .pkm form carry the same declarations, so a module loaded from a file is
// injected exactly like one built in process.

#include "Sema.h"
#include "TestUtils.h"
#include "paykan/pkm/Interface.h"

#include <gtest/gtest.h>

using paykan::sema::Sema;

namespace {

Sema::ModuleInfo sampleInfo() {
  Sema::ModuleInfo info;
  Sema::ModuleInfo::ClassInfo shape;
  shape.Name = "Shape";
  shape.OriginModule = "geometry::shapes";
  shape.Fields = {{"w", "int"}, {"h", "int"}};
  shape.Methods = {{"area", "float", {}, 0},
                   {"__scale", "void", {"float"}, 1},
                   {"__init__", "void", {"int", "int"}, 0}};
  Sema::ModuleInfo::ClassInfo vec;
  vec.Name = "Vec";
  vec.SuperClassName = "Shape";
  vec.IsLocal = false;
  vec.OriginModule = "geometry::vec";
  info.ExportedClasses = {shape, vec};
  info.ExportedEnums = {{"Color", {"Red", "Green"}, true, "geometry::shapes"}};
  info.ExportedFunctions = {{"Shape", "Shape", {"int", "int"}},
                            {"area", "float", {"Shape"}}};
  return info;
}

TEST(InterfaceConversion, RoundTripIsExact) {
  Sema::ModuleInfo info = sampleInfo();
  paykan::pkm::Interface iface = Sema::toInterface(info);
  EXPECT_EQ(iface.Classes.size(), 2u);
  EXPECT_EQ(iface.Classes[0].Methods[1].Flags, 1);
  EXPECT_EQ(iface.Enums[0].Variants,
            (std::vector<std::string>{"Red", "Green"}));
  EXPECT_EQ(Sema::fromInterface(iface), info);
}

// Through the codec: the file sorts records by name, fromInterface restores
// the export order (the module's own types first, then by name).
TEST(InterfaceConversion, RoundTripThroughTheCodec) {
  Sema::ModuleInfo info = sampleInfo();
  paykan::pkm::Interface iface = Sema::toInterface(info);
  iface.Module = "geometry::shapes";
  auto back = paykan::pkm::readInterface(paykan::pkm::writeInterface(iface),
                                         "geometry::shapes");
  ASSERT_TRUE(back) << back.status().message();
  EXPECT_EQ(Sema::fromInterface(*back), info);
}

} // namespace
