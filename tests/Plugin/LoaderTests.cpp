// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The plugin loader, end to end: C plugins (tests/Plugin/modules) loaded
// into the real paykan through every discovery path, the checks that reject
// a plugin before any of its callbacks run, and the host table a backend
// uses (docs/plugins/overview.md).
//
// Every test runs paykan in a scratch directory with its own $HOME and none
// of the caller's plugin settings, so the developer's plugins never leak in.
// The test plugins record which of their code ran in
// $PAYKAN_TEST_MARKER_DIR: <name>.ctor from a global constructor (the
// platform's loader runs it: the documented guarantee is that nothing ELSE
// runs before the check) and <name>.called from every callback.

#include <gtest/gtest.h>

#include "Version.h"
#include "paykan/plugin_api.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#ifndef PAYKAN_BIN
#error "PAYKAN_BIN must be defined via CMake"
#endif

namespace fs = std::filesystem;

namespace {

constexpr size_t kPluginStructSize = sizeof(PaykanPlugin);
constexpr size_t kBackendStructSize = sizeof(PaykanBackend);

const std::string kPaykan = PAYKAN_BIN;
const std::string kPluginDir = PAYKAN_TEST_PLUGIN_DIR;
const std::string kSuffix = PAYKAN_TEST_PLUGIN_SUFFIX;
const std::string kSample =
    std::string(PAYKAN_SAMPLES_DIR) + "/codegen/01_literals.pkn";

/// The test module lib<name><suffix> in <plugins>/<subdir>.
std::string plugin(const std::string &subdir, const std::string &name) {
  return kPluginDir + "/" + subdir + "/lib" + name + kSuffix;
}
std::string pluginDir(const std::string &subdir) {
  return kPluginDir + "/" + subdir;
}

std::string incompatible() {
  return std::string(
             "incompatible: built with PaykanLang 0.0.9; this paykan ") +
         paykan::kVersion + " accepts " + PAYKAN_TEST_ACCEPTED_VERSIONS;
}

std::string slurp(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::string> lines(const std::string &out) {
  std::vector<std::string> result;
  std::istringstream in(out);
  for (std::string line; std::getline(in, line);)
    result.push_back(line);
  return result;
}

bool hasLine(const std::string &out, const std::string &line) {
  auto ls = lines(out);
  return std::find(ls.begin(), ls.end(), line) != ls.end();
}

struct Result {
  int Code = -1;
  std::string Out;
  std::string Err;
};

/// Single-quote @p s for the shell.
std::string quote(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  return out + "'";
}

class PluginLoader : public ::testing::Test {
protected:
  void SetUp() override {
    static int counter = 0;
    Scratch = fs::temp_directory_path() /
              ("paykan_loader_" + std::to_string(getpid()) + "_" +
               std::to_string(counter++));
    fs::remove_all(Scratch);
    fs::create_directories(Home());
    fs::create_directories(Markers());
    fs::create_directories(Cwd());
  }
  void TearDown() override { fs::remove_all(Scratch); }

  fs::path Home() const { return Scratch / "home"; }
  fs::path Markers() const { return Scratch / "markers"; }
  fs::path Cwd() const { return Scratch / "cwd"; }
  std::string UserDir() const {
    return (Home() / ".paykan" / "plugins" / paykan::kVersion).string();
  }

  bool ran(const std::string &name, const char *what) const {
    return fs::exists(Markers() / (name + what));
  }

  /// paykan @p args (shell words) with @p env (NAME=value words) in the
  /// scratch environment, from the scratch working directory.
  Result paykan(const std::string &args, const std::string &env = "",
                const std::string &bin = kPaykan) const {
    fs::path err = Scratch / "stderr.txt";
    std::string cmd = "cd " + quote(Cwd().string()) +
                      " && env -u PAYKAN_NO_PLUGINS -u PAYKAN_PLUGIN_PATH "
                      "-u PAYKAN_PLUGIN_DEBUG -u PAYKAN_TEST_PLUGIN_FAIL "
                      "HOME=" +
                      quote(Home().string()) +
                      " PAYKAN_TEST_MARKER_DIR=" + quote(Markers().string()) +
                      " " + env + " " + quote(bin) + " " + args + " 2>" +
                      quote(err.string());
    Result r;
    FILE *fp = popen(cmd.c_str(), "r");
    if (!fp)
      return r;
    std::array<char, 4096> buf{};
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), fp)) > 0)
      r.Out.append(buf.data(), n);
    int status = pclose(fp);
    r.Code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    r.Err = slurp(err);
    return r;
  }

  fs::path Scratch;
};

