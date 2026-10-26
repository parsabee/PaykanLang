// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// The .pkm module system through the driver (docs/design/pkm.md §8): the
// per-project cache (written, reused, deterministic, invalidated by body and
// signature edits, rebuilt when corrupt, --rebuild-modules,
// --no-module-cache), prebuilt modules (source removed, --module-path,
// $PAYKAN_MODULE_PATH, the §8.2 rejection), --emit-pkm and `paykan pkm
// dump|check`.  PAYKAN_BIN names the driver; PAYKAN_TEST_BACKEND the
// backend (ctest runs the suite once per backend).

#include <gtest/gtest.h>

#include "BuildId.h"
#include "Version.h"
#include "paykan/pkm/File.h"
#include "paykan/pkm/Interface.h"
#include "paykan/pkm/Manifest.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#ifndef PAYKAN_BIN
#error "PAYKAN_BIN must be defined via CMake compile definition"
#endif

namespace {

namespace fs = std::filesystem;

const char *kPaykan = PAYKAN_BIN;

struct CmdResult {
  int exitCode;
  std::string out; // stdout and stderr
};

CmdResult run(const std::string &cmd) {
  std::array<char, 4096> buf{};
  std::string out;
  FILE *fp = popen((cmd + " 2>&1").c_str(), "r");
  if (!fp)
    return {-1, ""};
  while (fgets(buf.data(), buf.size(), fp))
    out += buf.data();
  int rc = pclose(fp); // a macro on macOS: needs an lvalue
  return {WEXITSTATUS(rc), out};
}

bool hasBackend() {
  static const bool has = [] {
    auto r = run(std::string(kPaykan) + " --list-backends");
    return r.exitCode == 0 && !r.out.empty();
  }();
  return has;
}

#define REQUIRE_BACKEND()                                                      \
  do {                                                                         \
    if (!hasBackend())                                                         \
      GTEST_SKIP() << "this build has no backend";                             \
  } while (0)

std::string backendFlag(const std::string &backend) {
  return backend.empty() ? "" : " --backend=" + backend;
}

/// The paykan command on the test backend (PAYKAN_TEST_BACKEND, or the
/// build's default).
std::string paykanCmd() {
  const char *env = std::getenv("PAYKAN_TEST_BACKEND");
  return std::string(kPaykan) + backendFlag(env ? env : "");
}

/// Every backend `--list-backends` names that can run programs.
std::vector<std::string> runnableBackends() {
  std::vector<std::string> out;
  auto r = run(std::string(kPaykan) + " --list-backends");
  size_t pos = 0;
  while (pos < r.out.size()) {
    size_t end = r.out.find('\n', pos);
    std::string line = r.out.substr(pos, end - pos);
    pos = end == std::string::npos ? r.out.size() : end + 1;
    std::string name = line.substr(0, line.find_first_of(" :"));
    if (name == "mlir" && line.find("incompatible") == std::string::npos)
      out.push_back(name);
  }
  return out;
}

void writeFile(const fs::path &path, const std::string &content) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << content;
}

std::string slurp(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

/// A fresh project directory for one test: `main` imports `mid`, which
/// imports `base` (a class, an enum and a function).
struct Project {
  fs::path Dir;
  explicit Project(const std::string &name) {
    Dir = fs::temp_directory_path() /
          ("pkm_" + name + "_" + std::to_string(getpid()));
    fs::remove_all(Dir);
    writeBase("1");
    writeFile(Dir / "mid.pkn",
              "import base;\n"
              "fn mid() -> int { return base::base() + 1; }\n"
              "fn pick(c: base::Cell) -> int { return c.v; }\n");
    writeFile(Dir / "main.pkn",
              "import ::io; import mid;\nimport base;\n"
              "fn main() -> int {\n"
              "  c: base::Cell = base::Cell(40);\n"
              "  io::println(Str<int>(mid::mid()));\n"
              "  io::println(Str<int>(mid::pick(c) + c.twice()));\n"
              "  return 0;\n}\n");
  }
  ~Project() { fs::remove_all(Dir); }
  void writeBase(const std::string &value, const std::string &extra = "") {
    writeFile(Dir / "base.pkn", "enum Kind { Odd, Even }\n"
                                "class Cell { v: int;\n"
                                "  fn twice() -> int { return self.v * 2; } }\n"
                                "impl Cell {\n"
                                "  fn __init__(v: int) { self.v = v; } }\n"
                                "fn base() -> int { return " +
                                    value + "; }\n" + extra);
  }
  fs::path cache() const { return Dir / ".paykan_cache"; }
  fs::path entry(const std::string &module) const {
    return cache() / (module + ".pkm");
  }
  /// `paykan [flags] main.pkn` from the project directory.
  CmdResult runMain(const std::string &flags = "",
                    const std::string &cmd = "") const {
    return run("cd " + Dir.string() + " && " +
               (cmd.empty() ? paykanCmd() : cmd) + " " + flags + " main.pkn");
  }
  /// The .pkm cache entries (the backends keep their own files beside them).
  std::vector<std::string> pkmEntries() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto &e : fs::recursive_directory_iterator(cache(), ec))
      if (e.path().extension() == ".pkm")
        out.push_back(e.path().lexically_relative(cache()).string());
    return out;
  }
};

const char *kExpected = "2\n120\n";

/// Rewrite the manifest of the .pkm file at @p path with @p change.
void rewriteManifest(
    const fs::path &path,
    const std::function<void(paykan::pkm::Manifest &)> &change) {
  std::string bytes = slurp(path);
  auto file = paykan::pkm::File::read(std::span<const uint8_t>(
      reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()));
  ASSERT_TRUE(file) << file.status().message();
  paykan::pkm::Manifest m = (*file).manifest();
  change(m);
  paykan::pkm::Writer w;
  for (const auto &e : (*file).sections()) {
    auto s = (*file).section(static_cast<paykan::pkm::Kind>(e.Kind));
    if (e.Kind == static_cast<uint32_t>(paykan::pkm::Kind::Manifest))
      w.add(e.Kind, e.Flags, paykan::pkm::encodeManifest(m));
    else
      w.add(e.Kind, e.Flags, std::vector<uint8_t>(s.begin(), s.end()));
  }
  auto patched = w.finish();
  ASSERT_TRUE(patched) << patched.status().message();
  writeFile(path, std::string((*patched).begin(), (*patched).end()));
}

/// The core.build the .pkm file at @p path records.
std::string coreBuildOf(const fs::path &path) {
  std::string bytes = slurp(path);
  auto file = paykan::pkm::File::read(std::span<const uint8_t>(
      reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()));
  return file ? (*file).manifest().Core.Build : "<unreadable>";
}

bool contains(const std::string &text, const std::string &what) {
  return text.find(what) != std::string::npos;
}

/// Whether `--verbose` output @p out reports module @p module built from
/// source.
bool builtFromSource(const std::string &out, const std::string &module) {
  std::istringstream lines(out);
  for (std::string line; std::getline(lines, line);)
    if (line.rfind("module " + module + ":", 0) == 0 &&
        line.find("building from source") != std::string::npos)
      return true;
  return false;
}

} // namespace

// -- The cache

TEST(Pkm, CacheIsWrittenOnTheFirstBuildAndReusedOnTheSecond) {
  REQUIRE_BACKEND();
  Project p("reuse");
  auto first = p.runMain("--verbose");
  ASSERT_EQ(first.exitCode, 0) << first.out;
  EXPECT_TRUE(contains(first.out, kExpected)) << first.out;
  EXPECT_TRUE(
      contains(first.out, "module base: no cache entry; building from source"))
      << first.out;
  EXPECT_TRUE(contains(first.out, "module base: wrote .paykan_cache/base.pkm"))
      << first.out;
  EXPECT_TRUE(contains(first.out, "module mid: wrote .paykan_cache/mid.pkm"))
      << first.out;
  EXPECT_TRUE(fs::is_regular_file(p.entry("base")));
  EXPECT_TRUE(fs::is_regular_file(p.entry("mid")));
  // The main module is never cached.
  EXPECT_FALSE(fs::exists(p.entry("main")));

  auto second = p.runMain("--verbose");
  ASSERT_EQ(second.exitCode, 0) << second.out;
  EXPECT_TRUE(contains(second.out, kExpected)) << second.out;
  EXPECT_TRUE(
      contains(second.out, "module base: cache .paykan_cache/base.pkm usable"))
      << second.out;
  EXPECT_TRUE(
      contains(second.out, "module mid: cache .paykan_cache/mid.pkm usable"))
      << second.out;
  EXPECT_FALSE(contains(second.out, "wrote")) << second.out;
  // (The standard library's modules are prebuilt .pkm files, read and never
  // written here.)
  EXPECT_FALSE(builtFromSource(second.out, "base")) << second.out;
  EXPECT_FALSE(builtFromSource(second.out, "mid")) << second.out;
}

TEST(Pkm, CacheEntriesAreByteIdenticalAcrossColdBuilds) {
  REQUIRE_BACKEND();
  Project p("determinism");
  ASSERT_EQ(p.runMain().exitCode, 0);
  std::string base = slurp(p.entry("base")), mid = slurp(p.entry("mid"));
  ASSERT_FALSE(base.empty());
  fs::remove_all(p.cache());
  ASSERT_EQ(p.runMain().exitCode, 0);
  EXPECT_EQ(slurp(p.entry("base")), base);
  EXPECT_EQ(slurp(p.entry("mid")), mid);
  // A file carries no path of the project it was built in.
  EXPECT_FALSE(contains(base, p.Dir.string()));
}

/// A cache entry records the build ID of the paykan that wrote it, and
/// another build of the same version rebuilds it (#188): its lowered code
/// may differ although the version string does not.
TEST(Pkm, CacheEntryOfAnotherBuildIsRebuilt) {
  REQUIRE_BACKEND();
  Project p("build_id");
  auto first = p.runMain();
  ASSERT_EQ(first.exitCode, 0) << first.out;
  EXPECT_EQ(coreBuildOf(p.entry("base")), paykan::kBuildId);
  rewriteManifest(p.entry("base"),
                  [](paykan::pkm::Manifest &m) { m.Core.Build = "elsewhere"; });
  auto second = p.runMain("--verbose");
  ASSERT_EQ(second.exitCode, 0) << second.out;
  EXPECT_TRUE(contains(second.out, kExpected)) << second.out;
  EXPECT_TRUE(contains(second.out, "module base: cache .paykan_cache/base.pkm "
                                   "is stale (compiled by paykan "))
      << second.out;
  EXPECT_TRUE(contains(second.out, " (elsewhere); this is paykan "))
      << second.out;
  EXPECT_TRUE(contains(second.out, "module base: wrote .paykan_cache/base.pkm"))
      << second.out;
  EXPECT_TRUE(
      contains(second.out, "module mid: cache .paykan_cache/mid.pkm usable"))
      << second.out;
  EXPECT_EQ(coreBuildOf(p.entry("base")), paykan::kBuildId);
}

/// A development build takes a prebuilt module of its own build only: two
/// builds of one version may lay out what the runtime reads differently (a
/// type's PaykanType did, within 0.3.0-dev), so another's file is an error
/// naming both builds, not a program that misreads it.  Its own is used.
TEST(Pkm, PrebuiltModuleOfAnotherDevelopmentBuildIsRejected) {
  REQUIRE_BACKEND();
  Project p("build_id_prebuilt");
  ASSERT_EQ(run("cd " + p.Dir.string() + " && " + paykanCmd() +
                " --emit-pkm base.pkn")
                .exitCode,
            0);
  fs::remove(p.Dir / "base.pkn");
  auto own = p.runMain();
  ASSERT_EQ(own.exitCode, 0) << own.out;
  EXPECT_TRUE(contains(own.out, kExpected)) << own.out;
  if (std::string(paykan::kBuildId).empty())
    GTEST_SKIP() << "a release build takes another build's prebuilt files";
  rewriteManifest(p.Dir / "base.pkm",
                  [](paykan::pkm::Manifest &m) { m.Core.Build = "elsewhere"; });
  fs::remove_all(p.cache());
  auto other = p.runMain();
  EXPECT_NE(other.exitCode, 0) << other.out;
  EXPECT_FALSE(contains(other.out, kExpected)) << other.out;
  EXPECT_TRUE(contains(other.out, "module 'base' (base.pkm) was compiled by "
                                  "paykan "))
      << other.out;
  EXPECT_TRUE(contains(other.out, "); build ID elsewhere; this paykan is "
                                  "development build " +
                                      std::string(paykan::kBuildId) +
                                      " and takes only files of its own "
                                      "build. Rebuild it from source."))
      << other.out;
}

TEST(Pkm, BodyEditRebuildsThatModuleOnly) {
  REQUIRE_BACKEND();
  Project p("bodyedit");
  ASSERT_EQ(p.runMain().exitCode, 0);
  p.writeBase("5");
  auto r = p.runMain("--verbose");
  ASSERT_EQ(r.exitCode, 0) << r.out;
  EXPECT_TRUE(contains(r.out, "6\n120\n")) << r.out;
  EXPECT_TRUE(contains(r.out, "module base: cache .paykan_cache/base.pkm is "
                              "stale (the source changed); building from "
                              "source"))
      << r.out;
  EXPECT_TRUE(contains(r.out, "module mid: cache .paykan_cache/mid.pkm usable"))
      << r.out;
  EXPECT_TRUE(contains(r.out, "module base: wrote")) << r.out;
  EXPECT_FALSE(contains(r.out, "module mid: wrote")) << r.out;
}

TEST(Pkm, SignatureEditRebuildsTheImporter) {
  REQUIRE_BACKEND();
  Project p("sigedit");
  ASSERT_EQ(p.runMain().exitCode, 0);
  p.writeBase("1", "fn extra() -> int { return 9; }\n");
  auto r = p.runMain("--verbose");
  ASSERT_EQ(r.exitCode, 0) << r.out;
  EXPECT_TRUE(contains(r.out, kExpected)) << r.out;
  EXPECT_TRUE(contains(r.out, "module base: cache .paykan_cache/base.pkm is "
                              "stale (the source changed)"))
      << r.out;
  EXPECT_TRUE(contains(r.out, "module mid: cache .paykan_cache/mid.pkm is "
                              "stale (the interface of module 'base' "
                              "changed); building from source"))
      << r.out;
  // Now both entries are fresh again.
  auto again = p.runMain("--verbose");
  EXPECT_FALSE(builtFromSource(again.out, "base")) << again.out;
  EXPECT_FALSE(builtFromSource(again.out, "mid")) << again.out;
}

