// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The plugin registry's compatibility check (#103): fake plugins registered
// as built with an accepted and with a rejected PaykanLang version.  The
// rejected one is listed with the reason, can't be created, and its factory
// never runs.

#include <gtest/gtest.h>

#include "paykan/PluginCompat.h"
#include "paykan/Registry.h"

#include <memory>
#include <string>
#include <vector>

#ifndef PAYKAN_TEST_ACCEPTED_VERSIONS
#error "PAYKAN_TEST_ACCEPTED_VERSIONS must be defined via CMake"
#endif

namespace {

using paykan::plugin::PluginInfo;
using paykan::plugin::Registration;

/// A plugin interface of its own, so the fakes get a registry of their own.
struct FakeInterface {
  virtual ~FakeInterface() = default;
};
struct FakePlugin : FakeInterface {};

using FakeRegistry = paykan::plugin::Registry<FakeInterface>;

int CurrentCreated = 0;
int OldCreated = 0;

std::unique_ptr<FakeInterface> createCurrent() {
  ++CurrentCreated;
  return std::make_unique<FakePlugin>();
}
std::unique_ptr<FakeInterface> createOld() {
  ++OldCreated;
  return std::make_unique<FakePlugin>();
}

// Registered from static initialisers, like real plugins: one built with
// these headers' version, one built with a version this toolchain does not
// accept, and one that recorded no version at all.
const Registration<FakeInterface> current(PluginInfo<FakeInterface>{
    "current", &createCurrent, PAYKAN_PLUGIN_BUILD_VERSION});
const Registration<FakeInterface> old(PluginInfo<FakeInterface>{
    "old", &createOld, "0.0.9"});
const Registration<FakeInterface> unknown(PluginInfo<FakeInterface>{
    "unknown", &createOld, nullptr});

const std::string kAccepted = PAYKAN_TEST_ACCEPTED_VERSIONS;

} // namespace

TEST(PluginCompat, TheToolchainAcceptsItsOwnVersion) {
  EXPECT_EQ(paykan::plugin::toolchainVersion(), PAYKAN_PLUGIN_BUILD_VERSION);
  EXPECT_TRUE(
      paykan::plugin::isCompatibleBuildVersion(PAYKAN_PLUGIN_BUILD_VERSION));
  std::string joined;
  for (const char *v : paykan::plugin::compatibleBuildVersions())
    joined += (joined.empty() ? "" : ", ") + std::string(v);
  EXPECT_EQ(joined, kAccepted);
}

// Versions match as exact strings, pre-release label included.
TEST(PluginCompat, VersionsMatchExactly) {
  using paykan::plugin::isCompatibleBuildVersion;
  std::string v = PAYKAN_PLUGIN_BUILD_VERSION;
  EXPECT_FALSE(isCompatibleBuildVersion(nullptr));
  EXPECT_FALSE(isCompatibleBuildVersion(""));
  EXPECT_FALSE(isCompatibleBuildVersion((v + "x").c_str()));
  EXPECT_FALSE(isCompatibleBuildVersion((" " + v).c_str()));
  EXPECT_FALSE(isCompatibleBuildVersion(v.substr(0, v.size() - 1).c_str()));
  if (auto dash = v.find('-'); dash != std::string::npos) {
    // "0.1.0-alpha" does not accept "0.1.0".
    EXPECT_FALSE(isCompatibleBuildVersion(v.substr(0, dash).c_str()));
  } else {
    EXPECT_FALSE(isCompatibleBuildVersion((v + "-alpha").c_str()));
  }
}

TEST(PluginCompat, ReasonNamesBothVersionsAndTheList) {
  EXPECT_EQ(paykan::plugin::incompatibilityReason("0.0.9"),
            "built with PaykanLang 0.0.9; this paykan " +
                std::string(PAYKAN_PLUGIN_BUILD_VERSION) + " accepts " +
                kAccepted);
  EXPECT_EQ(paykan::plugin::incompatibilityReason(nullptr),
            "built with an unknown PaykanLang version; this paykan " +
                std::string(PAYKAN_PLUGIN_BUILD_VERSION) + " accepts " +
                kAccepted);
}

