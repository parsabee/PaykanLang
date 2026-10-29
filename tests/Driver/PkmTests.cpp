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
#include "paykan/pir/Binary.h"
#include "paykan/pir/PIR.h"
#include "paykan/pkm/File.h"
#include "paykan/pkm/Interface.h"
#include "paykan/pkm/Manifest.h"
#include "paykan/pkm/Object.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/stat.h>
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

/// `run`, but @p cmd and every process it starts are killed once it has run
/// for @p seconds, so a command that blocks or reads for good fails the test
/// instead of hanging it.  macOS has no `timeout` command, and a CPU limit
/// would never stop a process blocked on a read.
CmdResult runFor(const std::string &cmd, int seconds) {
  int out[2];
  if (pipe(out) != 0)
    return {-1, "pipe failed"};
  const pid_t pid = fork();
  if (pid < 0)
    return {-1, "fork failed"};
  if (pid == 0) {
    setpgid(0, 0); // a group of its own, which the kill below reaches whole
    dup2(out[1], STDOUT_FILENO);
    dup2(out[1], STDERR_FILENO);
    close(out[0]);
    close(out[1]);
    execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char *>(nullptr));
    _exit(127);
  }
  setpgid(pid, pid); // whichever of the two runs first
  close(out[1]);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  std::string text;
  std::array<char, 4096> buf{};
  bool killed = false;
  for (;;) {
    int wait = -1; // once killed, the pipe closes when the group is gone
    if (!killed) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (left.count() > 0) {
        wait = static_cast<int>(left.count());
      } else {
        kill(-pid, SIGKILL);
        killed = true;
      }
    }
    pollfd ready{};
    ready.fd = out[0];
    ready.events = POLLIN;
    const int n = poll(&ready, 1, wait);
    if (n == 0 || (n < 0 && errno == EINTR))
      continue;
    if (n < 0)
      break;
    const ssize_t got = read(out[0], buf.data(), buf.size());
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      break;
    text.append(buf.data(), static_cast<size_t>(got));
  }
  close(out[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (killed)
    text += "(killed after " + std::to_string(seconds) + " s)\n";
  return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, text};
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
  /// `runMain`, killed after @p seconds (`runFor`).
  CmdResult runMainFor(int seconds, const std::string &flags = "") const {
    return runFor("cd " + Dir.string() + " && " + paykanCmd() + " " + flags +
                      " main.pkn",
                  seconds);
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

/// Rewrite section @p kind of the .pkm file at @p path with @p change, the
/// section and table hashes made to match: what a file damaged past what
/// the container checks looks like.
void rewriteSection(const fs::path &path, paykan::pkm::Kind kind,
                    const std::function<void(std::vector<uint8_t> &)> &change) {
  std::string bytes = slurp(path);
  auto file = paykan::pkm::File::read(std::span<const uint8_t>(
      reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()));
  ASSERT_TRUE(file) << file.status().message();
  paykan::pkm::Writer w;
  bool found = false;
  for (const auto &e : (*file).sections()) {
    auto s = (*file).section(static_cast<paykan::pkm::Kind>(e.Kind));
    std::vector<uint8_t> section(s.begin(), s.end());
    if (e.Kind == static_cast<uint32_t>(kind)) {
      change(section);
      found = true;
    }
    w.add(e.Kind, e.Flags, std::move(section));
  }
  ASSERT_TRUE(found) << path << " has no section " << static_cast<int>(kind);
  auto patched = w.finish();
  ASSERT_TRUE(patched) << patched.status().message();
  writeFile(path, std::string((*patched).begin(), (*patched).end()));
}

/// An OBJ section whose compiled code is cut in half: its framing reads,
/// its bitcode does not.
void truncateCompiledCode(std::vector<uint8_t> &section) {
  auto object = paykan::pkm::readObject(section);
  ASSERT_TRUE(object) << object.status().message();
  (*object).Bytes.resize((*object).Bytes.size() / 2);
  section = paykan::pkm::writeObject(*object);
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

// The mlir backend's bitcode cache entries carry the SHA-256 of their
// bitcode: one damaged byte, which LLVM's bitcode reader is not made for (a
// crash, an assertion or `LLVM ERROR: Broken module found` at these
// offsets of the entry this program writes when the file is the bare
// bitcode), makes the entry a miss, translated and written again, and
// --verbose says why.
TEST(Pkm, DamagedBitcodeCacheEntryIsTranslatedAgain) {
  REQUIRE_BACKEND();
  Project p("bitcode");
  writeFile(p.Dir / "main.pkn", "import ::io;\n"
                                "fn main() -> int {\n"
                                "  io::println(\"hi\");\n"
                                "  return 42;\n"
                                "}\n");
  ASSERT_EQ(p.runMain().exitCode, 42);
  const fs::path bc = p.cache() / "@system" / "io.bc";
  ASSERT_TRUE(fs::is_regular_file(bc));
  const std::string pristine = slurp(bc);
  ASSERT_GT(pristine.size(), 1385u);
  const std::string miss = "module ::io: bitcode cache "
                           ".paykan_cache/@system/io.bc is corrupt (";
  for (size_t offset : {275u, 1370u, 1385u}) {
    std::string bytes = pristine;
    bytes[offset] = bytes[offset] ? '\0' : '\xff';
    writeFile(bc, bytes);
    auto r = p.runMain("--verbose");
    EXPECT_EQ(r.exitCode, 42) << offset << ": " << r.out;
    EXPECT_TRUE(contains(r.out, "hi\n")) << offset << ": " << r.out;
    EXPECT_TRUE(contains(r.out, miss + "its bitcode does not match its hash); "
                                       "translating it again\n"))
        << offset << ": " << r.out;
    EXPECT_EQ(slurp(bc), pristine) << offset;
  }
  writeFile(bc, "not bitcode");
  auto r = p.runMain("--verbose");
  EXPECT_EQ(r.exitCode, 42) << r.out;
  EXPECT_TRUE(contains(r.out, miss + "not a bitcode cache entry); "
                                     "translating it again\n"))
      << r.out;
  // The entry written in its place is used, without a word.
  r = p.runMain("--verbose");
  EXPECT_EQ(r.exitCode, 42) << r.out;
  EXPECT_FALSE(contains(r.out, "bitcode cache")) << r.out;
}

// A bitcode cache entry that is no regular file is not read but translated
// again, and the entry written in its place is used: a FIFO blocked the
// compile for good, and a symbolic link to /dev/zero was read until LLVM
// ran out of memory (`LLVM ERROR: out of memory`).  An entry an older
// paykan wrote, bitcode with no header, is stale, not corrupt.
TEST(Pkm, BitcodeCacheEntryThatIsNoRegularFileIsTranslatedAgain) {
  REQUIRE_BACKEND();
  Project p("bitcodefifo");
  writeFile(p.Dir / "main.pkn", "import ::io;\n"
                                "fn main() -> int {\n"
                                "  io::println(\"hi\");\n"
                                "  return 42;\n"
                                "}\n");
  ASSERT_EQ(p.runMain().exitCode, 42);
  const fs::path bc = p.cache() / "@system" / "io.bc";
  const std::string pristine = slurp(bc);
  const std::string entry = "module ::io: bitcode cache "
                            ".paykan_cache/@system/io.bc ";
  // Bounded, so a compile that blocks or reads for good fails the test.
  auto translatedAgain = [&](const std::string &what, const std::string &why) {
    SCOPED_TRACE(what);
    auto r = p.runMainFor(60, "--verbose");
    EXPECT_EQ(r.exitCode, 42) << r.out;
    EXPECT_TRUE(contains(r.out, entry + why + "; translating it again\n"))
        << r.out;
    EXPECT_TRUE(fs::is_regular_file(fs::symlink_status(bc)));
    r = p.runMainFor(60, "--verbose");
    EXPECT_EQ(r.exitCode, 42) << r.out;
    EXPECT_FALSE(contains(r.out, "bitcode cache")) << r.out;
  };
  fs::remove(bc);
  ASSERT_EQ(mkfifo(bc.c_str(), 0600), 0);
  translatedAgain("a FIFO", "is corrupt (not a regular file)");
  fs::remove(bc);
  fs::create_symlink("/dev/zero", bc);
  translatedAgain("/dev/zero", "is corrupt (not a regular file)");
  const size_t bitcode = pristine.find("BC\xC0\xDE");
  ASSERT_NE(bitcode, std::string::npos);
  writeFile(bc, pristine.substr(bitcode));
  translatedAgain("no header",
                  "is stale (an older paykan wrote it, with no header)");
}

// A cache entry whose compiled code (OBJ) does not read is rebuilt, though
// every hash of the file matches: the backend reads the code it would link
// when the entry is loaded, before anything is compiled.
TEST(Pkm, CacheEntryWhoseCompiledCodeDoesNotReadIsRebuilt) {
  REQUIRE_BACKEND();
  Project p("objcache");
  ASSERT_EQ(p.runMain().exitCode, 0);
  rewriteSection(p.entry("base"), paykan::pkm::Kind::Object,
                 truncateCompiledCode);
  auto r = p.runMain("--verbose");
  ASSERT_EQ(r.exitCode, 0) << r.out;
  EXPECT_TRUE(contains(r.out, kExpected)) << r.out;
  EXPECT_TRUE(contains(r.out, "module base: cache .paykan_cache/base.pkm has "
                              "a bad OBJ section: the compiled code does not "
                              "read: "))
      << r.out;
  EXPECT_TRUE(builtFromSource(r.out, "base")) << r.out;
  auto again = p.runMain("--verbose");
  ASSERT_EQ(again.exitCode, 0) << again.out;
  EXPECT_FALSE(builtFromSource(again.out, "base")) << again.out;
}

// A cache entry whose CODE does not decode is rebuilt, though every hash
// of the file matches: CODE is decoded and verified when the entry is
// loaded, not when the program is lowered (which went on without the
// module and stopped at an assertion).
TEST(Pkm, CacheEntryWhoseCodeDoesNotDecodeIsRebuilt) {
  REQUIRE_BACKEND();
  Project p("codecache");
  ASSERT_EQ(p.runMain().exitCode, 0);
  rewriteSection(p.entry("base"), paykan::pkm::Kind::Code,
                 [](std::vector<uint8_t> &code) {
                   std::fill(code.begin(), code.end(), 0);
                 });
  auto r = p.runMain("--verbose");
  ASSERT_EQ(r.exitCode, 0) << r.out;
  EXPECT_TRUE(contains(r.out, kExpected)) << r.out;
  EXPECT_TRUE(contains(r.out, "module base: cache .paykan_cache/base.pkm has "
                              "a bad CODE section: binary PIR: bad magic at "
                              "byte 0; building from source\n"))
      << r.out;
}

// A cache entry whose bytes pass its hashes but not what reads them later is
// rebuilt too, as a damaged entry is promised: a TMPL section that does not
// decode was an error at the import on every run (after `--verbose` called
// the entry usable), and a dependency renamed to a module there is none of
// was `module 'bsae' not found` at the import of the module the entry is of.
TEST(Pkm, CacheEntryDamagedPastItsHashesIsRebuilt) {
  REQUIRE_BACKEND();
  Project p("pasthashes");
  p.writeBase("1", "fn ident<T>(x: T) -> T { return x; }\n");
  ASSERT_EQ(p.runMain().exitCode, 0);
  auto rebuilt = [&](const std::string &module, const std::string &why) {
    SCOPED_TRACE(module);
    auto r = p.runMain("--verbose");
    EXPECT_EQ(r.exitCode, 0) << r.out;
    EXPECT_TRUE(contains(r.out, kExpected)) << r.out;
    EXPECT_TRUE(contains(r.out, "module " + module + ": cache .paykan_cache/" +
                                    module + ".pkm " + why))
        << r.out;
    EXPECT_TRUE(builtFromSource(r.out, module)) << r.out;
    r = p.runMain("--verbose");
    EXPECT_EQ(r.exitCode, 0) << r.out;
    EXPECT_FALSE(builtFromSource(r.out, module)) << r.out;
  };
  rewriteSection(
      p.entry("base"), paykan::pkm::Kind::Tmpl, [](std::vector<uint8_t> &tmpl) {
        const std::string name = "ident";
        auto at =
            std::search(tmpl.begin(), tmpl.end(), name.begin(), name.end());
        ASSERT_NE(at, tmpl.end());
        *at = 0xff;
      });
  rebuilt("base",
          "has a bad TMPL section: TMPL: string is not valid UTF-8 (at byte ");
  rewriteManifest(p.entry("mid"), [](paykan::pkm::Manifest &m) {
    for (auto &d : m.Deps)
      if (d.Name == "base")
        d.Name = "bsae";
  });
  rebuilt("mid", "depends on module 'bsae', which is not found; building "
                 "from source\n");
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

// A slice's elements copied to the heap are warned about when the code is
// compiled (docs/language/02-basics.md, "Evaluation order"): a module, when
// it is compiled from its source, and not when its cache entry or its
// prebuilt file is read; an instantiation of its generic function, with
// the module that uses it, once though two modules do (`main` and
// `other`), pointing into the template's file, whose lines a module file
// does not hold, and naming the module's functions as that file does,
// unqualified.  `--check-only` compiles nothing and warns of nothing.
TEST(Pkm, ACopiedSliceIsWarnedAboutWhereItsModuleIsCompiled) {
  REQUIRE_BACKEND();
  const fs::path dir =
      fs::temp_directory_path() / ("pkm_slicecopy_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "lib/slices.pkn",
            "class C { a: int[3]; }\n"
            "fn reset(c: C) -> int { c.a = [9, 9, 9]; return 0; }\n"
            "fn total(xs: view int[], x: int) -> int => xs[0];\n"
            "fn plain(xs: view int[], c: C) -> int => total(xs, reset(c));\n"
            "fn first<T>(xs: view T[], x: int) -> int => xs.len() + x;\n"
            "fn generic<T>(xs: view T[], c: C) -> int => first(xs, "
            "reset(c));\n");
  writeFile(dir / "lib/other.pkn",
            "import lib::slices;\n"
            "fn more(c: slices::C) -> int => slices::generic(c.a, c);\n");
  writeFile(dir / "main.pkn",
            "import ::io;\nimport lib::slices;\nimport lib::other;\n"
            "fn main() -> int {\n"
            "  let c = slices::C([1, 2, 3]);\n"
            "  let n = slices::plain(c.a, c);\n"
            "  io::println(Str(n) + \" \" + Str(slices::generic(c.a, c) + "
            "other::more(c)));\n"
            "  return 0;\n}\n");
  writeFile(dir / "plain.pkn", "import ::io;\nimport lib::slices;\n"
                               "fn main() -> int {\n"
                               "  let c = slices::C([1, 2, 3]);\n"
                               "  io::println(Str(slices::plain(c.a, c)));\n"
                               "  return 0;\n}\n");
  auto in = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  const std::string copied = " in a later argument may write the elements of "
                             "slice 'xs': they are copied to the heap before "
                             "the call to ";
  const std::string plain =
      "lib/slices.pkn:4:48: warning: 'reset'" + copied +
      "'total'\n"
      "  3 | fn total(xs: view int[], x: int) -> int => xs[0];\n"
      "  4 | fn plain(xs: view int[], c: C) -> int => total(xs, reset(c));\n"
      "    |                                                ^~\n"
      "  5 | fn first<T>(xs: view T[], x: int) -> int => xs.len() + x;\n";
  const std::string generic =
      "lib/slices.pkn:6:51: warning: 'reset'" + copied + "'first<int>'\n";
  const std::string genericLines =
      "  5 | fn first<T>(xs: view T[], x: int) -> int => xs.len() + x;\n"
      "  6 | fn generic<T>(xs: view T[], c: C) -> int => first(xs, "
      "reset(c));\n"
      "    |                                                   ^~\n";
  // Built from source: both; from the cache entries, the instantiation
  // `main` makes, whose template comes from the entry.
  auto r = in("main.pkn");
  EXPECT_EQ(r.exitCode, 0) << r.out;
  EXPECT_EQ(r.out, plain + generic + genericLines + "1 6\n");
  r = in("main.pkn");
  EXPECT_EQ(r.exitCode, 0) << r.out;
  EXPECT_EQ(r.out, generic + "1 6\n");
  r = in("--rebuild-modules main.pkn");
  EXPECT_EQ(r.exitCode, 0) << r.out;
  EXPECT_EQ(r.out, plain + generic + genericLines + "1 6\n");
  r = in("--check-only main.pkn");
  EXPECT_EQ(r.exitCode, 0) << r.out;
  EXPECT_EQ(r.out, "");
  // A prebuilt module file: written from source, with the warning; read,
  // without one.
  fs::create_directories(dir / "pre/lib");
  r = in("--emit-pkm lib/slices.pkn -o pre/lib/slices.pkm");
  EXPECT_EQ(r.exitCode, 0) << r.out;
  EXPECT_EQ(r.out, plain);
  fs::remove(dir / "lib/slices.pkn");
  r = in("--module-path=pre plain.pkn");
  EXPECT_EQ(r.exitCode, 0) << r.out;
  EXPECT_EQ(r.out, "1\n");
  r = in("--module-path=pre --rebuild-modules main.pkn");
  EXPECT_EQ(r.exitCode, 0) << r.out;
  EXPECT_EQ(r.out, generic + "1 6\n");
  fs::remove_all(dir);
}

// In an instantiation of another module's template, the warning names that
// module's own functions unqualified, as its file does, and another
// module's by that module's full name, as an error in the instantiation
// does: a submodule's too, whose name starts with the template's module's
// (`lib::sl::inner::wipe`, not `inner::wipe`).  The file's plain function
// names them as it writes them.  The same with the template read from its
// module's cache entry.
TEST(Pkm, ACopiedSliceInAnInstantiationNamesAnotherModulesFunctionsInFull) {
  REQUIRE_BACKEND();
  const fs::path dir = fs::temp_directory_path() /
                       ("pkm_slicenames_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "lib/sl/inner.pkn",
            "class C { a: int[3]; }\n"
            "fn wipe(c: C) -> int { c.a = [0, 0, 0]; return 0; }\n"
            "fn total(xs: view int[], x: int) -> int => xs[0] + x;\n");
  writeFile(dir / "lib/sl.pkn",
            "import lib::sl::inner as q;\n"
            "fn own(xs: view int[], x: int) -> int => xs[0] + x;\n"
            "fn plain(xs: view int[], c: q::C) -> int => q::total(xs, "
            "q::wipe(c));\n"
            "fn gen<T>(xs: view int[], c: q::C, t: T) -> int => q::total(xs, "
            "q::wipe(c));\n"
            "fn mine<T>(xs: view int[], c: q::C, t: T) -> int => own(xs, "
            "q::wipe(c));\n");
  writeFile(dir / "main.pkn",
            "import ::io;\nimport lib::sl;\nimport lib::sl::inner;\n"
            "fn main() -> int {\n"
            "  let c = inner::C([1, 2, 3]);\n"
            "  let a = sl::plain(c.a, c);\n"
            "  c.a = [1, 2, 3];\n"
            "  let b = sl::gen(c.a, c, 1);\n"
            "  c.a = [1, 2, 3];\n"
            "  io::println(Str(a) + \" \" + Str(b) + \" \" + "
            "Str(sl::mine(c.a, c, 1)));\n"
            "  return 0;\n}\n");
  auto in = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  const std::string copied = " in a later argument may write the elements of "
                             "slice 'xs': they are copied to the heap before "
                             "the call to ";
  const std::string line3 = "fn plain(xs: view int[], c: q::C) -> int => "
                            "q::total(xs, q::wipe(c));\n";
  const std::string line4 = "fn gen<T>(xs: view int[], c: q::C, t: T) -> int "
                            "=> q::total(xs, q::wipe(c));\n";
  const std::string line5 = "fn mine<T>(xs: view int[], c: q::C, t: T) -> int "
                            "=> own(xs, q::wipe(c));\n";
  const std::string plain =
      "lib/sl.pkn:3:54: warning: 'q::wipe'" + copied + "'q::total'\n" +
      "  2 | fn own(xs: view int[], x: int) -> int => xs[0] + x;\n" + "  3 | " +
      line3 + "    | " + std::string(53, ' ') + "^~\n" + "  4 | " + line4;
  const std::string gen = "lib/sl.pkn:4:61: warning: 'lib::sl::inner::wipe'" +
                          copied + "'lib::sl::inner::total'\n";
  const std::string genLines = "  3 | " + line3 + "  4 | " + line4 + "    | " +
                               std::string(60, ' ') + "^~\n" + "  5 | " + line5;
  const std::string mine =
      "lib/sl.pkn:5:57: warning: 'lib::sl::inner::wipe'" + copied + "'own'\n";
  const std::string mineLines = "  4 | " + line4 + "  5 | " + line5 + "    | " +
                                std::string(56, ' ') + "^~\n";
  auto r = in("main.pkn");
  EXPECT_EQ(r.exitCode, 0) << r.out;
  EXPECT_EQ(r.out, plain + gen + genLines + mine + mineLines + "1 1 1\n");
  r = in("main.pkn");
  EXPECT_EQ(r.exitCode, 0) << r.out;
  EXPECT_EQ(r.out, gen + mine + "1 1 1\n");
  fs::remove_all(dir);
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

// A prebuilt module whose compiled code (OBJ) does not read is rejected
// where it is imported, with the other reasons a file is unusable, by
// every command that would link it: the backend reads it when the file is
// loaded, not when the program is linked.
TEST(Pkm, PrebuiltModuleWhoseCompiledCodeDoesNotReadIsRejected) {
  REQUIRE_BACKEND();
  Project p("objprebuilt");
  ASSERT_EQ(run("cd " + p.Dir.string() + " && " + paykanCmd() +
                " --emit-pkm base.pkn")
                .exitCode,
            0);
  fs::remove(p.Dir / "base.pkn");
  rewriteSection(p.Dir / "base.pkm", paykan::pkm::Kind::Object,
                 truncateCompiledCode);
  for (const char *cmd : {"", "--emit-llvm", "build -o prog"}) {
    auto r = p.runMain(cmd);
    EXPECT_EQ(r.exitCode, 1) << cmd << ": " << r.out;
    EXPECT_TRUE(contains(r.out, "error: module 'base' (base.pkm) has a bad "
                                "OBJ section: the compiled code does not "
                                "read: "))
        << cmd << ": " << r.out;
    EXPECT_TRUE(contains(r.out, ". Rebuild it from source.")) << cmd;
  }
  EXPECT_FALSE(fs::exists(p.Dir / "prog"));
}

// A prebuilt module whose CODE does not decode is rejected where it is
// imported, by every command, `--check-only` too: CODE is decoded when the
// file is loaded (the lowering went on without the module and stopped at
// an assertion, and `--check-only` accepted it).
TEST(Pkm, PrebuiltModuleWhoseCodeDoesNotDecodeIsRejected) {
  REQUIRE_BACKEND();
  Project p("codeprebuilt");
  ASSERT_EQ(run("cd " + p.Dir.string() + " && " + paykanCmd() +
                " --emit-pkm base.pkn")
                .exitCode,
            0);
  fs::remove(p.Dir / "base.pkn");
  rewriteSection(p.Dir / "base.pkm", paykan::pkm::Kind::Code,
                 [](std::vector<uint8_t> &code) {
                   std::fill(code.begin(), code.end(), 0);
                 });
  for (const char *cmd :
       {"", "--check-only", "--emit-pir", "--emit-c", "build -o prog"}) {
    auto r = p.runMain(cmd);
    EXPECT_EQ(r.exitCode, 1) << cmd << ": " << r.out;
    EXPECT_TRUE(contains(r.out, "error: module 'base' (base.pkm) has a bad "
                                "CODE section: binary PIR: bad magic at byte "
                                "0. Rebuild it from source.\n"))
        << cmd << ": " << r.out;
    EXPECT_FALSE(contains(r.out, "internal compiler error")) << cmd;
  }
}

TEST(Pkm, PrebuiltModulesAreFoundOnTheModulePath) {
  REQUIRE_BACKEND();
  Project p("modulepath");
  fs::create_directories(p.Dir / "lib");
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
  fs::create_directories(p.Dir / "out");
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
  EXPECT_EQ(junk.exitCode, 1);
  EXPECT_EQ(junk.out.rfind((p.Dir / "junk.pkm").string() + ": corrupt: ", 0),
            0u)
      << junk.out;
  auto none = run(std::string(kPaykan) + " pkm check " +
                  (p.Dir / "missing.pkm").string());
  EXPECT_NE(none.exitCode, 0);
  auto noVerb =
      run(std::string(kPaykan) + " pkm " + (p.Dir / "base.pkm").string());
  EXPECT_NE(noVerb.exitCode, 0);
  EXPECT_TRUE(contains(noVerb.out, "unknown pkm command")) << noVerb.out;
}

// `pkm check` reads a file as an importer does, and says `usable` only
// once every section a program would read reads: IFACE, CODE (decoded,
// verified, defining what IFACE says), TMPL and OBJ (read and verified by
// the backend that would link it).  It printed `usable` for a file whose
// CODE, TMPL or OBJ the compiler rejects (or crashes on), and printed it
// before it decoded anything.
TEST(Pkm, CheckReadsEverySectionAProgramReads) {
  REQUIRE_BACKEND();
  Project p("checksections");
  writeFile(p.Dir / "gen.pkn", "fn ident<T>(x: T) -> T { return x; }\n"
                               "fn one() -> int { return ident(1); }\n");
  for (const char *module : {"base", "gen"})
    ASSERT_EQ(run("cd " + p.Dir.string() + " && " + paykanCmd() +
                  " --emit-pkm " + module + ".pkn")
                  .exitCode,
              0)
        << module;
  const std::string base = slurp(p.Dir / "base.pkm");
  auto check = [&](const std::string &file) {
    return run(std::string(kPaykan) + " pkm check " + (p.Dir / file).string());
  };
  auto corrupt = [&](const std::string &file, const std::string &why) {
    auto r = check(file);
    EXPECT_EQ(r.exitCode, 1) << file << ": " << r.out;
    EXPECT_EQ(r.out.rfind((p.Dir / file).string() + ": corrupt: " + why, 0), 0u)
        << file << ": " << r.out;
    EXPECT_FALSE(contains(r.out, "usable")) << file << ": " << r.out;
  };
  ASSERT_EQ(check("gen.pkm").out, (p.Dir / "gen.pkm").string() + ": usable\n");
  rewriteSection(p.Dir / "base.pkm", paykan::pkm::Kind::Code,
                 [](std::vector<uint8_t> &code) {
                   std::fill(code.begin(), code.end(), 0);
                 });
  corrupt("base.pkm",
          "has a bad CODE section: binary PIR: bad magic at byte 0\n");
  writeFile(p.Dir / "base.pkm", base);
  rewriteSection(p.Dir / "base.pkm", paykan::pkm::Kind::Object,
                 truncateCompiledCode);
  corrupt("base.pkm",
          "has a bad OBJ section: the compiled code does not read: ");
  // A template's name made invalid UTF-8.
  rewriteSection(p.Dir / "gen.pkm", paykan::pkm::Kind::Tmpl,
                 [](std::vector<uint8_t> &tmpl) {
                   const std::string name = "ident";
                   auto at = std::search(tmpl.begin(), tmpl.end(), name.begin(),
                                         name.end());
                   ASSERT_NE(at, tmpl.end());
                   *at = 0xff;
                 });
  corrupt("gen.pkm", "has a bad TMPL section: TMPL: string is not valid "
                     "UTF-8");
}

// `pkm check` judges the compiled code by the backend a program would link
// it with, as `run` and `build` do: an unknown --backend (a typo such as
// `MLIR`) is an error, as it is for `pkm extract`, and code another backend
// or target compiled is rejected, as the link rejects it.  Both said
// `usable`, the first without reading the code at all.
TEST(Pkm, CheckJudgesTheCodeByTheBackendThatLinksIt) {
  REQUIRE_BACKEND();
  Project p("checkbackend");
  ASSERT_EQ(run("cd " + p.Dir.string() + " && " + paykanCmd() +
                " --emit-pkm base.pkn")
                .exitCode,
            0);
  const fs::path file = p.Dir / "base.pkm";
  rewriteSection(file, paykan::pkm::Kind::Object, truncateCompiledCode);
  for (const char *name : {"nosuch", "MLIR"}) {
    auto r = run(std::string(kPaykan) + " pkm check --backend=" + name + " " +
                 file.string());
    EXPECT_EQ(r.exitCode, 1) << name << ": " << r.out;
    EXPECT_EQ(r.out, "paykan: unknown backend '" + std::string(name) + "'\n");
  }
  ASSERT_EQ(run("cd " + p.Dir.string() + " && " + paykanCmd() +
                " --emit-pkm base.pkn")
                .exitCode,
            0);
  paykan::pkm::Object made;
  rewriteSection(file, paykan::pkm::Kind::Object,
                 [&](std::vector<uint8_t> &section) {
                   auto object = paykan::pkm::readObject(section);
                   ASSERT_TRUE(object) << object.status().message();
                   made = *object;
                   (*object).Target = "other-" + (*object).Target;
                   section = paykan::pkm::writeObject(*object);
                 });
  auto r = run(std::string(kPaykan) + " pkm check " + file.string());
  EXPECT_EQ(r.exitCode, 1) << r.out;
  EXPECT_EQ(r.out, file.string() +
                       ": rejected: its code was compiled by "
                       "backend '" +
                       made.Backend + "' (" + made.Producer + ", " +
                       made.Format + " for other-" + made.Target +
                       "); backend '" + made.Backend +
                       "' links code it compiled itself (" + made.Producer +
                       " for " + made.Target + ")\n");
}

// A module that failed once and is asked for again (here `x`, a prebuilt
// file built against another interface of `y`, the module being compiled,
// which imports it) names the file its import would be in the second
// report, as before: it named none (`not found (tried )`), the path of the
// resolver's failed entry, which has none.
TEST(Pkm, AModuleThatFailedNamesTheFileItsImportWouldBe) {
  REQUIRE_BACKEND();
  fs::path dir =
      fs::temp_directory_path() / ("pkm_tried_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "s1" / "y.pkn", "fn yv() -> int { return 1; }\n");
  writeFile(dir / "s2" / "x.pkn",
            "import y;\nfn xv() -> int { return y::yv() + 1; }\n");
  writeFile(dir / "s3" / "y.pkn",
            "import x;\nfn yv() -> int { return x::xv() + 40; }\n");
  fs::create_directories(dir / "pre");
  auto in = [&](const char *sub, const std::string &args) {
    return run("cd " + (dir / sub).string() + " && " + paykanCmd() + " " +
               args);
  };
  ASSERT_EQ(in("s1", "--emit-pkm y.pkn -o ../pre/y.pkm").exitCode, 0);
  ASSERT_EQ(in("s2", "--module-path=../pre --emit-pkm x.pkn -o ../pre/x.pkm")
                .exitCode,
            0);
  auto r = in("s3", "--module-path=../pre --no-module-cache --emit-pkm y.pkn "
                    "-o ../y.pkm");
  EXPECT_EQ(r.exitCode, 1) << r.out;
  EXPECT_TRUE(contains(r.out, "error: module 'x' (../pre/x.pkm) was built "
                              "against another interface of module 'y'"))
      << r.out;
  EXPECT_FALSE(contains(r.out, "(tried )")) << r.out;
  EXPECT_TRUE(contains(r.out, "error: module 'x' not found (tried x.pkn)\n"))
      << r.out;
  fs::remove_all(dir);
}

// A directory is not a module file: every verb says so and exits with 1,
// never an uncaught std::ios_base::failure (SIGABRT, status 134; finding 33).
TEST(Pkm, ADirectoryIsNotAModuleFile) {
  fs::path dir =
      fs::temp_directory_path() / ("pkm_dir_" + std::to_string(getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir / "d.pkm");
  const std::string path = (dir / "d.pkm").string();
  for (const char *verb : {"dump", "check", "extract --object"}) {
    auto r = run(std::string(kPaykan) + " pkm " + verb + " " + path);
    EXPECT_EQ(r.exitCode, 1) << verb << ": " << r.out;
    EXPECT_EQ(r.out, "paykan: '" + path + "' is a directory, not a file\n")
        << verb;
  }
  fs::remove_all(dir);
}

// A module file that comes through a pipe (`cat base.pkm | paykan pkm dump
// /dev/stdin`, or `<(...)`) is read to its end, as any file is; reading
// only regular files, as the fix of finding 33 first did, refused it
// (`'/dev/stdin' is not a regular file`).
TEST(Pkm, AModuleFileCanComeThroughAPipe) {
  Project p("pipe");
  auto cd = "cd " + p.Dir.string() + " && ";
  ASSERT_EQ(run(cd + paykanCmd() + " --emit-pkm base.pkn").exitCode, 0);
  auto file = run(cd + kPaykan + " pkm dump base.pkm");
  ASSERT_EQ(file.exitCode, 0) << file.out;
  auto piped = run(cd + "cat base.pkm | " + kPaykan + " pkm dump /dev/stdin");
  EXPECT_EQ(piped.exitCode, 0) << piped.out;
  EXPECT_EQ(piped.out, file.out);
  auto check = run(cd + "cat base.pkm | " + kPaykan + " pkm check /dev/stdin");
  EXPECT_EQ(check.exitCode, 0) << check.out;
  EXPECT_EQ(check.out, "/dev/stdin: usable\n");
}

TEST(Pkm, HelpDocumentsTheFlags) {
  auto r = run(std::string(kPaykan) + " --help");
  ASSERT_EQ(r.exitCode, 0);
  for (const char *flag :
       {"--emit-pkm", "--module-path=<dir>", "--rebuild-modules",
        "--no-module-cache", "--verbose", "pkm dump", "pkm check"})
    EXPECT_TRUE(contains(r.out, flag)) << flag << "\n" << r.out;
}

// -- Interface and code that disagree

namespace {

/// `lib::b` derives `Line` from `lib::a`'s abstract `Shape` and `Seg` from
/// `Line`, and has a struct and a function returning it, a function
/// returning an instance of `lib::a`'s template `Box` (whose copy its CODE
/// holds) and a native function; `main` uses them all (exit status 42).  Its
/// file is tried with an IFACE or a CODE that disagrees with itself, with its
/// CODE or with `lib::a`, every hash made to match: a cache entry is rebuilt, a
/// prebuilt file rejected where it is imported, by `--check-only` too, and `pkm
/// check` says `corrupt`.
struct Hierarchy {
  fs::path Dir;
  explicit Hierarchy(const std::string &name) {
    Dir = fs::temp_directory_path() /
          ("pkm_hier_" + name + "_" + std::to_string(getpid()));
    fs::remove_all(Dir);
    writeFile(Dir / "lib" / "a.pkn",
              "class Shape { n: int; m: int;\n"
              "  view fn area() -> int;\n"
              "  view fn perimeter() -> int { return self.n + self.m; } }\n"
              "class Box<T> { v: T; view fn get() -> T { return self.v; } "
              "}\n");
    writeFile(Dir / "lib" / "b.pkn",
              "import lib::a;\n"
              "class Line : a::Shape { k: int;\n"
              "  view fn extra() -> int { return self.k; } }\n"
              "class Seg : Line { z: int;\n"
              "  view fn area() -> int { return self.z; } }\n"
              "struct S { q: int; }\n"
              "impl S { view fn get() -> int { return self.q; } }\n"
              "fn mk() -> S { return S(1); }\n"
              "fn bx() -> a::Box<int> { return a::Box<int>(2); }\n"
              "native fn absval(x: int) -> int = \"llabs\";\n");
    writeFile(Dir / "main.pkn",
              "import lib::b;\n"
              "fn main() -> int {\n"
              "  let s = b::Seg(1, 2, 3, 30);\n"
              "  let t = b::mk();\n"
              "  return s.perimeter() + s.area() + s.extra() + t.get() +\n"
              "         b::bx().get() + b::absval(-3);\n"
              "}\n");
  }
  ~Hierarchy() { fs::remove_all(Dir); }
  CmdResult paykan(const std::string &args) const {
    return run("cd " + Dir.string() + " && " + paykanCmd() + " " + args);
  }
  fs::path entry(const std::string &module) const {
    return Dir / ".paykan_cache" / "lib" / (module + ".pkm");
  }
  /// The cache entries of `lib::a` and `lib::b` as prebuilt files in
  /// pre/lib, the sources gone.
  void prebuild() const {
    fs::create_directories(Dir / "pre" / "lib");
    for (const char *m : {"a", "b"})
      fs::copy_file(entry(m), Dir / "pre" / "lib" / (std::string(m) + ".pkm"),
                    fs::copy_options::overwrite_existing);
    fs::remove_all(Dir / "lib");
    fs::remove_all(Dir / ".paykan_cache");
  }
};

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

/// Rewrite the CODE section of the module file at @p path, decoded, with
/// @p change.
void rewriteCode(const fs::path &path,
                 const std::function<void(paykan::pir::Module &)> &change) {
  rewriteSection(path, paykan::pkm::Kind::Code,
                 [&](std::vector<uint8_t> &bytes) {
                   auto code = paykan::pir::binary::decode(bytes);
                   ASSERT_TRUE(code) << code.status().message();
                   change(*code);
                   bytes = paykan::pir::binary::encode(*code);
                 });
}

template <typename Record>
Record &named(std::vector<Record> &records, const std::string &name) {
  auto it = std::find_if(records.begin(), records.end(),
                         [&](const Record &r) { return r.Name == name; });
  EXPECT_NE(it, records.end()) << name;
  return *it;
}

/// What a damaged `lib::b` is tried on: @p damage applied to its cache
/// entry, then to its prebuilt file, each time from a fresh project.
/// @p why is what its load says; @p checkSeesIt whether `pkm check`,
/// which reads the file without its dependencies, sees it too.
void expectNotUsed(const std::string &name,
                   const std::function<void(const fs::path &)> &damage,
                   const std::string &why, bool checkSeesIt = true) {
  SCOPED_TRACE(name);
  {
    Hierarchy h(name + "_cache");
    ASSERT_EQ(h.paykan("main.pkn").exitCode, 42);
    damage(h.entry("b"));
    auto r = h.paykan("--verbose main.pkn");
    EXPECT_EQ(r.exitCode, 42) << r.out;
    EXPECT_TRUE(
        contains(r.out, "module lib::b: cache .paykan_cache/lib/b.pkm " + why +
                            "; building from source\n"))
        << r.out;
    // The entry written in its place is used.
    r = h.paykan("--verbose main.pkn");
    EXPECT_EQ(r.exitCode, 42) << r.out;
    EXPECT_TRUE(contains(r.out, "module lib::b: cache .paykan_cache/lib/b.pkm "
                                "usable\n"))
        << r.out;
  }
  Hierarchy h(name + "_prebuilt");
  ASSERT_EQ(h.paykan("main.pkn").exitCode, 42);
  h.prebuild();
  damage(h.Dir / "pre" / "lib" / "b.pkm");
  for (const char *mode : {"", "--check-only ", "--emit-c "}) {
    auto r = h.paykan(std::string("--module-path=pre --no-module-cache ") +
                      mode + "main.pkn");
    EXPECT_EQ(r.exitCode, 1) << mode << r.out;
    EXPECT_TRUE(contains(r.out, "error: module 'lib::b' (pre/lib/b.pkm) " +
                                    why + ". Rebuild it from source.\n"))
        << mode << r.out;
    EXPECT_FALSE(contains(r.out, "internal compiler error")) << mode << r.out;
  }
  auto check = h.paykan("pkm check pre/lib/b.pkm");
  if (checkSeesIt) {
    EXPECT_EQ(check.exitCode, 1) << check.out;
    EXPECT_EQ(check.out, "pre/lib/b.pkm: corrupt: " + why + "\n");
  } else {
    EXPECT_EQ(check.out, "pre/lib/b.pkm: usable\n");
  }
}

} // namespace

// An IFACE that disagrees with itself, with its CODE or with the
// interface of the module a type it records is from (its copy of a
// superclass of its own classes): an importer reconstructs the types from
// the file's records, and the lowering stopped with an internal compiler
// error on the vtable it got, or Sema reported a signature's unknown type
// twice and stopped (the duplicate-diagnostic guard), and a cache entry was
// not rebuilt.
TEST(Pkm, AnInterfaceThatDisagreesIsNotUsed) {
  REQUIRE_BACKEND();
  using paykan::pkm::Interface;
  auto iface = [](const std::function<void(Interface &)> &change) {
    return [change](const fs::path &file) {
      rewriteIface(file, "lib::b", change);
    };
  };
  expectNotUsed("slot", iface([](Interface &i) {
                  named(i.Classes, "Shape").Methods[0].Name = "";
                }),
                "has a bad IFACE section: the vtable of class 'Line' does "
                "not start with the slots of its superclass 'Shape'");
  expectNotUsed("copy", iface([](Interface &i) {
                  named(i.Classes, "Shape").Fields[0].TypeName = "Str";
                }),
                "has an IFACE section that does not match module 'lib::a': "
                "its class 'Shape' is not the one that module describes",
                /*checkSeesIt=*/false);
  expectNotUsed("type", iface([](Interface &i) {
                  named(i.Functions, "Line").ReturnTypeName = "";
                }),
                "has a bad IFACE section: return type of function 'Line' has "
                "unknown type ''");
  expectNotUsed("signature", iface([](Interface &i) {
                  named(i.Functions, "mk").ReturnTypeName = "Seg";
                }),
                "has an IFACE section that does not match its CODE: function "
                "'mk' is (ptr) -> void in CODE, () -> box by its types in "
                "IFACE");
  // Every type has its PIR type: the optional of an int is an opt_i64,
  // which a subclass's initializer in an importer passed where CODE took
  // an i64 (`internal compiler error: an optional value where a box is
  // due` for a class's).
  expectNotUsed("optional", iface([](Interface &i) {
                  for (const char *c : {"Line", "Seg"})
                    for (auto &m : named(i.Classes, c).Methods)
                      if (m.Name == "__init__")
                        m.ParamTypeNames[0] = "int?";
                }),
                "has an IFACE section that does not match its CODE: method "
                "'Line.__init__' is (obj, i64, i64, i64) -> void in CODE, "
                "(obj, opt_i64, i64, i64) -> void by its types in IFACE");
  // A native function is the module's function that calls C.
  expectNotUsed("native", iface([](Interface &i) {
                  named(i.Functions, "absval").ReturnTypeName = "float";
                }),
                "has an IFACE section that does not match its CODE: function "
                "'absval' is (i64) -> i64 in CODE, (i64) -> f64 by its types "
                "in IFACE");
}

// A CODE that verifies on its own, and still is not one a program can use:
// a `void` parameter (the mlir backend crashed on it), an empty vtable slot
// of a class it defines or of a copy of an instantiation, a slot that names
// another function than the one an importer's subclass inherits, or a
// constructor returning nothing (the program built, and crashed when it
// ran), a class's initializer an importer's subclass calls by its name
// missing (an internal compiler error), a function whose struct result is
// not written through its last parameter or a class whose fields do not
// begin with its superclass's (the program did not verify), an extern that
// does not match the module it names, a function whose next value id is
// not above its ids (rc-elimination read past its tables), a function with
// no name or a copy's constructor that is not shared (the link missed the
// one the compiled code calls).
TEST(Pkm, CodeAProgramCannotUseIsNotUsed) {
  REQUIRE_BACKEND();
  using paykan::pir::Module;
  auto code = [](const std::function<void(Module &)> &change) {
    return [change](const fs::path &file) { rewriteCode(file, change); };
  };
  auto fn = [](Module &m, const std::string &name) -> paykan::pir::Function & {
    return named(m.Functions, name);
  };
  // The function @p from renamed @p to wherever CODE names it, as one
  // changed byte of its strings does.
  auto rename = [](const std::string &from, const std::string &to) {
    return [=](const fs::path &file) {
      rewriteCode(file, [&](Module &m) {
        for (paykan::pir::Function &f : m.Functions) {
          if (f.Name == from)
            f.Name = to;
          for (std::string &u : f.Uses)
            if (u == from)
              u = to;
        }
        for (paykan::pir::Class &c : m.Classes)
          for (paykan::pir::VTableEntry &e : c.VTable)
            if (e.Target == from)
              e.Target = to;
      });
    };
  };
  expectNotUsed("void", code([&](Module &m) {
                  fn(m, "Shape.__init__").Sig.Params[1] =
                      paykan::pir::Type::Void;
                }),
                "has a bad CODE section: it does not verify:\nlib::b: "
                "function '@Shape.__init__' takes a void parameter\n");
  expectNotUsed("slot", code([](Module &m) {
                  named(m.Classes, "Seg").VTable[3].Target.clear();
                }),
                "has an IFACE section that does not match its CODE: class "
                "'Seg': slot 3 ('area') of its vtable is empty in CODE");
  expectNotUsed("copy", code([](Module &m) {
                  named(m.Classes, "Box<int>").VTable[0].Target.clear();
                }),
                "has an IFACE section that does not match its CODE: class "
                "'Box<int>': slot 0 ('destroy') of its vtable is empty in "
                "CODE");
  expectNotUsed("target", rename("Line.extra", "Line.extrb"),
                "has an IFACE section that does not match its CODE: class "
                "'Line': slot 5 ('extra') of its vtable names '@Line.extrb' "
                "in CODE, which is not the class's or an ancestor's method "
                "'extra'");
  expectNotUsed("init", rename("Line.__init__", "Line.__inix__"),
                "has an IFACE section that does not match its CODE: method "
                "'Line.__init__' is not in CODE");
  // The constructor of the copy of `Box<int>`, which `bx` calls, linked
  // by another name than the one the compiled code calls: the program's
  // link missed it.
  expectNotUsed("unshared",
                code([&](Module &m) { fn(m, "Box<int>").Shared = false; }),
                "has an IFACE section that does not match its CODE: function "
                "'@Box<int>' is not shared in CODE, its class 'Box<int>' is");
  // Or by no name.
  expectNotUsed("nameless",
                code([&](Module &m) { fn(m, "Box<int>").Name.clear(); }),
                "has a bad CODE section: it does not verify:\nlib::b: symbol "
                "has an empty name\n");
  expectNotUsed("ids", code([&](Module &m) {
                  ASSERT_EQ(fn(m, "Box<int>.get").NextValueId, 4u);
                  fn(m, "Box<int>.get").NextValueId = 3;
                }),
                "has a bad CODE section: it does not verify:\nlib::b:@Box<int>"
                ".get: value '%3' is not below the function's next value id "
                "3\n");
  expectNotUsed("ctor", code([&](Module &m) {
                  fn(m, "Seg").Sig.Ret = paykan::pir::Type::Void;
                }),
                "has an IFACE section that does not match its CODE: class "
                "'Seg': its constructor is (i64, i64, i64, i64) -> void in "
                "CODE, its initializer (obj, i64, i64, i64, i64) -> void");
  // A struct result is written where a last parameter points.
  expectNotUsed("result", code([&](Module &m) {
                  fn(m, "mk").Sig.Params[0] = paykan::pir::Type::Obj;
                }),
                "has an IFACE section that does not match its CODE: function "
                "'mk' is (obj) -> void in CODE, (ptr) -> void by its types in "
                "IFACE");
  expectNotUsed("inherited", code([](Module &m) {
                  named(m.Classes, "Seg").Fields[0].Name = "";
                }),
                "has a bad CODE section: it does not verify:\nlib::b: class "
                "'Seg': its fields do not begin with those of its superclass "
                "'Line'\n");
  // `lib::a`'s Shape, and the layouts that begin with it, changed alike.
  expectNotUsed("link", code([](Module &m) {
                  for (const char *c : {"Shape", "Line", "Seg"})
                    named(m.Classes, c).Fields[0].Ty = paykan::pir::Type::F64;
                }),
                "has a bad CODE section: it does not match the modules it "
                "names:\nlib::b: extern class 'Shape' has a different layout "
                "than its definition in module 'lib::a'\n",
                /*checkSeesIt=*/false);
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
                              "  return int(u32(d.len()));\n"
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
  // The table, a struct's field of a class type that is not optional, is
  // never null (docs/pir.md §6).
  EXPECT_TRUE(contains(module, "retain nonnull %elem.9\n")) << module;
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
    EXPECT_FALSE(contains(code.out, "retain nonnull %x.6\n")) << code.out;
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

// The instantiations a module's interface gives (Cell<Str> and Slot<Str> of
// cells), which its importers use as its copies, are read from their
// templates whatever the module comes from (docs/design/pkm.md §4.8): its
// source with --no-module-cache, its cache entry, cold then warm, and a
// prebuilt file without its source.  A string read from a cell or a slot
// is held across `put` and `set`, which let go of it, and not across
// `size`, which does not; and the program runs and builds without a leak.
// So in a program that imports only `labels`, whose interface gives
// Cell<Str> without a template of its own: cells' template is read there
// too, though the program imports no module that names cells.
TEST(Pkm, ImportedInstantiationsAreReadFromTheirTemplates) {
  REQUIRE_BACKEND();
  fs::path dir =
      fs::temp_directory_path() / ("pkm_instances_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "cells.pkn",
            "class Cell<T> { v: T; }\n"
            "impl Cell<T> {\n"
            "  fn put(x: T) -> int { self.v = x; return 0; }\n"
            "  view fn size() -> int => 1;\n"
            "}\n"
            "struct Slot<T> { v: T; }\n"
            "impl Slot<T> {\n"
            "  fn set(x: T) -> int { self.v = x; return 0; }\n"
            "  view fn size() -> int => 1;\n"
            "}\n"
            "fn cellOf(s: Str) -> Cell<Str> => Cell<Str>(s);\n"
            "fn slotOf(s: Str) -> Slot<Str> => Slot<Str>(s);\n");
  writeFile(
      dir / "main.pkn",
      "import ::io;\n"
      "import cells;\n"
      "class Holder { slot: cells::Slot<Str>; }\n"
      "fn reads(c: cells::Cell<Str>) -> Str => c.v + Str(c.size());\n"
      "fn replaces(c: cells::Cell<Str>) -> Str =>\n"
      "  c.v + Str(c.put(\"new\" + \"!\"));\n"
      "fn readsSlot(h: Holder) -> Str => h.slot.v + Str(h.slot.size());\n"
      "fn setsSlot(h: Holder) -> Str =>\n"
      "  h.slot.v + Str(h.slot.set(\"other\" + \"?\"));\n"
      "fn main() -> int {\n"
      "  let c = cells::cellOf(\"old\" + \"?\");\n"
      "  io::println(reads(c) + \" \" + replaces(c) + \" \" + reads(c));\n"
      "  let h = Holder(cells::slotOf(\"first\" + \".\"));\n"
      "  io::println(readsSlot(h) + \" \" + setsSlot(h) + \" \" +\n"
      "              readsSlot(h));\n"
      "  return 0;\n"
      "}\n");
  writeFile(dir / "labels.pkn", "import cells;\n"
                                "fn labelled(s: Str) -> cells::Cell<Str> =>\n"
                                "  cells::Cell<Str>(\"#\" + s);\n");
  writeFile(dir / "through.pkn",
            "import ::io;\n"
            "import labels;\n"
            "class Box { s: Str; }\n"
            "fn sized(b: Box) -> Str {\n"
            "  let c = labels::labelled(\"x\");\n"
            "  return b.s + Str(c.size());\n"
            "}\n"
            "fn stored(b: Box) -> Str {\n"
            "  let c = labels::labelled(\"x\");\n"
            "  return b.s + Str(c.put(\"y\"));\n"
            "}\n"
            "fn main() -> int {\n"
            "  let b = Box(\"b\" + \"!\");\n"
            "  io::println(sized(b) + \" \" + stored(b));\n"
            "  return 0;\n"
            "}\n");
  const std::string output = "old?1 old?0 new!1\nfirst.1 first.0 other?1\n";
  auto in = [&](const std::string &cmd) {
    return run("cd " + dir.string() + " && " + cmd);
  };
  // The PIR of @p program, @p flags given.
  auto pirOf = [&](const std::string &flags, const char *program,
                   const std::string &how) {
    auto pir = in(paykanCmd() + " " + flags + " --emit-pir " + program);
    EXPECT_EQ(pir.exitCode, 0) << how << ":\n" << pir.out;
    EXPECT_TRUE(contains(pir.out, "extern shared fn @Cell<Str>.put("))
        << how << ":\n"
        << pir.out;
    return pir.out;
  };
  // Its function @p fn.
  auto body = [](const std::string &pir, const std::string &fn) {
    const size_t at = pir.find("\nfn @" + fn + "(");
    return at == std::string::npos ? std::string()
                                   : pir.substr(at, pir.find("\n}\n", at) - at);
  };
  // The programs' PIR, @p flags given: each read held where it must be.
  auto holds = [&](const std::string &flags, const std::string &how) {
    const std::string pir = pirOf(flags, "main.pkn", how);
    for (const char *fn : {"reads", "readsSlot"})
      EXPECT_FALSE(contains(body(pir, fn), "retain"))
          << how << ", " << fn << ":\n"
          << body(pir, fn);
    EXPECT_TRUE(contains(body(pir, "replaces"), "retain nonnull %v."))
        << how << ":\n"
        << body(pir, "replaces");
    EXPECT_TRUE(contains(body(pir, "setsSlot"), "retain nonnull %elem."))
        << how << ":\n"
        << body(pir, "setsSlot");
    const std::string through = pirOf(flags, "through.pkn", how);
    EXPECT_FALSE(contains(body(through, "sized"), "retain"))
        << how << ":\n"
        << body(through, "sized");
    EXPECT_TRUE(contains(body(through, "stored"), "retain nonnull %s."))
        << how << ":\n"
        << body(through, "stored");
  };
  // `paykan [flags] <program>` prints what it must without a leak.
  auto runs = [&](const std::string &flags, const std::string &how) {
    auto prints = [&](const char *program, const std::string &out) {
      auto r = in(paykanCmd() + " " + flags + " --track-heap " + program);
      EXPECT_EQ(r.exitCode, 0) << how << ", " << program << ":\n" << r.out;
      EXPECT_TRUE(contains(r.out, out)) << how << ", " << program << ":\n"
                                        << r.out;
      EXPECT_TRUE(contains(r.out, "live blocks       : 0"))
          << how << ", " << program << ":\n"
          << r.out;
    };
    prints("main.pkn", output);
    prints("through.pkn", "b!1 b!0\n");
  };

  holds("--no-module-cache", "from source");
  runs("--no-module-cache", "from source");
  holds("", "cold cache");
  auto warm = in(paykanCmd() + " --verbose --emit-pir main.pkn");
  EXPECT_TRUE(contains(warm.out, "module cells: cache ")) << warm.out;
  holds("", "warm cache");
  runs("", "warm cache");
  for (const char *opt : {"-O0", "-O2"}) {
    auto b = in(paykanCmd() + " " + opt + " build -o prog main.pkn");
    ASSERT_EQ(b.exitCode, 0) << opt << ":\n" << b.out;
    auto ran = in("./prog");
    EXPECT_EQ(ran.exitCode, 0) << opt;
    EXPECT_EQ(ran.out, output) << opt;
  }

  // Prebuilt: cells and labels .pkm files on the module path, their
  // sources gone.
  fs::create_directories(dir / "pk");
  auto emit = in(paykanCmd() + " --emit-pkm -o pk/cells.pkm cells.pkn");
  ASSERT_EQ(emit.exitCode, 0) << emit.out;
  emit = in(paykanCmd() +
            " --module-path=pk --emit-pkm -o pk/labels.pkm labels.pkn");
  ASSERT_EQ(emit.exitCode, 0) << emit.out;
  fs::rename(dir / "cells.pkn", dir / "cells.away");
  fs::rename(dir / "labels.pkn", dir / "labels.away");
  fs::remove_all(dir / ".paykan_cache");
  auto pre =
      in(paykanCmd() + " --verbose --module-path=pk --emit-pir through.pkn");
  EXPECT_TRUE(contains(pre.out, "module cells: prebuilt pk/")) << pre.out;
  EXPECT_TRUE(contains(pre.out, "module labels: prebuilt pk/")) << pre.out;
  holds("--module-path=pk", "prebuilt");
  runs("--module-path=pk", "prebuilt");
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

// A module's parses are Str's methods, from its source, its cache entry and
// its prebuilt file alike; a template body still written with the older
// conversion (`int(x)`) is the error that names the method where an
// importer instantiates it with a `Str`, with the instantiation's note.
TEST(Pkm, StrParsesAcrossModules) {
  REQUIRE_BACKEND();
  fs::path dir =
      fs::temp_directory_path() / ("pkm_str_parse_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "lib" / "conv.pkn",
            "fn bytes(s: Str) -> u8? { return s.toU8(); }\n"
            "fn parsed<T>(x: T) -> int? { return x.toInt(); }\n"
            "fn old<T>(x: T) -> int? { return int(x); }\n");
  writeFile(dir / "main.pkn", "import ::io;\nimport lib::conv;\n"
                              "fn main() -> int {\n"
                              "  io::println(conv::bytes(\"200\"));\n"
                              "  io::println(conv::bytes(\"+1\"));\n"
                              "  io::println(conv::bytes(\"256\"));\n"
                              "  io::println(conv::parsed(\"-12\"));\n"
                              "  return 0;\n}\n");
  writeFile(dir / "bad.pkn", "import lib::conv;\n"
                             "fn main() -> int {\n"
                             "  o = conv::old(\"5\");\n"
                             "  return 0;\n}\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  const std::string expected = "200\nNone\nNone\n-12\n";
  const std::string e1 = "lib/conv.pkn:3:34: error: no specialization of "
                         "'int' for 'Str': parse a 'Str' with its method "
                         "'toInt()', which returns 'int?'";
  const std::string note = "bad.pkn:3:7: note: in instantiation of ";
  auto checkBad = [&](const std::string &flags) {
    auto bad = paykan(flags + " --check-only bad.pkn");
    EXPECT_NE(bad.exitCode, 0) << bad.out;
    EXPECT_TRUE(contains(bad.out, e1)) << bad.out;
    EXPECT_TRUE(contains(bad.out, note)) << bad.out;
  };
  // From source, without and with the cache (written, then read).
  for (const char *flags : {"--no-module-cache", "", ""}) {
    auto r = paykan(std::string(flags) + " main.pkn");
    EXPECT_EQ(r.exitCode, 0) << flags << "\n" << r.out;
    EXPECT_EQ(r.out, expected) << flags;
  }
  EXPECT_TRUE(fs::exists(dir / ".paykan_cache"));
  checkBad("--no-module-cache");
  checkBad("");
  // From the prebuilt file, its source gone.
  ASSERT_EQ(paykan("--emit-pkm lib/conv.pkn").exitCode, 0);
  fs::remove(dir / "lib" / "conv.pkn");
  fs::remove_all(dir / ".paykan_cache");
  for (const char *flags : {"--no-module-cache", ""}) {
    auto r = paykan(std::string(flags) + " --verbose main.pkn");
    EXPECT_EQ(r.exitCode, 0) << flags << "\n" << r.out;
    EXPECT_TRUE(contains(r.out, expected)) << flags << "\n" << r.out;
    EXPECT_TRUE(contains(r.out, "module lib::conv: prebuilt lib/conv.pkm "
                                "usable"))
        << r.out;
  }
  // And built ahead of time, the module's code from its file.
  ASSERT_EQ(paykan("build main.pkn -o prog").exitCode, 0);
  auto built = run((dir / "prog").string());
  EXPECT_EQ(built.exitCode, 0) << built.out;
  EXPECT_EQ(built.out, expected);
  checkBad("--no-module-cache");
  fs::remove_all(dir);
}

// `Str(x)` of a value with a text, in a module's function and in a
// template it exports, from its source, its cache entry and its prebuilt
// file alike: the text `toString()` gives, an imported class's own.  A
// template instantiated with a type without a text is the error that names
// it, with the instantiation's note.
TEST(Pkm, StrOfAValueAcrossModules) {
  REQUIRE_BACKEND();
  fs::path dir =
      fs::temp_directory_path() / ("pkm_str_value_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "lib" / "shows.pkn",
            "class Point {\n"
            "  x: int;\n"
            "  view fn toString() -> Str { return \"P\" + Str(self.x); }\n"
            "}\n"
            "struct Size { w: int; h: int; }\n"
            "fn show<T>(x: T) -> Str { return Str(x); }\n"
            "fn both(p: Point) -> Str { return Str(p) + Str(Size(1, 2)); }\n");
  writeFile(dir / "main.pkn",
            "import ::io;\nimport lib::shows;\n"
            "fn main() -> int {\n"
            "  p = shows::Point(3);\n"
            "  o: int? = None;\n"
            "  io::println(shows::show(p) + \" \" + shows::show((4, \"a\")) + "
            "\" \" + shows::show(o));\n"
            "  io::println(shows::both(p) + \" \" + Str(p) + \" \" + "
            "Str(shows::Size(5, 6)));\n"
            "  return 0;\n}\n");
  writeFile(dir / "bad.pkn", "import lib::shows;\n"
                             "fn main() -> int {\n"
                             "  s = shows::show(fn(x: int) => x);\n"
                             "  return 0;\n}\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  const std::string expected =
      "P3 (4, a) None\nP3Size(w: 1, h: 2) P3 Size(w: 5, h: 6)\n";
  const std::string e5 = "lib/shows.pkn:6:34: error: 'Str' cannot convert a "
                         "value of type '(int) -> int', which does not "
                         "implement trait 'Show'";
  const std::string note = "bad.pkn:3:7: note: in instantiation of ";
  auto checkBad = [&](const std::string &flags) {
    auto bad = paykan(flags + " --check-only bad.pkn");
    EXPECT_NE(bad.exitCode, 0) << bad.out;
    EXPECT_TRUE(contains(bad.out, e5)) << bad.out;
    EXPECT_TRUE(contains(bad.out, note)) << bad.out;
  };
  // From source, without and with the cache (written, then read).
  for (const char *flags : {"--no-module-cache", "", ""}) {
    auto r = paykan(std::string(flags) + " --track-heap main.pkn");
    EXPECT_EQ(r.exitCode, 0) << flags << "\n" << r.out;
    EXPECT_TRUE(contains(r.out, expected)) << flags << "\n" << r.out;
    EXPECT_TRUE(contains(r.out, "live blocks       : 0")) << r.out;
  }
  EXPECT_TRUE(fs::exists(dir / ".paykan_cache"));
  checkBad("--no-module-cache");
  checkBad("");
  // From the prebuilt file, its source gone.
  ASSERT_EQ(paykan("--emit-pkm lib/shows.pkn").exitCode, 0);
  fs::remove(dir / "lib" / "shows.pkn");
  fs::remove_all(dir / ".paykan_cache");
  for (const char *flags : {"--no-module-cache", ""}) {
    auto r = paykan(std::string(flags) + " --verbose main.pkn");
    EXPECT_EQ(r.exitCode, 0) << flags << "\n" << r.out;
    EXPECT_TRUE(contains(r.out, expected)) << flags << "\n" << r.out;
    EXPECT_TRUE(contains(r.out, "module lib::shows: prebuilt lib/shows.pkm "
                                "usable"))
        << r.out;
  }
  // And built ahead of time, the module's code from its file.
  ASSERT_EQ(paykan("build main.pkn -o prog").exitCode, 0);
  auto built = run((dir / "prog").string());
  EXPECT_EQ(built.exitCode, 0) << built.out;
  EXPECT_EQ(built.out, expected);
  checkBad("--no-module-cache");
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
            "  io::println(xs.get(1)); return int(u32(xs.len())); }\n");
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
  fs::create_directories(dir / "geometry");
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

// --emit-pkm names a module by its path made normal, so `./a/b.pkn` and
// `a/./b.pkn` are `a::b`, as `a/b.pkn` is, and an importer of `a::b` loads
// the file without the source.  A path with a part that is no identifier
// names no module (finding 52).
TEST(Pkm, EmitPkmNamesTheModuleByItsNormalPath) {
  REQUIRE_BACKEND();
  fs::path dir =
      fs::temp_directory_path() / ("pkm_normal_" + std::to_string(getpid()));
  fs::remove_all(dir);
  const std::string area = "fn area(w: int, h: int) -> int { return w * h; }\n";
  writeFile(dir / "main.pkn",
            "import a::b;\nfn main() -> int { return b::area(2, 3); }\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  for (const char *spelling : {"./a/b.pkn", "a/./b.pkn"}) {
    writeFile(dir / "a" / "b.pkn", area);
    auto emit = paykan(std::string("--emit-pkm -o a/b.pkm ") + spelling);
    ASSERT_EQ(emit.exitCode, 0) << spelling << ": " << emit.out;
    auto manifest = run(std::string(kPaykan) + " pkm dump --section=manifest " +
                        (dir / "a" / "b.pkm").string());
    EXPECT_TRUE(contains(manifest.out, "  module a::b\n"))
        << spelling << ": " << manifest.out;
    fs::remove(dir / "a" / "b.pkn");
    auto r = paykan("main.pkn");
    EXPECT_EQ(r.exitCode, 6) << spelling << ": " << r.out;
  }
  for (const auto &[path, part] :
       {std::pair{"1bad/x.pkn", "1bad"}, std::pair{"a/my-mod.pkn", "my-mod"}}) {
    writeFile(dir / path, area);
    auto bad = paykan(std::string("--emit-pkm ") + path);
    EXPECT_EQ(bad.exitCode, 1) << path << ": " << bad.out;
    EXPECT_EQ(bad.out, std::string("paykan: cannot name the module of '") +
                           path + "' after its path: '" + part +
                           "' is not an identifier (name it with "
                           "--module-name)\n");
    EXPECT_FALSE(fs::exists(fs::path(dir / path).replace_extension(".pkm")))
        << path;
  }
  fs::remove_all(dir);
}

// A keyword is no identifier, so it is no part of a module name, given by
// the path or by --module-name: `--emit-pkm x/y/if.pkn` wrote a module
// `x::y::if` that no import can name (`import x::y::if;` is a syntax error),
// and so did `--module-name=a::match` (SW-42).
TEST(Pkm, EmitPkmRejectsAKeywordInTheModuleName) {
  REQUIRE_BACKEND();
  fs::path dir =
      fs::temp_directory_path() / ("pkm_keyword_" + std::to_string(getpid()));
  fs::remove_all(dir);
  const std::string area = "fn area(w: int, h: int) -> int { return w * h; }\n";
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  for (const auto &[path, part] :
       {std::pair{"x/y/if.pkn", "if"}, std::pair{"match/ok.pkn", "match"},
        std::pair{"_/ok.pkn", "_"}}) {
    writeFile(dir / path, area);
    auto bad = paykan(std::string("--emit-pkm ") + path);
    EXPECT_EQ(bad.exitCode, 1) << path << ": " << bad.out;
    EXPECT_EQ(bad.out, std::string("paykan: cannot name the module of '") +
                           path + "' after its path: '" + part +
                           "' is a keyword, which no import can name (name "
                           "it with --module-name)\n");
    EXPECT_FALSE(fs::exists(fs::path(dir / path).replace_extension(".pkm")))
        << path;
  }
  writeFile(dir / "x" / "y" / "ok.pkn", area);
  for (const char *name : {"a::match", "::if", "fn::b"}) {
    auto bad = paykan(std::string("--emit-pkm --module-name=") + name +
                      " x/y/ok.pkn -o m.pkm");
    EXPECT_EQ(bad.exitCode, 1) << name << ": " << bad.out;
    EXPECT_TRUE(contains(bad.out, std::string("--module-name: '") + name +
                                      "' is not a module name: '"))
        << name << ": " << bad.out;
    EXPECT_TRUE(contains(bad.out, "' is a keyword, which no import can name"))
        << name << ": " << bad.out;
    EXPECT_FALSE(fs::exists(dir / "m.pkm")) << name;
  }
  // A name that only starts like a keyword is an identifier.
  auto ok = paykan("--emit-pkm --module-name=a::matches x/y/ok.pkn -o m.pkm");
  EXPECT_EQ(ok.exitCode, 0) << ok.out;
  fs::remove_all(dir);
}

// --module-name=a::b for `src/a/b.pkn` resolves the module's imports against
// `src`, the directory above the name's parts before its last, so it finds
// its sibling `a::c` where a program that imports it does; they were
// resolved against `src/a`, where `a::c` is `src/a/a/c.pkn` (KF-7).  The
// same from inside `src/a`; a name whose parts are not the file's
// directories keeps the file's own directory.
TEST(Pkm, ModuleNameFindsTheModulesSiblings) {
  REQUIRE_BACKEND();
  fs::path dir =
      fs::temp_directory_path() / ("pkm_modroot_" + std::to_string(getpid()));
  fs::remove_all(dir);
  const std::string sibling = "fn g() -> int => 41;\n";
  writeFile(dir / "src" / "a" / "b.pkn",
            "import a::c;\nfn f() -> int => c::g() + 1;\n");
  writeFile(dir / "src" / "a" / "c.pkn", sibling);
  writeFile(dir / "use" / "a" / "c.pkn", sibling);
  writeFile(dir / "use" / "main.pkn",
            "import a::b;\nfn main() -> int => b::f();\n");
  auto in = [&](const fs::path &cwd, const std::string &args) {
    return run("cd " + cwd.string() + " && " + paykanCmd() + " " + args);
  };
  auto above =
      in(dir, "--emit-pkm --module-name=a::b -o use/a/b.pkm src/a/b.pkn");
  ASSERT_EQ(above.exitCode, 0) << above.out;
  // The import's cache entry is the project's.
  EXPECT_TRUE(fs::exists(dir / "src" / ".paykan_cache" / "a" / "c.pkm"));
  EXPECT_FALSE(fs::exists(dir / "src" / "a" / ".paykan_cache"));
  auto r = in(dir / "use", "main.pkn");
  EXPECT_EQ(r.exitCode, 42) << r.out;
  auto inside =
      in(dir / "src" / "a", "--emit-pkm --module-name=a::b -o b.pkm b.pkn");
  ASSERT_EQ(inside.exitCode, 0) << inside.out;
  // The same file: it records its source by the path from the source root.
  EXPECT_EQ(slurp(dir / "src" / "a" / "b.pkm"),
            slurp(dir / "use" / "a" / "b.pkm"));
  auto other = in(dir, "--emit-pkm --module-name=x::b -o x.pkm src/a/b.pkn");
  EXPECT_EQ(other.exitCode, 1) << other.out;
  EXPECT_TRUE(contains(other.out, "src/a/b.pkn:1:1: error: module 'a::c' not "
                                  "found (tried src/a/a/c.pkn, "
                                  "src/a/a/c.pkm)"))
      << other.out;
  EXPECT_FALSE(fs::exists(dir / "x.pkm"));
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

// A literal above INT64_MAX in a template's body is kept with its value in
// the module file's TMPL (the AST interchange format's `(int ...)` up to
// 18446744073709551615, docs/design/bitwise.md), and an importer's
// instantiation returns it.
TEST(Pkm, AWideLiteralInATemplateKeepsItsValue) {
  REQUIRE_BACKEND();
  auto dir =
      fs::temp_directory_path() / ("pkm_wide_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "lib" / "wide.pkn",
            "fn widest<T>(x: T) -> unsigned => 0xFFFF_FFFF_FFFF_FFFF;\n"
            "fn top<T>(x: T) -> unsigned => 0x8000_0000_0000_0000;\n");
  writeFile(dir / "main.pkn", "import ::io; import lib::wide;\n"
                              "fn main() -> int {\n"
                              "  io::println(Str(wide::widest(1)) + \" \" + "
                              "Str(wide::top(\"s\")));\n"
                              "  return 0;\n}\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  const char *expected = "18446744073709551615 9223372036854775808\n";
  auto fromSource = paykan("main.pkn");
  EXPECT_EQ(fromSource.exitCode, 0) << fromSource.out;
  EXPECT_EQ(fromSource.out, expected);
  auto emit = paykan("--emit-pkm lib/wide.pkn");
  ASSERT_EQ(emit.exitCode, 0) << emit.out;
  fs::remove(dir / "lib" / "wide.pkn");
  auto tmpl = paykan("pkm dump --section=tmpl lib/wide.pkm");
  EXPECT_EQ(tmpl.exitCode, 0) << tmpl.out;
  EXPECT_TRUE(contains(tmpl.out, " 18446744073709551615)")) << tmpl.out;
  EXPECT_TRUE(contains(tmpl.out, " 9223372036854775808)")) << tmpl.out;
  auto fromPkm = paykan("main.pkn");
  EXPECT_EQ(fromPkm.exitCode, 0) << fromPkm.out;
  EXPECT_EQ(fromPkm.out, expected);
  fs::remove_all(dir);
}

// Generic bodies using the bitwise operators (docs/design/bitwise.md) are
// kept in a module's TMPL section: instantiated from the prebuilt module
// alone, with `u8` and with `unsigned`, they give what the source gives.
TEST(Pkm, BitwiseTemplatesFromAPrebuiltModule) {
  REQUIRE_BACKEND();
  auto dir =
      fs::temp_directory_path() / ("pkm_bitwise_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "lib" / "bits.pkn",
            "fn clear<T>(x: T, m: T) -> T => x & ~m;\n"
            "fn top<T>(x: T, n: unsigned) -> T => x >> n;\n"
            "fn mix<T>(x: T, y: T) -> T => (x << 1) ^ (y | x);\n");
  writeFile(
      dir / "main.pkn",
      "import ::io; import lib::bits;\n"
      "fn main() -> int {\n"
      "  let n: unsigned = 4;\n"
      "  let far: unsigned = 64;\n"
      "  io::println(Str(bits::clear(u8(0xFF), u8(0x0F))) + \" \"\n"
      "      + Str(bits::clear(unsigned(0xFFFF), unsigned(0xFF))));\n"
      "  io::println(Str(bits::top(u8(0xF0), n)) + \" \"\n"
      "      + Str(bits::top(unsigned(0xFFFF_FFFF_FFFF_FFFF), n)) + \" \"\n"
      "      + Str(bits::top(u8(0xF0), far)));\n"
      "  io::println(Str(bits::mix(u8(0x81), u8(2))) + \" \"\n"
      "      + Str(bits::mix(unsigned(0x81), unsigned(2))));\n"
      "  return 0;\n}\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  const char *expected = "240 65280\n15 1152921504606846975 0\n129 385\n";
  auto fromSource = paykan("main.pkn");
  EXPECT_EQ(fromSource.exitCode, 0) << fromSource.out;
  EXPECT_EQ(fromSource.out, expected);
  auto emit = paykan("--emit-pkm lib/bits.pkn");
  ASSERT_EQ(emit.exitCode, 0) << emit.out;
  fs::remove(dir / "lib" / "bits.pkn");
  auto tmpl = paykan("pkm dump --section=tmpl lib/bits.pkm");
  EXPECT_EQ(tmpl.exitCode, 0) << tmpl.out;
  for (const char *name :
       {" bitnot\n", " bitand\n", " shr\n", " shl\n", " bitxor\n", " bitor\n"})
    EXPECT_TRUE(contains(tmpl.out, name)) << name << "\n" << tmpl.out;
  auto fromPkm = paykan("main.pkn");
  EXPECT_EQ(fromPkm.exitCode, 0) << fromPkm.out;
  EXPECT_EQ(fromPkm.out, expected);
  fs::remove_all(dir);
}

// -- Conformances across modules (docs/language/06-traits.md, "Hashable")

namespace {

/// lib::keys declares a struct, a class and a private struct with `impl
/// Hashable for` blocks and makes instantiations of its generic types;
/// lib::mid returns keys' types and declares a subclass of one, lib::m2
/// makes other instantiations of keys' generic types, and lib::lib2's
/// template body uses one.  main uses each as a `HashMap`/`HashSet` key or a
/// `T: Hashable` argument, and so are the instantiations of keys' generic
/// types it makes itself, and as type arguments of bounded generic enums,
/// keys' and its own, in field types.
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
enum Maybe<T: Hashable> { Just(v: T), Nothing }
fn mkKeyed() -> Keyed<int> { return Keyed<int>(7); }
fn mkBox() -> GBox<int> { return GBox<int>(1); }
)");
    writeFile(Dir / "lib" / "mid.pkn", R"(import lib::keys;
fn mk() -> keys::Key { return keys::Key(1, 2); }
fn mkc() -> keys::CK { return keys::CK(4); }
class MSub : keys::CK {
  view fn equals(other: Obj) -> bool { match other { p: MSub { return p.v == self.v; } _ { return False; } } }
}
impl MSub { fn __init__(v: int) { __super__(v); } }
fn mksub(v: int) -> MSub { return MSub(v); }
)");
    writeFile(Dir / "lib" / "m2.pkn", R"(import lib::keys;
fn mkBox() -> keys::GBox<bool> { return keys::GBox<bool>(True); }
fn mkKeyed() -> keys::Keyed<keys::Key> { return keys::Keyed<keys::Key>(keys::Key(2, 3)); }
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
import lib::m2;
import lib::lib2;
class Sub : keys::CK {
  view fn equals(other: Obj) -> bool { match other { p: Sub { return p.v == self.v; } _ { return False; } } }
}
impl Sub { fn __init__(v: int) { __super__(v); } }
fn h<T: Hashable>(x: view T) -> int { return x.hash(); }
enum Opt<T: Hashable> { Some(v: T), No }
class Slots { m: keys::Maybe<keys::Key>; o: Opt<keys::CK>; }
impl Slots {
  fn __init__() {
    self.m = keys::Maybe<keys::Key>::Just(keys::Key(4, 0));
    self.o = Opt<keys::CK>::Some(keys::CK(5));
  }
}
fn main() -> int {
  m = collections::HashMap<keys::Key, int>();
  m.put(keys::Key(1, 2), 5);
  m.put(mid::mk(), 6);
  s = collections::HashSet<keys::CK>();
  s.add(keys::CK(3)); s.add(keys::CK(3)); s.add(mid::mkc());
  t = collections::HashSet<Sub>();
  t.add(Sub(1)); t.add(Sub(1));
  io::println(Str(m.len()) + " " + Str(s.len()) + " " + Str(t.len()));
  io::println(Str(h(keys::mkKeyed()) == h(7)) + " " + Str(h(keys::Keyed<Str>("a")) == h("a")));
  io::println(Str(h(keys::mkBox())) + " " + Str(h(keys::GBox<Str>("x"))) + " " + keys::GBox<Str>("x").name());
  io::println(Str(keys::distinct(0)));
  io::println(Str(lib::lib2::count(0)));
  a = collections::HashSet<mid::MSub>();
  a.add(mid::mksub(1)); a.add(mid::MSub(1)); a.add(mid::MSub(2));
  b = collections::HashSet<keys::Keyed<keys::Key>>();
  b.add(m2::mkKeyed()); b.add(m2::mkKeyed());
  io::println(Str(a.len()) + " " + Str(b.len()) + " " + Str(h(m2::mkBox())) + " " + m2::mkBox().name());
  slots = Slots();
  a0 = 0;
  match slots.m { j: Just { a0 = j.v.a; } Nothing {} }
  match slots.o { some: Some { io::println(Str(a0) + " " + Str(some.v.v)); } No {} }
  return 0;
}
)");
  }
  ~ConformanceProject() { fs::remove_all(Dir); }
  CmdResult paykan(const std::string &cmd, const std::string &args) const {
    return run("cd " + Dir.string() + " && " + cmd + " " + args);
  }
};

const char *kConformanceOutput =
    "1 2 1\nTrue True\n3 3 box\n2\n1\n2 1 3 box\n4 5\n";

} // namespace

// A type's conformance to Hashable reaches main on every path a module
// comes from: its source with a cold and a warm cache, --no-module-cache,
// --rebuild-modules, prebuilt files, and a prebuilt keys under modules
// built from source; run, built at -O0 and -O2, and as C.
TEST(Pkm, ConformancesAcrossModules) {
  REQUIRE_BACKEND();
  ConformanceProject p("conformances");
  const char *libs[] = {"lib::keys", "lib::mid", "lib::m2", "lib::lib2"};
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
    fs::create_directories(p.Dir / "pk" / "lib");
    for (const char *m : {"keys", "mid", "m2", "lib2"}) {
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

    // Mixed: keys prebuilt beside the sources of mid, m2 and lib2.
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
  // mid's records those of the types it describes again, and none of its
  // subclass of CK; m2's those of the instantiations it made.
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
  EXPECT_TRUE(contains(mid.out, "  class MSub super \"CK\" [local from "
                                "\"lib::mid\"]\n"))
      << mid.out;
  EXPECT_FALSE(contains(mid.out, "for MSub")) << mid.out;
  auto m2 = p.paykan(kPaykan, "pkm dump --section=iface pk/lib/m2.pkm");
  ASSERT_EQ(m2.exitCode, 0) << m2.out;
  EXPECT_TRUE(contains(m2.out, "  impl Hashable for GBox<bool>\n")) << m2.out;
  EXPECT_TRUE(contains(m2.out, "  impl Hashable for Keyed<Key>\n")) << m2.out;

  // keys' templates carry every `impl` block of its generic types, `impl
  // Trait for` blocks too: GBox's plain, Hashable and Named blocks, and
  // Keyed's Hashable block.
  auto tmpl = p.paykan(kPaykan, "pkm dump --section=tmpl pk/lib/keys.pkm");
  ASSERT_EQ(tmpl.exitCode, 0) << tmpl.out;
  EXPECT_TRUE(contains(tmpl.out, "  impls 4\n    class 0\n    class 0\n"
                                 "    class 0\n    struct 0\n"))
      << tmpl.out;
  auto count = [&](const std::string &what) {
    size_t n = 0;
    for (size_t at = tmpl.out.find(what); at != std::string::npos;
         at = tmpl.out.find(what, at + 1))
      ++n;
    return n;
  };
  EXPECT_EQ(count("(for\n"), 3u) << tmpl.out;
  EXPECT_EQ(count(" \"Hashable\"))\n"), 2u) << tmpl.out;
  EXPECT_EQ(count(" \"Named\"))\n"), 1u) << tmpl.out;
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

// A .pkm file whose interface claims a conformance it cannot have: the
// reader rejects one to a trait an `impl` block cannot give, so a cache
// entry is rebuilt from the source and a prebuilt file is an error, and the
// importer one whose type lacks the trait's methods.
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
  auto toNamed = [](paykan::pkm::Interface &i) {
    ASSERT_EQ(i.Impls.size(), 1u);
    i.Impls[0].Trait = "Named";
  };
  const char *notBuiltin = "has a bad IFACE section: DECLS: 'impl Named for "
                           "Key': 'Named' is not a built-in trait an 'impl' "
                           "block implements";

  // The cache entry, then the one rebuilt in its place.
  ASSERT_EQ(paykan("main.pkn").exitCode, 5);
  rewriteIface(dir / ".paykan_cache" / "keys.pkm", "keys", toNamed);
  auto rebuilt = paykan("--verbose main.pkn");
  EXPECT_EQ(rebuilt.exitCode, 5) << rebuilt.out;
  EXPECT_TRUE(contains(rebuilt.out, std::string("module keys: cache "
                                                ".paykan_cache/keys.pkm ") +
                                        notBuiltin))
      << rebuilt.out;
  EXPECT_TRUE(contains(rebuilt.out, "; building from source\n")) << rebuilt.out;
  auto again = paykan("--verbose main.pkn");
  EXPECT_EQ(again.exitCode, 5) << again.out;
  EXPECT_TRUE(
      contains(again.out, "module keys: cache .paykan_cache/keys.pkm usable"))
      << again.out;
  fs::remove_all(dir / ".paykan_cache");

  ASSERT_EQ(paykan("--emit-pkm keys.pkn").exitCode, 0);
  fs::remove(dir / "keys.pkn");
  ASSERT_EQ(paykan("main.pkn").exitCode, 5);
  const std::string pristine = slurp(dir / "keys.pkm");

  rewriteIface(dir / "keys.pkm", "keys", toNamed);
  auto named = paykan("main.pkn");
  EXPECT_NE(named.exitCode, 0) << named.out;
  EXPECT_TRUE(contains(named.out, notBuiltin)) << named.out;
  auto check = paykan("pkm check keys.pkm");
  EXPECT_NE(check.exitCode, 0) << check.out;
  EXPECT_EQ(check.out.rfind(std::string("keys.pkm: corrupt: ") + notBuiltin, 0),
            0u)
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

// A module file whose interface does not match its CODE (a class, a
// superclass, a vtable slot, a field or a function its CODE does not have)
// is unusable: a prebuilt one is an error where it is imported, under
// `--check-only` too, and a cache entry is rebuilt.  The importer was
// lowered against the interface, and failed PIR's verifier or the
// lowering's own checks (`internal compiler error`).
TEST(Pkm, AnInterfaceThatDoesNotMatchItsCodeIsRejected) {
  REQUIRE_BACKEND();
  auto dir = fs::temp_directory_path() /
             ("pkm_iface_code_" + std::to_string(getpid()));
  fs::remove_all(dir);
  writeFile(dir / "geo.pkn",
            "class Shape { name: Str; view fn kind() -> Str { return "
            "\"shape\"; } }\n"
            "class Circle : Shape { r: int; d: int; view fn kind() -> Str { "
            "return \"circle\"; } }\n"
            "impl Circle { fn __init__(r: int) { __super__(\"c\"); self.r = "
            "r; self.d = 2 * r; } }\n"
            "fn area(c: view Circle) -> int { return 3 * c.r * c.r; }\n");
  writeFile(dir / "main.pkn", "import geo;\n"
                              "fn main() -> int {\n"
                              "  let c = geo::Circle(2);\n"
                              "  return geo::area(c) + c.kind().len();\n"
                              "}\n");
  auto paykan = [&](const std::string &args) {
    return run("cd " + dir.string() + " && " + paykanCmd() + " " + args);
  };
  ASSERT_EQ(paykan("main.pkn").exitCode, 18);
  const std::string cached = slurp(dir / ".paykan_cache" / "geo.pkm");
  ASSERT_EQ(paykan("--emit-pkm geo.pkn").exitCode, 0);
  fs::rename(dir / "geo.pkn", dir / "geo.src");
  const std::string pristine = slurp(dir / "geo.pkm");
  auto classNamed = [](paykan::pkm::Interface &i,
                       const std::string &name) -> paykan::pkm::ClassRec & {
    for (auto &c : i.Classes)
      if (c.Name == name)
        return c;
    ADD_FAILURE() << "no class " << name;
    return i.Classes.front();
  };
  struct Case {
    const char *What;
    std::function<void(paykan::pkm::Interface &)> Change;
    std::string Why;
  };
  const std::string mismatch =
      "has an IFACE section that does not match its CODE: ";
  auto renameSlot = [&](paykan::pkm::ClassRec &c) {
    for (auto &m : c.Methods)
      if (m.Name == "equals")
        m.Name = "equal?";
  };
  const Case cases[] = {
      {"a class renamed",
       [&](paykan::pkm::Interface &i) {
         classNamed(i, "Circle").Name = "Circle2";
       },
       mismatch + "class 'Circle2' is not in CODE"},
      {"a superclass renamed",
       [&](paykan::pkm::Interface &i) {
         classNamed(i, "Circle").SuperClassName = "";
       },
       mismatch + "class 'Circle': its superclass is 'Shape' in CODE, '' "
                  "in IFACE"},
      {"a vtable slot renamed",
       [&](paykan::pkm::Interface &i) {
         renameSlot(classNamed(i, "Shape"));
         renameSlot(classNamed(i, "Circle"));
       },
       mismatch + "class 'Circle': slot 2 of its vtable is 'equals' in "
                  "CODE, 'equal?' in IFACE"},
      {"a field renamed",
       [&](paykan::pkm::Interface &i) {
         classNamed(i, "Circle").Fields.front().FieldName = "radius";
       },
       mismatch + "class 'Circle': its own fields are (r, d) in CODE, "
                  "(radius, d) in IFACE"},
      // An importer lays a class out in the order of its fields.
      {"fields reordered",
       [&](paykan::pkm::Interface &i) {
         auto &fields = classNamed(i, "Circle").Fields;
         std::swap(fields[0], fields[1]);
       },
       mismatch + "class 'Circle': its own fields are (r, d) in CODE, (d, "
                  "r) in IFACE"},
      {"a function renamed",
       [](paykan::pkm::Interface &i) {
         for (auto &f : i.Functions)
           if (f.Name == "area")
             f.Name = "areas";
       },
       mismatch + "function 'areas' is not in CODE"},
      // The interface's own records disagree: an importer builds a class's
      // vtable from its superclass's.
      {"a superclass it does not describe",
       [&](paykan::pkm::Interface &i) {
         classNamed(i, "Circle").SuperClassName = "ahape";
       },
       "has a bad IFACE section: class 'Circle' extends 'ahape', which it "
       "does not describe"},
      {"an inherited slot renamed in the subclass alone",
       [&](paykan::pkm::Interface &i) { renameSlot(classNamed(i, "Circle")); },
       "has a bad IFACE section: the vtable of class 'Circle' does not start "
       "with the slots of its superclass 'Shape'"},
  };
  for (const Case &c : cases) {
    writeFile(dir / "geo.pkm", pristine);
    rewriteIface(dir / "geo.pkm", "geo", c.Change);
    for (const char *cmd : {"main.pkn", "--check-only main.pkn"}) {
      auto r = paykan(cmd);
      EXPECT_EQ(r.exitCode, 1) << c.What << ", " << cmd << ": " << r.out;
      EXPECT_TRUE(contains(r.out, "error: module 'geo' (geo.pkm) " + c.Why +
                                      ". Rebuild it from source.\n"))
          << c.What << ", " << cmd << ": " << r.out;
    }
    auto check = paykan("pkm check geo.pkm");
    EXPECT_EQ(check.exitCode, 1) << c.What << ": " << check.out;
    EXPECT_EQ(check.out, "geo.pkm: corrupt: " + c.Why + "\n") << c.What;
  }
  // The cache entry, with the source back: rebuilt.
  fs::remove(dir / "geo.pkm");
  fs::rename(dir / "geo.src", dir / "geo.pkn");
  writeFile(dir / ".paykan_cache" / "geo.pkm", cached);
  rewriteIface(dir / ".paykan_cache" / "geo.pkm", "geo", cases[2].Change);
  auto rebuilt = paykan("--verbose main.pkn");
  EXPECT_EQ(rebuilt.exitCode, 18) << rebuilt.out;
  EXPECT_TRUE(contains(rebuilt.out, "module geo: cache .paykan_cache/geo.pkm " +
                                        cases[2].Why +
                                        "; building from source\n"))
      << rebuilt.out;
  fs::remove_all(dir);
}

namespace {

/// The errors and notes of @p out, without the source text under each.
std::string diagLines(const std::string &out) {
  std::istringstream lines(out);
  std::string kept;
  for (std::string line; std::getline(lines, line);)
    if (contains(line, ": error: ") || contains(line, ": note: "))
      kept += line + "\n";
  return kept;
}

/// Modules lib/<name>.pkn (module lib::<name>) of Libs in Dir, and programs
/// beside them.
struct EveryPathProject {
  fs::path Dir;
  std::vector<std::string> Libs;
  /// The library prebuilt beside the others' sources on the mixed path.
  std::string Mixed;
  /// Programs and the errors and notes each is rejected with.
  std::vector<std::pair<std::string, std::string>> Rejected;
  /// A program that runs and prints OkOutput.
  std::string Ok;
  std::string OkOutput;
};

/// Each program of @p p.Rejected is rejected with exactly its errors and
/// notes by run, build at -O0 and -O2, and --emit-c, and @p p.Ok runs, built
/// too, and prints its output without a leak, on every path the modules come
/// from: their sources with a cold and a warm cache, --no-module-cache,
/// --rebuild-modules, prebuilt files with the sources gone (where a note in
/// a library's file has no source text to show), and @p p.Mixed prebuilt
/// beside the others' sources.
void expectOnEveryPath(const EveryPathProject &p) {
  // The words of a command line.
  auto words = [](std::initializer_list<std::string_view> parts) {
    std::string line;
    for (std::string_view part : parts)
      line.append(" ").append(part);
    return line;
  };
  for (const std::string &be : runnableBackends()) {
    // `paykan --backend=<be> <args>` from the project directory.
    auto paykan = [&](const std::string &args) {
      return run("cd " + p.Dir.string() + " && " + kPaykan + backendFlag(be) +
                 args);
    };
    const std::string exe = (p.Dir / "prog").string();
    auto every = [&](const std::string &flags, const std::string &how) {
      SCOPED_TRACE(testing::Message() << be << ", " << how);
      for (const auto &[prog, errors] : p.Rejected) {
        SCOPED_TRACE(prog);
        auto r = paykan(words({flags, prog}));
        EXPECT_NE(r.exitCode, 0) << r.out;
        EXPECT_EQ(diagLines(r.out), errors) << r.out;
        for (const char *opt : {"-O0", "-O2"}) {
          auto b = paykan(words({opt, flags, "build -o", exe, prog}));
          EXPECT_NE(b.exitCode, 0) << opt;
          EXPECT_EQ(diagLines(b.out), errors) << opt << ":\n" << b.out;
        }
        auto c = paykan(words({flags, "--emit-c", prog}));
        EXPECT_NE(c.exitCode, 0) << "--emit-c";
        EXPECT_EQ(diagLines(c.out), errors) << "--emit-c:\n"
                                            << c.out.substr(0, 4000);
      }
      SCOPED_TRACE(p.Ok);
      for (const char *opt : {"-O0", "-O2"}) {
        auto b = paykan(words({opt, flags, "build -o", exe, p.Ok}));
        ASSERT_EQ(b.exitCode, 0) << opt << ":\n" << b.out;
        auto ran = run(exe);
        EXPECT_EQ(ran.exitCode, 0) << opt;
        EXPECT_EQ(ran.out, p.OkOutput) << opt;
        fs::remove(exe);
      }
      auto c = paykan(words({flags, "--emit-c", p.Ok}));
      EXPECT_EQ(c.exitCode, 0) << "--emit-c:\n" << c.out.substr(0, 4000);
      auto ok = paykan(words({flags, "--track-heap", p.Ok}));
      EXPECT_EQ(ok.exitCode, 0) << ok.out;
      EXPECT_TRUE(contains(ok.out, p.OkOutput)) << ok.out;
      EXPECT_TRUE(contains(ok.out, "live blocks       : 0")) << ok.out;
    };

    // From source: the cold run writes the cache entries the warm one reads.
    fs::remove_all(p.Dir / ".paykan_cache");
    every("", "cold cache");
    auto warm = paykan(words({"--verbose", p.Ok}));
    for (const std::string &lib : p.Libs)
      EXPECT_TRUE(contains(warm.out, "module lib::" + lib + ": cache "))
          << be << ", " << lib << ":\n"
          << warm.out;
    EXPECT_FALSE(contains(warm.out, "building from source")) << warm.out;
    every("", "warm cache");
    every("--no-module-cache", "--no-module-cache");
    every("--rebuild-modules", "--rebuild-modules");

    // Prebuilt: every library a .pkm file on the module path, its source
    // gone.
    fs::remove_all(p.Dir / "pk");
    fs::create_directories(p.Dir / "pk" / "lib");
    for (const std::string &lib : p.Libs) {
      const fs::path source = fs::path("lib") / (lib + ".pkn");
      const fs::path pkm = fs::path("pk") / "lib" / (lib + ".pkm");
      auto emit =
          paykan(words({"--emit-pkm -o", pkm.string(), source.string()}));
      ASSERT_EQ(emit.exitCode, 0) << be << ": " << emit.out;
    }
    fs::rename(p.Dir / "lib", p.Dir / "src");
    fs::remove_all(p.Dir / ".paykan_cache");
    every("--module-path=pk", "prebuilt");

    // Mixed: one library prebuilt beside the others' sources.
    fs::rename(p.Dir / "src", p.Dir / "lib");
    fs::rename(p.Dir / "lib" / (p.Mixed + ".pkn"), p.Dir / "mixed.away");
    fs::copy_file(p.Dir / "pk" / "lib" / (p.Mixed + ".pkm"),
                  p.Dir / "lib" / (p.Mixed + ".pkm"));
    fs::remove_all(p.Dir / ".paykan_cache");
    every("", "mixed");
    fs::remove(p.Dir / "lib" / (p.Mixed + ".pkm"));
    fs::rename(p.Dir / "mixed.away", p.Dir / "lib" / (p.Mixed + ".pkn"));
  }
}

} // namespace

// A template of another module bounded by that module's own trait is
// reported at the importer's request (#147), and a bound checked once the
// `impl` blocks are known in the template's file, whether the module comes
// from its source, the cache, or a prebuilt file, on run, build and
// --emit-c alike; the module's function that is not generic runs on each.
TEST(Pkm, AForeignTraitBoundIsReportedOnEveryPath) {
  REQUIRE_BACKEND();
  EveryPathProject p;
  p.Dir = fs::temp_directory_path() /
          ("pkm_foreign_bound_" + std::to_string(getpid()));
  fs::remove_all(p.Dir);
  writeFile(p.Dir / "lib" / "pets.pkn",
            R"(trait Named { view fn name() -> Str; }
class Dog { view fn name() -> Str { return "Rex"; } }
impl Named for Dog {}
fn greet<T: Named>(x: view T) -> Str { return "hello, " + x.name(); }
fn wrap<T>(x: T) -> Str { return greet(Dog()); }
fn wrapn() -> Str { return greet(Dog()); }
class Set<K: Hashable> { k: K; }
struct _F { a: float; }
class Holder<T> {
  s: Set<_F>;
}
)");
  writeFile(p.Dir / "lib" / "greeter.pkn",
            "import lib::pets;\n"
            "fn hi() -> Str { return pets::wrapn(); }\n");
  writeFile(p.Dir / "main.pkn",
            "import ::io;\n"
            "import lib::pets;\n"
            "class M { h: pets::Holder<int>; }\n"
            "fn main() -> int { io::println(pets::wrap(1)); return 0; }\n");
  writeFile(p.Dir / "ok.pkn", "import ::io;\n"
                              "import lib::greeter;\n"
                              "import lib::pets;\n"
                              "fn main() -> int {\n"
                              "  io::println(greeter::hi() + \" \" + "
                              "pets::wrapn());\n"
                              "  return 0;\n"
                              "}\n");
  p.Libs = {"pets", "greeter"};
  p.Mixed = "pets";
  p.Rejected = {
      {"main.pkn",
       "lib/pets.pkn:10:3: error: '_F' does not implement trait 'Hashable', "
       "which 'lib::pets::Set' requires of 'K'\n"
       "main.pkn:3:11: note: in instantiation of 'Holder<int>' requested "
       "here\n"
       "main.pkn:4:32: error: 'lib::pets::wrap<int>' cannot be instantiated "
       "here: it uses 'lib::pets::greet', which requires 'T' to implement "
       "trait 'Named' of module 'lib::pets', and another module's traits are "
       "not supported yet (#147)\n"
       "lib/pets.pkn:5:34: note: 'lib::pets::greet' is used here\n"}};
  p.Ok = "ok.pkn";
  p.OkOutput = "hello, Rex hello, Rex\n";
  expectOnEveryPath(p);
  fs::remove_all(p.Dir);
}

// A generic type's `impl Trait for` blocks go with its template whether its
// module comes from its source, the cache or a prebuilt file, and an
// importer's instantiation checks them as the module would check its own:
// an `impl Hashable for` block's body, a block of the module's own trait,
// whose methods it gets but not the trait, and an `impl Show for` block of
// a class.  An importer's subclass of the module's Hashable class is
// Hashable through it, and cannot repeat the conformance.
TEST(Pkm, TraitBlocksAndSubclassesOnEveryPath) {
  REQUIRE_BACKEND();
  EveryPathProject p;
  p.Dir = fs::temp_directory_path() /
          ("pkm_trait_blocks_" + std::to_string(getpid()));
  fs::remove_all(p.Dir);
  writeFile(p.Dir / "lib" / "gb.pkn", R"(trait Named { view fn name() -> Str; }
class Box<T> { v: T; }
impl Box<T> { fn __init__(v: T) { self.v = v; } }
impl Hashable for Box<T> { view fn hash() -> int { return self.v.hash(); } }
impl Named for Box<T> { view fn name() -> Str { return "gbox"; } }
class NBox<T> { v: T; }
impl NBox<T> { fn __init__(v: T) { self.v = v; } }
impl Named for NBox<T> { view fn name() -> Str { return self.v.name(); } }
class SBox<T> { v: T; }
impl SBox<T> { fn __init__(v: T) { self.v = v; } }
impl Show for SBox<T> {}
class CK { v: int; }
impl CK { fn __init__(v: int) { self.v = v; } }
impl Hashable for CK { view fn hash() -> int { return self.v; } }
)");
  writeFile(p.Dir / "lib" / "mk.pkn",
            "import lib::gb;\n"
            "fn mk() -> gb::Box<int> { return gb::Box<int>(6); }\n");
  const std::string kRequest = ".pkn:2:24: note: in instantiation of '";
  writeFile(p.Dir / "pf.pkn",
            "import lib::gb;\n"
            "fn main() -> int { b = gb::Box<float>(1.5); return 0; }\n");
  writeFile(p.Dir / "named.pkn",
            "import lib::gb;\n"
            "fn main() -> int { b = gb::NBox<int>(1); return 0; }\n");
  writeFile(p.Dir / "show.pkn",
            "import lib::gb;\n"
            "fn main() -> int { b = gb::SBox<int>(1); return 0; }\n");
  writeFile(p.Dir / "own.pkn",
            "import lib::gb;\n"
            "trait Named { view fn name() -> Str; }\n"
            "fn g<T: Named>(x: view T) -> Str { return x.name(); }\n"
            "fn main() -> int { s = g(gb::Box<Str>(\"s\")); return 0; }\n");
  writeFile(p.Dir / "sub.pkn", "import lib::gb;\n"
                               "class Sub : gb::CK {}\n"
                               "impl Hashable for Sub {}\n"
                               "fn main() -> int { return 0; }\n");
  writeFile(p.Dir / "ok.pkn",
            R"(import ::io;
import lib::gb;
import lib::mk;
fn h<T: Hashable>(x: view T) -> int { return x.hash(); }
class Sub : gb::CK {}
impl Sub { fn __init__(v: int) { __super__(v); } }
fn main() -> int {
  io::println(gb::Box<Str>("s").name() + " " + gb::NBox<gb::Box<int>>(gb::Box<int>(1)).name());
  io::println(Str(h(gb::Box<int>(4)) == h(4)) + " " + Str(h(Sub(2))) + " " + Str(h(mk::mk()) == h(6)));
  return 0;
}
)");
  p.Libs = {"gb", "mk"};
  p.Mixed = "gb";
  p.Rejected = {
      {"pf.pkn", "lib/gb.pkn:4:59: error: method call on non-class type "
                 "'float'\npf" +
                     kRequest + "Box<float>' requested here\n"},
      {"named.pkn", "lib/gb.pkn:8:57: error: method call on non-class type "
                    "'int'\nnamed" +
                        kRequest + "NBox<int>' requested here\n"},
      {"show.pkn", "lib/gb.pkn:11:1: error: class 'SBox<int>' already "
                   "implements 'Show' (through 'Obj')\nshow" +
                       kRequest + "SBox<int>' requested here\n"},
      {"own.pkn", "own.pkn:4:24: error: 'Box<Str>' does not implement trait "
                  "'Named', which 'g' requires of 'T'\n"},
      {"sub.pkn", "sub.pkn:3:1: error: class 'Sub' already implements "
                  "'Hashable' (through 'CK')\n"}};
  p.Ok = "ok.pkn";
  p.OkOutput = "gbox gbox\nTrue 2 True\n";
  expectOnEveryPath(p);
  fs::remove_all(p.Dir);
}