std::string flag(const std::string &name, const std::string &value) {
  return "--" + name + "=" + quote(value);
}

// -- Loading
// -------------------------------------------------------------------

TEST_F(PluginLoader, ExplicitPluginIsListedWithItsFile) {
  std::string good = plugin("good", "good");
  auto r = paykan("--no-plugins " + flag("plugin", good) + " --list-backends");
  ASSERT_EQ(r.Code, 0) << r.Err;
  EXPECT_TRUE(
      hasLine(r.Out, "test-good: the loader tests' backend [" + good + "]"))
      << r.Out;
  EXPECT_TRUE(hasLine(r.Out, "c (default)")) << r.Out;
  // Listing reads the descriptor only.
  EXPECT_TRUE(ran("test-good", ".ctor"));
  EXPECT_FALSE(ran("test-good", ".called"));

  auto v = paykan("--no-plugins " + flag("plugin", good) + " --version");
  ASSERT_EQ(v.Code, 0) << v.Err;
  EXPECT_TRUE(hasLine(v.Out, std::string("backend test-good (built with "
                                         "PaykanLang ") +
                                 paykan::kVersion +
                                 ", compatible): the loader tests' backend [" +
                                 good + "]"))
      << v.Out;
  EXPECT_TRUE(hasLine(v.Out, "plugin " + good +
                                 " (test-good-plugin 0.1): backend test-good"))
      << v.Out;
  EXPECT_TRUE(hasLine(v.Out, "plugin API 1")) << v.Out;
  EXPECT_FALSE(ran("test-good", ".called"));
}

TEST_F(PluginLoader, BackendGetsTheProgramAsPIRText) {
  std::string good = plugin("good", "good");
  auto viaPlugin =
      paykan("--no-plugins " + flag("plugin", good) +
             " --backend=test-good --emit-source " + quote(kSample));
  ASSERT_EQ(viaPlugin.Code, 0) << viaPlugin.Err;
  auto pir = paykan("--no-plugins --emit-pir " + quote(kSample));
  ASSERT_EQ(pir.Code, 0) << pir.Err;
  EXPECT_FALSE(pir.Out.empty());
  EXPECT_EQ(viaPlugin.Out, pir.Out);
  EXPECT_TRUE(ran("test-good", ".called"));
}

// Built against a newer header of the same plugin API: its structs have a
// field appended.  paykan reads what it knows and steps through the backend
// array by the plugin's struct_size.
TEST_F(PluginLoader, AppendedFieldsAreCompatible) {
  std::string future = plugin("future", "future");
  auto l =
      paykan("--no-plugins " + flag("plugin", future) + " --list-backends");
  ASSERT_EQ(l.Code, 0) << l.Err;
  for (const char *name : {"test-future-1", "test-future-2"})
    EXPECT_TRUE(hasLine(l.Out, std::string(name) +
                                   ": a newer header's backend [" + future +
                                   "]"))
        << l.Out;
  auto r = paykan("--no-plugins " + flag("plugin", future) +
                  " --backend=test-future-2 --emit-source " + quote(kSample));
  EXPECT_EQ(r.Code, 0) << r.Err;
  auto pir = paykan("--no-plugins --emit-pir " + quote(kSample));
  EXPECT_EQ(r.Out, pir.Out);
}