TEST(Pkm, CorruptCacheEntryIsRebuilt) {
  REQUIRE_BACKEND();
  Project p("corrupt");
  ASSERT_EQ(p.runMain().exitCode, 0);
  std::string bytes = slurp(p.entry("base"));
  ASSERT_GT(bytes.size(), 200u);
  bytes[bytes.size() / 2] ^= 0x5a;
  writeFile(p.entry("base"), bytes);
  auto r = p.runMain("--verbose");
  ASSERT_EQ(r.exitCode, 0) << r.out;
  EXPECT_TRUE(contains(r.out, kExpected)) << r.out;
  EXPECT_TRUE(contains(r.out, "module base: cache .paykan_cache/base.pkm is "
                              "corrupt"))
      << r.out;
  EXPECT_TRUE(contains(r.out, "module base: wrote")) << r.out;
  // Garbage in place of the file is handled the same way.
  writeFile(p.entry("mid"), "not a module file at all");
  r = p.runMain("--verbose");
  ASSERT_EQ(r.exitCode, 0) << r.out;
  EXPECT_TRUE(contains(r.out, "module mid: cache .paykan_cache/mid.pkm is "
                              "corrupt"))
      << r.out;
  EXPECT_TRUE(contains(r.out, "module base: cache .paykan_cache/base.pkm "
                              "usable"))
      << r.out;
}

TEST(Pkm, RebuildModulesIgnoresTheEntries) {
  REQUIRE_BACKEND();
  Project p("rebuild");
  ASSERT_EQ(p.runMain().exitCode, 0);
  auto r = p.runMain("--rebuild-modules --verbose");
  ASSERT_EQ(r.exitCode, 0) << r.out;
  EXPECT_TRUE(contains(r.out, kExpected)) << r.out;
  EXPECT_TRUE(contains(r.out, "module base: cache not read; building from "
                              "source"))
      << r.out;
  EXPECT_TRUE(contains(r.out, "module mid: cache not read; building from "
                              "source"))
      << r.out;
  EXPECT_TRUE(contains(r.out, "module base: wrote")) << r.out;
  EXPECT_TRUE(contains(r.out, "module mid: wrote")) << r.out;
}

TEST(Pkm, NoModuleCacheNeverReadsOrWrites) {
  REQUIRE_BACKEND();
  Project p("nocache");
  auto r = p.runMain("--no-module-cache --verbose");
  ASSERT_EQ(r.exitCode, 0) << r.out;
  EXPECT_TRUE(contains(r.out, kExpected)) << r.out;
  EXPECT_TRUE(contains(r.out, "module base: cache not read")) << r.out;
  EXPECT_FALSE(contains(r.out, "wrote")) << r.out;
  EXPECT_TRUE(p.pkmEntries().empty());
  ASSERT_EQ(p.runMain().exitCode, 0);
  EXPECT_EQ(p.pkmEntries().size(), 2u);
  r = p.runMain("--no-module-cache --verbose");
  ASSERT_EQ(r.exitCode, 0) << r.out;
  EXPECT_TRUE(contains(r.out, "module base: cache not read")) << r.out;
}

// A cache entry is written from moved code (docs/pir.md §12; pkm.md §5.13):
// the run that builds the module counts the moves at last use made in its
// code, and the run that reuses the entry makes none of them again, with
// the same result and nothing leaked.
TEST(Pkm, ACacheEntryIsMovedCode) {
  REQUIRE_BACKEND();
  Project p("moves");
  writeFile(p.Dir / "base.pkn",
            "class Cell { v: int; }\n"
            "fn pass<T>(x: T) -> T { y = x; return y; }\n"
            "fn pick(c: Cell) -> int { return c.v; }\n"
            "fn give(c: Cell) -> int { return pick(pass(c)); }\n");
  writeFile(p.Dir / "main.pkn",
            "import base;\n"
            "fn main() -> int { return base::give(base::Cell(7)); }\n");
  auto first = p.runMain("--stats --track-heap");
  EXPECT_EQ(first.exitCode, 7) << first.out;
  EXPECT_TRUE(contains(first.out, "  moves-at-last-use transfers : 3\n"))
      << first.out;
  EXPECT_TRUE(contains(first.out, "live blocks       : 0\n")) << first.out;
  ASSERT_TRUE(fs::is_regular_file(p.entry("base")));
  auto second = p.runMain("--stats --track-heap");
  EXPECT_EQ(second.exitCode, 7) << second.out;
  EXPECT_TRUE(contains(second.out, "  moves-at-last-use transfers : 0\n"))
      << second.out;
  EXPECT_TRUE(contains(second.out, "live blocks       : 0\n")) << second.out;
}

// -- Prebuilt modules

/// `--emit-pkm base.pkn` then the source removed: the program builds and
/// runs from the prebuilt file on every backend, with the same output.
TEST(Pkm, PrebuiltModuleReplacesItsSourceOnEveryBackend) {
  REQUIRE_BACKEND();
  Project p("prebuilt");
  auto emit = run("cd " + p.Dir.string() + " && " + paykanCmd() +
                  " --emit-pkm base.pkn");
  ASSERT_EQ(emit.exitCode, 0) << emit.out;
  ASSERT_TRUE(fs::is_regular_file(p.Dir / "base.pkm"));
  fs::remove(p.Dir / "base.pkn");
  fs::remove_all(p.cache());
  for (const std::string &be : runnableBackends()) {
    fs::remove_all(p.cache());
    std::string cmd = std::string(kPaykan) + backendFlag(be);
    auto r = p.runMain("--verbose", cmd);
    ASSERT_EQ(r.exitCode, 0) << be << ": " << r.out;
    EXPECT_TRUE(contains(r.out, kExpected)) << be << ": " << r.out;
    EXPECT_TRUE(contains(r.out, "module base: prebuilt base.pkm usable"))
        << be << ": " << r.out;
    // `mid` is built from source (and cached) against the prebuilt base.
    EXPECT_TRUE(contains(r.out, "module mid: wrote")) << be << ": " << r.out;
    auto warm = p.runMain("", cmd);
    ASSERT_EQ(warm.exitCode, 0) << be << ": " << warm.out;
    EXPECT_EQ(warm.out, kExpected) << be;
    auto exe = p.Dir / ("prog-" + be);
    auto b = run("cd " + p.Dir.string() + " && " + cmd + " build -o " +
                 exe.string() + " main.pkn");
    ASSERT_EQ(b.exitCode, 0) << be << ": " << b.out;
    auto ran = run(exe.string());
    EXPECT_EQ(ran.exitCode, 0) << be;
    EXPECT_EQ(ran.out, kExpected) << be;
  }
  // The prebuilt file is a module of its own: no cache entry is written for
  // it, and a stale cache entry does not shadow it.
  EXPECT_FALSE(fs::exists(p.entry("base")));
}

TEST(Pkm, PrebuiltModulesAreFoundOnTheModulePath) {
  REQUIRE_BACKEND();
  Project p("modulepath");
  ASSERT_EQ(run("cd " + p.Dir.string() + " && " + paykanCmd() +
                " --emit-pkm base.pkn -o lib/base.pkm")
                .exitCode,
            0);
  fs::remove(p.Dir / "base.pkn");
  auto missing = p.runMain();
  EXPECT_NE(missing.exitCode, 0);
  EXPECT_TRUE(contains(missing.out,
                       "module 'base' not found (tried base.pkn, base.pkm)"))
      << missing.out;
  auto r = p.runMain("--module-path=lib --verbose");
  ASSERT_EQ(r.exitCode, 0) << r.out;
  EXPECT_TRUE(contains(r.out, kExpected)) << r.out;
  EXPECT_TRUE(contains(r.out, "module base: prebuilt lib/base.pkm usable"))
      << r.out;
  auto env =
      run("cd " + p.Dir.string() + " && PAYKAN_MODULE_PATH=nowhere:lib " +
          paykanCmd() + " main.pkn");
  ASSERT_EQ(env.exitCode, 0) << env.out;
  EXPECT_EQ(env.out, kExpected);
}

/// A prebuilt file written for another runtime ABI (the manifest rewritten,
/// the other sections kept) is rejected with the §8.2 diagnostic.
TEST(Pkm, PrebuiltModuleForAnotherToolchainIsRejected) {
  REQUIRE_BACKEND();
  Project p("rejected");
  ASSERT_EQ(run("cd " + p.Dir.string() + " && " + paykanCmd() +
                " --emit-pkm base.pkn")
                .exitCode,
            0);
  std::string bytes = slurp(p.Dir / "base.pkm");
  auto file = paykan::pkm::File::read(std::span<const uint8_t>(
      reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()));
  ASSERT_TRUE(file) << file.status().message();
  paykan::pkm::Manifest m = (*file).manifest();
  m.RuntimeAbi += 1;
  for (auto &lib : m.Libraries)
    lib.Abi = m.RuntimeAbi;
  paykan::pkm::Writer w;
  for (const auto &e : (*file).sections()) {
    auto bytesOf = [&](paykan::pkm::Kind k) {
      auto s = (*file).section(k);
      return std::vector<uint8_t>(s.begin(), s.end());
    };
    if (e.Kind == static_cast<uint32_t>(paykan::pkm::Kind::Manifest))
      w.add(e.Kind, e.Flags, paykan::pkm::encodeManifest(m));
    else
      w.add(e.Kind, e.Flags, bytesOf(static_cast<paykan::pkm::Kind>(e.Kind)));
  }
  auto patched = w.finish();
  ASSERT_TRUE(patched) << patched.status().message();
  writeFile(p.Dir / "base.pkm",
            std::string((*patched).begin(), (*patched).end()));
  fs::remove(p.Dir / "base.pkn");
  auto r = p.runMain();
  EXPECT_NE(r.exitCode, 0);
  EXPECT_TRUE(contains(r.out, "error: module 'base' (base.pkm) was compiled "
                              "by paykan "))
      << r.out;
  EXPECT_TRUE(contains(r.out, "runtime ABI")) << r.out;
  EXPECT_TRUE(contains(r.out, "Rebuild it from source.")) << r.out;
  auto check =
      run(std::string(kPaykan) + " pkm check " + (p.Dir / "base.pkm").string());
  EXPECT_EQ(check.exitCode, 1);
  EXPECT_TRUE(contains(check.out, "rejected: runtime ABI")) << check.out;
}

/// A prebuilt importer built against another interface of a module whose
/// source is present is an error, not a silent mismatch.
TEST(Pkm, PrebuiltModuleAgainstAnotherInterfaceIsRejected) {
  REQUIRE_BACKEND();
  Project p("depmismatch");
  ASSERT_EQ(
      run("cd " + p.Dir.string() + " && " + paykanCmd() + " --emit-pkm mid.pkn")
          .exitCode,
      0);
  fs::remove(p.Dir / "mid.pkn");
  ASSERT_EQ(p.runMain().exitCode, 0);
  p.writeBase("1", "fn extra() -> int { return 9; }\n");
  auto r = p.runMain();
  EXPECT_NE(r.exitCode, 0);
  EXPECT_TRUE(contains(r.out, "error: module 'mid' (mid.pkm) was built "
                              "against another interface of module 'base'. "
                              "Rebuild it from source."))
      << r.out;
}

// -- --emit-pkm, pkm dump, pkm check

TEST(Pkm, EmitPkmAndDump) {
  REQUIRE_BACKEND();
  Project p("dump");
  fs::create_directories(p.Dir / "geometry");
  writeFile(p.Dir / "geometry" / "shapes.pkn",
            "class Rect { w: int; h: int;\n"
            "  fn area() -> int { return self.w * self.h; } }\n"
            "impl Rect {\n"
            "  fn __init__(w: int, h: int) { self.w = w; self.h = h; } }\n"
            "fn describe(r: Rect) -> Str { return \"rect\"; }\n");
  // A nested module named by its path from the project root.
  auto emit = run("cd " + p.Dir.string() + " && " + paykanCmd() +
                  " --emit-pkm geometry/shapes.pkn -o out/shapes.pkm");
  ASSERT_EQ(emit.exitCode, 0) << emit.out;
  auto dump = run(std::string(kPaykan) + " pkm dump " +
                  (p.Dir / "out" / "shapes.pkm").string());
  ASSERT_EQ(dump.exitCode, 0) << dump.out;
  const std::string header =
      "pkm " + paykan::versionString(paykan::kVersionNumber) + ", 5 sections";
  EXPECT_TRUE(contains(dump.out, header)) << dump.out;
  // Its code compiled (OBJ), CODE declaring it.
  for (const char *expected :
       {"#0 MANIFEST", "#1 IFACE", "#2 CODE", "#3 SYMIDX", "#4 OBJ",
        "module geometry::shapes", "contents 0x41 (HAS_CODE,HAS_OBJECT)",
        "obj: ", "  backend \"mlir\"\n  format \"llvm-bc\"",
        "display_file \"geometry/shapes.pkn\"", "class Rect super \"\"",
        "method area() -> int", "func describe(Rect) -> Str",
        "func Rect(int, int) -> Rect", "payloads 0", "pir:\n",
        "module \"geometry::shapes\""})
    EXPECT_TRUE(contains(dump.out, expected)) << expected << "\n" << dump.out;
  EXPECT_FALSE(contains(dump.out, p.Dir.string())) << dump.out;
  auto code = run(std::string(kPaykan) + " pkm dump --section=code " +
                  (p.Dir / "out" / "shapes.pkm").string());
  ASSERT_EQ(code.exitCode, 0) << code.out;
  EXPECT_EQ(code.out.rfind("module \"geometry::shapes\"", 0), 0u) << code.out;
  EXPECT_TRUE(contains(code.out, "describe")) << code.out;
  EXPECT_FALSE(contains(code.out, "MANIFEST")) << code.out;
  auto manifest = run(std::string(kPaykan) + " pkm dump --section=manifest " +
                      (p.Dir / "out" / "shapes.pkm").string());
  EXPECT_EQ(manifest.out.rfind("manifest:\n", 0), 0u) << manifest.out;
  EXPECT_FALSE(contains(manifest.out, "iface:")) << manifest.out;
  auto bad = run(std::string(kPaykan) + " pkm dump --section=nope " +
                 (p.Dir / "out" / "shapes.pkm").string());
  EXPECT_NE(bad.exitCode, 0);
  EXPECT_TRUE(contains(bad.out, "unknown section 'nope'")) << bad.out;

  // The main program's own file: HAS_MAIN, its imports as dependencies.
  ASSERT_EQ(p.runMain().exitCode, 0);
  auto emitMain = run("cd " + p.Dir.string() + " && " + paykanCmd() +
                      " --emit-pkm main.pkn");
  ASSERT_EQ(emitMain.exitCode, 0) << emitMain.out;
  auto mainDump = run(std::string(kPaykan) + " pkm dump --section=manifest " +
                      (p.Dir / "main.pkm").string());
  EXPECT_TRUE(contains(mainDump.out, "module main")) << mainDump.out;
  EXPECT_TRUE(contains(mainDump.out, "HAS_MAIN")) << mainDump.out;
  EXPECT_TRUE(contains(mainDump.out, "deps 3")) << mainDump.out; // and ::io
  EXPECT_TRUE(contains(mainDump.out, "    base flags 0x0 iface "))
      << mainDump.out;
  EXPECT_TRUE(contains(mainDump.out, "    mid flags 0x0 iface "))
      << mainDump.out;
}

