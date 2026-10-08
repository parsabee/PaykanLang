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

#include "paykan/pkm/File.h"
#include "paykan/pkm/Manifest.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    if ((name == "c" || name == "llvm") &&
        line.find("incompatible") == std::string::npos)
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
              "import mid;\nimport base;\n"
              "fn main() -> int {\n"
              "  c: base::Cell = base::Cell(40);\n"
              "  println(Str<int>(mid::mid()));\n"
              "  println(Str<int>(mid::pick(c) + c.twice()));\n"
              "  return 0;\n}\n");
  }
  ~Project() { fs::remove_all(Dir); }
  void writeBase(const std::string &value, const std::string &extra = "") {
    writeFile(Dir / "base.pkn", "enum Kind { Odd, Even }\n"
                                "class Cell { v: int;\n"
                                "  fn __init__(v: int) { self.v = v; }\n"
                                "  fn twice() -> int { return self.v * 2; } }\n"
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

bool contains(const std::string &text, const std::string &what) {
  return text.find(what) != std::string::npos;
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
  EXPECT_FALSE(contains(second.out, "building from source")) << second.out;
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
  EXPECT_FALSE(contains(again.out, "building from source")) << again.out;
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
            "  fn __init__(w: int, h: int) { self.w = w; self.h = h; }\n"
            "  fn area() -> int { return self.w * self.h; } }\n"
            "fn describe(r: Rect) -> Str { return \"rect\"; }\n");
  // A nested module named by its path from the project root.
  auto emit = run("cd " + p.Dir.string() + " && " + paykanCmd() +
                  " --emit-pkm geometry/shapes.pkn -o out/shapes.pkm");
  ASSERT_EQ(emit.exitCode, 0) << emit.out;
  auto dump = run(std::string(kPaykan) + " pkm dump " +
                  (p.Dir / "out" / "shapes.pkm").string());
  ASSERT_EQ(dump.exitCode, 0) << dump.out;
  for (const char *expected :
       {"pkm 1.0, 4 sections", "#0 MANIFEST", "#1 IFACE", "#2 CODE",
        "#3 SYMIDX", "module geometry::shapes", "contents 0x1 (HAS_CODE)",
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
  EXPECT_TRUE(contains(mainDump.out, "deps 2")) << mainDump.out;
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

/// `view` parameters in `base`, used and overridden by `main`.
struct ModesProject {
  fs::path Dir;
  explicit ModesProject(const std::string &name) {
    Dir = fs::temp_directory_path() /
          ("pkm_modes_" + name + "_" + std::to_string(getpid()));
    fs::remove_all(Dir);
    writeFile(Dir / "base.pkn",
              "class Counter { n: int;\n"
              "  fn __init__(view start: int) { self.n = start; }\n"
              "  fn add(view k: int) -> int { self.n = self.n + k;\n"
              "    return self.n; } }\n"
              "fn twice(view n: int) -> int { return n * 2; }\n");
    writeFile(Dir / "main.pkn",
              "import base;\n"
              "class Fast : base::Counter {\n"
              "  fn __init__(view s: int) { __super__(s); }\n"
              "  fn add(view k: int) -> int { self.n = self.n + 10 * k;\n"
              "    return self.n; } }\n"
              "fn main() -> int {\n"
              "  c: base::Counter = Fast(1);\n"
              "  println(Str(c.add(2)) + \" \" + Str(base::twice(3)));\n"
              "  return 0;\n}\n");
    writeFile(Dir / "bad.pkn", "import base;\n"
                               "class Slow : base::Counter {\n"
                               "  fn __init__(s: int) { __super__(s); }\n"
                               "  fn add(k: int) -> int { return k; } }\n"
                               "fn main() -> int { return 0; }\n");
  }
  ~ModesProject() { fs::remove_all(Dir); }
  CmdResult paykan(const std::string &args) const {
    return run("cd " + Dir.string() + " && " + paykanCmd() + " " + args);
  }
};

const char *kModesExpected = "21 6\n";

} // namespace

// The modes survive the source import, the cache entry and a prebuilt file:
// calls and overrides are checked against them.
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
      "bad.pkn:4:3: error: override of 'add' must keep 'view' on parameter 'k'";
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
       {"func twice(view int) -> int", "func Counter(view int) -> Counter",
        "method add(view int) -> int flags 0x0",
        "method __init__(view int) -> void flags 0x0"})
    EXPECT_TRUE(contains(dump.out, sig)) << sig << "\n" << dump.out;
  auto manifest = p.paykan("pkm dump --section=manifest base.pkm");
  EXPECT_TRUE(contains(manifest.out, "format_versions iface 1.1"))
      << manifest.out;
}