TEST_F(PluginLoader, BackendBuildsAndRunsThroughTheHost) {
  std::string good = plugin("good", "good");
  std::string exe = (Scratch / "prog").string();
  auto b = paykan("--no-plugins " + flag("plugin", good) +
                  " --backend=test-good build -o " + quote(exe) + " " +
                  quote(kSample));
  ASSERT_EQ(b.Code, 0) << b.Err;
  ASSERT_TRUE(fs::exists(exe));
  auto direct = paykan("x", "", exe);
  EXPECT_EQ(direct.Code, 2) << direct.Err;
  EXPECT_TRUE(hasLine(direct.Out, "arg 1: x")) << direct.Out;

  auto r = paykan("--no-plugins " + flag("plugin", good) +
                  " --backend=test-good run " + quote(kSample) + " a b");
  EXPECT_EQ(r.Code, 3) << r.Err; // argc
  EXPECT_EQ(r.Out,
            "arg 0: " + kSample + "\narg 1: a\narg 2: b\ntrack-heap: off\n");
  auto t = paykan("--no-plugins " + flag("plugin", good) +
                  " --backend=test-good --track-heap run " + quote(kSample));
  EXPECT_EQ(t.Code, 1) << t.Err;
  EXPECT_TRUE(hasLine(t.Out, "track-heap: 1")) << t.Out;
}

TEST_F(PluginLoader, BackendDiagnosticsAndFailures) {
  std::string good = plugin("good", "good");
  std::string cmd = "--no-plugins " + flag("plugin", good) +
                    " --backend=test-good --emit-source " + quote(kSample);
  auto r = paykan(cmd, "PAYKAN_TEST_PLUGIN_FAIL=diag");
  EXPECT_EQ(r.Code, 1);
  EXPECT_EQ(r.Out, "");
  EXPECT_EQ(r.Err, kSample + ":2: warning: a warning first\n" + kSample +
                       ":3:7: error: test failure requested\n"
                       "other.pkn: error: and a second error\n");
  auto silent = paykan(cmd, "PAYKAN_TEST_PLUGIN_FAIL=silent");
  EXPECT_EQ(silent.Code, 1);
  EXPECT_EQ(silent.Err, "backend 'test-good' failed (status 5)\n");
  // host->log: debug messages only with PAYKAN_PLUGIN_DEBUG.
  auto quiet = paykan(cmd);
  EXPECT_EQ(quiet.Code, 0) << quiet.Err;
  EXPECT_EQ(quiet.Err, "");
  auto debug = paykan(cmd, "PAYKAN_PLUGIN_DEBUG=1");
  EXPECT_EQ(debug.Code, 0) << debug.Err;
  EXPECT_EQ(debug.Err, "paykan: plugin: debug from the test plugin\n");
}

TEST_F(PluginLoader, PluginPathIsSearchedButNeverTheWorkingDirectory) {
  // A plugin in the working directory: an empty $PAYKAN_PLUGIN_PATH entry
  // does not mean ".", and nothing else searches it.
  fs::copy_file(plugin("good", "good"), Cwd() / ("libcwd" + kSuffix));
  std::string dir = pluginDir("good");
  auto r = paykan("--list-backends",
                  "PAYKAN_PLUGIN_PATH=" + quote("::" + dir + ":" + dir + ":"));
  ASSERT_EQ(r.Code, 0) << r.Err;
  // Listed once, from the path (the same directory twice loads it once).
  EXPECT_TRUE(hasLine(r.Out, "test-good: the loader tests' backend [" +
                                 plugin("good", "good") + "]"))
      << r.Out;
  EXPECT_EQ(r.Out.find("ambiguous"), std::string::npos) << r.Out;
  EXPECT_EQ(r.Out.find("libcwd"), std::string::npos) << r.Out;

  auto v = paykan("--version", "PAYKAN_PLUGIN_PATH=" + quote(":" + dir));
  ASSERT_EQ(v.Code, 0) << v.Err;
  std::string dirs = "plugin directories: " + dir + " " + UserDir() + " ";
  bool found = false;
  for (const std::string &l : lines(v.Out))
    found |= l.rfind(dirs, 0) == 0;
  EXPECT_TRUE(found) << v.Out;
}

TEST_F(PluginLoader, UserDirectoryIsSearched) {
  fs::create_directories(UserDir());
  fs::path copy = fs::path(UserDir()) / ("libgood" + kSuffix);
  fs::copy_file(plugin("good", "good"), copy);
  // Files without the plugin suffix are not candidates.
  std::ofstream(fs::path(UserDir()) / "README.txt") << "not a plugin\n";
  auto r = paykan("--list-backends");
  ASSERT_EQ(r.Code, 0) << r.Err;
  EXPECT_TRUE(hasLine(r.Out, "test-good: the loader tests' backend [" +
                                 copy.string() + "]"))
      << r.Out;
  EXPECT_EQ(r.Out.find("rejected"), std::string::npos) << r.Out;
}