TEST(Pkm, CheckReportsTheVerdict) {
  REQUIRE_BACKEND();
  Project p("check");
  ASSERT_EQ(run("cd " + p.Dir.string() + " && " + paykanCmd() +
                " --emit-pkm base.pkn")
                .exitCode,
            0);
  auto ok =
      run(std::string(kPaykan) + " pkm check " + (p.Dir / "base.pkm").string());
  EXPECT_EQ(ok.exitCode, 0) << ok.out;
  EXPECT_EQ(ok.out, (p.Dir / "base.pkm").string() + ": usable\n");
  writeFile(p.Dir / "junk.pkm", "PKM?junk");
  auto junk =
      run(std::string(kPaykan) + " pkm check " + (p.Dir / "junk.pkm").string());
  EXPECT_NE(junk.exitCode, 0);
  EXPECT_TRUE(contains(junk.out, "junk.pkm:")) << junk.out;
  auto none = run(std::string(kPaykan) + " pkm check " +
                  (p.Dir / "missing.pkm").string());
  EXPECT_NE(none.exitCode, 0);
  auto noVerb =
      run(std::string(kPaykan) + " pkm " + (p.Dir / "base.pkm").string());
  EXPECT_NE(noVerb.exitCode, 0);
  EXPECT_TRUE(contains(noVerb.out, "unknown pkm command")) << noVerb.out;
}

TEST(Pkm, HelpDocumentsTheFlags) {
  auto r = run(std::string(kPaykan) + " --help");
  ASSERT_EQ(r.exitCode, 0);
  for (const char *flag :
       {"--emit-pkm", "--module-path=<dir>", "--rebuild-modules",
        "--no-module-cache", "--verbose", "pkm dump", "pkm check"})
    EXPECT_TRUE(contains(r.out, flag)) << flag << "\n" << r.out;
}

// -- Parameter modes across modules

namespace {

/// `view` and `inout` parameters in `base`, used and overridden by `main`:
/// a function, a constructor, a method of an imported class called on it
/// directly and through an override.
struct ModesProject {
  fs::path Dir;
  explicit ModesProject(const std::string &name) {
    Dir = fs::temp_directory_path() /
          ("pkm_modes_" + name + "_" + std::to_string(getpid()));
    fs::remove_all(Dir);
    writeFile(Dir / "base.pkn",
              "class Counter { n: int;\n"
              "  fn add(k: view int) -> int { self.n = self.n + k;\n"
              "    return self.n; }\n"
              "  fn take(to: inout int) { to = to + self.n; } }\n"
              "impl Counter {\n"
              "  fn __init__(start: view int) { self.n = start; } }\n"
              "class Cell { v: int; }\n"
              "impl Cell {\n"
              "  fn __init__(v: inout int) { self.v = v; v = 0; } }\n"
              "fn twice(n: view int) -> int { return n * 2; }\n"
              "fn bump(n: inout int) { n = n + 1; }\n");
    writeFile(
        Dir / "main.pkn",
        "import ::io; import base;\n"
        "class Fast : base::Counter {\n"
        "  fn add(k: view int) -> int { self.n = self.n + 10 * k;\n"
        "    return self.n; }\n"
        "  fn take(to: inout int) { to = to * 100; } }\n"
        "impl Fast {\n"
        "  fn __init__(s: view int) { __super__(s); } }\n"
        "fn main() -> int {\n"
        "  c: base::Counter = Fast(1);\n"
        "  plain = base::Counter(5);\n"
        "  k = 2;\n"
        "  base::bump(k);\n"
        "  plain.take(k);\n"
        "  j = 3;\n"
        "  c.take(j);\n"
        "  cell = base::Cell(k);\n"
        "  io::println(Str(c.add(2)) + \" \" + Str(base::twice(3)) + \" \" +\n"
        "          Str(k) + \" \" + Str(j) + \" \" + Str(cell.v));\n"
        "  return 0;\n}\n");
    writeFile(Dir / "bad.pkn", "import base;\n"
                               "class Slow : base::Counter {\n"
                               "  fn add(k: int) -> int { return k; } }\n"
                               "impl Slow {\n"
                               "  fn __init__(s: int) { __super__(s); } }\n"
                               "fn main() -> int { return 0; }\n");
  }
  ~ModesProject() { fs::remove_all(Dir); }
  CmdResult paykan(const std::string &args) const {
    return run("cd " + Dir.string() + " && " + paykanCmd() + " " + args);
  }
};

const char *kModesExpected = "21 6 0 300 8\n";

} // namespace

// The modes survive the source import, the cache entry and a prebuilt file:
// calls and overrides are checked against them, and an `inout` argument is
// passed by address into the other module (also to a method of one of its
// classes, which an importer that dropped the mode would call with a value).
TEST(Pkm, ParamModesAcrossModules) {
  REQUIRE_BACKEND();
  ModesProject p("view");
  auto src = p.paykan("--track-heap main.pkn");
  ASSERT_EQ(src.exitCode, 0) << src.out;
  EXPECT_TRUE(contains(src.out, kModesExpected)) << src.out;
  EXPECT_TRUE(contains(src.out, "live blocks       : 0")) << src.out;
  auto cached = p.paykan("--verbose main.pkn");
  ASSERT_EQ(cached.exitCode, 0) << cached.out;
  EXPECT_TRUE(contains(cached.out, "module base: cache .paykan_cache/base.pkm "
                                   "usable"))
      << cached.out;
  EXPECT_TRUE(contains(cached.out, kModesExpected)) << cached.out;
  const char *kOverride =
      "bad.pkn:3:3: error: override of 'add' must keep 'view' on parameter 'k'";
  auto bad = p.paykan("--check-only bad.pkn");
  EXPECT_NE(bad.exitCode, 0);
  EXPECT_TRUE(contains(bad.out, kOverride)) << bad.out;

  ASSERT_EQ(p.paykan("--emit-pkm base.pkn").exitCode, 0);
  fs::remove(p.Dir / "base.pkn");
  fs::remove_all(p.Dir / ".paykan_cache");
  auto pkm = p.paykan("--track-heap --verbose main.pkn");
  ASSERT_EQ(pkm.exitCode, 0) << pkm.out;
  EXPECT_TRUE(contains(pkm.out, "module base: prebuilt base.pkm usable"))
      << pkm.out;
  EXPECT_TRUE(contains(pkm.out, kModesExpected)) << pkm.out;
  EXPECT_TRUE(contains(pkm.out, "live blocks       : 0")) << pkm.out;
  bad = p.paykan("--check-only bad.pkn");
  EXPECT_NE(bad.exitCode, 0);
  EXPECT_TRUE(contains(bad.out, kOverride)) << bad.out;

  auto dump = p.paykan("pkm dump --section=iface base.pkm");
  for (const char *sig :
       {"func twice(view int) -> int", "func bump(inout int) -> void",
        "func Counter(view int) -> Counter", "func Cell(inout int) -> Cell",
        "method add(view int) -> int flags 0x0",
        "method take(inout int) -> void flags 0x0",
        "method __init__(view int) -> void flags 0x0"})
    EXPECT_TRUE(contains(dump.out, sig)) << sig << "\n" << dump.out;
  auto manifest = p.paykan("pkm dump --section=manifest base.pkm");
  EXPECT_TRUE(
      contains(manifest.out, "format_versions iface " +
                                 paykan::versionString(paykan::kVersionNumber)))
      << manifest.out;
}

