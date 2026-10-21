// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
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
  // (The standard library's ::io and ::result hold templates, which a .pkm
  // does not carry yet (#160): they are built from source every time.)
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

/// A prebuilt module is judged by its version alone: one written by another
/// build of an accepted version is used.
TEST(Pkm, PrebuiltModuleOfAnotherBuildIsUsed) {
  REQUIRE_BACKEND();
  Project p("build_id_prebuilt");
  ASSERT_EQ(run("cd " + p.Dir.string() + " && " + paykanCmd() +
                " --emit-pkm base.pkn")
                .exitCode,
            0);
  rewriteManifest(p.Dir / "base.pkm",
                  [](paykan::pkm::Manifest &m) { m.Core.Build = "elsewhere"; });
  fs::remove(p.Dir / "base.pkn");
  auto r = p.runMain();
  ASSERT_EQ(r.exitCode, 0) << r.out;
  EXPECT_TRUE(contains(r.out, kExpected)) << r.out;
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
      "pkm " + paykan::versionString(paykan::kVersionNumber) + ", 4 sections";
  EXPECT_TRUE(contains(dump.out, header)) << dump.out;
  for (const char *expected :
       {"#0 MANIFEST", "#1 IFACE", "#2 CODE", "#3 SYMIDX",
        "module geometry::shapes", "contents 0x1 (HAS_CODE)",
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
            "class Box { c: Counter;\n"
            "  view fn peek() -> view Counter { return self.c; }\n"
            "  fn count() -> inout int { return self.c.n; } }\n"
            "impl Box { fn __init__() { self.c = Counter(); } }\n"
            "fn longer(a: view Str, b: view Str) -> view Str {\n"
            "  return if a.len() > b.len() then a else b; }\n");
  writeFile(dir / "main.pkn", "import ::io; import base;\n"
                              "fn main() -> int {\n"
                              "  b = base::Box();\n"
                              "  n: inout = b.count();\n"
                              "  n = n + 2;\n"
                              "  v: view = b.peek();\n"
                              "  io::println(base::longer(\"ab\", \"c\") + "
                              "Str(v.get()));\n"
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
  EXPECT_TRUE(contains(dump.out, "contents 0x3 (HAS_CODE,HAS_TEMPLATES)"))
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
  for (const char *m : {"::io", "::result", "::memory", "::collections"})
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