TEST_F(PluginLoader, SystemDirectoryIsFoundRelativeToTheExecutable) {
  // An "installed" paykan: <prefix>/bin/paykan and the plugin in
  // <prefix>/lib/paykan/plugins/<version>.
  fs::path bin = Scratch / "prefix" / "bin";
  fs::create_directories(bin);
  fs::copy_file(kPaykan, bin / "paykan");
  fs::path sys = (bin / PAYKAN_TEST_PLUGIN_DIR_FROM_BINDIR).lexically_normal();
  fs::create_directories(sys);
  fs::copy_file(plugin("good", "good"), sys / ("libgood" + kSuffix));
  auto r = paykan("--list-backends", "", (bin / "paykan").string());
  ASSERT_EQ(r.Code, 0) << r.Err;
  EXPECT_TRUE(hasLine(r.Out, "test-good: the loader tests' backend [" +
                                 (sys / ("libgood" + kSuffix)).string() + "]"))
      << r.Out;
  auto v = paykan("--version", "", (bin / "paykan").string());
  EXPECT_TRUE(
      hasLine(v.Out, "plugin directories: " + UserDir() + " " + sys.string()))
      << v.Out;
}

TEST_F(PluginLoader, NoPluginsTurnsDiscoveryOff) {
  std::string path = "PAYKAN_PLUGIN_PATH=" + quote(pluginDir("good"));
  for (const auto &[args, env] :
       std::vector<std::pair<std::string, std::string>>{
           {"--no-plugins --list-backends", path},
           {"-no-plugins --list-backends", path},
           {"--list-backends", path + " PAYKAN_NO_PLUGINS=1"}}) {
    auto r = paykan(args, env);
    ASSERT_EQ(r.Code, 0) << r.Err;
    EXPECT_EQ(r.Out.find("test-good"), std::string::npos) << args << r.Out;
  }
  // Nothing was even loaded.
  EXPECT_FALSE(ran("test-good", ".ctor"));
  auto v = paykan("--version", path + " PAYKAN_NO_PLUGINS=1");
  EXPECT_TRUE(hasLine(v.Out, "plugin directories: none searched (--no-plugins "
                             "or PAYKAN_NO_PLUGINS)"))
      << v.Out;

  // PAYKAN_NO_PLUGINS=0 (or empty) does not disable it.
  for (const char *value : {"0", "''"}) {
    auto r = paykan("--list-backends",
                    path + " PAYKAN_NO_PLUGINS=" + std::string(value));
    EXPECT_NE(r.Out.find("test-good"), std::string::npos) << value << r.Out;
  }
  // --plugin files are still loaded.
  auto r = paykan("--no-plugins " + flag("plugin", plugin("good", "good")) +
                  " --list-backends");
  EXPECT_NE(r.Out.find("test-good"), std::string::npos) << r.Out;
}

TEST_F(PluginLoader, HelpMentionsThePluginOptions) {
  auto r = paykan("--help");
  ASSERT_EQ(r.Code, 0);
  EXPECT_NE(r.Out.find("--plugin=<file>"), std::string::npos) << r.Out;
  EXPECT_NE(r.Out.find("--no-plugins"), std::string::npos) << r.Out;
}

// -- Rejection
// -----------------------------------------------------------------

TEST_F(PluginLoader, WrongBuildVersionIsIncompatibleAndNeverCalled) {
  std::string old = plugin("old", "old");
  std::string reason = incompatible();
  auto l = paykan("--no-plugins " + flag("plugin", old) + " --list-backends");
  ASSERT_EQ(l.Code, 0) << l.Err;
  EXPECT_TRUE(hasLine(l.Out, "test-old (" + reason + ") [" + old + "]"))
      << l.Out;
  auto v = paykan("--no-plugins " + flag("plugin", old) + " --version");
  EXPECT_TRUE(hasLine(v.Out, "backend test-old (" + reason + ") [" + old + "]"))
      << v.Out;

  // Selected: exit 2 with the reason, through --plugin and the path.
  const std::string expected =
      "paykan: cannot use backend 'test-old' (" + reason + ") [" + old + "]\n";
  for (const auto &[args, env] :
       std::vector<std::pair<std::string, std::string>>{
           {"--no-plugins " + flag("plugin", old), ""},
           {"", "PAYKAN_PLUGIN_PATH=" + quote(pluginDir("old"))}}) {
    for (const std::string action :
         {"--emit-source", "--check-only", "build", "run"}) {
      std::string cmd = args;
      cmd += " --backend=test-old ";
      cmd += action;
      cmd += " " + quote(kSample);
      auto r = paykan(cmd, env);
      EXPECT_EQ(r.Code, 2) << action << r.Err;
      EXPECT_EQ(r.Err, expected);
      EXPECT_EQ(r.Out, "");
    }
  }
  // The loader ran its global constructor (as the platform's loader always
  // does) and read its descriptor, but called none of its code.
  EXPECT_TRUE(ran("test-old", ".ctor"));
  EXPECT_FALSE(ran("test-old", ".called"));
}