// A `view fn` keeps its marker in a prebuilt module (flag 0x2): an importer
// calls it, and an override of it there must be a `view fn` too.
TEST(Pkm, ViewFnAcrossModules) {
  REQUIRE_BACKEND();
  auto dir =
      fs::temp_directory_path() / ("pkm_viewfn_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "base.pkn", "class P { n: int;\n"
                              "  view fn get() -> int { return self.n; }\n"
                              "  fn set(v: int) { self.n = v; } }\n"
                              "impl P { fn __init__() { self.n = 4; } }\n");
  writeFile(dir / "main.pkn", "import ::io; import base;\n"
                              "fn main() -> int {\n"
                              "  let p = base::P();\n"
                              "  io::println(Str(p.get()));\n"
                              "  return 0;\n}\n");
  writeFile(dir / "bad.pkn", "import base;\n"
                             "class Q : base::P {\n"
                             "  fn get() -> int { return 1; } }\n"
                             "impl Q { fn __init__() { __super__(); } }\n"
                             "fn main() -> int { return 0; }\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  ASSERT_EQ(paykan("--emit-pkm base.pkn").exitCode, 0);
  fs::remove(dir / "base.pkn");
  auto ok = paykan("main.pkn");
  ASSERT_EQ(ok.exitCode, 0) << ok.out;
  EXPECT_TRUE(contains(ok.out, "4\n")) << ok.out;
  auto bad = paykan("--check-only bad.pkn");
  EXPECT_NE(bad.exitCode, 0);
  EXPECT_TRUE(contains(bad.out, "bad.pkn:3:3: error: override of 'get' must be "
                                "a 'view fn', like the method it overrides"))
      << bad.out;
  auto dump = paykan("pkm dump --section=iface base.pkm");
  EXPECT_TRUE(contains(dump.out, "method get() -> int flags 0x2")) << dump.out;
  EXPECT_TRUE(contains(dump.out, "method set(int) -> void flags 0x0"))
      << dump.out;
  fs::remove_all(dir);
}

// A module's pure virtual methods are in its `.pkm` (each method's flags
// byte, bit 4): the importer, without the source, works out which classes
// are abstract, implements them in a subclass and runs it, and is refused
// what it may not do with an abstract class.
TEST(Pkm, PureVirtualMethodsAcrossModules) {
  REQUIRE_BACKEND();
  auto dir =
      fs::temp_directory_path() / ("pkm_pure_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "base.pkn",
            "class Shape {\n"
            "  name: Str;\n"
            "  view fn area() -> float;\n"
            "  fn grow();\n"
            "}\n"
            "impl Shape {\n"
            "  fn __init__(name: Str) { self.name = name; }\n"
            "  view fn report() -> Str { return self.name + \" \" + "
            "Str(self.area()); }\n"
            "}\n"
            "class Half : Shape { fn grow() { } }\n");
  writeFile(dir / "main.pkn", "import ::io; import base;\n"
                              "class Square : base::Half {\n"
                              "  side: float;\n"
                              "  view fn area() -> float {\n"
                              "    return self.side * self.side; }\n"
                              "}\n"
                              "fn main() -> int {\n"
                              "  s: base::Shape = Square(\"sq\", 3.0);\n"
                              "  s.grow();\n"
                              "  io::println(s.report());\n"
                              "  return 0;\n}\n");
  writeFile(dir / "bad.pkn", "import base;\n"
                             "fn f(o: Obj) -> int {\n"
                             "  match o { base::Half { return 1; } _ { } }\n"
                             "  return 0;\n}\n"
                             "fn main() -> int {\n"
                             "  h = base::Half(\"h\");\n"
                             "  return 0;\n}\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  ASSERT_EQ(paykan("--emit-pkm base.pkn").exitCode, 0);
  fs::remove(dir / "base.pkn");
  auto ok = paykan("--track-heap main.pkn");
  ASSERT_EQ(ok.exitCode, 0) << ok.out;
  EXPECT_TRUE(contains(ok.out, "sq 9\n")) << ok.out;
  EXPECT_TRUE(contains(ok.out, "live blocks       : 0")) << ok.out;
  auto bad = paykan("--check-only bad.pkn");
  EXPECT_NE(bad.exitCode, 0);
  EXPECT_TRUE(contains(bad.out, "bad.pkn:3:13: error: a 'match' arm cannot "
                                "name abstract class 'Half'"))
      << bad.out;
  EXPECT_TRUE(contains(bad.out, "bad.pkn:7:7: error: cannot instantiate "
                                "abstract class 'Half': 'Shape.area' has no "
                                "implementation"))
      << bad.out;
  // The pure bit is the interface's: a `view fn` one is 0x12, a plain one
  // 0x10, and the subclass's vtable still has the pure `area`.
  auto dump = paykan("pkm dump --section=iface base.pkm");
  EXPECT_TRUE(contains(dump.out, "method area() -> float flags 0x12"))
      << dump.out;
  EXPECT_TRUE(contains(dump.out, "method grow() -> void flags 0x10"))
      << dump.out;
  EXPECT_TRUE(contains(dump.out, "method report() -> Str flags 0x6"))
      << dump.out;
  fs::remove_all(dir);
}

// A class without an `__init__` keeps its default one in a prebuilt module:
// an importer constructs it from its fields and subclasses it.
TEST(Pkm, DefaultInitAcrossModules) {
  REQUIRE_BACKEND();
  auto dir =
      fs::temp_directory_path() / ("pkm_definit_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "geo.pkn", "class Point { x: int; y: int; }\n"
                             "class Named : Point { name: Str; }\n");
  writeFile(
      dir / "main.pkn",
      "import ::io; import geo;\n"
      "class P3 : geo::Point { z: int; }\n"
      "fn main() -> int {\n"
      "  p = geo::Point(1, 2);\n"
      "  n = geo::Named(3, 4, \"n\");\n"
      "  q = P3(5, 6, 7);\n"
      "  io::println(Str(p.x + p.y) + \" \" + n.name + Str(n.y) + \" \" +\n"
      "          Str(q.x + q.y + q.z));\n"
      "  return 0;\n}\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  auto src = paykan("--track-heap main.pkn");
  ASSERT_EQ(src.exitCode, 0) << src.out;
  EXPECT_TRUE(contains(src.out, "3 n4 18\n")) << src.out;
  EXPECT_TRUE(contains(src.out, "live blocks       : 0")) << src.out;
  ASSERT_EQ(paykan("--emit-pkm geo.pkn").exitCode, 0);
  fs::remove(dir / "geo.pkn");
  auto pkm = paykan("--track-heap main.pkn");
  ASSERT_EQ(pkm.exitCode, 0) << pkm.out;
  EXPECT_TRUE(contains(pkm.out, "3 n4 18\n")) << pkm.out;
  EXPECT_TRUE(contains(pkm.out, "live blocks       : 0")) << pkm.out;
  auto dump = paykan("pkm dump --section=iface geo.pkm");
  EXPECT_TRUE(contains(dump.out, "func Named(int, int, Str) -> Named"))
      << dump.out;
  fs::remove_all(dir);
}

// A struct keeps its fields, its `impl` methods (statically dispatched),
// its `static fn`s and its constructor in a prebuilt module: an importer
// constructs it, reads and assigns its fields in place and calls its
// methods, from the source and from the `.pkm` alike.
TEST(Pkm, StructsAcrossModules) {
  REQUIRE_BACKEND();
  auto dir =
      fs::temp_directory_path() / ("pkm_structs_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "geo.pkn",
            "struct Point { x: int; y: int; }\n"
            "impl Point {\n"
            "  fn __init__(x: int, y: int) { self.x = x; self.y = y; }\n"
            "  static fn origin() -> Point { return Point(0, 0); }\n"
            "  view fn sum() -> int { return self.x + self.y; }\n"
            "  fn shift(d: int) { self.x = self.x + d; }\n"
            "}\n"
            "struct Named { p: Point; name: Str; }\n"
            "class Shape { at: Point; view fn where() -> Point { return "
            "self.at; } }\n"
            "fn make(n: int) -> Named { return Named(Point(n, n), \"n\" + "
            "Str(n)); }\n");
  writeFile(dir / "main.pkn",
            "import ::io; import geo;\n"
            "fn main() -> int {\n"
            "  p = geo::Point(1, 2);\n"
            "  p.shift(3);\n"
            "  o = geo::Point::origin();\n"
            "  n = geo::make(4);\n"
            "  n.p.shift(1);\n"
            "  n.name = n.name + \"!\";\n"
            "  s = geo::Shape(p);\n"
            "  q: geo::Point = s.where();\n"
            "  io::println(p.toString() + \" \" + Str(p.sum()) + \" \" + "
            "Str(o == p) + \" \" + n.name + Str(n.p.x) + \" \" + "
            "Str(q.sum()) + \" \" + Str(p == q));\n"
            "  return 0;\n}\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  const std::string expected = "Point(x: 4, y: 2) 6 False n4!5 6 True\n";
  auto src = paykan("--track-heap main.pkn");
  ASSERT_EQ(src.exitCode, 0) << src.out;
  EXPECT_TRUE(contains(src.out, expected)) << src.out;
  EXPECT_TRUE(contains(src.out, "live blocks       : 0")) << src.out;
  ASSERT_EQ(paykan("--emit-pkm geo.pkn").exitCode, 0);
  fs::remove(dir / "geo.pkn");
  auto pkm = paykan("--track-heap main.pkn");
  ASSERT_EQ(pkm.exitCode, 0) << pkm.out;
  EXPECT_TRUE(contains(pkm.out, expected)) << pkm.out;
  EXPECT_TRUE(contains(pkm.out, "live blocks       : 0")) << pkm.out;
  auto dump = paykan("pkm dump --section=iface geo.pkm");
  for (const char *line :
       {"struct Point [local from \"geo\"]", "    field x: int",
        "    method __init__(int, int) -> void flags 0x0",
        "    method sum() -> int flags 0x6",
        "    method shift(int) -> void flags 0x4",
        "struct Named [local from \"geo\"]", "    field name: Str",
        "func Point(int, int) -> Point", "func Point::origin() -> Point",
        "func Named(Point, Str) -> Named", "func make(int) -> Named"})
    EXPECT_TRUE(contains(dump.out, line)) << line << "\n" << dump.out;
  fs::remove_all(dir);
}

// A borrowed result keeps its mode in a prebuilt module: the dump shows it,
// an importer sees a `view` result as a `view` and writes through an
// `inout` one.
TEST(Pkm, BorrowedResultsAcrossModules) {
  REQUIRE_BACKEND();
  auto dir =
      fs::temp_directory_path() / ("pkm_results_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "base.pkn",
            "class Counter { n: int;\n"
            "  fn tick() { self.n = self.n + 1; }\n"
            "  view fn get() -> int { return self.n; } }\n"
            "impl Counter { fn __init__() { self.n = 5; } }\n"
            "class Box { c: Counter; k: int;\n"
            "  view fn peek() -> view Counter { return self.c; }\n"
            "  fn count() -> inout int { return self.k; } }\n"
            "impl Box { fn __init__() { self.c = Counter(); self.k = 0; } }\n"
            "fn longer(a: view Str, b: view Str) -> view Str {\n"
            "  return if a.len() > b.len() then a else b; }\n");
  writeFile(dir / "main.pkn", "import ::io; import base;\n"
                              "fn main() -> int {\n"
                              "  b = base::Box();\n"
                              "  n: inout = b.count();\n"
                              "  n = n + 2;\n"
                              "  k = b.count();\n"
                              "  v: view = b.peek();\n"
                              "  io::println(base::longer(\"ab\", \"c\") + "
                              "Str(v.get() + k));\n"
                              "  return 0;\n}\n");
  writeFile(dir / "bad.pkn", "import base;\n"
                             "fn main() -> int {\n"
                             "  b = base::Box();\n"
                             "  b.peek().tick();\n"
                             "  return 0;\n}\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  ASSERT_EQ(paykan("--emit-pkm base.pkn").exitCode, 0);
  fs::remove(dir / "base.pkn");
  auto ok = paykan("main.pkn");
  ASSERT_EQ(ok.exitCode, 0) << ok.out;
  EXPECT_TRUE(contains(ok.out, "ab7\n")) << ok.out;
  auto bad = paykan("--check-only bad.pkn");
  EXPECT_NE(bad.exitCode, 0);
  EXPECT_TRUE(contains(bad.out, "bad.pkn:4:3: error: the result of 'peek' is "
                                "a 'view'; 'tick' is not a 'view fn'"))
      << bad.out;
  // Where an `inout` result may point is checked as its module is compiled:
  // one in an object reached through a field makes no module file.
  writeFile(dir / "far.pkn", "class C { k: Str; }\n"
                             "class H { c: C;\n"
                             "  fn e() -> inout Str { return self.c.k; } }\n");
  auto far = paykan("--emit-pkm far.pkn");
  EXPECT_NE(far.exitCode, 0);
  EXPECT_TRUE(contains(far.out, "far.pkn:3:32: error: 'e' returns an 'inout' "
                                "reference into 'self.c', a separate object"))
      << far.out;
  EXPECT_FALSE(fs::exists(dir / "far.pkm"));
  // A template's is checked where an importer instantiates it from the
  // module file: `self.v.x` is part of `self` in `Wrap<P>` for a struct
  // `P`, and in a separate object in `Wrap<D>` for a class `D`.
  writeFile(dir / "gen.pkn", "class D { x: int; }\n"
                             "struct P { x: int; }\n"
                             "class Wrap<T> { v: T;\n"
                             "  fn x() -> inout int { return self.v.x; } }\n");
  writeFile(dir / "usegen.pkn", "import ::io; import gen;\n"
                                "fn main() -> int {\n"
                                "  w = gen::Wrap<gen::P>(gen::P(1));\n"
                                "  x: inout = w.x();\n"
                                "  x = x + 4;\n"
                                "  io::println(Str(w.v.x));\n"
                                "  return 0;\n}\n");
  writeFile(dir / "badgen.pkn", "import gen;\n"
                                "fn main() -> int {\n"
                                "  w = gen::Wrap<gen::D>(gen::D(1));\n"
                                "  return 0;\n}\n");
  ASSERT_EQ(paykan("--emit-pkm gen.pkn").exitCode, 0);
  fs::remove(dir / "gen.pkn");
  auto usegen = paykan("usegen.pkn");
  ASSERT_EQ(usegen.exitCode, 0) << usegen.out;
  EXPECT_TRUE(contains(usegen.out, "5\n")) << usegen.out;
  auto badgen = paykan("--check-only badgen.pkn");
  EXPECT_NE(badgen.exitCode, 0);
  EXPECT_TRUE(contains(badgen.out, "gen.pkn:4:32: error: 'x' returns an "
                                   "'inout' reference into 'self.v', a "
                                   "separate object"))
      << badgen.out;
  EXPECT_TRUE(contains(badgen.out, "badgen.pkn:3:7: note: in instantiation "
                                   "of 'Wrap<D>' requested here"))
      << badgen.out;
  // Whether an object can hold a place of a call's result type is read from
  // the module file too: a method of `C` returns a `Str`, which a subclass
  // finds in its own object, so `s.get()` can point past `s`; a `C` holds
  // no `int`, so `s.num()` is in its storage.
  writeFile(dir / "slots.pkn",
            "class C { fn slot() -> inout Str; }\n"
            "class D : C { k: Str;\n"
            "  fn slot() -> inout Str { return self.k; } }\n"
            "struct S { c: C; n: int; }\n"
            "impl S { fn get() -> inout Str { return self.c.slot(); }\n"
            "  fn num() -> inout int { return self.n; } }\n");
  writeFile(dir / "useslots.pkn",
            "import slots;\n"
            "class H { s: slots::S;\n"
            "  fn e() -> inout Str { return self.s.get(); }\n"
            "  fn f() -> inout int { return self.s.num(); } }\n"
            "fn main() -> int { return 0; }\n");
  ASSERT_EQ(paykan("--emit-pkm slots.pkn").exitCode, 0);
  fs::remove(dir / "slots.pkn");
  auto useslots = paykan("--check-only useslots.pkn");
  EXPECT_NE(useslots.exitCode, 0);
  EXPECT_TRUE(contains(useslots.out,
                       "useslots.pkn:3:32: error: 'e' returns the 'inout' "
                       "result of 'get', which can point into an object "
                       "'self.s' holds"))
      << useslots.out;
  EXPECT_FALSE(contains(useslots.out, "useslots.pkn:4:")) << useslots.out;
  auto dump = paykan("pkm dump --section=iface base.pkm");
  EXPECT_TRUE(contains(dump.out, "method peek() -> view Counter flags 0x2"))
      << dump.out;
  EXPECT_TRUE(contains(dump.out, "method count() -> inout int flags 0x0"))
      << dump.out;
  EXPECT_TRUE(contains(dump.out, "func longer(view Str, view Str) -> view Str"))
      << dump.out;
  fs::remove_all(dir);
}

// Generic templates across modules (docs/design/pkm.md §4.8): a module
// with templates is cached with them, and so are one that instantiates
// another module's (it holds its own copy) and one that only uses such an
// instantiation; two programs sharing the cache each keep one Box<int>.
// `m3` uses m2's Box<int>; in `p2`, m1 makes Box<int> too.  Editing a
// template's body makes every entry that may hold a copy of it stale.
TEST(Pkm, GenericsAcrossModulesAreCached) {
  REQUIRE_BACKEND();
  fs::path dir =
      fs::temp_directory_path() / ("pkm_generics_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "lib" / "gen.pkn",
            "class Box<T> { v: T;\n"
            "  view fn one() -> int { return 1; } }\n"
            "impl Box<T> { fn __init__(v: T) { self.v = v; } }\n");
  writeFile(dir / "m1.pkn", "import lib::gen;\n"
                            "fn one() -> int { return gen::Box<int>(0).one(); "
                            "}\n");
  writeFile(dir / "m2.pkn", "import lib::gen;\n"
                            "fn make() -> gen::Box<int> { "
                            "return gen::Box<int>(5); }\n");
  writeFile(dir / "m3.pkn", "import m2;\n"
                            "fn use() -> int { return m2::make().one(); }\n");
  writeFile(dir / "plain.pkn", "fn two() -> int { return 2; }\n");
  writeFile(dir / "p1.pkn", "import m3;\nimport plain;\n"
                            "fn main() -> int { return m3::use() + "
                            "plain::two(); }\n");
  writeFile(dir / "p2.pkn", "import m1;\nimport m3;\n"
                            "fn main() -> int { return m1::one() + m3::use(); "
                            "}\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  for (int round = 0; round < 2; ++round) {
    auto p1 = paykan("--verbose p1.pkn");
    EXPECT_EQ(p1.exitCode, 3) << p1.out;
    for (const char *m : {"lib::gen", "m2", "m3", "plain"})
      EXPECT_TRUE(contains(p1.out, std::string("module ") + m +
                                       (round == 0 ? ": wrote" : ": cache")))
          << m << "\n"
          << p1.out;
    auto p2 = paykan("--verbose p2.pkn");
    EXPECT_EQ(p2.exitCode, 2) << p2.out;
    EXPECT_TRUE(
        contains(p2.out, round == 0 ? "module m1: wrote" : "module m1: cache"))
        << p2.out;
  }
  writeFile(dir / "lib" / "gen.pkn",
            "class Box<T> { v: T;\n"
            "  view fn one() -> int { return 10; } }\n"
            "impl Box<T> { fn __init__(v: T) { self.v = v; } }\n");
  auto p1 = paykan("--verbose p1.pkn");
  EXPECT_EQ(p1.exitCode, 12) << p1.out;
  EXPECT_TRUE(contains(p1.out, "module m2: cache .paykan_cache/m2.pkm is stale "
                               "(the templates of module 'lib::gen' "
                               "changed)"))
      << p1.out;
  // m3 holds no copy of its own, but m2's interface records gen's templates.
  EXPECT_TRUE(contains(p1.out, "module m3: cache .paykan_cache/m3.pkm is stale "
                               "(the interface of module 'm2' changed)"))
      << p1.out;
  EXPECT_TRUE(contains(p1.out, "module plain: cache .paykan_cache/plain.pkm "
                               "usable"))
      << p1.out;
  auto p2 = paykan("p2.pkn");
  EXPECT_EQ(p2.exitCode, 20) << p2.out;
  fs::remove_all(dir);
}

// A module's .pkm holds its templates (docs/design/pkm.md §4.3): with its
// source gone, an importer instantiates its classes, structs and functions
// from the file, an error inside an instantiated body names the module's
// file, and `paykan pkm dump` lists the section.
// A module's file keeps its own code, its copies of the instantiations its
// interface exposes, and of the others what its own code uses; the copies
// stay PIR in CODE and the rest is compiled (OBJ), CODE declaring it
// (docs/design/pkm.md §5.13).  A program runs the same from the cache.
TEST(Pkm, ModuleCodeKeepsWhatTheModuleAndItsImportersUse) {
  REQUIRE_BACKEND();
  fs::path dir = fs::temp_directory_path() /
                 ("pkm_module_code_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(
      dir / "lib" / "gen.pkn",
      "struct Pair<A, B> { a: A; b: B; }\n"
      "impl Pair<A, B> { view fn first() -> view A { return self.a; }\n"
      "  view fn second() -> view B { return self.b; } }\n"
      "class Box<T> { v: T; view fn get() -> view T { return self.v; } }\n"
      "impl Box<T> { fn __init__(v: T) { self.v = v; } }\n");
  writeFile(
      dir / "m2.pkn",
      "import lib::gen;\n"
      "fn make() -> gen::Pair<Str, int> { return gen::Pair<Str, int>(\"m\", "
      "2); }\n"
      "fn boxes(b: view gen::Box<gen::Pair<int, int>>) -> int { "
      "return b.get().first(); }\n"
      "fn hidden() -> int { p = gen::Pair<int, bool>(1, True); "
      "return p.first(); }\n");
  writeFile(dir / "main.pkn", "import m2;\n"
                              "fn main() -> int { return m2::make().second() "
                              "+ m2::hidden(); }\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  auto first = paykan("--verbose main.pkn");
  EXPECT_EQ(first.exitCode, 3) << first.out;
  EXPECT_TRUE(contains(first.out, "module m2: wrote")) << first.out;
  auto code = paykan("pkm dump --section=code .paykan_cache/m2.pkm");
  ASSERT_EQ(code.exitCode, 0) << code.out;
  // The exposed instantiations whole (an importer may call any of their
  // functions), the hidden one as far as `hidden` uses it.
  for (const char *kept :
       {"shared fn @\"Pair<Str, int>.second\"(",
        "shared fn @\"Pair<Str, int>.first\"(",
        "shared fn @\"Pair<int, int>.second\"(",
        "shared class \"Box<Pair<int, int>>\"",
        "shared fn @\"Pair<int, bool>.first\"(",
        "extern fn @make(ptr) -> void module \"m2\" uses @\"Pair<Str, int>\"",
        "extern fn @hidden() -> i64 module \"m2\" uses"})
    EXPECT_TRUE(contains(code.out, kept)) << kept << "\n" << code.out;
  EXPECT_FALSE(contains(code.out, "Pair<int, bool>.second")) << code.out;
  // The functions' bodies are compiled, not in CODE.
  EXPECT_FALSE(contains(code.out, "fn @hidden() -> i64 {")) << code.out;
  auto obj = paykan("pkm dump --section=obj .paykan_cache/m2.pkm");
  EXPECT_TRUE(contains(obj.out, "  backend \"mlir\"\n  format \"llvm-bc\""))
      << obj.out;
  auto again = paykan("--verbose main.pkn");
  EXPECT_EQ(again.exitCode, 3) << again.out;
  EXPECT_TRUE(
      contains(again.out, "module m2: cache .paykan_cache/m2.pkm usable"))
      << again.out;
  // A binary keeps only what main reaches: `boxes` and its copies go.
  auto pir = paykan("--emit-pir main.pkn");
  ASSERT_EQ(pir.exitCode, 0) << pir.out;
  EXPECT_FALSE(contains(pir.out, "boxes")) << pir.out;
  EXPECT_FALSE(contains(pir.out, "Box<Pair<int, int>>")) << pir.out;
  EXPECT_TRUE(contains(pir.out, "Pair<Str, int>.second")) << pir.out;
  fs::remove_all(dir);
}

// The main module of a program knows every class derived from its classes
// (docs/pir.md §7), so its copy of `HashMap<Str, Dog>.put` reads the map's
// table in place across `_Table.put`, which lets go only of Dogs and
// strings, by a stable load: it retains nothing, and hands its parameters
// on with their bits, whether they were lent or handed over (docs/pir.md
// §3, §12).  Written to a `.pkm` file, which a module that derives a class
// holding anything from `Dog` may import, the same file holds the table
// across the call.
TEST(Pkm, AModuleFileAssumesNothingOfItsClassesSubclasses) {
  REQUIRE_BACKEND();
  fs::path dir =
      fs::temp_directory_path() / ("pkm_closed_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "dogs.pkn", "import ::collections as c;\n"
                              "class Dog { name: Str; }\n"
                              "fn main() -> int {\n"
                              "  d = c::HashMap<Str, Dog>();\n"
                              "  d.put(\"d\", Dog(\"rex\"));\n"
                              "  return int(d.len());\n"
                              "}\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  // The function's text in @p text.
  auto put = [](const std::string &text) {
    const size_t start = text.find("fn @\"HashMap<Str, Dog>.put\"(");
    if (start == std::string::npos)
      return std::string();
    return text.substr(start, text.find("\n}\n", start) - start);
  };
  auto pir = paykan("--emit-pir dogs.pkn");
  ASSERT_EQ(pir.exitCode, 0) << pir.out;
  ASSERT_EQ(paykan("--emit-pkm dogs.pkn").exitCode, 0);
  auto code = paykan("pkm dump --section=code dogs.pkm");
  ASSERT_EQ(code.exitCode, 0) << code.out;
  const std::string program = put(pir.out);
  const std::string module = put(code.out);
  ASSERT_FALSE(program.empty()) << pir.out;
  ASSERT_FALSE(module.empty()) << code.out;
  EXPECT_TRUE(contains(program, "= ptr.load box, %self.addr.8 stable\n"))
      << program;
  size_t retains = 0;
  for (size_t at = program.find("retain "); at != std::string::npos;
       at = program.find("retain ", at + 1))
    ++retains;
  EXPECT_EQ(retains, 0u) << program;
  EXPECT_TRUE(contains(program, "(%recv.obj.10, owned(%k.own.4) %k.11, "
                                "owned(%v.own.6) %v.12)\n"))
      << program;
  EXPECT_TRUE(contains(module, "= ptr.load box, %self.addr.8\n")) << module;
  EXPECT_TRUE(contains(module, "retain %elem.9\n")) << module;
  EXPECT_TRUE(contains(module, "release %elem.9\n")) << module;
  fs::remove_all(dir);
}

// A module's code, in a cache entry and in its `.pkm`, keeps no mark of the
// copies the lowering made only to hand an argument over (`retain lend`,
// docs/pir.md §6): the lending pass lends them or clears them before the
// code is kept, here lending both copies of `x`, which `both` keeps for the
// calls, whatever the importer's program does with its own.
TEST(Pkm, ModuleCodeKeepsNoLendMark) {
  REQUIRE_BACKEND();
  fs::path dir =
      fs::temp_directory_path() / ("pkm_lend_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "lib.pkn",
            "class Pair<T> {\n"
            "  a: T;\n"
            "  view fn first(x: T) -> int { return 1; }\n"
            "  view fn both(x: T) -> int {\n"
            "    return self.first(x) + self.first(x);\n"
            "  }\n"
            "}\n"
            "fn make(s: Str) -> Pair<Str> { return Pair<Str>(s); }\n");
  writeFile(dir / "main.pkn", "import lib;\n"
                              "fn main() -> int {\n"
                              "  p = lib::make(\"x\");\n"
                              "  return p.both(\"y\");\n"
                              "}\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  auto ran = paykan("main.pkn");
  EXPECT_EQ(ran.exitCode, 2) << ran.out;
  ASSERT_EQ(paykan("--emit-pkm lib.pkn").exitCode, 0);
  for (const char *file : {".paykan_cache/lib.pkm", "lib.pkm"}) {
    auto code = paykan(std::string("pkm dump --section=code ") + file);
    ASSERT_EQ(code.exitCode, 0) << code.out;
    EXPECT_TRUE(contains(code.out, "shared fn @Pair<Str>.both(")) << code.out;
    EXPECT_TRUE(contains(code.out, " [3] (lent %x.6)\n")) << code.out;
    EXPECT_TRUE(contains(code.out, " [3] (lent %x.9)\n")) << code.out;
    EXPECT_FALSE(contains(code.out, "retain %x.6\n")) << code.out;
    EXPECT_FALSE(contains(code.out, "retain lend")) << code.out;
  }
  fs::remove_all(dir);
}

// A module's compiled code is lent what its importer keeps (#98 part 2,
// docs/pir.md §3, §12): the program's copies of `s` and `n` for lib's
// constructor, its `stash`, which keeps the node in a field, and its
// native function's body, and for the standard library's List and
// HashMap, are lent arguments, and the compiled callees, whose lending ran
// when their `.pkm` was made, release nothing they were lent and retain
// what they keep.  The program frees everything and prints what it did.
TEST(Pkm, ACompiledCalleeIsLentWhatItsImporterKeeps) {
  REQUIRE_BACKEND();
  fs::path dir =
      fs::temp_directory_path() / ("pkm_lent_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "lib.pkn",
            "native fn width(s: view Str) -> int = \"pk_lent_width\";\n"
            "class Node { name: Str; }\n"
            "class Holder { n: Node?; }\n"
            "fn stash(h: Holder, n: Node) { h.n = n; }\n"
            "fn named(n: Node) -> Str { return n.name; }\n");
  writeFile(dir / "lib.c", "#include <stdint.h>\n"
                           "int64_t pk_lent_width(void *s) { return s ? 7 : 0; "
                           "}\n");
  writeFile(
      dir / "main.pkn",
      "import ::io;\n"
      "import ::collections as c;\n"
      "import lib;\n"
      "fn main() -> int {\n"
      "  s = \"abc\" + \"def\";\n"
      "  n = lib::Node(s);\n"
      "  h = lib::Holder(None);\n"
      "  lib::stash(h, n);\n"
      "  l = c::List<lib::Node>();\n"
      "  l.push(n);\n"
      "  m = c::HashMap<Str, Str>();\n"
      "  m.put(s, \"v\");\n"
      "  io::println(Str<int>(lib::width(s)) + \" \" + lib::named(n) +\n"
      "              \" \" + l.at(0).name + \" \" + m.getOr(s, s) + \" \" + "
      "s);\n"
      "  return 0;\n"
      "}\n");
  auto in = [&](const std::string &cmd) {
    return run("cd " + dir.string() + " && " + cmd);
  };
  ASSERT_EQ(in("cc -std=c11 -fPIC -c lib.c -o lib.o").exitCode, 0);
  auto ran = in(paykanCmd() + " --track-heap --object=lib.o main.pkn");
  EXPECT_EQ(ran.exitCode, 0) << ran.out;
  EXPECT_TRUE(contains(ran.out, "7 abcdef abcdef v abcdef\n")) << ran.out;
  EXPECT_TRUE(contains(ran.out, "live blocks       : 0\n")) << ran.out;
  // lib came from its cache entry this time: its compiled code is lent to.
  auto again = in(paykanCmd() + " --track-heap --object=lib.o main.pkn");
  EXPECT_EQ(again.out, ran.out);
  auto pir = in(paykanCmd() + " --emit-pir main.pkn");
  ASSERT_EQ(pir.exitCode, 0) << pir.out;
  for (const char *call :
       {"call @\"lib::Node\"(lent %s.", "call @\"lib::stash\"(%h.8, lent %n.",
        "call @List<Node>.push(%l.addr.12, lent %n.",
        "call @\"HashMap<Str, Str>.put\"(%m.addr.16, lent %s.",
        "call @\"lib::width\"(lent %s.",
        "call @\"HashMap<Str, Str>.getOr\"(%m.addr.40, lent %s.41, lent %s."})
    EXPECT_TRUE(contains(pir.out, call)) << call << "\n" << pir.out;
  // n's last use hands it over.
  EXPECT_TRUE(contains(pir.out, "call @\"lib::named\"(%n.")) << pir.out;
  fs::remove_all(dir);
}

// A native function's FUNC record says it is one (docs/design/pkm.md
// §3.5): an importer reading lib's interface from its cache entry knows the
// native keeps nothing it is lent, and lends it a field read, which it
// copies for another module's function.
TEST(Pkm, ANativeFunctionAcrossModulesIsLentAFieldRead) {
  REQUIRE_BACKEND();
  fs::path dir =
      fs::temp_directory_path() / ("pkm_native_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "lib.pkn",
            "native fn tag(o: view Obj) -> int = \"pk_native_tag\";\n"
            "fn look(o: view Obj) -> int { return 2; }\n");
  writeFile(dir / "lib.c", "#include <stdint.h>\n"
                           "int64_t pk_native_tag(void *o) { return o ? 1 : 0; "
                           "}\n");
  writeFile(dir / "main.pkn",
            "import ::io;\n"
            "import lib;\n"
            "class Node { name: Str; }\n"
            "class H { n: Node; }\n"
            "fn main() -> int {\n"
            "  h = H(Node(\"a\"));\n"
            "  io::println(Str<int>(lib::tag(h.n) + lib::look(h.n)));\n"
            "  return 0;\n"
            "}\n");
  auto in = [&](const std::string &cmd) {
    return run("cd " + dir.string() + " && " + cmd);
  };
  ASSERT_EQ(in("cc -std=c11 -fPIC -c lib.c -o lib.o").exitCode, 0);
  auto ran = in(paykanCmd() + " --track-heap --object=lib.o main.pkn");
  EXPECT_EQ(ran.exitCode, 0) << ran.out;
  EXPECT_TRUE(contains(ran.out, "3\n")) << ran.out;
  EXPECT_TRUE(contains(ran.out, "live blocks       : 0\n")) << ran.out;
  // lib comes from its cache entry now, its interface read from the entry.
  auto again = in(paykanCmd() + " --track-heap --object=lib.o main.pkn");
  EXPECT_EQ(again.out, ran.out);
  auto pir = in(paykanCmd() + " --emit-pir main.pkn");
  ASSERT_EQ(pir.exitCode, 0) << pir.out;
  EXPECT_TRUE(contains(pir.out, "call @\"lib::tag\"(lent %n.")) << pir.out;
  EXPECT_TRUE(contains(pir.out, "call @\"lib::look\"(%n.")) << pir.out;
  fs::remove_all(dir);
}

// `pkm extract --object` writes a module's compiled code as an object the
// system linker takes (what a program's --emit-c C links with).
TEST(Pkm, ExtractWritesTheCompiledCodeAsAnObject) {
  REQUIRE_BACKEND();
  fs::path dir =
      fs::temp_directory_path() / ("pkm_extract_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "lib.pkn", "fn twice(n: int) -> int { return n * 2; }\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  ASSERT_EQ(paykan("--emit-pkm lib.pkn").exitCode, 0);
  auto extract = paykan("pkm extract --object lib.pkm -o lib.o");
  ASSERT_EQ(extract.exitCode, 0) << extract.out;
  std::string object = slurp(dir / "lib.o");
  ASSERT_GE(object.size(), 4u);
  const bool elf = object.compare(0, 4,
                                  "\x7f"
                                  "ELF") == 0;
  const bool macho = object.compare(0, 4, "\xcf\xfa\xed\xfe") == 0;
  EXPECT_TRUE(elf || macho);
  auto none = paykan("pkm extract lib.pkm");
  EXPECT_NE(none.exitCode, 0);
  EXPECT_TRUE(contains(none.out, "needs what to extract: --object"))
      << none.out;
  fs::remove_all(dir);
}

TEST(Pkm, PrebuiltModuleCarriesItsTemplates) {
  REQUIRE_BACKEND();
  fs::path dir = fs::temp_directory_path() /
                 ("pkm_prebuilt_tmpl_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "lib" / "gen.pkn",
            "class Box<T> { v: T;\n"
            "  view fn get() -> view T { return self.v; }\n"
            "  view fn twice() -> T { return self.v + self.v; } }\n"
            "impl Box<T> { fn __init__(v: T) { self.v = v; } }\n"
            "struct Pair<A, B> { a: A; b: B; }\n"
            "fn first<T>(x: T, y: T) -> T { return _keep(x); }\n"
            "fn _keep<T>(x: T) -> T { return x; }\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  auto emit = paykan("--emit-pkm lib/gen.pkn");
  ASSERT_EQ(emit.exitCode, 0) << emit.out;
  fs::remove(dir / "lib" / "gen.pkn");
  auto dump = paykan("pkm dump lib/gen.pkm");
  EXPECT_EQ(dump.exitCode, 0) << dump.out;
  EXPECT_TRUE(
      contains(dump.out, "contents 0x43 (HAS_CODE,HAS_TEMPLATES,HAS_OBJECT)"))
      << dump.out;
  EXPECT_TRUE(contains(dump.out, "  file \"lib/gen.pkn\"\n  uses 0\n"
                                 "  impls 1\n    class 0\n"))
      << dump.out;
  auto tmpl = paykan("pkm dump --section=tmpl lib/gen.pkm");
  EXPECT_EQ(tmpl.exitCode, 0) << tmpl.out;
  EXPECT_TRUE(contains(tmpl.out, "(paykan-ast ")) << tmpl.out;
  EXPECT_TRUE(contains(tmpl.out, "\"$lib::gen::_keep\"")) << tmpl.out;
  writeFile(dir / "main.pkn", "import ::io;\nimport lib::gen;\n"
                              "fn main() -> int { b = gen::Box<int>(20); "
                              "p = gen::Pair<Str, int>(\"x\", 1);\n"
                              "  io::println(gen::first(\"a\", \"b\") + p.a);\n"
                              "  return b.twice() + gen::first(2, 3); }\n");
  auto r = paykan("--verbose main.pkn");
  EXPECT_EQ(r.exitCode, 42) << r.out;
  EXPECT_TRUE(contains(r.out, "ax\n")) << r.out;
  EXPECT_TRUE(contains(r.out, "module lib::gen: prebuilt lib/gen.pkm usable"))
      << r.out;
  writeFile(dir / "bad.pkn",
            "import lib::gen;\n"
            "fn main() -> int { b = gen::Box<bool>(true); b.twice(); "
            "return 0; }\n");
  auto bad = paykan("bad.pkn");
  EXPECT_NE(bad.exitCode, 0) << bad.out;
  EXPECT_TRUE(contains(bad.out, "lib/gen.pkn:3:")) << bad.out;
  EXPECT_TRUE(contains(bad.out, "in instantiation of")) << bad.out;
  fs::remove_all(dir);
}

// The standard library is staged as .pkm files only, each named `::name`
// (--module-name): a program loads every module of it, generic ones
// included, from those files.
TEST(Pkm, StandardLibraryIsPkmFilesOnly) {
  REQUIRE_BACKEND();
  size_t modules = 0;
  for (const auto &e : fs::directory_iterator(PAYKAN_TEST_STDLIB_DIR)) {
    EXPECT_EQ(e.path().extension(), ".pkm") << e.path();
    ++modules;
  }
  EXPECT_GE(modules, 5u);
  fs::path dir =
      fs::temp_directory_path() / ("pkm_stdlib_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "main.pkn",
            "import ::io;\nimport ::collections;\n"
            "fn main() -> int { xs = collections::List<Str>();\n"
            "  xs.push(\"a\"); xs.push(\"b\");\n"
            "  io::println(xs.get(1)); return int(xs.len()); }\n");
  auto r = run("cd " + dir.string() + " && " + paykanCmd() +
               " --verbose --no-module-cache main.pkn");
  EXPECT_EQ(r.exitCode, 2) << r.out;
  EXPECT_TRUE(contains(r.out, "b\n")) << r.out;
  for (const char *m :
       {"::io", "::error", "::result", "::memory", "::collections"})
    EXPECT_TRUE(contains(r.out, std::string("module ") + m + ": prebuilt "))
        << m << "\n"
        << r.out;
  fs::remove_all(dir);
}

// --module-name names the module --emit-pkm writes, and nothing else takes
// it.
TEST(Pkm, ModuleNameNamesTheEmittedModule) {
  REQUIRE_BACKEND();
  fs::path dir =
      fs::temp_directory_path() / ("pkm_modname_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "src" / "shapes.pkn", "fn area() -> int { return 6; }\n");
  writeFile(dir / "main.pkn", "import geometry::shapes;\n"
                              "fn main() -> int { return shapes::area(); }\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  auto emit = paykan("--emit-pkm --module-name=geometry::shapes "
                     "-o geometry/shapes.pkm src/shapes.pkn");
  ASSERT_EQ(emit.exitCode, 0) << emit.out;
  auto r = paykan("main.pkn");
  EXPECT_EQ(r.exitCode, 6) << r.out;
  auto noEmit = paykan("--module-name=geometry::shapes main.pkn");
  EXPECT_NE(noEmit.exitCode, 0);
  EXPECT_TRUE(contains(noEmit.out, "--module-name names the module "
                                   "--emit-pkm writes"))
      << noEmit.out;
  auto badName = paykan("--emit-pkm --module-name=a::1b src/shapes.pkn");
  EXPECT_NE(badName.exitCode, 0);
  EXPECT_TRUE(contains(badName.out, "is not a module name")) << badName.out;
  fs::remove_all(dir);
}

// A function type in a signature or a field keeps its spelling in a
// prebuilt module, modes included; an importer passes closures in and calls
// the ones it gets back, from the source and from the `.pkm` alike.
TEST(Pkm, FunctionTypesAcrossModules) {
  REQUIRE_BACKEND();
  auto dir =
      fs::temp_directory_path() / ("pkm_fntypes_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "ops.pkn",
            "fn square(x: int) -> int { return x * x; }\n"
            "fn twice(f: (int) -> int) -> (int) -> int {\n"
            "  return fn(x: int) -> int { return f(f(x)); }; }\n"
            "fn each(xs: view int[], f: (view int, inout int) -> void) -> int "
            "{\n"
            "  t = 0; i = 0;\n"
            "  while i < xs.len() { f(xs[i], t); i = i + 1; }\n"
            "  return t; }\n"
            "class Button { label: Str; onClick: (Str) -> Str;\n"
            "  fn click() -> Str { return self.onClick(self.label); } }\n");
  writeFile(dir / "main.pkn",
            "import ::io; import ops;\n"
            "fn main() -> int {\n"
            "  t = ops::twice(ops::square);\n"
            "  b = ops::Button(\"b\", fn(l: Str) -> Str { return l + \"!\"; "
            "});\n"
            "  n = ops::each([1, 2, 3], fn(x: view int, acc: inout int) { acc "
            "= acc + x; });\n"
            "  io::println(Str(t(3)) + \" \" + b.click() + \" \" + Str(n));\n"
            "  return 0;\n}\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  const std::string expected = "81 b! 6\n";
  auto src = paykan("--track-heap main.pkn");
  ASSERT_EQ(src.exitCode, 0) << src.out;
  EXPECT_TRUE(contains(src.out, expected)) << src.out;
  EXPECT_TRUE(contains(src.out, "live blocks       : 0")) << src.out;
  ASSERT_EQ(paykan("--emit-pkm ops.pkn").exitCode, 0);
  fs::remove(dir / "ops.pkn");
  auto pkm = paykan("--track-heap main.pkn");
  ASSERT_EQ(pkm.exitCode, 0) << pkm.out;
  EXPECT_TRUE(contains(pkm.out, expected)) << pkm.out;
  EXPECT_TRUE(contains(pkm.out, "live blocks       : 0")) << pkm.out;
  auto dump = paykan("pkm dump --section=iface ops.pkm");
  for (const char *line :
       {"func twice((int) -> int) -> (int) -> int",
        "func each(view int[], (view int, inout int) -> void) -> int",
        "    field onClick: (Str) -> Str",
        "func Button(Str, (Str) -> Str) -> Button"})
    EXPECT_TRUE(contains(dump.out, line)) << line << "\n" << dump.out;
  fs::remove_all(dir);
}

// A module's `__` names are its own (docs/design/visibility.md): an
// importer cannot write them, from the source or from the `.pkm`, while the
// module's public functions, static fns and exported templates keep using
// them; a public class's `__` field or method is that class's own.
TEST(Pkm, PrivateNamesAreTheModulesOwn) {
  REQUIRE_BACKEND();
  auto dir =
      fs::temp_directory_path() / ("pkm_private_" + std::to_string(getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir / "lib");
  writeFile(dir / "lib" / "shapes.pkn",
            "class _Node { v: int; }\n"
            "fn _helper(n: int) -> int { return n + 1; }\n"
            "enum _Mode { A, B }\n"
            "class Pub { __n: int; v: int; _w: int;\n"
            "  fn get() -> int { return _helper(self.__n); }\n"
            "  view fn _twice() -> int { return self._w * 2; } }\n"
            "impl Pub { static fn __mk() -> Pub { return Pub(1, 2, 3); }\n"
            "  static fn _make() -> Pub { return Pub(0, 0, 3); }\n"
            "  static fn mk() -> Pub { return Pub::__mk(); } }\n"
            "class Gen<T> { __v: T;\n"
            "  fn get() -> T {\n"
            "    return self.__v + _helper(0) + Pub::_make()._w - 3; } }\n"
            "impl Gen<T> { fn __init__(v: T) { self.__v = v; } }\n"
            "class _GenHidden<T> { v: T; }\n"
            "fn useHidden() -> int {\n"
            "  h = _GenHidden<int>(3); p = Pub::_make();\n"
            "  return h.v + p._twice() - p._w * 2; }\n"
            "class Conn { v: int; }\n"
            "impl Conn { fn ___init__(v: int) { self.v = v; }\n"
            "  static fn open(v: int) -> Conn { return Conn(v); } }\n");
  writeFile(dir / "main.pkn",
            "import ::io; import lib::shapes;\n"
            "fn main() -> int {\n"
            "  p = shapes::Pub::mk();\n"
            "  io::println(Str(p.get()) + \" \" + Str(shapes::useHidden()));\n"
            "  io::println(shapes::Conn::open(7).v);\n"
            "  return 0;\n}\n");
  // A template's body uses the module's private names in the importer
  // (templates come from the source: a `.pkm` carries none, #160).
  writeFile(dir / "gen.pkn", "import ::io; import lib::shapes;\n"
                             "fn main() -> int {\n"
                             "  g = shapes::Gen(4);\n"
                             "  io::println(Str(g.get()));\n"
                             "  return 0;\n}\n");
  writeFile(dir / "bad.pkn", "import lib::shapes;\n"
                             "fn main() -> int {\n"
                             "  a = shapes::_helper(1);\n"
                             "  n = shapes::_Node(1);\n"
                             "  m: shapes::_Mode = shapes::_Mode::A;\n"
                             "  p = shapes::Pub::__mk();\n"
                             "  q = shapes::Pub::mk();\n"
                             "  r = q.__n;\n"
                             "  s = q._w;\n"
                             "  t = q._twice();\n"
                             "  u = shapes::Pub::_make();\n"
                             "  h = shapes::_GenHidden<int>(1);\n"
                             "  b: _Node = n;\n"
                             "  c = shapes::Conn(1);\n"
                             "  return 0;\n}\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  auto check = [&](const char *when) {
    auto ok = paykan("--track-heap main.pkn");
    EXPECT_EQ(ok.exitCode, 0) << when << "\n" << ok.out;
    EXPECT_TRUE(contains(ok.out, "2 3\n7\n")) << when << "\n" << ok.out;
    EXPECT_TRUE(contains(ok.out, "live blocks       : 0")) << when << "\n"
                                                           << ok.out;
    auto bad = paykan("--check-only bad.pkn");
    EXPECT_NE(bad.exitCode, 0) << when;
    for (const char *line :
         {"'shapes::_helper' is private to module 'lib::shapes'",
          "'shapes::_Node' is private to module 'lib::shapes'",
          "'shapes::_Mode' is private to module 'lib::shapes'",
          "'__mk' is private to 'Pub': it is reachable only from its own "
          "methods",
          "'__n' is private to 'Pub': it is reachable only from its own "
          "methods",
          "'_w' of 'Pub' is private to module 'lib::shapes'",
          "'_twice' of 'Pub' is private to module 'lib::shapes'",
          "'_make' of 'Pub' is private to module 'lib::shapes'",
          "'shapes::_GenHidden' is private to module 'lib::shapes'",
          "'_Node' is private to module 'lib::shapes'",
          "'shapes::Conn' cannot be constructed here: its initializer "
          "'___init__' is private to module 'lib::shapes'"})
      EXPECT_TRUE(contains(bad.out, line)) << when << ": " << line << "\n"
                                           << bad.out;
  };
  check("from source");
  auto gen = paykan("--track-heap gen.pkn");
  EXPECT_EQ(gen.exitCode, 0) << gen.out;
  EXPECT_TRUE(contains(gen.out, "5\n")) << gen.out;
  EXPECT_TRUE(contains(gen.out, "live blocks       : 0")) << gen.out;
  ASSERT_EQ(paykan("--emit-pkm lib/shapes.pkn").exitCode, 0);
  fs::remove(dir / "lib" / "shapes.pkn");
  check("from the .pkm");
  fs::remove_all(dir);
}

// An instantiation whose type argument is a function type keeps its name
// across modules: the `>` of the `->` is not the instantiation's closing
// bracket, and the arguments after it are still its own (`Pair<(int) ->
// int, Str>` names one struct, `Map<(int, int) -> int, Str>` one class).
TEST(Pkm, FunctionTypeArgumentsInInstantiationNames) {
  REQUIRE_BACKEND();
  auto dir = fs::temp_directory_path() /
             ("pkm_fntype_args_" + std::to_string(getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir / "lib");
  writeFile(dir / "lib" / "shapes.pkn",
            "struct Pair<A, B> { a: A; b: B; }\n"
            "impl Pair<A, B> { view fn first() -> view A { return self.a; } }\n"
            "class Map<K, V> { k: K; v: V;\n"
            "  view fn key() -> view K { return self.k; } }\n"
            "fn makePair() -> Pair<(int, int) -> int, Str> {\n"
            "  return Pair<(int, int) -> int, Str>(fn(a: int, b: int) -> int "
            "{ return a * b; }, \"mul\"); }\n"
            "fn makeMap() -> Map<(int) -> int, Str> {\n"
            "  return Map<(int) -> int, Str>(fn(a: int) -> int { return a * "
            "2; }, \"dbl\"); }\n"
            "fn apply(p: view Pair<(int, int) -> int, Str>, x: int) -> int "
            "{ return p.a(x, x); }\n");
  writeFile(dir / "main.pkn",
            "import ::io; import lib::shapes;\n"
            "fn main() -> int {\n"
            "  p = shapes::makePair();\n"
            "  q = shapes::Pair(fn(x: int) -> int { return x + 1; }, 2);\n"
            "  m = shapes::makeMap();\n"
            "  r: shapes::Pair<(int, int) -> int, Str> = p;\n"
            "  io::println(Str(shapes::apply(p, 3)) + \" \" + p.b + \" \" + "
            "Str(q.a(q.b)) + \" \" + Str(m.key()(3)) + \" \" + m.v + \" "
            "\" + Str(r.first()(2, 2)));\n"
            "  return 0;\n}\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  auto src = paykan("--track-heap main.pkn");
  ASSERT_EQ(src.exitCode, 0) << src.out;
  EXPECT_TRUE(contains(src.out, "9 mul 3 6 dbl 4\n")) << src.out;
  EXPECT_TRUE(contains(src.out, "live blocks       : 0")) << src.out;
  fs::remove_all(dir);
}

// A struct template and a class template of one name in two modules: the
// second instantiation of `Pair<int, int>` conflicts with the first, in
// either order (a class after a struct stopped the compiler).
TEST(Pkm, StructAndClassInstantiationsOfOneNameConflict) {
  REQUIRE_BACKEND();
  auto dir = fs::temp_directory_path() /
             ("pkm_inst_clash_" + std::to_string(getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  writeFile(dir / "a.pkn", "struct Pair<A, B> { a: A; b: B; }\n");
  writeFile(dir / "b.pkn", "class Pair<A, B> { a: A; b: B; }\n");
  writeFile(dir / "main.pkn", "import a;\nimport b;\n"
                              "fn main() -> int {\n"
                              "  x = a::Pair(1, 2);\n"
                              "  y = b::Pair(1, 2);\n"
                              "  return x.a + y.b;\n}\n");
  writeFile(dir / "main2.pkn", "import a;\nimport b;\n"
                               "fn main() -> int {\n"
                               "  y = b::Pair(1, 2);\n"
                               "  x = a::Pair(1, 2);\n"
                               "  return x.a + y.b;\n}\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  for (const char *file : {"main.pkn", "main2.pkn"}) {
    auto r = paykan(file);
    EXPECT_NE(r.exitCode, 0) << file << "\n" << r.out;
    EXPECT_TRUE(contains(r.out, "instantiation 'Pair<int, int>' conflicts "
                                "with an imported type of the same name"))
        << file << "\n"
        << r.out;
  }
  fs::remove_all(dir);
}

// A `weak` field crosses modules and prebuilt `.pkm` files: the importer
// stores and reads it as a weak reference (the interface records the flag),
// so a tree built from another module's class is freed with its root.
TEST(Pkm, WeakFieldsAcrossModules) {
  REQUIRE_BACKEND();
  auto dir =
      fs::temp_directory_path() / ("pkm_weak_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "tree.pkn",
            "class Node {\n"
            "  name: Str;\n"
            "  weak parent: Node?;\n"
            "  first: Node?;\n"
            "  next: Node?;\n"
            "  fn add(child: Node) {\n"
            "    child.parent = self; child.next = self.first; self.first = "
            "child; }\n"
            "  fn path() -> Str {\n"
            "    match self.parent { p: Node { return p.path() + \"/\" + "
            "self.name; } None { return self.name; } } }\n"
            "}\n"
            "impl Node { fn __init__(name: Str) { self.name = name; "
            "self.parent = None; self.first = None; self.next = None; } }\n");
  writeFile(dir / "main.pkn",
            "import ::io; import tree;\n"
            "fn main() -> int {\n"
            "  root = tree::Node(\"root\");\n"
            "  a = tree::Node(\"a\");\n"
            "  root.add(a);\n"
            "  a.parent = root;\n"
            "  weak w = root;\n"
            "  io::print(a.path() + \" \");\n"
            "  root = tree::Node(\"other\");\n"
            "  io::print(a.path() + \" \");\n"
            "  match w { n: tree::Node { io::println(n.name); } None { "
            "io::println(\"gone\"); } }\n"
            "  return 0;\n}\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  const std::string expected = "root/a a gone\n";
  auto src = paykan("--track-heap main.pkn");
  ASSERT_EQ(src.exitCode, 0) << src.out;
  EXPECT_TRUE(contains(src.out, expected)) << src.out;
  EXPECT_TRUE(contains(src.out, "live blocks       : 0")) << src.out;
  ASSERT_EQ(paykan("--emit-pkm tree.pkn").exitCode, 0);
  fs::remove(dir / "tree.pkn");
  auto pkm = paykan("--track-heap main.pkn");
  ASSERT_EQ(pkm.exitCode, 0) << pkm.out;
  EXPECT_TRUE(contains(pkm.out, expected)) << pkm.out;
  EXPECT_TRUE(contains(pkm.out, "live blocks       : 0")) << pkm.out;
  auto dump = paykan("pkm dump --section=iface tree.pkm");
  for (const char *line :
       {"    field name: Str", "    weak field parent: Node?",
        "    field first: Node?"})
    EXPECT_TRUE(contains(dump.out, line)) << line << "\n" << dump.out;
  fs::remove_all(dir);
}

// A generic struct is a template like a generic class: its module, and a
// module that instantiates it, are cached, and the instantiation is one
// struct whoever makes it, from the sources and from the cache alike.
TEST(Pkm, GenericStructsAcrossModulesAreCached) {
  REQUIRE_BACKEND();
  fs::path dir = fs::temp_directory_path() /
                 ("pkm_generic_structs_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "lib" / "gen.pkn",
            "struct Pair<A, B> { a: A; b: B; }\n"
            "impl Pair<A, B> { view fn sum() -> int { return 1; } }\n"
            "fn mk() -> Pair<int, Str> { return Pair<int, Str>(2, \"m\"); }\n");
  writeFile(dir / "m1.pkn", "import lib::gen;\n"
                            "fn one() -> int { return gen::Pair(1, 2).sum(); "
                            "}\n");
  writeFile(dir / "m2.pkn", "import lib::gen;\n"
                            "fn make() -> gen::Pair<Str, int> { "
                            "return gen::Pair<Str, int>(\"m\", 2); }\n");
  writeFile(dir / "plain.pkn", "fn two() -> int { return 2; }\n");
  // m2's Pair<Str, int> is one struct with main's; gen's own Pair<int, Str>
  // (mk's result) is one with main's explicit instantiation of it.
  writeFile(
      dir / "main.pkn",
      "import ::io; import m1;\nimport m2;\nimport plain;\nimport lib::gen;\n"
      "fn main() -> int { p = m2::make(); p.b = p.b + 1;\n"
      "  q = gen::Pair<Str, int>(\"m\", 3);\n"
      "  r = gen::mk(); r.a = 0;\n"
      "  io::println(p.toString() + \" \" + Str(p == q) + \" \" +\n"
      "          Str(r == gen::Pair<int, Str>(0, \"m\")));\n"
      "  return m1::one() + plain::two() + p.b; }\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  for (int round = 0; round < 2; ++round) {
    auto r = paykan("--verbose --track-heap main.pkn");
    EXPECT_EQ(r.exitCode, 6) << r.out;
    EXPECT_TRUE(contains(r.out, "Pair<Str, int>(a: m, b: 3) True True\n"))
        << r.out;
    EXPECT_TRUE(contains(r.out, "live blocks       : 0")) << r.out;
    for (const char *m : {"lib::gen", "m1", "m2", "plain"})
      EXPECT_TRUE(contains(r.out, std::string("module ") + m +
                                      (round == 0 ? ": wrote" : ": cache")))
          << m << "\n"
          << r.out;
  }
  EXPECT_TRUE(fs::exists(dir / ".paykan_cache" / "m2.pkm"));
  fs::remove_all(dir);
}

// The sized numeric types (docs/design/sized-types.md) cross a module
// boundary from the source and from a `.pkm`: a struct with sized fields, a
// function taking and returning them, and `unsigned` through its spelling
// `u64` all keep their types, and a literal argument takes the parameter's
// sized type.
TEST(Pkm, SizedTypesCrossModulesAndPkm) {
  REQUIRE_BACKEND();
  auto dir =
      fs::temp_directory_path() / ("pkm_sized_" + std::to_string(getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir / "lib");
  writeFile(dir / "lib" / "pix.pkn",
            "struct Rgb { r: u8; g: u8; b: u8; }\n"
            "fn brightness(c: Rgb) -> u16 {\n"
            "  return u16(c.r) + u16(c.g) + u16(c.b); }\n"
            "fn big() -> u64 { return unsigned(-1); }\n"
            "fn half(x: f32) -> f32 { return x / 2; }\n"
            "fn wrap(n: i8) -> i8 { return n + 1; }\n"
            "fn tiny(x: f16) -> f16 { return x; }\n");
  writeFile(
      dir / "main.pkn",
      "import ::io; import lib::pix;\n"
      "fn main() -> int {\n"
      "  c = pix::Rgb(200, 200, 255);\n"
      "  io::println(Str(pix::brightness(c)) + \" \" + Str(pix::big()) + \" "
      "\"\n"
      "          + Str(pix::half(0.5)) + \" \" + Str(pix::wrap(127)) + \" \"\n"
      "          + Str(pix::tiny(0.1)) + \" \" + c.toString());\n"
      "  u: unsigned = pix::big() / 2;\n"
      "  io::println(Str(u));\n"
      "  return 0;\n}\n");
  writeFile(dir / "bad.pkn", "import lib::pix;\n"
                             "fn main() -> int {\n"
                             "  a = pix::wrap(128);\n"
                             "  b: int = pix::big();\n"
                             "  c = pix::half(1.5) + 1.0;\n"
                             "  d: float = c;\n"
                             "  return 0;\n}\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  auto check = [&](const char *when) {
    auto ok = paykan("--track-heap main.pkn");
    EXPECT_EQ(ok.exitCode, 0) << when << "\n" << ok.out;
    EXPECT_TRUE(contains(ok.out, "655 18446744073709551615 0.25 -128 "
                                 "0.0999756 Rgb(r: 200, g: 200, b: 255)\n"
                                 "9223372036854775807\n"))
        << when << "\n"
        << ok.out;
    EXPECT_TRUE(contains(ok.out, "live blocks       : 0")) << when << "\n"
                                                           << ok.out;
    auto bad = paykan("--check-only bad.pkn");
    EXPECT_NE(bad.exitCode, 0) << when;
    for (const char *line :
         {"the literal 128 does not fit in 'i8' (-128 to 127)",
          "initializer of type 'unsigned' does not match declared type 'int' "
          "for variable 'b'",
          "initializer of type 'f32' does not match declared type 'float' "
          "for variable 'd'"})
      EXPECT_TRUE(contains(bad.out, line)) << when << ": " << line << "\n"
                                           << bad.out;
  };
  check("from source");
  ASSERT_EQ(paykan("--emit-pkm lib/pix.pkn").exitCode, 0);
  fs::remove(dir / "lib" / "pix.pkn");
  check("from the .pkm");
  fs::remove_all(dir);
}

// -- Conformances across modules (docs/language/06-traits.md, "Hashable")

namespace {

/// lib::keys declares a struct, a class and a private struct with `impl
/// Hashable for` blocks and makes instantiations of its generic types;
/// lib::mid only returns keys' types and lib::lib2's template body uses one.
/// main uses each as a `HashMap`/`HashSet` key or a `T: Hashable` argument.
struct ConformanceProject {
  fs::path Dir;
  explicit ConformanceProject(const std::string &name) {
    Dir = fs::temp_directory_path() /
          ("pkm_" + name + "_" + std::to_string(getpid()));
    fs::remove_all(Dir);
    writeFile(Dir / "lib" / "keys.pkn", R"(import ::collections;
struct Key { a: int; b: int; }
impl Hashable for Key { view fn hash() -> int { return (self.a * 31 + self.b).hash(); } }
class CK {
  v: int;
  view fn equals(other: Obj) -> bool {
    match other {
      p: CK { return p.v == self.v; }
      _ { return False; }
    }
  }
}
impl CK { fn __init__(v: int) { self.v = v; } }
impl Hashable for CK { view fn hash() -> int { return self.v.hash(); } }
struct _Priv { n: int; }
impl Hashable for _Priv { view fn hash() -> int { return self.n.hash(); } }
fn distinct<T>(x: T) -> unsigned {
  s = collections::HashSet<_Priv>(); s.add(_Priv(1)); s.add(_Priv(1)); s.add(_Priv(2));
  return s.len();
}
trait Named { view fn name() -> Str; }
struct Keyed<K: Hashable> { k: K; }
impl Hashable for Keyed<K> { view fn hash() -> int { return self.k.hash(); } }
class GBox<T> { v: T; }
impl GBox<T> { fn __init__(v: T) { self.v = v; } }
impl Hashable for GBox<T> { view fn hash() -> int { return 3; } }
impl Named for GBox<T> { view fn name() -> Str { return "box"; } }
fn mkKeyed() -> Keyed<int> { return Keyed<int>(7); }
fn mkBox() -> GBox<int> { return GBox<int>(1); }
)");
    writeFile(Dir / "lib" / "mid.pkn", R"(import lib::keys;
fn mk() -> keys::Key { return keys::Key(1, 2); }
fn mkc() -> keys::CK { return keys::CK(4); }
)");
    writeFile(Dir / "lib" / "lib2.pkn", R"(import lib::keys;
import ::collections;
fn count<T>(x: T) -> unsigned {
  s = collections::HashSet<keys::Key>(); s.add(keys::Key(1, 2)); s.add(keys::Key(1, 2));
  return s.len();
}
)");
    writeFile(Dir / "main.pkn", R"(import ::io;
import ::collections;
import lib::keys;
import lib::mid;
import lib::lib2;
class Sub : keys::CK {
  view fn equals(other: Obj) -> bool { match other { p: Sub { return p.v == self.v; } _ { return False; } } }
}
impl Sub { fn __init__(v: int) { __super__(v); } }
fn h<T: Hashable>(x: view T) -> int { return x.hash(); }
fn main() -> int {
  m = collections::HashMap<keys::Key, int>();
  m.put(keys::Key(1, 2), 5);
  m.put(mid::mk(), 6);
  s = collections::HashSet<keys::CK>();
  s.add(keys::CK(3)); s.add(keys::CK(3)); s.add(mid::mkc());
  t = collections::HashSet<Sub>();
  t.add(Sub(1)); t.add(Sub(1));
  io::println(Str(m.len()) + " " + Str(s.len()) + " " + Str(t.len()));
  io::println(Str(h(keys::mkKeyed()) == h(7)));
  io::println(Str(h(keys::mkBox())) + " " + keys::mkBox().name());
  io::println(Str(keys::distinct(0)));
  io::println(Str(lib::lib2::count(0)));
  return 0;
}
)");
  }
  ~ConformanceProject() { fs::remove_all(Dir); }
  CmdResult paykan(const std::string &cmd, const std::string &args) const {
    return run("cd " + Dir.string() + " && " + cmd + " " + args);
  }
};

const char *kConformanceOutput = "1 2 1\nTrue\n3 box\n2\n1\n";

} // namespace

// A type's conformance to Hashable reaches main on every path a module
// comes from: its source with a cold and a warm cache, --no-module-cache,
// --rebuild-modules, prebuilt files, and a prebuilt keys under modules
// built from source; run, built at -O0 and -O2, and as C.
TEST(Pkm, ConformancesAcrossModules) {
  REQUIRE_BACKEND();
  ConformanceProject p("conformances");
  const char *libs[] = {"lib::keys", "lib::mid", "lib::lib2"};
  for (const std::string &be : runnableBackends()) {
    const std::string cmd = std::string(kPaykan) + backendFlag(be);
    // `paykan [flags] main.pkn` runs main without a leak, and its output.
    auto runs = [&](const std::string &flags, const std::string &how) {
      auto r = p.paykan(cmd, flags + " --track-heap main.pkn");
      EXPECT_EQ(r.exitCode, 0) << be << ", " << how << ":\n" << r.out;
      EXPECT_TRUE(contains(r.out, kConformanceOutput))
          << be << ", " << how << ":\n"
          << r.out;
      EXPECT_TRUE(contains(r.out, "live blocks       : 0"))
          << be << ", " << how << ":\n"
          << r.out;
      return r.out;
    };
    // `paykan build -O<n>` with @p flags, whose executable prints the same.
    auto builds = [&](const std::string &flags, const std::string &how) {
      for (const char *opt : {"-O0", "-O2"}) {
        const fs::path exe = p.Dir / ("prog" + std::string(opt));
        auto b = p.paykan(cmd, std::string(opt) + " " + flags + " build -o " +
                                   exe.string() + " main.pkn");
        ASSERT_EQ(b.exitCode, 0) << be << ", " << how << " " << opt << ":\n"
                                 << b.out;
        auto ran = run(exe.string());
        EXPECT_EQ(ran.exitCode, 0) << be << ", " << how << " " << opt;
        EXPECT_EQ(ran.out, kConformanceOutput)
            << be << ", " << how << " " << opt;
        fs::remove(exe);
      }
      auto c = p.paykan(cmd, flags + " --emit-c main.pkn");
      EXPECT_EQ(c.exitCode, 0) << be << ", " << how << " --emit-c:\n"
                               << c.out.substr(0, 4000);
    };

    fs::remove_all(p.Dir / ".paykan_cache");
    runs("", "cold cache");
    std::string warm = runs("--verbose", "warm cache");
    for (const char *m : libs)
      EXPECT_TRUE(contains(warm, std::string("module ") + m + ": cache "))
          << m << "\n"
          << warm;
    EXPECT_FALSE(contains(warm, "building from source")) << warm;
    builds("", "cache");
    runs("--no-module-cache", "--no-module-cache");
    runs("--rebuild-modules", "--rebuild-modules");

    // Prebuilt: every library a .pkm file on the module path, its source
    // gone.
    fs::remove_all(p.Dir / "pk");
    for (const char *m : {"keys", "mid", "lib2"}) {
      auto emit = p.paykan(cmd, std::string("--emit-pkm -o pk/lib/") + m +
                                    ".pkm lib/" + m + ".pkn");
      ASSERT_EQ(emit.exitCode, 0) << be << ": " << emit.out;
    }
    fs::rename(p.Dir / "lib", p.Dir / "src");
    fs::remove_all(p.Dir / ".paykan_cache");
    std::string pre = runs("--module-path=pk --verbose", "prebuilt");
    for (const char *m : libs)
      EXPECT_TRUE(contains(pre, std::string("module ") + m + ": prebuilt pk/"))
          << m << "\n"
          << pre;
    builds("--module-path=pk", "prebuilt");

    // Mixed: keys prebuilt beside the sources of mid and lib2.
    fs::rename(p.Dir / "src", p.Dir / "lib");
    fs::rename(p.Dir / "lib" / "keys.pkn", p.Dir / "keys.away");
    fs::copy_file(p.Dir / "pk" / "lib" / "keys.pkm",
                  p.Dir / "lib" / "keys.pkm");
    fs::remove_all(p.Dir / ".paykan_cache");
    std::string mixed = runs("--verbose", "mixed");
    EXPECT_TRUE(contains(mixed, "module lib::keys: prebuilt lib/keys.pkm"))
        << mixed;
    EXPECT_TRUE(builtFromSource(mixed, "lib::mid")) << mixed;
    fs::remove(p.Dir / "lib" / "keys.pkm");
    fs::rename(p.Dir / "keys.away", p.Dir / "lib" / "keys.pkn");
  }

  // keys' interface records the conformances of its types, its private
  // struct's and those of the instantiations it made, not a user trait's;
  // mid's records those of the types it describes again.
  auto keys = p.paykan(kPaykan, "pkm dump --section=iface pk/lib/keys.pkm");
  ASSERT_EQ(keys.exitCode, 0) << keys.out;
  EXPECT_TRUE(contains(keys.out, "  impl Hashable for CK\n"
                                 "  impl Hashable for GBox<int>\n"
                                 "  impl Hashable for Key\n"
                                 "  impl Hashable for Keyed<int>\n"
                                 "  impl Hashable for _Priv\n"))
      << keys.out;
  EXPECT_FALSE(contains(keys.out, "Named")) << keys.out;
  auto mid = p.paykan(kPaykan, "pkm dump --section=iface pk/lib/mid.pkm");
  ASSERT_EQ(mid.exitCode, 0) << mid.out;
  EXPECT_TRUE(contains(mid.out, "  struct Key [imported from \"lib::keys\"]"))
      << mid.out;
  EXPECT_TRUE(contains(mid.out, "  impl Hashable for Key\n")) << mid.out;
}

// The conformances are part of the interface: adding or removing one makes
// the importers' cache entries stale, and a prebuilt importer built against
// the other interface is rejected.
TEST(Pkm, AConformanceEditRebuildsTheImporter) {
  REQUIRE_BACKEND();
  auto dir =
      fs::temp_directory_path() / ("pkm_impl_edit_" + std::to_string(getpid()));
  fs::remove_all(dir);
  const std::string v1 =
      "struct Key { a: int; b: int; }\n"
      "impl Key { view fn hash() -> int { return self.a * 31 + self.b; } }\n";
  const std::string v2 = v1 + "impl Hashable for Key {}\n";
  writeFile(dir / "keys.pkn", v1);
  writeFile(dir / "mid.pkn", "import keys;\n"
                             "fn mk() -> keys::Key { return keys::Key(1, 2); "
                             "}\n");
  // ok compiles with either keys; main needs the conformance.
  writeFile(dir / "ok.pkn",
            "import mid;\nfn main() -> int { return mid::mk().hash(); }\n");
  writeFile(dir / "main.pkn",
            "import ::io;\nimport mid;\n"
            "fn h<T: Hashable>(x: view T) -> int { return x.hash(); }\n"
            "fn main() -> int { io::println(Str(h(mid::mk()))); return 0; "
            "}\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  const char *unmet =
      "'Key' does not implement trait 'Hashable', which 'h' requires of 'T'";
  ASSERT_EQ(paykan("ok.pkn").exitCode, 33);
  auto before = paykan("main.pkn");
  EXPECT_NE(before.exitCode, 0) << before.out;
  EXPECT_TRUE(contains(before.out, unmet)) << before.out;

  writeFile(dir / "keys.pkn", v2);
  auto added = paykan("--verbose main.pkn");
  EXPECT_EQ(added.exitCode, 0) << added.out;
  EXPECT_TRUE(contains(added.out, "33\n")) << added.out;
  EXPECT_TRUE(contains(added.out, "module mid: cache .paykan_cache/mid.pkm is "
                                  "stale (the interface of module 'keys' "
                                  "changed); building from source"))
      << added.out;

  writeFile(dir / "keys.pkn", v1);
  auto removed = paykan("--verbose main.pkn");
  EXPECT_NE(removed.exitCode, 0) << removed.out;
  EXPECT_TRUE(contains(removed.out, unmet)) << removed.out;
  EXPECT_TRUE(contains(removed.out, "module mid: cache .paykan_cache/mid.pkm "
                                    "is stale (the interface of module "
                                    "'keys' changed)"))
      << removed.out;

  // mid.pkm built against v2's keys, beside v1's keys.pkm.
  writeFile(dir / "keys.pkn", v2);
  ASSERT_EQ(paykan("--emit-pkm mid.pkn").exitCode, 0);
  writeFile(dir / "keys.pkn", v1);
  ASSERT_EQ(paykan("--emit-pkm keys.pkn").exitCode, 0);
  fs::remove(dir / "keys.pkn");
  fs::remove(dir / "mid.pkn");
  fs::remove_all(dir / ".paykan_cache");
  auto stale = paykan("main.pkn");
  EXPECT_NE(stale.exitCode, 0) << stale.out;
  EXPECT_TRUE(contains(stale.out, "error: module 'mid' (mid.pkm) was built "
                                  "against another interface of module "
                                  "'keys'. Rebuild it from source."))
      << stale.out;
  fs::remove_all(dir);
}

namespace {

/// Rewrite the IFACE section of the .pkm file at @p path with @p change.
void rewriteIface(const fs::path &path, const std::string &module,
                  const std::function<void(paykan::pkm::Interface &)> &change) {
  std::string bytes = slurp(path);
  auto file = paykan::pkm::File::read(std::span<const uint8_t>(
      reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()));
  ASSERT_TRUE(file) << file.status().message();
  auto iface = paykan::pkm::readInterface(
      (*file).section(paykan::pkm::Kind::Iface), module);
  ASSERT_TRUE(iface) << iface.status().message();
  paykan::pkm::Interface changed = *iface;
  change(changed);
  paykan::pkm::Writer w;
  for (const auto &e : (*file).sections()) {
    auto s = (*file).section(static_cast<paykan::pkm::Kind>(e.Kind));
    if (e.Kind == static_cast<uint32_t>(paykan::pkm::Kind::Iface))
      w.add(e.Kind, e.Flags, paykan::pkm::writeInterface(changed));
    else
      w.add(e.Kind, e.Flags, std::vector<uint8_t>(s.begin(), s.end()));
  }
  auto patched = w.finish();
  ASSERT_TRUE(patched) << patched.status().message();
  writeFile(path, std::string((*patched).begin(), (*patched).end()));
}

} // namespace

// A prebuilt file whose interface claims a conformance it cannot have: the
// reader rejects one to a trait an `impl` block cannot give, the importer
// one whose type lacks the trait's methods.
TEST(Pkm, AForgedConformanceIsRejected) {
  REQUIRE_BACKEND();
  auto dir = fs::temp_directory_path() /
             ("pkm_impl_forged_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "keys.pkn",
            "struct Key { a: int; }\n"
            "impl Hashable for Key { view fn hash() -> int { return self.a; } "
            "}\n"
            "struct Plain { a: int; }\n");
  writeFile(dir / "main.pkn",
            "import keys;\n"
            "fn h<T: Hashable>(x: view T) -> int { return x.hash(); }\n"
            "fn main() -> int { return h(keys::Key(5)); }\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  ASSERT_EQ(paykan("--emit-pkm keys.pkn").exitCode, 0);
  fs::remove(dir / "keys.pkn");
  ASSERT_EQ(paykan("main.pkn").exitCode, 5);
  const std::string pristine = slurp(dir / "keys.pkm");

  rewriteIface(dir / "keys.pkm", "keys", [](paykan::pkm::Interface &i) {
    ASSERT_EQ(i.Impls.size(), 1u);
    i.Impls[0].Trait = "Named";
  });
  const char *notBuiltin = "has a bad IFACE section: DECLS: 'impl Named for "
                           "Key': 'Named' is not a built-in trait an 'impl' "
                           "block implements";
  auto named = paykan("main.pkn");
  EXPECT_NE(named.exitCode, 0) << named.out;
  EXPECT_TRUE(contains(named.out, notBuiltin)) << named.out;
  auto check = paykan("pkm check keys.pkm");
  EXPECT_NE(check.exitCode, 0) << check.out;
  EXPECT_TRUE(contains(check.out, "DECLS: 'impl Named for Key': 'Named' is not "
                                  "a built-in trait an 'impl' block "
                                  "implements"))
      << check.out;

  writeFile(dir / "keys.pkm", pristine);
  rewriteIface(dir / "keys.pkm", "keys", [](paykan::pkm::Interface &i) {
    i.Impls.push_back({"Plain", "Hashable"});
  });
  auto plain = paykan("main.pkn");
  EXPECT_NE(plain.exitCode, 0) << plain.out;
  EXPECT_TRUE(contains(plain.out, "error: import of module 'keys': the "
                                  "interface says 'Plain' implements "
                                  "'Hashable', but it has no method 'view fn "
                                  "hash() -> int'"))
      << plain.out;
  fs::remove_all(dir);
}