TEST(PluginRegistry, EveryPluginIsListedWithItsCompatibility) {
  const auto &entries = FakeRegistry::get().entries();
  ASSERT_EQ(entries.size(), 3u);
  // Sorted by name.
  EXPECT_EQ(entries[0].Name, "current");
  EXPECT_EQ(entries[1].Name, "old");
  EXPECT_EQ(entries[2].Name, "unknown");
  EXPECT_EQ(FakeRegistry::get().names(),
            (std::vector<std::string>{"current", "old", "unknown"}));

  EXPECT_TRUE(entries[0].Compatible);
  EXPECT_EQ(entries[0].BuildVersion, PAYKAN_PLUGIN_BUILD_VERSION);
  EXPECT_EQ(entries[0].Incompatibility, "");

  EXPECT_FALSE(entries[1].Compatible);
  EXPECT_EQ(entries[1].BuildVersion, "0.0.9");
  EXPECT_EQ(entries[1].Incompatibility,
            "built with PaykanLang 0.0.9; this paykan " +
                std::string(PAYKAN_PLUGIN_BUILD_VERSION) + " accepts " +
                kAccepted);

  EXPECT_FALSE(entries[2].Compatible);
  EXPECT_EQ(entries[2].BuildVersion, "");
}

TEST(PluginRegistry, AnIncompatiblePluginIsNeverInstantiated) {
  auto &reg = FakeRegistry::get();
  ASSERT_NE(reg.find("old"), nullptr);
  EXPECT_EQ(reg.create("old"), nullptr);
  EXPECT_EQ(reg.create("unknown"), nullptr);
  EXPECT_EQ(OldCreated, 0);

  int before = CurrentCreated;
  EXPECT_NE(reg.create("current"), nullptr);
  EXPECT_EQ(CurrentCreated, before + 1);
  EXPECT_EQ(reg.create("missing"), nullptr);
}

TEST(PluginRegistry, TheFirstRegistrationOfANameWins) {
  auto &reg = FakeRegistry::get();
  // A compatible plugin can't replace an incompatible one of the same name,
  // nor the other way round.
  EXPECT_FALSE(reg.add(PluginInfo<FakeInterface>{"old", &createCurrent,
                                                 PAYKAN_PLUGIN_BUILD_VERSION}));
  EXPECT_FALSE(
      reg.add(PluginInfo<FakeInterface>{"current", &createOld, "0.0.9"}));
  EXPECT_FALSE(reg.find("old")->Compatible);
  EXPECT_TRUE(reg.find("current")->Compatible);
  EXPECT_EQ(reg.entries().size(), 3u);
}

// -- Loaded plugins: entries the plugin loader registers ----------------------

namespace {
/// Another interface, so these entries don't disturb the ones above.
struct LoadedInterface {
  virtual ~LoadedInterface() = default;
};
struct LoadedPlugin : LoadedInterface {};
using LoadedRegistry = paykan::plugin::Registry<LoadedInterface>;
} // namespace

TEST(PluginRegistry, LoadedEntriesCarryTheirFileAndCanConflict) {
  auto &reg = LoadedRegistry::get();
  int created = 0;
  LoadedRegistry::Entry e;
  e.Name = "loaded";
  e.Create = [&created] {
    ++created;
    return std::make_unique<LoadedPlugin>();
  };
  e.BuildVersion = PAYKAN_PLUGIN_BUILD_VERSION;
  e.Compatible = true;
  e.Path = "/plugins/a.so";
  e.Description = "from a";
  ASSERT_TRUE(reg.add(e));
  EXPECT_FALSE(reg.add(e)); // the name is taken
  EXPECT_NE(reg.create("loaded"), nullptr);
  EXPECT_EQ(created, 1);
  EXPECT_EQ(reg.find("loaded")->Path, "/plugins/a.so");
  EXPECT_EQ(reg.find("loaded")->Description, "from a");

  // A compatible entry without a factory (an incompatible plugin's, say)
  // is never created.
  LoadedRegistry::Entry bare;
  bare.Name = "bare";
  bare.Compatible = true;
  bare.BuildVersion = PAYKAN_PLUGIN_BUILD_VERSION;
  ASSERT_TRUE(reg.add(bare));
  EXPECT_EQ(reg.create("bare"), nullptr);

  // A second provider makes the name ambiguous: listed, never created.
  EXPECT_FALSE(reg.markConflict("missing", "/plugins/x.so"));
  ASSERT_TRUE(reg.markConflict("loaded", "/plugins/b.so"));
  EXPECT_EQ(reg.find("loaded")->Conflict,
            "provided by both /plugins/a.so and /plugins/b.so");
  ASSERT_TRUE(reg.markConflict("loaded", "/plugins/c.so"));
  EXPECT_EQ(reg.find("loaded")->Conflict,
            "provided by both /plugins/a.so and /plugins/b.so and "
            "/plugins/c.so");
  EXPECT_EQ(reg.create("loaded"), nullptr);
  EXPECT_EQ(created, 1);
  // A built-in one is named as such.
  ASSERT_TRUE(reg.markConflict("bare", "/plugins/d.so"));
  EXPECT_EQ(reg.find("bare")->Conflict,
            "provided by both the built-in plugin and /plugins/d.so");
}