TEST_F(PluginLoader, WrongPluginAPIIsRejected) {
  std::string api = plugin("api", "api");
  std::string why =
      "built for plugin API 999; this paykan supports plugin API 1";
  auto l = paykan("--no-plugins " + flag("plugin", api) + " --list-backends");
  ASSERT_EQ(l.Code, 0) << l.Err;
  EXPECT_TRUE(hasLine(l.Out, "rejected plugin " + api + ": " + why)) << l.Out;
  EXPECT_EQ(l.Out.find("test-api"), std::string::npos) << l.Out;
  auto fe = paykan("--no-plugins " + flag("plugin", api) + " --list-frontends");
  EXPECT_TRUE(hasLine(fe.Out, "rejected plugin " + api + ": " + why)) << fe.Out;
  auto v = paykan("--no-plugins " + flag("plugin", api) + " --version");
  EXPECT_TRUE(hasLine(v.Out, "rejected plugin " + api + ": " + why)) << v.Out;

  // Named with --plugin, a rejected file stops a compile (exit 2) ...
  auto r = paykan("--no-plugins " + flag("plugin", api) + " --check-only " +
                  quote(kSample));
  EXPECT_EQ(r.Code, 2);
  EXPECT_EQ(r.Err, "paykan: cannot load plugin '" + api + "': " + why + "\n");
  // ... found in a directory, it only leaves its names unknown.
  auto d = paykan("--backend=test-api --emit-source " + quote(kSample),
                  "PAYKAN_PLUGIN_PATH=" + quote(pluginDir("api")));
  EXPECT_EQ(d.Code, 1);
  EXPECT_EQ(d.Err, "paykan: unknown backend 'test-api' (see --list-backends; "
                   "1 plugin file was rejected)\n");
  auto ok = paykan("--check-only " + quote(kSample),
                   "PAYKAN_PLUGIN_PATH=" + quote(pluginDir("api")));
  EXPECT_EQ(ok.Code, 0) << ok.Err;
  EXPECT_FALSE(ran("test-api", ".called"));
}

TEST_F(PluginLoader, BrokenFilesAreRejectedWithTheReason) {
  struct Case {
    std::string Path;
    std::string Why; // the expected reason, or its start ("...")
  };
  const Case cases[] = {
      {plugin("null", "null"),
       "paykan_plugin_init returned no plugin descriptor"},
      {plugin("noentry", "noentry"),
       "no paykan_plugin_init entry point (not a PaykanLang plugin)"},
      {plugin("noemit", "noemit"),
       "invalid descriptor: backend 'test-noemit' has no emit callback"},
      {plugin("norun", "norun"),
       "invalid descriptor: backend 'test-norun' can run programs but has "
       "no run callback"},
      {plugin("noname", "noname"),
       "invalid descriptor: backend #1 has no name"},
      {plugin("twice", "twice"),
       "invalid descriptor: backend 'test-twice' is listed twice"},
      {plugin("none", "none"), "invalid descriptor: it provides no backend"},
      {plugin("tiny", "tiny"),
       "its descriptor is 4 bytes, too small to hold a plugin API version"},
      {plugin("short", "short"),
       "its descriptor is 16 bytes, plugin API 1 needs " +
           std::to_string(kPluginStructSize)},
      {plugin("bshort", "bshort"),
       "invalid descriptor: backend #1: its descriptor is 8 bytes, plugin API "
       "1 needs " +
           std::to_string(kBackendStructSize)},
      {plugin("corrupt", "corrupt"), "cannot load it: ..."},
      {pluginDir("good") + "/libmissing" + kSuffix, "no such file"},
  };
  for (const Case &c : cases) {
    auto l =
        paykan("--no-plugins " + flag("plugin", c.Path) + " --list-backends");
    ASSERT_EQ(l.Code, 0) << c.Path << l.Err;
    std::string head = "rejected plugin " + c.Path + ": ";
    if (c.Why.ends_with("...")) {
      std::string start = head + c.Why.substr(0, c.Why.size() - 3);
      bool found = false;
      for (const std::string &line : lines(l.Out))
        found |= line.rfind(start, 0) == 0 && line.size() > start.size();
      EXPECT_TRUE(found) << l.Out;
    } else {
      EXPECT_TRUE(hasLine(l.Out, head + c.Why)) << l.Out;
    }
    auto r = paykan("--no-plugins " + flag("plugin", c.Path) +
                    " --check-only " + quote(kSample));
    EXPECT_EQ(r.Code, 2) << c.Path;
    EXPECT_EQ(r.Err.rfind("paykan: cannot load plugin '" + c.Path + "': ", 0),
              0u)
        << r.Err;
  }
  // A library without the entry point is loaded (so its constructors ran)
  // and then left alone.
  EXPECT_TRUE(ran("test-noentry", ".ctor"));
  for (const char *name :
       {"test-null", "test-noentry", "test-noemit", "test-norun", "test-noname",
        "test-twice", "test-none", "test-tiny", "test-short", "test-bshort"})
    EXPECT_FALSE(ran(name, ".called")) << name;
}

TEST_F(PluginLoader, DuplicateNamesAreAmbiguous) {
  std::string a = plugin("dup", "dup_a"), b = plugin("dup", "dup_b");
  std::string conflict = "provided by both " + a + " and " + b;
  auto l = paykan("--list-backends",
                  "PAYKAN_PLUGIN_PATH=" + quote(pluginDir("dup")));
  ASSERT_EQ(l.Code, 0) << l.Err;
  EXPECT_TRUE(hasLine(l.Out, "dup (ambiguous: " + conflict + ") [" + a + "]"))
      << l.Out;
  auto v = paykan("--version", "PAYKAN_PLUGIN_PATH=" + quote(pluginDir("dup")));
  EXPECT_TRUE(
      hasLine(v.Out, "backend dup (ambiguous: " + conflict + ") [" + a + "]"))
      << v.Out;
  auto r = paykan("--backend=dup --emit-source " + quote(kSample),
                  "PAYKAN_PLUGIN_PATH=" + quote(pluginDir("dup")));
  EXPECT_EQ(r.Code, 2);
  EXPECT_EQ(r.Err,
            "paykan: cannot use backend 'dup' (ambiguous: " + conflict + ")\n");
  EXPECT_FALSE(ran("dup", ".called"));
  // A --plugin file comes first; the one in the path is the second provider.
  auto e = paykan(flag("plugin", b) + " --list-backends",
                  "PAYKAN_PLUGIN_PATH=" + quote(pluginDir("dup")));
  EXPECT_TRUE(hasLine(e.Out, "dup (ambiguous: provided by both " + b + " and " +
                                 a + ") [" + b + "]"))
      << e.Out;
}

TEST_F(PluginLoader, ClashWithABuiltInIsAmbiguous) {
  std::string clash = plugin("clash", "clash");
  std::string conflict = "provided by both the built-in plugin and " + clash;
  auto l = paykan("--no-plugins " + flag("plugin", clash) + " --list-backends");
  ASSERT_EQ(l.Code, 0) << l.Err;
  EXPECT_TRUE(hasLine(l.Out, "c (ambiguous: " + conflict + ")")) << l.Out;
  // The default backend is c: running anything is now an error.
  auto r = paykan("--no-plugins " + flag("plugin", clash) + " --emit-source " +
                  quote(kSample));
  EXPECT_EQ(r.Code, 2);
  EXPECT_EQ(r.Err,
            "paykan: cannot use backend 'c' (ambiguous: " + conflict + ")\n");
  EXPECT_FALSE(ran("c", ".called"));
}

} // namespace
