// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Smoke tests for the paykan driver CLI (--check-only, --dump-ast, --version).
// The PAYKAN_BIN CMake variable is injected so tests find the binary.

#include <gtest/gtest.h>

#include "Version.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#ifndef PAYKAN_BIN
#error "PAYKAN_BIN must be defined via CMake compile definition"
#endif

static const char *kPaykan = PAYKAN_BIN;

// Write source to a temp .pkn file and return its path.
static std::string writeTmp(const std::string &src) {
  auto tmp = std::filesystem::temp_directory_path() /
             ("drv_test_" + std::to_string(getpid()) + "_" +
              std::to_string(rand()) + ".pkn");
  std::ofstream ofs(tmp);
  ofs << src;
  return tmp.string();
}

struct CmdResult {
  int exitCode;
  std::string out;
};

static CmdResult run(const std::string &cmd) {
  std::array<char, 4096> buf{};
  std::string out;
  FILE *fp = popen(cmd.c_str(), "r");
  if (!fp)
    return {-1, ""};
  while (fgets(buf.data(), buf.size(), fp))
    out += buf.data();
  int rc = pclose(fp);
  return {WEXITSTATUS(rc), out};
}

// True when the build has a backend that can run programs (an empty
// PAYKAN_BACKENDS builds a compiler that only parses and checks).
static bool hasBackend() {
  static const bool has = [] {
    auto r = run(std::string(kPaykan) + " --list-backends 2>&1");
    return r.exitCode == 0 && !r.out.empty();
  }();
  return has;
}

#define REQUIRE_BACKEND()                                                      \
  do {                                                                         \
    if (!hasBackend())                                                         \
      GTEST_SKIP() << "this build has no backend";                             \
  } while (0)

// The backend every program-running test uses: PAYKAN_TEST_BACKEND (ctest
// runs this suite once per backend), or the build's default when unset.
static std::string testBackend() {
  const char *env = std::getenv("PAYKAN_TEST_BACKEND");
  return env && env[0] ? env : "";
}

// " --backend=<name>" for the test backend, or "" for the default one.
static std::string backendFlag() {
  std::string b = testBackend();
  return b.empty() ? "" : " --backend=" + b;
}

// The paykan command running programs on the test backend.
static std::string paykanRun() { return std::string(kPaykan) + backendFlag(); }

// The backends that run programs, of those this build has: what
// `paykan --list-backends` lists (`<name>: <description>`, or
// `<name> (default)`).
static std::vector<std::string> runBackends() {
  std::string listing =
      run(std::string(kPaykan) + " --no-plugins --list-backends 2>&1").out;
  std::vector<std::string> out;
  for (const std::string be : {"mlir"}) {
    size_t at = 0;
    while ((at = listing.find(be, at)) != std::string::npos) {
      bool lineStart = at == 0 || listing[at - 1] == '\n';
      char after =
          at + be.size() < listing.size() ? listing[at + be.size()] : '\n';
      if (lineStart && (after == ':' || after == ' ' || after == '\n')) {
        out.push_back(be);
        break;
      }
      at += be.size();
    }
  }
  return out;
}

// -- The --check-only flag

TEST(Driver, CheckOnlySucceedsOnValidInput) {
  auto src = writeTmp("fn main() -> int { return 0; }");
  auto [rc, _] = run(std::string(kPaykan) + " --check-only " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 0);
}

TEST(Driver, CheckOnlyFailsOnSyntaxError) {
  auto src = writeTmp("fn main() -> int { return }");
  auto [rc, out] = run(std::string(kPaykan) + " --check-only " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_FALSE(out.empty());
}

TEST(Driver, CheckOnlyFailsOnSemanticError) {
  auto src = writeTmp("fn main() -> int { x: int = \"hello\"; return 0; }");
  auto [rc, out] = run(std::string(kPaykan) + " --check-only " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_FALSE(out.empty());
}

// -- The --version / -v flag

TEST(Driver, VersionFlagPrintsVersion) {
  auto [rc, out] = run(std::string(kPaykan) + " --version 2>&1");
  EXPECT_EQ(rc, 0);
  EXPECT_NE(out.find("PaykanLang"), std::string::npos);
  EXPECT_NE(out.find(paykan::kVersion), std::string::npos);
  // The build's frontends and backends are listed too.
  EXPECT_NE(out.find("frontend "), std::string::npos) << out;
}

TEST(Driver, ShortVersionFlagMatchesLong) {
  auto longOut = run(std::string(kPaykan) + " --version 2>&1");
  auto shortOut = run(std::string(kPaykan) + " -v 2>&1");
  EXPECT_EQ(shortOut.exitCode, 0);
  EXPECT_EQ(shortOut.out, longOut.out);
}

// Plugin compatibility (#103): every plugin records the PaykanLang version it
// was built with, and paykan accepts only the versions on its list.

#ifndef PAYKAN_INCOMPATIBLE_PLUGINS_BIN
#error "PAYKAN_INCOMPATIBLE_PLUGINS_BIN must be defined via CMake"
#endif
#ifndef PAYKAN_TEST_ACCEPTED_VERSIONS
#error "PAYKAN_TEST_ACCEPTED_VERSIONS must be defined via CMake"
#endif

// A driver with the build's plugins plus "old-frontend" and "old-backend",
// built with PaykanLang 0.0.9 (Driver/IncompatiblePlugins.cpp).  Their
// factories abort, so any instantiation fails the test.
static const char *kPaykanIncompat = PAYKAN_INCOMPATIBLE_PLUGINS_BIN;

static std::string incompatibleReason() {
  return std::string(
             "incompatible: built with PaykanLang 0.0.9; this paykan ") +
         paykan::kVersion + " accepts " + PAYKAN_TEST_ACCEPTED_VERSIONS;
}

static std::vector<std::string> lines(const std::string &out) {
  std::vector<std::string> result;
  std::istringstream in(out);
  for (std::string line; std::getline(in, line);)
    result.push_back(line);
  return result;
}

static bool hasLine(const std::string &out, const std::string &line) {
  auto ls = lines(out);
  return std::find(ls.begin(), ls.end(), line) != ls.end();
}

// The built-in plugins go through the same check and are always compatible:
// --version lists each with the version it was built with.
TEST(Driver, VersionListsEveryPluginAsCompatible) {
  auto [rc, out] = run(std::string(kPaykan) + " --version 2>/dev/null");
  ASSERT_EQ(rc, 0) << out;
  auto ls = lines(out);
  ASSERT_GE(ls.size(), 4u) << out;
  EXPECT_EQ(ls[0], std::string("PaykanLang ") + paykan::kVersion);
  EXPECT_EQ(ls[1], std::string("accepts plugins built with PaykanLang ") +
                       PAYKAN_TEST_ACCEPTED_VERSIONS);
  const std::string compatible = std::string(" (built with PaykanLang ") +
                                 paykan::kVersion + ", compatible)";
  unsigned frontends = 0, backends = 0, otherLines = 0;
  for (size_t i = 2; i < ls.size(); ++i) {
    const std::string &l = ls[i];
    // Then the plugin API, the standard library and the plugin directories
    // (none searched: the ctest runs with PAYKAN_NO_PLUGINS=1).
    if (l.rfind("plugin ", 0) == 0 || l.rfind("standard library: ", 0) == 0) {
      ++otherLines;
      continue;
    }
    EXPECT_NE(l.find(compatible), std::string::npos) << l;
    EXPECT_EQ(l.find("incompatible"), std::string::npos) << l;
    frontends += l.rfind("frontend ", 0) == 0;
    backends += l.rfind("backend ", 0) == 0;
  }
  EXPECT_TRUE(hasLine(out, "frontend recursive-descent" + compatible)) << out;
  EXPECT_GE(frontends, 1u);
  EXPECT_GE(backends, 1u);
  EXPECT_EQ(frontends + backends + otherLines, ls.size() - 2) << out;
  // Where the system imports come from (the build tree's, here).
  EXPECT_NE(out.find("\nstandard library: "), std::string::npos) << out;
  EXPECT_TRUE(hasLine(out, "plugin API " +
                               paykan::versionString(paykan::kVersionNumber)))
      << out;
  EXPECT_TRUE(hasLine(out, "plugin directories: none searched (--no-plugins "
                           "or PAYKAN_NO_PLUGINS)"))
      << out;
}

TEST(Driver, VersionMarksAnIncompatiblePlugin) {
  auto [rc, out] = run(std::string(kPaykanIncompat) + " --version 2>&1");
  ASSERT_EQ(rc, 0) << out;
  EXPECT_TRUE(
      hasLine(out, "frontend old-frontend (" + incompatibleReason() + ")"))
      << out;
  EXPECT_TRUE(
      hasLine(out, "backend old-backend (" + incompatibleReason() + ")"))
      << out;
  EXPECT_TRUE(hasLine(out, std::string("frontend recursive-descent (built "
                                       "with PaykanLang ") +
                               paykan::kVersion + ", compatible)"))
      << out;
}

TEST(Driver, ListingShowsAnIncompatiblePlugin) {
  auto fe = run(std::string(kPaykanIncompat) + " --list-frontends 2>&1");
  ASSERT_EQ(fe.exitCode, 0) << fe.out;
  EXPECT_TRUE(hasLine(fe.out, "old-frontend (" + incompatibleReason() + ")"))
      << fe.out;
  // The compatible ones are listed as usual.
  EXPECT_TRUE(hasLine(fe.out, "recursive-descent (default)")) << fe.out;

  auto be = run(std::string(kPaykanIncompat) + " --list-backends 2>&1");
  ASSERT_EQ(be.exitCode, 0) << be.out;
  EXPECT_TRUE(hasLine(be.out, "old-backend (" + incompatibleReason() + ")"))
      << be.out;
  auto normal = run(std::string(kPaykan) + " --list-backends 2>&1");
  for (const std::string &l : lines(normal.out))
    EXPECT_TRUE(hasLine(be.out, l)) << l << "\n" << be.out;
  EXPECT_EQ(lines(be.out).size(), lines(normal.out).size() + 1) << be.out;
}

// Selecting an incompatible plugin fails with exit status 2 and the reason,
// before anything is parsed or compiled, and never instantiates it.
TEST(Driver, SelectingAnIncompatiblePluginFails) {
  auto src = writeTmp("fn main() -> int { return 0; }");
  for (const std::string kind : {"frontend", "backend"}) {
    const std::string plugin = "old-" + kind;
    std::string expected = "paykan: cannot use ";
    expected += kind;
    expected += " '" + plugin + "' (";
    expected += incompatibleReason();
    expected += ")\n";
    for (const std::string args :
         {" --check-only ", " --emit-source ", " --dump-ast ", " "}) {
      std::string cmd = kPaykanIncompat;
      cmd += " --" + kind;
      cmd += "=" + plugin;
      cmd += args;
      cmd += src;
      cmd += " 2>&1";
      auto [rc, out] = run(cmd);
      EXPECT_EQ(rc, 2) << cmd << "\n" << out;
      EXPECT_EQ(out, expected) << cmd;
    }
    std::string spaced = kPaykanIncompat;
    spaced += " --" + kind;
    spaced += " " + plugin;
    spaced += " --check-only ";
    spaced += src;
    spaced += " 2>&1";
    auto r = run(spaced);
    EXPECT_EQ(r.exitCode, 2) << spaced << "\n" << r.out;
    EXPECT_EQ(r.out, expected) << spaced;
  }
  // The compatible plugins of the same driver still work.
  auto ok =
      run(std::string(kPaykanIncompat) + " --check-only " + src + " 2>&1");
  EXPECT_EQ(ok.exitCode, 0) << ok.out;
  auto okFe = run(std::string(kPaykanIncompat) +
                  " --frontend=recursive-descent --dump-ast " + src + " 2>&1");
  EXPECT_EQ(okFe.exitCode, 0) << okFe.out;
  std::filesystem::remove(src);
}

// --list-backends: the default is the one the build configured, mlir, the
// in-tree backend.

#ifndef PAYKAN_EXPECTED_DEFAULT_BACKEND
#error "PAYKAN_EXPECTED_DEFAULT_BACKEND must be defined via CMake"
#endif

TEST(Driver, ListBackendsMarksTheConfiguredDefault) {
  auto [rc, out] = run(std::string(kPaykan) + " --list-backends 2>&1");
  ASSERT_EQ(rc, 0) << out;
  const std::string expected = PAYKAN_EXPECTED_DEFAULT_BACKEND;
  // Exactly one backend is marked, and it is the configured one.
  size_t mark = out.find(" (default)");
  ASSERT_NE(mark, std::string::npos) << out;
  EXPECT_EQ(out.find(" (default)", mark + 1), std::string::npos) << out;
  // Each line is "<name>[ (default)][: <description>]".
  std::string defaultName;
  size_t pos = 0;
  while (pos < out.size()) {
    size_t eol = out.find('\n', pos);
    std::string line = out.substr(pos, eol - pos);
    pos = eol == std::string::npos ? out.size() : eol + 1;
    std::string name = line.substr(0, line.find_first_of(" :"));
    if (line.compare(name.size(), 10, " (default)") == 0)
      defaultName = name;
  }
  EXPECT_EQ(defaultName, expected) << out;
  EXPECT_EQ(expected, "mlir");
}

// Without --backend the program goes to the mlir backend: --emit-source and
// --emit-llvm print its LLVM IR, --emit-c its C, and `run` runs it.
TEST(Driver, DefaultBackendIsMLIR) {
  auto src = writeTmp(
      "import ::io; fn main() -> int { io::println(\"hi\"); return 0; }");
  for (const char *flag : {" --emit-source ", " --emit-llvm "}) {
    auto [rc, ir] = run(std::string(kPaykan) + flag + src + " 2>&1");
    EXPECT_EQ(rc, 0) << flag << ir;
    EXPECT_NE(ir.find("ModuleID"), std::string::npos) << flag << ir;
  }
  auto [rcC, code] = run(std::string(kPaykan) + " --emit-c " + src + " 2>&1");
  EXPECT_EQ(rcC, 0) << code;
  EXPECT_NE(code.find("#include \"Runtime.h\""), std::string::npos) << code;
  auto [rcRun, out] = run(std::string(kPaykan) + " " + src + " 2>&1");
  EXPECT_EQ(rcRun, 0) << out;
  EXPECT_EQ(out, "hi\n");
  std::filesystem::remove(src);
}

// Without --backend, programs run on the configured default backend.
TEST(Driver, DefaultBackendRunsPrograms) {
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_defaultbe_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  auto src = (dir / "hi.pkn").string();
  std::ofstream(src)
      << "import ::io; fn main() -> int { io::println(\"hi\"); return 3; }";
  auto [rc, out] = run(std::string(kPaykan) + " " + src + " 2>&1");
  auto [rc2, out2] = run(std::string(kPaykan) + " --backend=" +
                         PAYKAN_EXPECTED_DEFAULT_BACKEND + " " + src + " 2>&1");
  std::filesystem::remove_all(dir);
  EXPECT_EQ(rc, 3) << out;
  EXPECT_EQ(out, "hi\n");
  EXPECT_EQ(rc2, rc);
  EXPECT_EQ(out2, out);
}

// -- The --frontend / --list-frontends flags

TEST(Driver, ListFrontendsNamesTheDefault) {
  auto [rc, out] = run(std::string(kPaykan) + " --list-frontends 2>&1");
  EXPECT_EQ(rc, 0);
  // Every build has a default frontend, marked in the listing.
  EXPECT_NE(out.find(" (default)"), std::string::npos) << out;
}

TEST(Driver, EveryListedFrontendParses) {
  auto listed = run(std::string(kPaykan) + " --list-frontends 2>&1");
  ASSERT_EQ(listed.exitCode, 0);
  auto src = writeTmp("fn main() -> int { return 0; }");
  size_t pos = 0;
  unsigned count = 0;
  while (pos < listed.out.size()) {
    size_t eol = listed.out.find('\n', pos);
    std::string line = listed.out.substr(pos, eol - pos);
    pos = (eol == std::string::npos) ? listed.out.size() : eol + 1;
    std::string name = line.substr(0, line.find(' '));
    if (name.empty())
      continue;
    ++count;
    std::string cmd = kPaykan;
    cmd += " --frontend=";
    cmd += name;
    cmd += " --check-only ";
    cmd += src;
    cmd += " 2>&1";
    auto [rc, out] = run(cmd);
    EXPECT_EQ(rc, 0) << name << ": " << out;
    std::string spacedCmd = kPaykan;
    spacedCmd += " --frontend ";
    spacedCmd += name;
    spacedCmd += " --check-only ";
    spacedCmd += src;
    spacedCmd += " 2>&1";
    auto spaced = run(spacedCmd);
    EXPECT_EQ(spaced.exitCode, 0) << name << ": " << spaced.out;
  }
  std::filesystem::remove(src);
  EXPECT_GE(count, 1u);
}

// The frontends --list-frontends names.
static std::vector<std::string> listedFrontends() {
  std::vector<std::string> names;
  auto listed = run(std::string(kPaykan) + " --list-frontends 2>&1");
  std::istringstream lines(listed.out);
  for (std::string line; std::getline(lines, line);)
    if (auto name = line.substr(0, line.find(' ')); !name.empty())
      names.push_back(name);
  return names;
}

static size_t countErrors(const std::string &out) {
  size_t n = 0;
  for (size_t at = out.find(" error: "); at != std::string::npos;
       at = out.find(" error: ", at + 1))
    ++n;
  return n;
}

// A missing or ill-typed `main` (or an empty file) is one Sema error with a
// source location, on every frontend -- not a PIR verifier failure (#132).
// --check-only accepts a module without `main` but still checks a declared
// one.
TEST(Driver, EntryPointIsCheckedWithASourceLocation) {
  auto frontends = listedFrontends();
  ASSERT_FALSE(frontends.empty());
  struct Case {
    std::string Source;
    std::string Error; // the one error, after the file name
    int CheckOnlyExit;
  };
  const std::string none = ":1:1: error: program has no entry point 'fn "
                           "main() -> int' (or 'fn main(args: view Str[]) -> "
                           "int')";
  const std::string must = ":1:1: error: the program's entry point must be "
                           "'fn main() -> int' or 'fn main(args: view Str[]) "
                           "-> int', not ";
  const std::vector<Case> cases = {
      {"", none, 0},
      {"fn f() -> int { return 1; }\n", none, 0},
      {"fn main() -> Str { return \"\"; }\n", must + "'fn main() -> Str'", 1},
      {"fn main(n: int) -> int { return n; }\n", must + "'fn main(int) -> int'",
       1},
      {"fn main(args: inout Str[]) -> int { return 0; }\n",
       must + "'fn main(inout Str[]) -> int'", 1},
  };
  for (const auto &fe : frontends) {
    const std::string paykan = std::string(kPaykan) + " --frontend=" + fe;
    for (const auto &c : cases) {
      auto src = writeTmp(c.Source);
      std::string expected = src;
      expected += c.Error;
      for (const char *mode : {" ", " --emit-pir ", " -o /dev/null build "}) {
        std::string cmd = paykan;
        cmd += mode;
        cmd += src;
        cmd += " 2>&1";
        auto [rc, out] = run(cmd);
        EXPECT_EQ(rc, 1) << cmd << "\n" << out;
        EXPECT_EQ(countErrors(out), 1u) << cmd << "\n" << out;
        EXPECT_NE(out.find(expected), std::string::npos) << cmd << "\n" << out;
        EXPECT_EQ(out.find("PIR verification"), std::string::npos) << out;
      }
      std::string cmd = paykan;
      cmd += " --check-only ";
      cmd += src;
      cmd += " 2>&1";
      auto [rc, out] = run(cmd);
      EXPECT_EQ(rc, c.CheckOnlyExit) << cmd << "\n" << out;
      EXPECT_EQ(countErrors(out), c.CheckOnlyExit ? 1u : 0u) << out;
      std::filesystem::remove(src);
    }
  }
}

TEST(Driver, UnknownFrontendIsRejected) {
  auto src = writeTmp("fn main() -> int { return 0; }");
  auto [rc, out] = run(std::string(kPaykan) + " --frontend=no-such-frontend " +
                       src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_NE(out.find("unknown frontend 'no-such-frontend'"), std::string::npos)
      << out;
}

// -- The --dump-tokens flag

TEST(Driver, DumpTokensPrintsTheTokenStream) {
  auto src = writeTmp("fn main() -> int {\n  return 42;\n}\n");
  auto [rc, out] =
      run(std::string(kPaykan) +
          " --frontend=recursive-descent --dump-tokens " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 0) << out;
  EXPECT_NE(out.find("1:1-1:3 KW_FN fn\n"), std::string::npos) << out;
  EXPECT_NE(out.find("2:10-2:12 INT 42\n"), std::string::npos) << out;
  EXPECT_NE(out.find("4:1-4:1 EOF\n"), std::string::npos) << out;
}

// A lexical error is reported as the frontend's other diagnostics are, with
// the file, the position and a snippet (docs/grammar.md section 9), and
// fails the dump, which is still written whole (the bad bytes skipped).
TEST(Driver, DumpTokensReportsLexicalErrors) {
  auto src = writeTmp("fn main() -> int { x = 1 $ 2; return \"abc; }\n");
  auto [rc, err] =
      run(std::string(kPaykan) + " --dump-tokens " + src + " 2>&1 >/dev/null");
  auto [rcOut, out] =
      run(std::string(kPaykan) + " --dump-tokens " + src + " 2>/dev/null");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 1) << err;
  EXPECT_EQ(err, src +
                     ":1:26: error: invalid character '$'\n"
                     "  1 | fn main() -> int { x = 1 $ 2; return \"abc; }\n"
                     "    |                          ^\n" +
                     src +
                     ":1:38: error: unterminated string literal (string "
                     "literals cannot span multiple lines; use \\n)\n"
                     "  1 | fn main() -> int { x = 1 $ 2; return \"abc; }\n"
                     "    |                                      ^~~~~~~\n");
  EXPECT_EQ(rcOut, 1);
  EXPECT_NE(out.find("1:24-1:25 INT 1\n1:28-1:29 INT 2\n"), std::string::npos)
      << out;
  EXPECT_NE(out.find("2:1-2:1 EOF\n"), std::string::npos) << out;
}

TEST(Driver, DumpTokensFailsOnUnreadableFile) {
  auto [rc, out] = run(std::string(kPaykan) +
                       " --dump-tokens /nonexistent/paykan_no_such.pkn 2>&1");
  EXPECT_NE(rc, 0);
  EXPECT_NE(out.find("cannot open"), std::string::npos) << out;
}

TEST(Driver, UnknownOptionIsRejected) {
  auto src = writeTmp("fn main() -> int { return 0; }");
  auto [rc, out] =
      run(std::string(kPaykan) + " --no-such-option " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_NE(out.find("unknown command line argument '--no-such-option'"),
            std::string::npos)
      << out;
}

// -O takes 0..3 (#120): anything above is rejected, not clamped or ignored.
TEST(Driver, OptimizationLevelAboveThreeIsRejected) {
  auto src = writeTmp("fn main() -> int { return 0; }");
  for (const char *opt : {"-O4", "-O9", "-O10", "-O999", "-O=4", "-O 4"}) {
    auto [rc, out] = run(std::string(kPaykan) + " --check-only " + opt + " " +
                         src + " 2>&1");
    EXPECT_NE(rc, 0) << opt;
    EXPECT_NE(out.find("invalid optimization level"), std::string::npos)
        << opt << ": " << out;
  }
  for (const char *opt : {"-O0", "-O1", "-O2", "-O3", "-O=3", "-O 0"}) {
    auto [rc, out] = run(std::string(kPaykan) + " --check-only " + opt + " " +
                         src + " 2>&1");
    EXPECT_EQ(rc, 0) << opt << ": " << out;
  }
  std::filesystem::remove(src);
}

// A directory is not a source file (#120): a clean error, never an uncaught
// std::ios_base::failure (SIGABRT), whether it is the input or an import.
TEST(Driver, DirectoryAsSourceFileIsRejected) {
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_test_dir_" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / "src.pkn");
  for (const char *mode :
       {" --check-only ", " --dump-ast ", " --dump-tokens ", " "}) {
    auto [rc, out] =
        run(std::string(kPaykan) + mode + (dir / "src.pkn").string() + " 2>&1");
    EXPECT_TRUE(rc == 1 || rc == 2) << mode << rc << ": " << out;
    EXPECT_NE(out.find("is a directory, not a source file"), std::string::npos)
        << mode << out;
  }
  std::filesystem::remove_all(dir);
}

TEST(Driver, ImportOfADirectoryIsRejected) {
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_test_dirimport_" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / "lib" / "m.pkn");
  std::ofstream(dir / "main.pkn")
      << "import lib::m;\nfn main() -> int { return m::f(); }\n";
  auto [rc, out] = run(std::string(kPaykan) + " --check-only " +
                       (dir / "main.pkn").string() + " 2>&1");
  EXPECT_EQ(rc, 1) << out;
  EXPECT_NE(out.find("error: module 'lib::m': 'lib/m.pkn' is a directory, not "
                     "a source file"),
            std::string::npos)
      << out;
  // One error: the use of `m::f` is not reported as well.
  EXPECT_EQ(out.find("undeclared"), std::string::npos) << out;
  std::filesystem::remove_all(dir);
}

TEST(Driver, ProgramArgumentsFollowTheSourceFile) {
  REQUIRE_BACKEND();
  // Everything after the source file is the program's, even if it looks
  // like an option.
  auto src =
      writeTmp("fn main(args: view Str[]) -> int { return args.len(); }");
  auto [rc, out] = run(paykanRun() + " " + src + " --dump-ast -O2 x 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 4) << out;
}

// -- integer divide / modulo by zero trap

TEST(Driver, IntDivByZeroTraps) {
  REQUIRE_BACKEND();
  auto src =
      writeTmp("fn main() -> int { z: int = 0; q: int = 7 / z; return q; }");
  auto [rc, out] = run(paykanRun() + " " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_NE(out.find("division or modulo by zero"), std::string::npos) << out;
}

TEST(Driver, IntModByZeroTraps) {
  REQUIRE_BACKEND();
  auto src =
      writeTmp("fn main() -> int { z: int = 0; r: int = 7 % z; return r; }");
  auto [rc, out] = run(paykanRun() + " " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_NE(out.find("division or modulo by zero"), std::string::npos) << out;
}

// A fixed-size array's element read or written outside its bounds panics.
TEST(Driver, FixedArrayIndexOutOfBoundsPanics) {
  REQUIRE_BACKEND();
  auto src = writeTmp("fn main() -> int {\n"
                      "  xs: int[3] = [1, 2, 3];\n"
                      "  i = 3;\n"
                      "  return xs[i];\n"
                      "}\n");
  auto [rc, out] = run(paykanRun() + " " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 128 + SIGABRT) << out;
  EXPECT_NE(out.find("paykan: array index 3 out of bounds (len=3)"),
            std::string::npos)
      << out;
  // A slice checks every index, a constant one too, against its length.
  src = writeTmp("fn at(xs: view int[]) -> int { return xs[2]; }\n"
                 "fn main() -> int {\n"
                 "  xs: int[2] = [1, 2];\n"
                 "  return at(xs);\n"
                 "}\n");
  auto [sliceRc, sliceOut] = run(paykanRun() + " " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(sliceRc, 128 + SIGABRT) << sliceOut;
  EXPECT_NE(sliceOut.find("paykan: array index 2 out of bounds (len=2)"),
            std::string::npos)
      << sliceOut;
}

// #117: a user function spelled like the runtime's panic, with its very
// signature, is an ordinary function: it runs when called, and a division by
// zero still reaches the runtime's panic (message, stdout flushed, SIGABRT),
// on every frontend, with `paykan run` and a built executable.
TEST(Driver, UserPanicNamedFunctionLeavesTheRuntimePanicAlone) {
  REQUIRE_BACKEND();
  auto src = writeTmp("import ::io; fn Paykan_panic_div_by_zero() { "
                      "io::println(\"user fn\"); }\n"
                      "fn z() -> int { return 0; }\n"
                      "fn main() -> int {\n"
                      "  Paykan_panic_div_by_zero();\n"
                      "  io::println(Str<int>(5 / z()));\n"
                      "  io::println(\"after\");\n"
                      "  return 0;\n"
                      "}\n");
  auto exe = src + ".exe";
  auto listed = run(std::string(kPaykan) + " --list-frontends 2>&1");
  ASSERT_EQ(listed.exitCode, 0);
  std::istringstream lines(listed.out);
  unsigned count = 0;
  for (std::string line; std::getline(lines, line);) {
    std::string fe = line.substr(0, line.find(' '));
    if (fe.empty())
      continue;
    ++count;
    std::string runCmd = paykanRun();
    runCmd += " --frontend=";
    runCmd += fe;
    std::string build = runCmd;
    build += " -o ";
    build += exe;
    build += " build ";
    build += src;
    build += " 2>&1";
    auto [brc, bout] = run(build);
    ASSERT_EQ(brc, 0) << fe << ": " << bout;
    std::string runSrc = runCmd;
    runSrc += " ";
    runSrc += src;
    for (const std::string &cmd : {runSrc, exe}) {
      std::string merged = "exec 3>&1 2>/dev/null; (";
      merged += cmd;
      merged += ") 2>&3";
      auto [rc, both] = run(merged);
      EXPECT_EQ(rc, 128 + SIGABRT) << fe << ": " << cmd;
      EXPECT_EQ(both, "user fn\npaykan: integer division or modulo by zero\n")
          << fe << ": " << cmd;
    }
  }
  EXPECT_GE(count, 1u);
  std::filesystem::remove(src);
  std::filesystem::remove(exe);
}

// INT64_MIN / -1 does not fit in an int: a runtime panic, not SIGFPE.
TEST(Driver, IntDivOverflowTraps) {
  REQUIRE_BACKEND();
  auto src = writeTmp("fn main() -> int { m: int = -9223372036854775807 - 1; "
                      "d: int = -1; q: int = m / d; return q; }");
  auto [rc, out] = run(paykanRun() + " " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_NE(rc, 128 + SIGFPE) << out;
  EXPECT_NE(out.find("integer overflow in division"), std::string::npos) << out;
}

// -- Conversion panics (#64): int<float> outside int64, char<int> outside
// 0..255

static void expectConversionPanic(const std::string &decl,
                                  const std::string &conv,
                                  const std::string &message) {
  // (Nothing printed after the conversion may appear; the output printed
  // before a panic is covered by PanicFlushesStdoutOnEveryBackend.)
  auto src = writeTmp("import ::io; fn main() -> int { " + decl + " " + conv +
                      " io::println(\"after\"); return 0; }");
  auto [rc, out] = run(paykanRun() + " " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0) << conv << "\n" << out;
  EXPECT_EQ(out.find("after"), std::string::npos) << out;
  EXPECT_NE(out.find(message), std::string::npos) << out;
}

TEST(Driver, IntOfFloatPanicsOnNaNInfinityAndOutOfRange) {
  REQUIRE_BACKEND();
  const std::string tail = "): the value is NaN, infinite or outside the int "
                           "range";
  expectConversionPanic("z: float = 0.0;", "n = int<float>(z / z);",
                        "paykan: int<float>(nan" + tail);
  expectConversionPanic("z: float = 0.0;", "n = int<float>(1.0 / z);",
                        "paykan: int<float>(inf" + tail);
  expectConversionPanic("z: float = 0.0;", "n = int<float>(-1.0 / z);",
                        "paykan: int<float>(-inf" + tail);
  // 2^63 is the first double past INT64_MAX.
  expectConversionPanic("f: float = 9223372036854775808.0;",
                        "n = int<float>(f);",
                        "paykan: int<float>(9.22337e+18" + tail);
  expectConversionPanic("f: float = -9223372036854777856.0;",
                        "n = int<float>(f);",
                        "paykan: int<float>(-9.22337e+18" + tail);
}

TEST(Driver, IntOfFloatAcceptsTheInt64Boundaries) {
  REQUIRE_BACKEND();
  auto src = writeTmp(
      "import ::io; fn main() -> int { lo: float = -9223372036854775808.0; "
      "hi: float = 9223372036854774784.0; "
      "io::println(Str<int>(int<float>(lo))); "
      "io::println(Str<int>(int<float>(hi))); "
      "return 0; }");
  auto [rc, out] = run(paykanRun() + " " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 0) << out;
  EXPECT_EQ(out, "-9223372036854775808\n9223372036854774784\n");
}

TEST(Driver, CharOfIntPanicsOutsideTheByteRange) {
  REQUIRE_BACKEND();
  const std::string tail = "): the value is outside the char range 0..255";
  expectConversionPanic("n: int = 256;", "c = char<int>(n);",
                        "paykan: char<int>(256" + tail);
  expectConversionPanic("n: int = -1;", "c = char<int>(n);",
                        "paykan: char<int>(-1" + tail);
  expectConversionPanic("n: int = -9223372036854775807 - 1;",
                        "c = char<int>(n);",
                        "paykan: char<int>(-9223372036854775808" + tail);
}

// INT64_MIN % -1 is mathematically 0 and must not trap.
TEST(Driver, IntModMinByNegOneIsZero) {
  REQUIRE_BACKEND();
  auto src = writeTmp(
      "import ::io; fn main() -> int { m: int = -9223372036854775807 - 1; "
      "d: int = -1; r: int = m % d; io::println(Str<int>(r)); "
      "return 7 % d + 3; }");
  auto [rc, out] = run(paykanRun() + " " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 3) << out;
  EXPECT_EQ(out, "0\n");
}

// The raw wait status of `argv` (no shell in between, which would turn a
// death by signal into an exit status), with stdout and stderr discarded.
static int rawWaitStatus(const std::vector<std::string> &argv) {
  pid_t pid = fork();
  if (pid == 0) {
    if (FILE *null = std::freopen("/dev/null", "w", stdout))
      dup2(fileno(null), STDERR_FILENO);
    std::vector<char *> args;
    args.reserve(argv.size() + 1);
    for (const auto &a : argv)
      args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    execv(args[0], args.data());
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR)
      return -1;
  return status;
}

// A runtime panic aborts the program.  `paykan run` reports it as the exit
// status 128 + SIGABRT (the JIT turns the signal into that status), and an
// executable from `build` dies by SIGABRT (#79), on every backend.
TEST(Driver, PanicExitStatusIsTheSameOnEveryBackend) {
  REQUIRE_BACKEND();
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_panic_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  auto src = (dir / "panic.pkn").string();
  {
    std::ofstream ofs(src);
    ofs << "fn main() -> int { z: int = 0; return 7 / z; }";
  }
  auto backends = runBackends();
  int ran = 0;
  for (const std::string &be : backends) {
    ++ran;
    int st = rawWaitStatus({kPaykan, "--backend=" + be, src});
    EXPECT_TRUE(WIFEXITED(st)) << be << ": killed by signal " << WTERMSIG(st);
    if (WIFEXITED(st)) {
      EXPECT_EQ(WEXITSTATUS(st), 128 + SIGABRT) << be;
    }

    auto exe = (dir / ("panic-" + be)).string();
    std::string build = kPaykan;
    build += " --backend=" + be;
    build += " -o " + exe;
    build += " build " + src + " 2>&1";
    auto [rc, out] = run(build);
    ASSERT_EQ(rc, 0) << be << ": " << out;
    st = rawWaitStatus({exe});
    EXPECT_TRUE(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT)
        << be << ": wait status " << st;
  }
  std::filesystem::remove_all(dir);
  EXPECT_GT(ran, 0);
}

// A panic flushes the program's stdout before printing its message and
// aborting (#92).  stdout is fully buffered when it is a pipe, and abort()
// flushes no stdio buffer, so the output printed before the panic used to be
// lost there (it showed on a terminal).  Checked for every kind of panic, on
// every backend, for `paykan run` and for executables from `build`: stdout
// alone through a pipe holds the output, and with stderr merged into the same
// pipe the output comes before the message.
TEST(Driver, PanicFlushesStdoutOnEveryBackend) {
  REQUIRE_BACKEND();
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_panic_flush_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  struct Case {
    const char *name;
    const char *body; // statements after the println, ending in the panic
    const char *message;
  };
  const Case cases[] = {
      {"index", "xs = [1, 2, 3]; i: int = 10; io::println(Str<int>(xs[i]));",
       "paykan: array index 10 out of bounds (len=3)"},
      {"store", "xs = [1, 2, 3]; i: int = 5; xs[i] = 4;",
       "paykan: array index 5 out of bounds (len=3)"},
      {"strindex", "s: Str = \"ab\"; c = s[7]; io::println(Str<char>(c));",
       "paykan: string index 7 out of bounds (len=2)"},
      {"divzero", "z: int = 0; io::println(Str<int>(7 / z));",
       "paykan: integer division or modulo by zero"},
      {"intfloat",
       "z: float = 0.0; n = int<float>(1.0 / z); "
       "io::println(Str<int>(n));",
       "paykan: int<float>(inf): the value is NaN, infinite or outside the "
       "int range"},
      {"charint", "n: int = 300; c = char<int>(n); io::println(Str<char>(c));",
       "paykan: char<int>(300): the value is outside the char range 0..255"},
      {"forstep",
       "z: int = 0; for i in 0..3 step z { io::println(Str<int>(i)); }",
       "paykan: the step of a 'for' loop is 0"},
  };
  const std::string before = "first line\nabout to panic...\n";
  auto backends = runBackends();
  int ran = 0;
  for (const std::string &be : backends) {
    ++ran;
    for (const auto &c : cases) {
      auto src = (dir / (std::string(c.name) + ".pkn")).string();
      {
        std::ofstream ofs(src);
        ofs << "import ::io; fn main() -> int { io::println(\"first line\"); "
               "io::print(\"about to panic...\\n\"); "
            << c.body << " io::println(\"after\"); return 0; }";
      }
      auto exe = (dir / (std::string(c.name) + "-" + be)).string();
      std::string runCmd = kPaykan;
      runCmd += " --backend=" + be;
      std::string build = runCmd;
      build += " -o " + exe;
      build += " build " + src + " 2>&1";
      auto [brc, bout] = run(build);
      ASSERT_EQ(brc, 0) << be << " " << c.name << ": " << bout;
      runCmd += " " + src;
      // The shell reports the built executable's death by SIGABRT as 134,
      // like `paykan run`'s own exit status; its own stderr (where it notes
      // the abort) is discarded, the program's goes where each check says.
      for (const std::string &cmd : {runCmd, exe}) {
        std::string what = be;
        what += " ";
        what += c.name;
        auto [rc, out] = run("exec 2>/dev/null; (" + cmd + ")");
        EXPECT_EQ(rc, 128 + SIGABRT) << what << ": " << cmd;
        EXPECT_EQ(out, before) << what << ": " << cmd;
        std::string merged = "exec 3>&1 2>/dev/null; (" + cmd;
        merged += ") 2>&3";
        auto [rc2, both] = run(merged);
        EXPECT_EQ(rc2, 128 + SIGABRT) << what << ": " << cmd;
        std::string expected = before + c.message;
        expected += "\n";
        EXPECT_EQ(both, expected) << what << ": " << cmd;
      }
    }
  }
  std::filesystem::remove_all(dir);
  EXPECT_GT(ran, 0);
}

// A panic also flushes what the program has written to a File (#92).
TEST(Driver, PanicFlushesOpenFiles) {
  REQUIRE_BACKEND();
  auto out = std::filesystem::temp_directory_path() /
             ("drv_panic_file_" + std::to_string(getpid()) + ".txt");
  std::filesystem::remove(out);
  auto src = writeTmp(
      "import ::io; fn main() -> int { match io::open(\"" + out.string() +
      "\", \"w\") { e: Err { return 1; } "
      "f: Ok { f.value.write(\"kept\\n\"); xs = [1]; i: int = 3; "
      "io::println(Str<int>(xs[i])); } } return 0; }");
  auto [rc, msg] = run(paykanRun() + " " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 128 + SIGABRT) << msg;
  std::ifstream ifs(out);
  std::string content((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
  std::filesystem::remove(out);
  EXPECT_EQ(content, "kept\n") << msg;
}

TEST(Driver, IntDivNonZeroSucceeds) {
  REQUIRE_BACKEND();
  auto src = writeTmp("fn main() -> int { return 17 / 5; }");
  auto [rc, _] = run(paykanRun() + " " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 3); // 17 / 5 == 3, returned as the process exit code
}

// -- The chain limit (#17)

// A chain at the limit, 2048 links (docs/grammar.md section 9), compiles and
// runs, on every path, and its AST is written (--emit-ast); one link more is
// the frontend's error.  Typing and lowering such a chain overflowed the
// stack, and its AST could not be written.
TEST(Driver, ChainAtTheLimitRunsAndWritesItsAST) {
  REQUIRE_BACKEND();
  auto program = [](int links) {
    std::string sum = "1", str = "\"a\"";
    for (int i = 0; i < links; ++i) {
      sum += " + 1";
      str += ".toString()";
    }
    return "import ::io;\nfn main() -> int {\n  x: int = " + sum +
           ";\n  s = " + str + ";\n  io::println(x.toString() + s);\n" +
           "  return 0;\n}\n";
  };
  auto src = writeTmp(program(2048));
  auto [rc, out] = run(paykanRun() + " -O0 " + src + " 2>&1");
  EXPECT_EQ(rc, 0) << out.substr(0, 2000);
  EXPECT_EQ(out, "2049a\n");
  auto exe = src + ".bin";
  auto [brc, bout] =
      run(paykanRun() + " -O2 build " + src + " -o " + exe + " 2>&1 && " + exe);
  EXPECT_EQ(brc, 0) << bout.substr(0, 2000);
  EXPECT_EQ(bout, "2049a\n");
  auto [crc, c] = run(paykanRun() + " -O0 --emit-c " + src + " 2>&1");
  EXPECT_EQ(crc, 0) << c.substr(0, 2000);
  auto [arc, ast] = run(std::string(kPaykan) + " --emit-ast " + src);
  EXPECT_EQ(arc, 0);
  EXPECT_EQ(ast.rfind("(paykan-ast ", 0), 0u) << ast.substr(0, 2000);
  auto longer = writeTmp(program(2049));
  auto [lrc, lout] =
      run(std::string(kPaykan) + " --check-only " + longer + " 2>&1");
  EXPECT_EQ(lrc, 1);
  EXPECT_NE(lout.find("error: nesting too deep (more than 2048 levels)"),
            std::string::npos)
      << lout.substr(0, 2000);
  for (const std::string &f : {src, exe, longer})
    std::filesystem::remove(f);
}

// The stack the programs at the frontend's limits are compiled with: a
// quarter of the default 8 MB, so that a pass recursing once per link of a
// chain, or deeper than it should per level of nesting, fails here in every
// build; in a sanitizer build, whose frames are 2 to 4 times larger, the
// default itself.
static std::string atLimitStack() {
#ifdef PAYKAN_TEST_SANITIZED
  return "ulimit -s 8192 && ";
#else
  return "ulimit -s 2048 && ";
#endif
}

// A chain of @p links links: @p head, @p step repeated, padded with the
// one-link @p pad, and the one-link @p end, if any.
static std::string chainOf(int links, const std::string &head,
                           const std::vector<std::string> &step,
                           const std::string &pad,
                           const std::string &end = "") {
  const int body = links - (end.empty() ? 0 : 1);
  const int steps = body / static_cast<int>(step.size());
  std::string out = head;
  for (int i = 0; i < steps; ++i)
    for (const std::string &s : step)
      out += s;
  for (int i = steps * static_cast<int>(step.size()); i < body; ++i)
    out += pad;
  return out + end;
}

// One function a chain of each kind, @p links links long: binary operators,
// and each suffix on each kind of receiver that is emitted before the
// suffix, a struct's, a generic struct's and a class's method, field, tuple
// element, array element and function value, and a string's character.
static std::string everyChainKind(int links) {
  struct Kind {
    const char *Name;
    std::string Chain;
  };
  const Kind kinds[] = {
      {"c_sum", chainOf(links, "1", {" + 1"}, " + 1")},
      {"c_and", chainOf(links, "True", {" && True"}, " && True")},
      {"c_str",
       chainOf(links, "\"a\"", {".toString()"}, ".toString()", ".len()")},
      {"c_chr", chainOf(links, "\"ab\"", {"[0]", ".toString()"}, ".toString()",
                        ".len()")},
      {"c_smc", chainOf(links, "P(0)", {".m()"}, ".m()", ".x")},
      {"c_gsmc", chainOf(links, "G<int>(0)", {".m()"}, ".m()", ".x")},
      {"c_stup", chainOf(links, "P(0)", {".t()", ".0"}, ".m()", ".x")},
      {"c_ssub", chainOf(links, "P(0)", {".a()", "[0]"}, ".m()", ".x")},
      {"c_scv", chainOf(links, "P(0)", {".g()", "()"}, ".m()", ".x")},
      {"c_sfc", chainOf(links, "smk(0)", {".f()"}, ".f()", ".x")},
      {"c_cmc", chainOf(links, "mk(0)", {".m()"}, ".m()", ".x")},
      {"c_ctup", chainOf(links, "mk(0)", {".t()", ".0"}, ".m()", ".x")},
      {"c_csub", chainOf(links, "mk(0)", {".a()", "[0]"}, ".m()", ".x")},
      {"c_ccv", chainOf(links, "mk(0)", {".g()", "()"}, ".m()", ".x")},
      {"c_cfc", chainOf(links, "mk(0)", {".f()"}, ".f()", ".x")},
      {"c_cps", chainOf(links, "mk(0)", {".p", ".c()"}, ".m()", ".x")},
  };
  std::string src =
      "import ::io;\n"
      "struct P { x: int; }\n"
      "impl P {\n"
      "  view fn m() -> P { return P(self.x + 1); }\n"
      "  view fn t() -> (P, int) { return (P(self.x + 1), 0); }\n"
      "  view fn a() -> P[1] { return [P(self.x + 1)]; }\n"
      "  view fn g() -> () -> P { n = self.x; return fn() -> P { return "
      "P(n + 1); }; }\n"
      "  view fn c() -> C { return mk(self.x + 1); }\n"
      "}\n"
      "struct G<T> { x: T; }\n"
      "impl G<T> { view fn m() -> G<T> { return G<T>(self.x); } }\n"
      "struct S { f: () -> S; x: int; }\n"
      "class C { x: int; f: () -> C; p: P;\n"
      "  view fn m() -> C { return mk(self.x + 1); }\n"
      "  view fn t() -> (C, int) { return (mk(self.x + 1), 0); }\n"
      "  view fn a() -> C[1] { return [mk(self.x + 1)]; }\n"
      "  view fn g() -> () -> C { n = self.x; return fn() -> C { return "
      "mk(n + 1); }; }\n"
      "}\n"
      "fn mk(x: int) -> C { return C(x, fn() -> C { return mk(x + 1); }, "
      "P(x)); }\n"
      "fn smk(x: int) -> S { return S(fn() -> S { return smk(x + 1); }, x); "
      "}\n";
  std::string print;
  for (const Kind &k : kinds) {
    src += "fn " + std::string(k.Name) + "() => " + k.Chain + ";\n";
    print += std::string(print.empty() ? "" : " + \" \" + ") + k.Name +
             "().toString()";
  }
  return src + "fn main() -> int {\n  io::println(" + print +
         ");\n  return 0;\n}\n";
}

// A chain of each kind takes the value of each of its links in turn, and at
// the limit, 2048 links, is typed, lowered and optimised in a quarter of
// the default stack (atLimitStack): each link's operand is emitted before
// it in a loop, not by a recursion a level per link (#17).
TEST(Driver, ChainsOfEveryKindAtTheLimitFitAQuarterOfTheStack) {
  REQUIRE_BACKEND();
  auto small = writeTmp(everyChainKind(12));
  auto [rc, out] = run(paykanRun() + " -O0 run " + small + " 2>&1");
  EXPECT_EQ(rc, 0) << out.substr(0, 2000);
  EXPECT_EQ(out, "13 True 1 1 11 0 6 6 6 11 11 6 6 6 11 6\n");
  auto src = writeTmp(everyChainKind(2048));
  auto [prc, pir] = run(atLimitStack() + std::string(kPaykan) +
                        " -O2 --emit-pir " + src + " 2>&1");
  EXPECT_EQ(prc, 0) << pir.substr(0, 2000);
  EXPECT_NE(pir.find("fn @c_smc("), std::string::npos) << pir.substr(0, 2000);
  for (const std::string &f : {small, src})
    std::filesystem::remove(f);
}

// One function a kind of nesting, @p levels deep in its body (as
// docs/grammar.md section 9 counts them, the body a level of its own): blocks,
// statements and expressions of each kind, a type, lambdas, and the deepest
// mix, lambdas in lambdas' blocks around a chain of @p links links.
static std::string everyNestingKind(int levels, int links) {
  auto rep = [](const std::string &s, int n) {
    std::string out;
    for (int i = 0; i < n; ++i)
      out += s;
    return out;
  };
  std::string chain = "1";
  for (int i = 0; i < links; ++i)
    chain += " + 1";
  const int n = levels;
  const std::pair<const char *, std::string> kinds[] = {
      {"k_blocks", rep("{ ", n) + "x = 1;" + rep(" }", n)},
      {"k_ifs", "a = True; " + rep("if a { ", n) + "x = 1;" + rep(" }", n)},
      {"k_whiles",
       "a = False; " + rep("while a { ", n) + "x = 1;" + rep(" }", n)},
      {"k_matches", "x = 1; " + rep("match x { 1 { ", n / 2) + "y = 1;" +
                        rep(" } _ { } }", n / 2)},
      {"k_parens", "x = " + rep("(", n) + "1" + rep(")", n) + ";"},
      {"k_operands", "x = " + rep("1 + (", n) + "1" + rep(")", n) + ";"},
      {"k_arrays", "x = " + rep("[", n) + "1" + rep("]", n) + ";"},
      {"k_calls", "x = " + rep("id(", n) + "1" + rep(")", n) + ";"},
      {"k_arguments",
       "o = O(); x = " + rep("o.m(", n) + "1" + rep(")", n) + ";"},
      {"k_prefix", "a = 1; x = " + rep("-", n) + "a;"},
      {"k_conditionals", "a = True; x = " + rep("if a then 1 else ", n) + "2;"},
      {"k_types",
       "x: " + rep("B<", n - 1) + "int" + rep(">", n - 1) + "? = None;"},
      {"k_lambdas", "let f = " + rep("fn() => ", n) + "0;"},
      {"k_deepest", "let f = " + rep("fn() { ", n - 1) + "x = " + chain + ";" +
                        rep(" };", n - 1)},
  };
  std::string src = "import ::io;\n"
                    "class B<T> { v: T; }\n"
                    "class O { view fn m(x: int) -> int { return x; } }\n"
                    "fn id(x: int) -> int { return x; }\n";
  std::string sum;
  for (const auto &[name, body] : kinds) {
    src += "fn " + std::string(name) + "() -> int { " + body + " return 0; }\n";
    sum += std::string(sum.empty() ? "" : " + ") + name + "()";
  }
  return src + "fn main() -> int {\n  io::println((" + sum +
         ").toString());\n  return 0;\n}\n";
}

// Each kind of nesting at the limit, 511 levels in a function's body, and
// the deepest mix of nesting and a chain, compile and run on every path in
// a quarter of the default stack (atLimitStack): `run`, which takes the
// MLIR backend's regions to the JIT, and `--emit-c`, whose translation
// recurses once a region (#17).
TEST(Driver, NestingOfEveryKindAtTheLimitFitsAQuarterOfTheStack) {
  REQUIRE_BACKEND();
  auto src = writeTmp(everyNestingKind(511, 2048));
  auto [rc, out] =
      run(atLimitStack() + paykanRun() + " -O0 run " + src + " 2>/dev/null");
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(out, "0\n");
  auto [prc, pir] = run(atLimitStack() + std::string(kPaykan) +
                        " -O2 --emit-pir " + src + " 2>&1");
  EXPECT_EQ(prc, 0) << pir.substr(0, 2000);
  auto [crc, c] =
      run(atLimitStack() + paykanRun() + " -O0 --emit-c " + src + " 2>&1");
  EXPECT_EQ(crc, 0) << c.substr(0, 2000);
  // One level more is the frontend's error.
  auto deeper = writeTmp(everyNestingKind(512, 0));
  auto [drc, dout] =
      run(std::string(kPaykan) + " --check-only " + deeper + " 2>&1");
  EXPECT_EQ(drc, 1);
  EXPECT_NE(dout.find("error: nesting too deep (more than 512 levels)"),
            std::string::npos)
      << dout.substr(0, 2000);
  for (const std::string &f : {src, deeper})
    std::filesystem::remove(f);
}

// -- Negative numbers as match patterns (#51)

// `-1`, `- 7`, `-2.5`, `-0.0` and INT64_MIN, `-9223372036854775808`, are
// match patterns (docs/grammar.md section 6), on every path, at -O0 and
// -O2: `run`, `build`, and the strict C11 `--emit-c` prints, in which
// INT64_MIN is no literal a case can be.  The exit status has a bit for
// each call that took its arm.  A `-1` pattern on an unsigned subject is the
// error a `-1` assigned to one is, on every path.
TEST(Driver, NegativeNumbersAreMatchPatternsOnEveryPath) {
  REQUIRE_BACKEND();
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_negpat_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  auto src = (dir / "negpat.pkn").string();
  std::ofstream(src) << "fn ints(n: int) -> int {\n"
                        "  match n {\n"
                        "    -1 { return 1; }\n"
                        "    -9223372036854775808 { return 2; }\n"
                        "    - 7 { return 4; }\n"
                        "    7 { return 0; }\n"
                        "    _ { return 8; }\n"
                        "  }\n"
                        "}\n"
                        "fn floats(x: float) -> int {\n"
                        "  match x {\n"
                        "    -2.5 { return 16; }\n"
                        "    -0.0 { return 32; }\n"
                        "    _ { return 0; }\n"
                        "  }\n"
                        "}\n"
                        "fn main() -> int {\n"
                        "  return ints(-1) + ints(-9223372036854775807 - 1) +\n"
                        "         ints(-7) + ints(1) + floats(-2.5) +\n"
                        "         floats(0.0);\n"
                        "}\n";
  auto everyPath = [&](const std::string &opt) {
    auto ran = run(paykanRun() + " " + opt + " " + src + " 2>&1");
    EXPECT_EQ(ran.exitCode, 63) << opt << ": " << ran.out;
    auto exe = (dir / ("negpat" + opt)).string();
    auto built =
        run(paykanRun() + " " + opt + " -o " + exe + " build " + src + " 2>&1");
    ASSERT_EQ(built.exitCode, 0) << opt << ": " << built.out;
    EXPECT_EQ(run(exe).exitCode, 63) << opt;
    auto c = (dir / ("negpat" + opt + ".c")).string();
    auto emitted = run(std::string(kPaykan) + " " + opt + " --emit-c " + src +
                       " > " + c + " 2>&1");
    ASSERT_EQ(emitted.exitCode, 0) << opt;
    auto cexe = (dir / ("negpat-c" + opt)).string();
    auto cc =
        run("cc -std=c11 -pedantic-errors -Wall -Wextra -Werror " + opt +
            " -I " PAYKAN_RUNTIME_SOURCE_DIR " " + c +
            " " PAYKAN_RUNTIME_SOURCE_DIR "/*.c -lm -o " + cexe + " 2>&1");
    ASSERT_EQ(cc.exitCode, 0) << opt << ": " << cc.out;
    EXPECT_EQ(run(cexe).exitCode, 63) << opt;
  };
  everyPath("-O0");
  everyPath("-O2");
  const std::string message = "error: the literal -1 does not fit in "
                              "'unsigned' (0 to 18446744073709551615)";
  auto pattern = (dir / "unsigned.pkn").string();
  std::ofstream(pattern) << "fn main() -> int {\n"
                            "  u: unsigned = 5;\n"
                            "  match u { -1 { } _ { } }\n"
                            "  v: unsigned = -1;\n"
                            "  return 0;\n"
                            "}\n";
  auto rejects = [&](const std::string &how) {
    auto r = run(paykanRun() + how + pattern + " 2>&1");
    EXPECT_EQ(r.exitCode, 1) << how << ": " << r.out;
    EXPECT_NE(r.out.find(":3:13: " + message), std::string::npos)
        << how << ": " << r.out;
    EXPECT_NE(r.out.find(":4:17: " + message), std::string::npos)
        << how << ": " << r.out;
  };
  rejects(" run ");
  rejects(" -o " + (dir / "unsigned").string() + " build ");
  rejects(" --emit-c ");
  std::filesystem::remove_all(dir);
}

// -- The --dump-ast flag

TEST(Driver, DumpAstProducesOutput) {
  auto src = writeTmp("fn main() -> int { return 42; }");
  auto [rc, out] = run(std::string(kPaykan) + " --dump-ast " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 0);
  EXPECT_FALSE(out.empty());
}

TEST(Driver, DumpAstContainsFuncDecl) {
  auto src = writeTmp("fn main() -> int { return 0; }");
  auto [rc, out] = run(std::string(kPaykan) + " --dump-ast " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 0);
  EXPECT_NE(out.find("main"), std::string::npos);
}

TEST(Driver, DumpAstShowsOptionalType) {
  auto src = writeTmp("fn main() -> int { s: Str? = None; return 0; }");
  auto [rc, out] = run(std::string(kPaykan) + " --dump-ast " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 0);
  EXPECT_NE(out.find("OptionalType"), std::string::npos) << out;
}

TEST(Driver, OptionalPrimitiveIsAccepted) {
  auto src = writeTmp("fn main() -> int { x: int? = None; return 0; }");
  auto [rc, out] = run(std::string(kPaykan) + " --check-only " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 0) << out;
}

TEST(Driver, OptionalVoidIsRejectedAtParse) {
  auto src = writeTmp("fn f() -> void? { } fn main() -> int { return 0; }");
  auto [rc, out] = run(std::string(kPaykan) + " --check-only " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_NE(out.find("optional type 'void?' is not supported"),
            std::string::npos)
      << out;
}

// -- PIR, C, build

TEST(Driver, EmitPirPrintsTheProgram) {
  REQUIRE_BACKEND();
  auto src = writeTmp(
      "import ::io; fn main() -> int { s: Str = \"hi\"; io::println(s); "
      "return 0; }");
  auto [rc, out] = run(std::string(kPaykan) + " --emit-pir " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 0) << out;
  EXPECT_NE(out.find("fn @main() -> i64 {"), std::string::npos) << out;
  EXPECT_NE(out.find("release"), std::string::npos) << out;
  EXPECT_NE(out.find("call @\"$::io::println<Str>\"("), std::string::npos)
      << out;
}

// --stats: what the PIR passes did (docs/pir.md §12), on stderr, after the
// last of them ran: for a program, and for the code of a module written to
// a `.pkm` (whose compiled code is made by the same passes).
TEST(Driver, StatsCountTheMovesAtLastUse) {
  REQUIRE_BACKEND();
  auto src = writeTmp("class N { n: int; }\n"
                      "fn take(x: N) -> int { return x.n; }\n"
                      "fn main() -> int { a = N(7); return take(a); }\n");
  auto [rc, out] = run(paykanRun() + " --stats " + src + " 2>&1");
  EXPECT_EQ(rc, 7) << out;
  EXPECT_NE(out.find("paykan pass stats:\n"
                     "  moves-at-last-use transfers : 1\n"
                     "  moves-at-last-use retains removed : 1\n"
                     "  moves-at-last-use releases removed : 1\n"),
            std::string::npos)
      << out;
  auto quiet = run(paykanRun() + " " + src + " 2>&1");
  EXPECT_EQ(quiet.out.find("pass stats"), std::string::npos) << quiet.out;
  std::string pkm = src + ".pkm";
  auto lib = run(std::string(kPaykan) + " --no-module-cache --stats " +
                 "--emit-pkm -o " + pkm + " " + src + " 2>&1");
  std::filesystem::remove(src);
  std::filesystem::remove(pkm);
  EXPECT_EQ(lib.exitCode, 0) << lib.out;
  EXPECT_NE(lib.out.find("  moves-at-last-use transfers : 1\n"),
            std::string::npos)
      << lib.out;
}

// A match on a field whose arms call nothing: the reference-count
// elimination removes its subject's retain and its binding's, with their
// releases on every way out (docs/pir.md §12), in a program and in a
// module's code.
TEST(Driver, StatsCountTheReferenceCountElimination) {
  REQUIRE_BACKEND();
  auto src = writeTmp("class N { n: int; next: N?; }\n"
                      "fn sum(x: N) -> int {\n"
                      "  match x.next {\n"
                      "    m: N { return x.n + m.n; }\n"
                      "    None { return x.n; }\n"
                      "  }\n"
                      "}\n"
                      "fn main() -> int { return sum(N(1, N(2, None))); }\n");
  auto [rc, out] = run(paykanRun() + " --stats " + src + " 2>&1");
  EXPECT_EQ(rc, 3) << out;
  EXPECT_NE(out.find("  rc-elimination retains removed : 2\n"
                     "  rc-elimination releases removed : 4\n"),
            std::string::npos)
      << out;
  std::string pkm = src + ".pkm";
  auto lib = run(std::string(kPaykan) + " --no-module-cache --stats " +
                 "--emit-pkm -o " + pkm + " " + src + " 2>&1");
  std::filesystem::remove(src);
  std::filesystem::remove(pkm);
  EXPECT_EQ(lib.exitCode, 0) << lib.out;
  EXPECT_NE(lib.out.find("  rc-elimination retains removed : 2\n"),
            std::string::npos)
      << lib.out;
}

// The lending pass lends the copies the lowering made only to hand an
// argument over that the moves at last use left where the caller's
// reference outlives the call (`lending marks`, `lending arguments lent`,
// `lending copies kept for the window`; here the first `take(a)`'s), and
// clears their mark (docs/pir.md §6, §12); and it lends the box parameters
// their bodies never write (`lending parameters lent`, `handed on`, `owned
// on entry`, `retains at keeps`; here take's x, whose release then asks the
// bit).  --pir-skip=<pass>[,<pass>] (not in --help) leaves passes out: here
// the lending pass, whose marks and prologues then stay in what --emit-pir
// prints, and the reference-count elimination.  It leaves them out of the
// code the run lowers only: code a `.pkm` holds went through them when it
// was written, as the shared `ok<Str, Error>` the standard library's io
// made did (the program's own `println<Str>` keeps its prologue).  A name
// no pass has, and --emit-pkm, are refused.
TEST(Driver, PirSkipLeavesPassesOut) {
  REQUIRE_BACKEND();
  auto src =
      writeTmp("class N { n: int; }\n"
               "fn take(x: N) -> int { return x.n; }\n"
               "fn main() -> int { a = N(7); return take(a) + take(a); }\n");
  const std::string paykan = kPaykan;
  auto all = run(paykan + " --stats --emit-pir " + src + " 2>&1");
  EXPECT_EQ(all.exitCode, 0) << all.out;
  EXPECT_NE(all.out.find("  moves-at-last-use transfers : 1\n"
                         "  moves-at-last-use retains removed : 1\n"
                         "  moves-at-last-use releases removed : 1\n"
                         "  lending marks : 1\n"
                         "  lending arguments lent : 1\n"
                         "  lending copies kept for the window : 0\n"
                         "  lending parameters lent : 1\n"
                         "  lending parameters handed on : 0\n"
                         "  lending parameters owned on entry : 0\n"
                         "  lending retains at keeps : 0\n"
                         "  rc-elimination retains removed : 0\n"),
            std::string::npos)
      << all.out;
  EXPECT_EQ(all.out.find("retain lend "), std::string::npos) << all.out;
  EXPECT_NE(all.out.find("call @take(lent %a."), std::string::npos) << all.out;
  EXPECT_NE(all.out.find("  if %x.own.2 {\n    release %7\n  }\n"),
            std::string::npos)
      << all.out;
  EXPECT_EQ(all.out.find("retain %x.1"), std::string::npos) << all.out;
  auto skip =
      run(paykan + " --stats --pir-skip=lending,rc-elimination --emit-pir " +
          src + " 2>&1");
  EXPECT_EQ(skip.exitCode, 0) << skip.out;
  EXPECT_NE(skip.out.find("  moves-at-last-use transfers : 1\n"),
            std::string::npos)
      << skip.out;
  EXPECT_EQ(skip.out.find("  lending "), std::string::npos) << skip.out;
  EXPECT_EQ(skip.out.find("  rc-elimination "), std::string::npos) << skip.out;
  EXPECT_NE(skip.out.find("  retain lend %a."), std::string::npos) << skip.out;
  EXPECT_NE(skip.out.find("  if %x.lent.3 {\n    retain %x.1\n  }\n"),
            std::string::npos)
      << skip.out;
  EXPECT_EQ(run(paykanRun() + " --pir-skip=lending " + src).exitCode, 14);
  auto reads = writeTmp("import ::io;\n"
                        "fn main() -> int {\n"
                        "  match io::stdin().readln() {\n"
                        "    line: Ok { io::println(line.value); return 1; }\n"
                        "    _ { return 0; }\n"
                        "  }\n"
                        "}\n");
  auto imported =
      run(paykan + " --pir-skip=lending --emit-pir " + reads + " 2>&1");
  std::filesystem::remove(reads);
  EXPECT_EQ(imported.exitCode, 0) << imported.out;
  const size_t ok =
      imported.out.find("shared fn @\"$::result::ok<Str, Error>\"(");
  const size_t println =
      imported.out.find("shared fn @\"$::io::println<Str>\"(");
  ASSERT_NE(ok, std::string::npos) << imported.out;
  ASSERT_NE(println, std::string::npos) << imported.out;
  const std::string okBody =
      imported.out.substr(ok, imported.out.find("\n}\n", ok) - ok);
  const std::string printlnBody = imported.out.substr(
      println, imported.out.find("\n}\n", println) - println);
  EXPECT_EQ(okBody.find("    retain %value.1\n"), std::string::npos) << okBody;
  EXPECT_NE(okBody.find("  if %value.lent.4 {\n    retain %value.6\n  }\n"),
            std::string::npos)
      << okBody;
  EXPECT_NE(printlnBody.find("  if %value.lent.3 {\n    retain %value.1\n"),
            std::string::npos)
      << printlnBody;
  auto unknown =
      run(paykan + " --pir-skip=lending,folding --check-only " + src + " 2>&1");
  EXPECT_NE(unknown.exitCode, 0);
  EXPECT_NE(unknown.out.find("--pir-skip: no PIR pass is named 'folding'"),
            std::string::npos)
      << unknown.out;
  std::string pkm = src + ".pkm";
  auto lib = run(paykan + " --pir-skip=lending --emit-pkm -o " + pkm + " " +
                 src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(lib.exitCode, 0);
  EXPECT_NE(lib.out.find("--pir-skip cannot be used with --emit-pkm"),
            std::string::npos)
      << lib.out;
  EXPECT_FALSE(std::filesystem::exists(pkm));
  auto help = run(paykan + " --help 2>&1");
  EXPECT_EQ(help.out.find("pir-skip"), std::string::npos) << help.out;
}

TEST(Driver, EmitCPrintsCSource) {
  REQUIRE_BACKEND();
  auto src = writeTmp("class P { x: int; }\n"
                      "impl P { fn __init__(x: int) { self.x = x; } }\n"
                      "fn main() -> int { p = P(3); return p.x; }");
  auto [rc, out] = run(std::string(kPaykan) + " --emit-c " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 0) << out;
  EXPECT_NE(out.find("#include \"Runtime.h\""), std::string::npos) << out;
  EXPECT_NE(out.find("int main(int argc, char **argv)"), std::string::npos)
      << out;
  // The program's main behind C's, and P's vtable.
  EXPECT_NE(out.find("static int64_t paykan_main(void) {"), std::string::npos)
      << out;
  EXPECT_NE(out.find("PaykanMethod pkvt_"), std::string::npos) << out;
}

// --emit-mlir=<stage> selects the mlir backend and prints the program at
// that stage of its pipeline (docs/design/mlir-backend.md); with another
// backend, or a stage the backend has not, it is an error.
TEST(Driver, EmitMLIRPrintsEachStage) {
  REQUIRE_BACKEND();
  auto src = writeTmp("fn main() -> int { x = 40; return x + 2; }");
  auto [rc, out] =
      run(std::string(kPaykan) + " --emit-mlir=pir " + src + " 2>&1");
  EXPECT_EQ(rc, 0) << out;
  EXPECT_NE(out.find("pir.func @main() -> i64"), std::string::npos) << out;
  EXPECT_NE(out.find(" = add "), std::string::npos) << out;
  auto [rcSCF, outSCF] =
      run(std::string(kPaykan) + " --emit-mlir=scf " + src + " 2>&1");
  EXPECT_EQ(rcSCF, 0) << outSCF;
  EXPECT_NE(outSCF.find("func.func @main() -> i64"), std::string::npos)
      << outSCF;
  EXPECT_NE(outSCF.find("arith.addi"), std::string::npos) << outSCF;
  auto [rcLLVM, outLLVM] =
      run(std::string(kPaykan) + " --emit-mlir=llvm " + src + " 2>&1");
  EXPECT_EQ(rcLLVM, 0) << outLLVM;
  EXPECT_NE(outLLVM.find("llvm.func @main() -> i64"), std::string::npos)
      << outLLVM;
  // EmitC: the program's main as paykan_main, and C's main() calling it.
  auto [rcEmitC, outEmitC] =
      run(std::string(kPaykan) + " --emit-mlir=emitc " + src + " 2>&1");
  EXPECT_EQ(rcEmitC, 0) << outEmitC;
  EXPECT_NE(outEmitC.find("emitc.func private @paykan_main() -> i64"),
            std::string::npos)
      << outEmitC;
  EXPECT_NE(outEmitC.find("int main(int argc, char **argv)"), std::string::npos)
      << outEmitC;
  auto [rcC, outC] = run(std::string(kPaykan) +
                         " --backend=other --emit-mlir=pir " + src + " 2>&1");
  EXPECT_NE(rcC, 0) << outC;
  EXPECT_NE(outC.find("--emit-mlir needs the mlir backend"), std::string::npos)
      << outC;
  auto [rcStage, outStage] =
      run(std::string(kPaykan) + " --emit-mlir=bogus " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rcStage, 0) << outStage;
  EXPECT_NE(outStage.find("unknown MLIR stage 'bogus'"), std::string::npos)
      << outStage;
}

// Every vtable is an array of PaykanMethod (Runtime.h), its slots read as
// the PaykanMethod they are and converted to the method's own type at the
// call: no vtable is accessed through a pointer to another type (#62).
TEST(Driver, EmitCVTablesArePaykanMethodArrays) {
  REQUIRE_BACKEND();
  auto src = writeTmp("class A { fn f() -> int { return 1; } }"
                      "class B : A { fn f() -> int { return 2; } }"
                      "fn g(a: A) -> int { return a.f(); }"
                      "fn main() -> int { return g(B()); }");
  auto [rc, out] = run(std::string(kPaykan) + " --emit-c " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 0) << out;
  EXPECT_TRUE(
      std::regex_search(out, std::regex(R"(\nPaykanMethod pkvt_\w+\[)")))
      << out;
  EXPECT_NE(out.find("(PaykanMethod)PaykanObject_toString"), std::string::npos)
      << out;
  // The object's vtable pointer is written and read as a PaykanMethod *, its
  // slot read as a PaykanMethod, never as a void *.
  EXPECT_NE(out.find("PaykanMethod** "), std::string::npos) << out;
  EXPECT_NE(out.find("PaykanMethod* "), std::string::npos) << out;
  EXPECT_EQ(out.find("void** "), std::string::npos) << out;
  EXPECT_NE(out.find("((int64_t (*)(PaykanObject *))(uintptr_t)fn)("),
            std::string::npos)
      << out;
}

// Nothing unused reaches the C (a -Wall -Wextra warning otherwise): an
// unread parameter (`self` here) is marked read, a discarded call result has
// no variable, and only the branch taken of an `if` on a literal is left
// (#62).
TEST(Driver, EmitCOmitsUnusedEntities) {
  REQUIRE_BACKEND();
  auto src = writeTmp(
      "class K { fn seven() -> int { return 7; } }"
      "fn side() -> int { s = \"side\"; return s.len(); }"
      "fn main() -> int { side(); k = K(); x: int = if True then 1 else 2;"
      "  return k.seven() + x; }");
  auto [rc, out] = run(std::string(kPaykan) + " --emit-c " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 0) << out;
  EXPECT_NE(out.find("(void)v"), std::string::npos) << out; // unread `self`
  EXPECT_TRUE(std::regex_search(out, std::regex(R"(\n  \w+_Sside\(\);)")))
      << out;
  EXPECT_EQ(out.find("if (true)"), std::string::npos) << out;
}

// A bit cast (a memcpy in C) is emitted only for a PIR `cast` between i64
// and f64 (MemSlot.FloatBitsRoundTripThroughAnInteger runs one), which the
// lowering never emits: a float held in place, in a tuple, an array or a
// `float?`, is stored as a double.
TEST(Driver, EmitCStoresFloatsInPlaceWithoutBitCasts) {
  REQUIRE_BACKEND();
  auto src = writeTmp("import ::io; fn main() -> int { t = (1.5, 2);"
                      "  xs: float[2] = [0.5, t.0]; o: float? = t.0;"
                      "  io::println(Str<float>(t.0 + xs[1])); io::println(t);"
                      "  return 0; }");
  auto [rc, out] = run(std::string(kPaykan) + " --emit-c " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 0) << out;
  EXPECT_NE(out.find("(double*) "), std::string::npos) << out;
  EXPECT_EQ(out.find("memcpy("), std::string::npos) << out;
}

// A `native fn`'s C symbol is prototyped from its PIR signature (#198).
TEST(Driver, EmitCPrototypesNativeFunctions) {
  REQUIRE_BACKEND();
  auto src = writeTmp("native fn put(fd: int, s: view Str) -> int = "
                      "\"pk_put\";"
                      "native fn get() -> Str? = \"pk_get\";"
                      "fn main() -> int { s = get(); return put(1, \"x\"); }");
  auto [rc, out] = run(std::string(kPaykan) + " --emit-c " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 0) << out;
  EXPECT_NE(out.find("extern int64_t pk_put(int64_t, PaykanObject*);"),
            std::string::npos)
      << out;
  EXPECT_NE(out.find("extern PaykanShared* pk_get(void);"), std::string::npos)
      << out;
}

// --object=a.o,b.o links every listed object (#198): each defines one
// native function here, built by the test as a user would.
TEST(Driver, ObjectListLinksEveryNativeObject) {
  REQUIRE_BACKEND();
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_native_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "a.c")
      << "#include <stdint.h>\n"
         "int64_t drv_twice(int64_t x) { return 2 * x; }\n";
  std::ofstream(dir / "b.c")
      << "#include <stdint.h>\n"
         "int64_t drv_plus1(int64_t x) { return x + 1; }\n";
  for (const char *stem : {"a", "b"}) {
    auto cc = run("cc -std=c11 -fPIC -c " + (dir / stem).string() + ".c -o " +
                  (dir / stem).string() + ".o 2>&1");
    ASSERT_EQ(cc.exitCode, 0) << cc.out;
  }
  std::ofstream(dir / "main.pkn")
      << "import ::io; native fn twice(x: int) -> int = \"drv_twice\";\n"
         "native fn plus1(x: int) -> int = \"drv_plus1\";\n"
         "fn main() -> int { io::println(Str<int>(plus1(twice(20)))); return "
         "0; "
         "}\n";
  std::string objects = " --object=" + (dir / "a.o").string() + "," +
                        (dir / "b.o").string() + " ";
  auto r = run(std::string(kPaykan) + backendFlag() + objects +
               (dir / "main.pkn").string() + " 2>&1");
  EXPECT_EQ(r.exitCode, 0) << r.out;
  EXPECT_EQ(r.out, "41\n");
  // Without b.o, drv_plus1 is undefined: the link fails.
  auto missing = run(std::string(kPaykan) + backendFlag() +
                     " --object=" + (dir / "a.o").string() + " " +
                     (dir / "main.pkn").string() + " 2>&1");
  EXPECT_NE(missing.exitCode, 0) << missing.out;
  std::filesystem::remove_all(dir);
}

// Two modules that bind one C symbol with different C signatures (finding
// 39) each check on their own: the program is rejected once it is lowered,
// naming both modules, whether the import is built from source or loaded
// from the module cache, whose CODE keeps the C functions its compiled code
// calls.  It used to run, calling llabs with a double (`-2.5`).
TEST(Driver, ModulesBindACSymbolWithOneSignature) {
  REQUIRE_BACKEND();
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_native_clash_" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "na.pkn") << "native fn fa(x: int) -> int = \"llabs\";\n"
                                   "fn ca(x: int) -> int { return fa(x); }\n";
  std::ofstream(dir / "main.pkn")
      << "import ::io;\nimport na;\n"
         "native fn fb(x: float) -> float = \"llabs\";\n"
         "fn main() -> int { io::println(na::ca(-3)); io::println(fb(-2.5)); "
         "return 0; }\n";
  std::ofstream(dir / "ok.pkn")
      << "import ::io;\nimport na;\n"
         "native fn fc(x: int) -> int = \"llabs\";\n"
         "fn main() -> int { io::println(na::ca(-3) + fc(-4)); return 0; }\n";
  const std::string main = (dir / "main.pkn").string();
  const std::string clash =
      "error: native functions of modules 'main' and 'na' bind C symbol "
      "'llabs' with different signatures\n";
  auto fresh = run(paykanRun() + " --no-module-cache " + main + " 2>&1");
  EXPECT_EQ(fresh.exitCode, 1) << fresh.out;
  EXPECT_EQ(fresh.out, clash);
  // ok.pkn binds llabs as na does: it runs, and na gets its cache entry.
  auto ok = run(paykanRun() + " " + (dir / "ok.pkn").string() + " 2>&1");
  EXPECT_EQ(ok.exitCode, 0) << ok.out;
  EXPECT_EQ(ok.out, "7\n");
  EXPECT_TRUE(std::filesystem::exists(dir / ".paykan_cache" / "na.pkm"));
  for (const std::string &how :
       {std::string(" "), std::string(" --emit-c "),
        " build -o " + (dir / "main.out").string() + " "}) {
    std::string cmd = paykanRun();
    cmd.append(how).append(main).append(" 2>&1");
    auto cached = run(cmd);
    EXPECT_EQ(cached.exitCode, 1) << how << "\n" << cached.out;
    EXPECT_EQ(cached.out, clash) << how;
  }
  std::filesystem::remove_all(dir);
}

TEST(Driver, EmitCRejectsAnotherBackend) {
  REQUIRE_BACKEND();
  auto src = writeTmp("fn main() -> int { return 0; }");
  auto [rc, out] =
      run(std::string(kPaykan) + " --backend=other --emit-c " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_NE(out.find("--emit-c needs the mlir backend"), std::string::npos)
      << out;
}

// Float `!=` is IEEE's unordered not-equal (true for a NaN operand) and the
// other comparisons are ordered: the LLVM IR has `fcmp une`, the C C's own
// `!=`, with no NaN test of its own.
TEST(Driver, FloatNotEqualIsUnordered) {
  REQUIRE_BACKEND();
  auto src = writeTmp(
      "import ::io; fn main() -> int { inf: float = 1.0e308 * 10.0; n: float = "
      "inf - inf;"
      " io::println(Str<bool>(n != n) + Str<bool>(n == n) + Str<bool>(n < 1.0)"
      " + Str<bool>(1.0 != n) + Str<bool>(1.0 != 1.0)); return 0; }");
  auto [rc, out] = run(paykanRun() + " " + src + " 2>&1");
  EXPECT_EQ(rc, 0) << out;
  EXPECT_EQ(out, "TrueFalseFalseTrueFalse\n");
  // At -O0: the default -O2 folds the comparisons of constants away.
  auto [irRc, ir] = run(paykanRun() + " -O0 --emit-llvm " + src + " 2>&1");
  EXPECT_EQ(irRc, 0) << ir;
  EXPECT_NE(ir.find("fcmp une double"), std::string::npos) << ir;
  EXPECT_EQ(ir.find("fcmp one"), std::string::npos) << ir;
  auto [cRc, code] = run(paykanRun() + " --emit-c " + src + " 2>&1");
  EXPECT_EQ(cRc, 0) << code;
  EXPECT_NE(code.find(" != "), std::string::npos) << code;
  EXPECT_EQ(code.find(" || "), std::string::npos) << code;
  std::filesystem::remove(src);
}

// A char ordered against a number is Sema's error on every path, as `==`
// on the pair is: `run`, `build` and `--emit-c` each report it and stop.
// `c < n` got past Sema and failed PIR verification (`cmp operands have
// different types: char and i64`).
TEST(Driver, ACharOrderedAgainstANumberIsAnErrorOnEveryPath) {
  REQUIRE_BACKEND();
  auto src = writeTmp("fn main() -> int {\n  c = 'a';\n  n = 100;\n"
                      "  if c < n { return 1; }\n  return 0;\n}\n");
  const std::string exe = src + ".exe";
  const std::string message =
      "4:6: error: operands of '<' have mismatched types 'char' and 'int'";
  const std::vector<std::string> commands = {
      paykanRun() + " " + src, paykanRun() + " -o " + exe + " build " + src,
      paykanRun() + " --emit-c " + src};
  for (const std::string &cmd : commands) {
    auto r = run(cmd + " 2>&1");
    EXPECT_EQ(r.exitCode, 1) << cmd << ": " << r.out;
    EXPECT_NE(r.out.find(message), std::string::npos) << cmd << ": " << r.out;
    EXPECT_EQ(r.out.find("PIR verification"), std::string::npos)
        << cmd << ": " << r.out;
  }
  EXPECT_FALSE(std::filesystem::exists(exe));
  std::filesystem::remove(src);
}

// `build` gives a native executable, linked against the runtime, that
// behaves like `run`: arguments, exit code, PAYKAN_TRACK_HEAP, at -O0 and
// -O2.
TEST(Driver, BuildProducesAStandaloneExecutable) {
  REQUIRE_BACKEND();
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_build_any_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  auto src = (dir / "prog.pkn").string();
  {
    std::ofstream ofs(src);
    ofs << "import ::io; class Box { v: int; }\n"
           "impl Box { fn __init__(v: int) { self.v = v; } }\n"
           "fn main(args: view Str[]) -> int { b = Box(40); "
           "io::println(args[1]); return b.v + args.len(); }";
  }
  auto buildAndRun = [&](const std::string &opt) {
    auto exe = (dir / ("prog" + opt)).string();
    std::filesystem::remove(exe);
    auto [rc, out] =
        run(paykanRun() + " " + opt + " -o " + exe + " build " + src + " 2>&1");
    ASSERT_EQ(rc, 0) << opt << ": " << out;
    ASSERT_TRUE(std::filesystem::exists(exe)) << opt;

    auto [rc2, out2] = run(exe + " hello 2>&1");
    EXPECT_EQ(rc2, 42) << opt << ": " << out2; // 40 + argv[0] + "hello"
    EXPECT_EQ(out2, "hello\n") << opt;

    auto [rc3, out3] = run("PAYKAN_TRACK_HEAP=1 " + exe + " x 2>&1");
    EXPECT_EQ(rc3, 42) << opt << ": " << out3;
    EXPECT_NE(out3.find("live blocks       : 0"), std::string::npos)
        << opt << ": " << out3;
  };
  buildAndRun("-O0");
  buildAndRun("-O2");
  // A program without arguments, and the default output name (the input's
  // stem, in the current directory).
  {
    std::ofstream ofs(src);
    ofs << "import ::io; fn main() -> int { io::println(\"no args\"); return "
           "7; }";
  }
  auto [rc4, out4] = run("cd " + dir.string() + " && " + paykanRun() +
                         " build " + src + " 2>&1");
  ASSERT_EQ(rc4, 0) << out4;
  auto [rc5, out5] = run((dir / "prog").string() + " ignored 2>&1");
  EXPECT_EQ(rc5, 7) << out5;
  EXPECT_EQ(out5, "no args\n");
  std::filesystem::remove_all(dir);
}

// `build` has no program arguments: options may follow the source file, and
// a second file is an error rather than silently ignored.
TEST(Driver, BuildAcceptsOptionsAfterTheSourceFile) {
  REQUIRE_BACKEND();
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_build_order_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  auto src = (dir / "order.pkn").string();
  std::ofstream(src)
      << "import ::io; fn main() -> int { io::println(\"ok\"); return 7; }";
  // Before and after the source file.
  for (const std::string &args :
       {std::string(" --backend=mlir -o ") + (dir / "before").string() +
            " build " + src,
        std::string(" build ") + src + " -o " + (dir / "after").string() +
            " --backend=mlir"}) {
    auto [rc, out] = run(std::string(kPaykan) + args + " 2>&1");
    EXPECT_EQ(rc, 0) << args << "\n" << out;
  }
  for (const char *name : {"before", "after"}) {
    auto exe = (dir / name).string();
    ASSERT_TRUE(std::filesystem::exists(exe)) << exe;
    auto [rc, out] = run(exe + " 2>&1");
    EXPECT_EQ(rc, 7) << out;
    EXPECT_EQ(out, "ok\n");
  }
  std::filesystem::remove_all(dir);
}

// A coverage build's runtime archive is instrumented, so a built program
// must be linked with the same coverage flags (and a compiler whose profile
// runtime matches): the program builds, runs and writes its own profile.
TEST(Driver, ProgramsAreInstrumentedInACoverageBuild) {
#ifndef PAYKAN_TEST_COVERAGE
  GTEST_SKIP() << "not a PAYKAN_COVERAGE build";
#else
  REQUIRE_BACKEND();
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_cov_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  auto src = (dir / "cov.pkn").string();
  std::ofstream(src)
      << "import ::io; fn main() -> int { io::println(\"cov\"); return 3; }";
  auto exe = (dir / "cov").string();
  auto [rc, out] = run(paykanRun() + " -o " + exe + " build " + src + " 2>&1");
  ASSERT_EQ(rc, 0) << out;
  auto profile = (dir / "cov.profraw").string();
  auto [rc2, out2] = run("LLVM_PROFILE_FILE=" + profile + " " + exe + " 2>&1");
  EXPECT_EQ(rc2, 3) << out2;
  EXPECT_EQ(out2, "cov\n");
  std::error_code ec;
  EXPECT_GT(std::filesystem::file_size(profile, ec), 0U) << ec.message();
  std::filesystem::remove_all(dir);
#endif
}

TEST(Driver, BuildRejectsAStrayArgument) {
  auto src = writeTmp("fn main() -> int { return 0; }");
  auto [rc, out] = run(std::string(kPaykan) + " build " + src + " extra 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_NE(out.find("unexpected argument 'extra'"), std::string::npos) << out;
}

TEST(Driver, RunForwardsArgumentsAndTracksTheHeap) {
  REQUIRE_BACKEND();
  auto src = writeTmp(
      "import ::io; fn main(args: view Str[]) -> int { io::println(args[1]); "
      "return args.len(); }");
  auto [rc, out] = run(paykanRun() + " --track-heap " + src + " hello 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 2) << out;
  EXPECT_NE(out.find("hello\n"), std::string::npos) << out;
  EXPECT_NE(out.find("live blocks       : 0"), std::string::npos) << out;
}

// @p word as one word of a shell command, whatever it holds.
static std::string shellWord(const std::string &word) {
  std::string out = "'";
  for (char c : word)
    out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return out + "'";
}

// @p cmd with its stdout and stderr one pipe, which makes stdout fully
// buffered, and then its own exit status, which a pipeline's is not:
// "exit <status>" on a line after its output.
static std::string inAPipe(const std::string &cmd) {
  return "{ " + cmd + " 2>&1; echo \"exit $?\"; } | cat";
}

// The C path of a program: the C `paykan --emit-c` prints, compiled at an -O
// level with the runtime's sources and linked with the standard library's
// compiled code (`paykan pkm extract --object` of each module file, done
// once, into the directory the C path is given).
class CPath {
public:
  explicit CPath(const std::filesystem::path &dir) {
    for (const auto &e :
         std::filesystem::directory_iterator(PAYKAN_TEST_STDLIB_DIR)) {
      auto obj = dir / (e.path().stem().string() + ".o");
      auto x = run(std::string(kPaykan) + " pkm extract --object " +
                   shellWord(e.path().string()) + " -o " +
                   shellWord(obj.string()) + " 2>&1");
      EXPECT_TRUE(x.exitCode == 0 ||
                  x.out.find("has no compiled code") != std::string::npos)
          << x.out;
      if (x.exitCode == 0)
        Stdlib += " " + shellWord(obj.string());
    }
  }

  // Builds @p src at @p opt into the executable @p exe, giving paykan
  // @p flags and the C compiler @p cflags; the emitted C is <exe>.c.  The
  // output of the step that failed, or of the C compiler.
  CmdResult build(const std::string &src, const std::string &opt,
                  const std::string &exe, const std::string &flags = "",
                  const std::string &cflags = "") const {
    auto c = shellWord(exe + ".c");
    auto emitted = run(std::string(kPaykan) + " " + opt + " " + flags +
                       " --emit-c " + shellWord(src) + " > " + c + " 2>&1");
    if (emitted.exitCode != 0)
      return emitted;
    std::string runtime = shellWord(PAYKAN_RUNTIME_SOURCE_DIR);
    return run("cc -std=c11 " + cflags + " " + opt + " -I " + runtime + " " +
               c + " " + runtime + "/*.c" + Stdlib + " -lm -o " +
               shellWord(exe) + " 2>&1");
  }

private:
  std::string Stdlib;
};

// The heap statistics come after the program's own output, also when stdout
// is a pipe (fully buffered) and stderr is the same pipe: the runtime flushes
// stdout before printing them.  Checked on every path, at -O0 and -O2: `paykan
// run --track-heap`, and a program built with `paykan build` and on the C
// path run with PAYKAN_TRACK_HEAP; each exits 0.
TEST(Driver, HeapStatisticsFollowTheProgramsOutputInAPipe) {
  REQUIRE_BACKEND();
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_heap_order_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  auto src = (dir / "hello.pkn").string();
  std::ofstream(src)
      << "import ::io; fn main() -> int { io::println(\"Hello, world!\"); "
         "return 0; }";
  auto expectOrder = [](const std::string &what, const std::string &cmd) {
    auto r = run(inAPipe(cmd));
    size_t hello = r.out.find("Hello, world!\n");
    size_t stats = r.out.find("paykan heap stats:");
    ASSERT_NE(hello, std::string::npos) << what << ": " << r.out;
    ASSERT_NE(stats, std::string::npos) << what << ": " << r.out;
    EXPECT_LT(hello, stats) << what << ": " << r.out;
    EXPECT_NE(r.out.find("live blocks       : 0"), std::string::npos)
        << what << ": " << r.out;
    EXPECT_TRUE(r.out.ends_with("\nexit 0\n")) << what << ": " << r.out;
  };
  CPath cpath(dir);
  auto everyPath = [&](const std::string &opt) {
    expectOrder("run " + opt,
                paykanRun() + " " + opt + " --track-heap " + shellWord(src));
    auto exe = (dir / ("hello" + opt)).string();
    auto built = run(paykanRun() + " " + opt + " -o " + shellWord(exe) +
                     " build " + shellWord(src) + " 2>&1");
    ASSERT_EQ(built.exitCode, 0) << opt << ": " << built.out;
    expectOrder("build " + opt, "PAYKAN_TRACK_HEAP=1 " + shellWord(exe));
    auto cexe = (dir / ("hello-c" + opt)).string();
    auto c = cpath.build(src, opt, cexe);
    ASSERT_EQ(c.exitCode, 0) << opt << ": " << c.out;
    expectOrder("C " + opt, "PAYKAN_TRACK_HEAP=1 " + shellWord(cexe));
  };
  everyPath("-O0");
  everyPath("-O2");
  std::filesystem::remove_all(dir);
}

// A program that runs out of memory panics (exit 134) with a message that
// says so, after what it printed: here under a 96 MB address-space limit, with
// stdout a pipe, a HashMap, a string and a chain of objects that grow until
// an allocation fails.  Built with `paykan build` and on the C path, at -O0
// and -O2 (not run with `paykan run`: the compiler itself cannot start under
// the limit).  Linux only: macOS does not limit allocations by RLIMIT_AS,
// and an ASan or TSan runtime cannot start under it.
TEST(Driver, AProgramOutOfMemoryPanicsAfterItsOutput) {
#if !defined(__linux__)
  GTEST_SKIP() << "RLIMIT_AS limits allocations on Linux only";
#elif defined(PAYKAN_TEST_SHADOW_SANITIZER)
  GTEST_SKIP() << "an ASan or TSan runtime cannot start under a low RLIMIT_AS";
#else
  REQUIRE_BACKEND();
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_oom_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  auto src = (dir / "oom.pkn").string();
  std::ofstream(src) << "import ::collections; import ::io;\n"
                        "class Node { next: Node?; }\n"
                        "fn main(args: view Str[]) -> int {\n"
                        "  io::println(\"start\");\n"
                        "  if args[1] == \"map\" {\n"
                        "    m = collections::HashMap<int, int>(); i = 0;\n"
                        "    while True { m.put(i, i); i = i + 1; }\n"
                        "  }\n"
                        "  if args[1] == \"str\" {\n"
                        "    s = \"x\";\n"
                        "    while True { s = s + s; }\n"
                        "  }\n"
                        "  head: Node? = None;\n"
                        "  while True { head = Node(head); }\n"
                        "  return 0;\n"
                        "}\n";
  auto runOutOfMemory = [](const std::string &exe, const std::string &what) {
    auto r = run("ulimit -v 98304 && " + shellWord(exe) + " " + what + " 2>&1");
    EXPECT_EQ(r.exitCode, 134) << exe << " " << what << ": " << r.out;
    EXPECT_EQ(r.out.rfind("start\n", 0), 0U)
        << exe << " " << what << ": " << r.out;
    // A HashMap's buckets are a memory::Buffer, which fails with
    // OutOfMemory ("out of memory: Cannot allocate memory"); every other
    // allocation is Paykan_malloc's.
    EXPECT_NE(r.out.find(what == "map" ? "\npaykan: out of memory"
                                       : "\npaykan: out of memory allocating "),
              std::string::npos)
        << exe << " " << what << ": " << r.out;
  };
  CPath cpath(dir);
  auto buildAndRun = [&](const std::string &opt) {
    auto exe = (dir / ("oom" + opt)).string();
    auto built = run(paykanRun() + " " + opt + " -o " + shellWord(exe) +
                     " build " + shellWord(src) + " 2>&1");
    ASSERT_EQ(built.exitCode, 0) << opt << ": " << built.out;
    auto cexe = (dir / ("oom-c" + opt)).string();
    auto c = cpath.build(src, opt, cexe);
    ASSERT_EQ(c.exitCode, 0) << opt << ": " << c.out;
    for (const auto &what : {"map", "str", "objects"}) {
      runOutOfMemory(exe, what);
      runOutOfMemory(cexe, what);
    }
  };
  buildAndRun("-O0");
  buildAndRun("-O2");
  std::filesystem::remove_all(dir);
#endif
}

// A file opened for update ("w+", "r+", "a+") keeps a write that a read
// follows, and each read and write starts where the other stopped: C needs
// an fflush or a file positioning call between the two (C11 7.21.5.3),
// which the runtime makes when the stream switches.  Each read's result is
// written back into the file, so the files say what was read.  Run and
// built at -O0 and -O2; the files are checked byte for byte.
TEST(Driver, FileUpdateModesKeepEveryWrite) {
  REQUIRE_BACKEND();
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_update_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  auto src = (dir / "update.pkn").string();
  std::ofstream(src)
      << "import ::io; import ::error as err; import ::result as res;\n"
         "fn shown(r: res::Result<Str, err::Error>) -> Str {\n"
         "  match r {\n"
         "    s: Ok { return \"<\" + s.value + \">\"; }\n"
         "    e: Err { return \"<\" + err::name(e.error.code) + \">\"; }\n"
         "  }\n"
         "}\n"
         "fn update(path: Str, mode: Str) {\n"
         "  match io::open(path, \"w\") {\n"
         "    f: Ok { f.value.write(\"abc\\ndef\\nghi\\n\"); }\n"
         "    _ { }\n"
         "  }\n"
         "  match io::open(path, mode) {\n"
         "    u: Ok {\n"
         "      f = u.value;\n"
         "      f.write(\"one\\n\");\n"
         "      f.write(shown(f.readbytes(2)));\n"
         "      f.write(shown(f.read()));\n"
         "      f.write(\"two\\n\");\n"
         "    }\n"
         "    _ { }\n"
         "  }\n"
         "}\n"
         "fn main(args: view Str[]) -> int {\n"
         "  update(args[1] + \"/w.txt\", \"w+\");\n"
         "  update(args[1] + \"/r.txt\", \"r+\");\n"
         "  update(args[1] + \"/a.txt\", \"a+\");\n"
         "  return 0;\n"
         "}\n";
  auto contents = [](const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
  };
  auto expectFiles = [&](const std::string &what, const std::string &cmd) {
    auto out = dir / what;
    std::filesystem::create_directories(out);
    auto r = run(cmd + " " + out.string() + " 2>&1");
    EXPECT_EQ(r.exitCode, 0) << what << ": " << r.out;
    // "w+" empties the file: both reads find the end.
    EXPECT_EQ(contents(out / "w.txt"), "one\n<EndOfFile><EndOfFile>two\n")
        << what;
    // "r+" writes over the start, reads on after it, writes on after that.
    EXPECT_EQ(contents(out / "r.txt"), "one\nde<de>i\n<i\n>two\n") << what;
    // "a+" writes at the end, where the reads after the writes start.
    EXPECT_EQ(contents(out / "a.txt"),
              "abc\ndef\nghi\none\n<EndOfFile><EndOfFile>two\n")
        << what;
  };
  auto runAndBuild = [&](const std::string &opt) {
    expectFiles("run" + opt, paykanRun() + " " + opt + " " + src);
    auto exe = (dir / ("update" + opt)).string();
    auto [rc, out] =
        run(paykanRun() + " " + opt + " -o " + exe + " build " + src + " 2>&1");
    ASSERT_EQ(rc, 0) << opt << ": " << out;
    expectFiles("build" + opt, exe);
  };
  runAndBuild("-O0");
  runAndBuild("-O2");
  std::filesystem::remove_all(dir);
}

// A memory::Buffer slot never set, of a type whose zero bytes are no value,
// panics when at() or get() reads it, after what the program printed: a
// tagged union whose first variant holds a Str, a function value, and a
// struct holding one.  Before, at() lent the zeros as that first variant and
// get() gave a function value whose call jumped to address 0.  On every
// path, at -O0 and -O2: `run`, `build`, and the C `--emit-c` prints, linked
// with the runtime's sources and the standard library's compiled code.
TEST(Driver, ABufferSlotNeverSetPanicsOnEveryPath) {
  REQUIRE_BACKEND();
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_unset_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  auto src = (dir / "unset.pkn").string();
  std::ofstream(src) << "import ::io; import ::memory;\n"
                        "enum Shape { Named(s: Str), Empty }\n"
                        "struct Handler { id: int; run: () -> int; }\n"
                        "fn main(args: view Str[]) -> int {\n"
                        "  io::println(\"start\");\n"
                        "  if args[1] == \"union\" {\n"
                        "    b = memory::Buffer<Shape>(); b.grow(1);\n"
                        "    io::println(b.at(0));\n"
                        "  }\n"
                        "  if args[1] == \"function\" {\n"
                        "    b = memory::Buffer<() -> int>();\n"
                        "    b.push(fn() => 1); b.grow(1);\n"
                        "    f = b.get(1); io::println(f());\n"
                        "  }\n"
                        "  if args[1] == \"struct\" {\n"
                        "    b = memory::Buffer<Handler>(); b.grow(1);\n"
                        "    h = b.get(0); io::println(h.run());\n"
                        "  }\n"
                        "  return 0;\n"
                        "}\n";
  CPath cpath(dir);
  // Each of the program's cases panics after printing "start".
  auto expectPanics = [](const std::string &how, const std::string &cmd) {
    auto expect = [&](const std::string &what, const std::string &message) {
      auto r = run(cmd + " " + what + " 2>&1");
      EXPECT_EQ(r.exitCode, 134) << how << " " << what << ": " << r.out;
      EXPECT_EQ(r.out.rfind("start\n", 0), 0U)
          << how << " " << what << ": " << r.out;
      EXPECT_NE(r.out.find("\npaykan: " + message + "\n"), std::string::npos)
          << how << " " << what << ": " << r.out;
    };
    expect("union", "buffer value 0 was never set");
    expect("function", "buffer value 1 was never set");
    expect("struct", "buffer value 0 was never set");
  };
  auto everyPath = [&](const std::string &opt) {
    expectPanics("run " + opt, paykanRun() + " " + opt + " " + shellWord(src));
    auto exe = (dir / ("unset" + opt)).string();
    auto built = run(paykanRun() + " " + opt + " -o " + shellWord(exe) +
                     " build " + shellWord(src) + " 2>&1");
    ASSERT_EQ(built.exitCode, 0) << opt << ": " << built.out;
    expectPanics("build " + opt, shellWord(exe));
    auto cexe = (dir / ("unset-c" + opt)).string();
    auto c = cpath.build(src, opt, cexe);
    ASSERT_EQ(c.exitCode, 0) << opt << ": " << c.out;
    expectPanics("C " + opt, shellWord(cexe));
  };
  everyPath("-O0");
  everyPath("-O2");
  std::filesystem::remove_all(dir);
}

// A native function of an import bound to a C function that compiled code
// calls as one of the runtime's (fmod for a float's `%`, Paykan_panic_index
// for an index check, Paykan_heap_dump in C's main()) is declared once,
// ahead of every module's part of the C.  The main module's part, first in
// the unit, used to call the symbol with no declaration in scope: the
// header's was renamed for the native (`#define fmod pk__hidden_fmod`) and
// the native's own came in the import's part, after it ("implicit
// declaration of function 'fmod'").  The import is built from source
// (--no-module-cache), so its C is in the unit; the C is compiled as
// scripts/c_strict.py does, at -O0 and -O2.
TEST(Driver, AnImportsNativeOfARuntimeFunctionIsDeclaredForEveryModule) {
  REQUIRE_BACKEND();
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_native_rt_" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "na.pkn")
      << "native fn fmod(a: float, b: float) -> float = \"fmod\";\n"
         "native fn boom(i: int, n: int) = \"Paykan_panic_index\";\n"
         "native fn dump() = \"Paykan_heap_dump\";\n"
         "fn wrap(a: float, b: float) -> float { return fmod(a, b); }\n"
         "fn check(i: int, n: int) { if i >= n { boom(i, n); } }\n"
         "fn report(on: bool) { if on { dump(); } }\n";
  auto src = (dir / "main.pkn").string();
  std::ofstream(src) << "import ::io;\nimport na;\n"
                        "fn at(xs: view int[], i: int) -> int { return "
                        "xs[i]; }\n"
                        "fn main() -> int {\n"
                        "  x: float = 7.5;\n"
                        "  io::println(x % 2.0);\n"
                        "  io::println(na::wrap(9.5, 4.0));\n"
                        "  na::check(1, 3);\n"
                        "  na::report(False);\n"
                        "  io::println(at([10, 20, 30], 2));\n"
                        "  return 0;\n"
                        "}\n";
  CPath cpath(dir);
  for (const char *opt : {"-O0", "-O2"}) {
    auto exe = (dir / (std::string("main") + opt)).string();
    auto c = cpath.build(src, opt, exe, "--no-module-cache",
                         "-pedantic-errors -Wall -Wextra -Werror");
    ASSERT_EQ(c.exitCode, 0) << opt << ": " << c.out;
    auto r = run("PAYKAN_TRACK_HEAP=1 " + shellWord(exe) + " 2>&1");
    EXPECT_EQ(r.exitCode, 0) << opt << ": " << r.out;
    EXPECT_EQ(r.out.rfind("1.5\n1.5\n30\n", 0), 0U) << opt << ": " << r.out;
    EXPECT_NE(r.out.find("live blocks       : 0"), std::string::npos)
        << opt << ": " << r.out;
  }
  std::filesystem::remove_all(dir);
}

// The C of --emit-c includes Runtime.h with each native function's C symbol
// renamed (`#define memcpy pk__hidden_memcpy`), so the header's static
// inline functions are compiled under the rename too, and one that called a
// C library function a native function binds would call a function nothing
// defines: Paykan_vtable_of called memcpy ("undefined reference to
// `pk__hidden_memcpy'").  Each inline function, compiled and called under
// the rename of the C library functions the C path itself calls, links and
// works, at -O0 and -O2 (without _FORTIFY_SOURCE, whose inline memcpy would
// define the renamed one).
TEST(Driver, RuntimeHeaderInlinesCallNoRenamedFunction) {
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_rt_inline_" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  auto src = (dir / "inl.c").string();
  std::ofstream(src) << R"(#undef _FORTIFY_SOURCE
#define memcpy pk__hidden_memcpy
#define memmove pk__hidden_memmove
#define memset pk__hidden_memset
#define getenv pk__hidden_getenv
#include <stdlib.h>
#include <string.h>
#include "Runtime.h"
#undef memcpy
#undef memmove
#undef memset
#undef getenv
int main(void) {
  PaykanMethod table[1] = {0};
  PaykanMethod *header[2] = {table, 0};
  PaykanType *type = NULL;
  Paykan_retain_inline(NULL);
  Paykan_release_inline(NULL);
  if (type)
    return (int)(PaykanType_ref(type, 0) + PaykanType_unset(type, 0));
  return Paykan_vtable_of(header) == table &&
                 PaykanShared_get_inline(NULL) == NULL
             ? 0
             : 1;
}
)";
  std::string runtime = shellWord(PAYKAN_RUNTIME_SOURCE_DIR);
  for (const char *opt : {"-O0", "-O2"}) {
    auto exe = (dir / (std::string("inl") + opt)).string();
    std::string cmd = "cc -std=c11 -pedantic-errors -Wall -Wextra -Werror ";
    cmd.append(opt).append(" -I ").append(runtime).append(" ");
    cmd.append(shellWord(src)).append(" ").append(runtime);
    cmd.append("/*.c -lm -o ").append(shellWord(exe)).append(" 2>&1");
    auto cc = run(cmd);
    ASSERT_EQ(cc.exitCode, 0) << opt << ": " << cc.out;
    auto r = run(shellWord(exe) + " 2>&1");
    EXPECT_EQ(r.exitCode, 0) << opt << ": " << r.out;
  }
  std::filesystem::remove_all(dir);
}

// Reproducible output (#102): modules are named by their canonical module
// names, so nothing the compiler produces depends on where the sources live
// or on the directory it runs in.

namespace {

std::string slurp(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

/// A multi-module project under @p dir: nested modules and two modules with
/// the same stem in different directories (`a::util`, `b::util`).
void writeZooProject(const std::filesystem::path &dir) {
  std::filesystem::create_directories(dir / "geometry");
  std::filesystem::create_directories(dir / "a");
  std::filesystem::create_directories(dir / "b");
  std::ofstream(dir / "geometry" / "shapes.pkn")
      << "class Rect { w: int; h: int;\n"
         "  view fn area() -> int { return self.w * self.h; } }\n"
         "impl Rect {\n"
         "  fn __init__(w: int, h: int) { self.w = w; self.h = h; } }\n"
         "fn describe(r: Rect) -> Str { return \"rect\"; }\n";
  std::ofstream(dir / "a" / "util.pkn") << "fn tag() -> int { return 1; }\n";
  std::ofstream(dir / "b" / "util.pkn") << "fn tag() -> int { return 2; }\n";
  std::ofstream(dir / "zoo.pkn")
      << "import ::io; import geometry::shapes;\nimport a::util;\nimport "
         "b::util as bu;\n"
         "fn main() -> int { r: shapes::Rect = shapes::Rect(3, 4);\n"
         "  io::println(shapes::describe(r)); "
         "io::println(Str<int>(r.area()));\n"
         "  io::println(Str<int>(util::tag() * 10 + bu::tag())); return 0; }\n";
}

} // namespace

TEST(Driver, OutputDoesNotDependOnTheSourceDirectory) {
  REQUIRE_BACKEND();
  namespace fs = std::filesystem;
  auto base =
      fs::temp_directory_path() / ("drv_repro_" + std::to_string(getpid()));
  fs::remove_all(base);
  fs::path one = base / "one" / "proj";
  fs::path two = base / "two" / "deeper" / "proj";
  writeZooProject(one);
  writeZooProject(two);

  // The same program from two directories, invoked as `zoo.pkn` from its
  // directory, by a relative path from elsewhere and by an absolute path.
  struct Invocation {
    fs::path Cwd;
    std::string File;
  };
  const std::vector<Invocation> invocations = {
      {one, "zoo.pkn"},
      {two, "zoo.pkn"},
      {base / "two", "deeper/proj/zoo.pkn"},
      {"/", (one / "zoo.pkn").string()},
  };
  auto inDir = [](const Invocation &inv, const std::string &args) {
    return run("cd " + inv.Cwd.string() + " && " + paykanRun() + " " + args +
               " " + inv.File + " 2>&1");
  };
  for (const char *flag : {"--emit-pir", "--emit-source"}) {
    std::string first;
    for (const Invocation &inv : invocations) {
      auto [rc, out] = inDir(inv, flag);
      ASSERT_EQ(rc, 0) << flag << " in " << inv.Cwd << ": " << out;
      EXPECT_EQ(out.find(base.string()), std::string::npos) << flag << out;
      EXPECT_EQ(out.find(".pkn"), std::string::npos) << flag << out;
      if (first.empty())
        first = out;
      else
        EXPECT_EQ(out, first)
            << flag << " differs in " << inv.Cwd << " for " << inv.File;
    }
    if (std::string(flag) == "--emit-pir") {
      for (const char *m : {"module \"zoo\"\n", "module \"geometry::shapes\"\n",
                            "module \"a::util\"\n", "module \"b::util\"\n",
                            "module \"a::util\" symbol @tag\n",
                            "module \"b::util\" symbol @tag\n"})
        EXPECT_NE(first.find(m), std::string::npos) << m << "\n" << first;
      EXPECT_EQ(first.rfind("module \"zoo\"\n", 0), 0u) << first;
    }
  }

  // Same-stem modules stay apart when run (and when built, below).
  for (const Invocation &inv : invocations) {
    auto [rc, out] = inDir(inv, "--track-heap");
    EXPECT_EQ(rc, 0) << out;
    EXPECT_NE(out.find("rect\n12\n12\n"), std::string::npos) << out;
    EXPECT_NE(out.find("live blocks       : 0"), std::string::npos) << out;
  }

  // The executables built in the two trees are identical and carry no source
  // path.  Each is built under the same file name, in a directory of its
  // own: on macOS the
  // linker hashes the output's file name into LC_UUID and ad-hoc signs it
  // with the file name as its identifier, so differently named outputs of
  // the same program differ.
  std::vector<std::string> exes;
  for (const Invocation &inv :
       {invocations[0], invocations[3], invocations[2]}) {
    auto outDir = base / ("out" + std::to_string(exes.size()));
    fs::create_directories(outDir);
    auto exe = (outDir / "zoo").string();
    auto [rc, out] = inDir(inv, "-o " + exe + " build");
    ASSERT_EQ(rc, 0) << out;
    auto [rc2, out2] = run(exe + " 2>&1");
    EXPECT_EQ(rc2, 0) << out2;
    EXPECT_EQ(out2, "rect\n12\n12\n");
    exes.push_back(slurp(exe));
  }
  for (const std::string &bytes : exes) {
    EXPECT_EQ(bytes.find(base.string()), std::string::npos);
    EXPECT_EQ(bytes.find(".pkn"), std::string::npos);
    EXPECT_EQ(bytes, exes[0]);
  }
  fs::remove_all(base);
}

// `run` optimises the program under the layout the JIT runs it with.  Under
// LLVM's default layout an `int` after a `char` sits 4 bytes past it, so the
// object size the optimiser folds was 4 bytes short and a split store wrote the
// wrong offset.  --emit-llvm prints run's IR.
TEST(Driver, RunOptimisesUnderTheHostLayout) {
  REQUIRE_BACKEND();
  namespace fs = std::filesystem;
  auto dir =
      fs::temp_directory_path() / ("drv_layout_" + std::to_string(getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::ofstream(dir / "lay.pkn")
      << "import ::io; class P {\n  c: char;\n  n: int;\n}\n"
         "impl P { fn __init__() { self.c = 'x'; self.n = 7; } }\n"
         "fn main() -> int { p = P(); io::println(Str(p.c) + Str(p.n)); "
         "return 0; }\n";
  std::string src = (dir / "lay.pkn").string();
  auto [rc, ir] = run(paykanRun() + " --emit-source " + src + " 2>&1");
  ASSERT_EQ(rc, 0) << ir;
  EXPECT_NE(ir.find("target datalayout"), std::string::npos) << ir;
  // The header (vtable, 32-bit strong count, 32-bit weak count) is 16
  // bytes, the char padded to 8, the int 8.
  EXPECT_NE(ir.find("@Paykan_malloc(i64 32)"), std::string::npos) << ir;
  auto [rc2, out] = run(paykanRun() + " " + src + " 2>&1");
  EXPECT_EQ(rc2, 0) << out;
  EXPECT_EQ(out, "x7\n");
  fs::remove_all(dir);
}

// `paykan build` and `paykan run` optimise at -O2 unless told otherwise
// (#102); -O0 still turns optimisation off.
TEST(Driver, BuildAndRunDefaultToO2) {
  REQUIRE_BACKEND();
  namespace fs = std::filesystem;
  auto dir = fs::temp_directory_path() /
             ("drv_optdefault_" + std::to_string(getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::ofstream(dir / "lib.pkn")
      << "fn sq(x: int) -> int { y = x * x; return y; }\n";
  std::ofstream(dir / "opt.pkn")
      << "import ::io; import lib;\n"
         "fn main() -> int { s = 0; i = 0;\n"
         "  while i < 10 { s = s + lib::sq(i); i = i + 1; }\n"
         "  io::println(Str<int>(s)); return 0; }\n";
  std::string src = (dir / "opt.pkn").string();

  // build: the default produces the -O2 executable, not the -O0 one.  (Each
  // under the same file name, in a directory of its own: the name is part of
  // a macOS executable, its LC_UUID and code signature identifier.)
  auto build = [&](const std::string &opt) {
    auto outDir = dir / ("out" + opt);
    fs::create_directories(outDir);
    auto exe = (outDir / "opt").string();
    auto [rc, out] =
        run(paykanRun() + " " + opt + " -o " + exe + " build " + src + " 2>&1");
    EXPECT_EQ(rc, 0) << opt << ": " << out;
    auto [rc2, out2] = run(exe + " 2>&1");
    EXPECT_EQ(out2, "285\n") << opt;
    return slurp(exe);
  };
  std::string plain = build("");
  EXPECT_EQ(plain, build("-O2"));
  EXPECT_NE(plain, build("-O0"));

  // run: the level reaches the backend -- its IR (--emit-source goes
  // through `run`'s compile) is the optimised one.
  auto runWith = [&](const std::string &opt) {
    auto [rc, out] = run(paykanRun() + " " + opt + " " + src + " 2>&1");
    EXPECT_EQ(rc, 0) << opt << ": " << out;
    EXPECT_EQ(out, "285\n") << opt;
  };
  runWith("");
  runWith("-O0");
  auto emit = [&](const std::string &opt) {
    return run(paykanRun() + " " + opt + " --emit-source " + src + " 2>&1").out;
  };
  std::string ir = emit("");
  EXPECT_EQ(ir, emit("-O2"));
  EXPECT_NE(ir, emit("-O0"));
  // The program's own code is optimised: no stack slot is left in main.
  // (::io's, whose `errno` slots C writes through, keep theirs.)
  // (`define [attributes] i64 @main(`: the optimiser adds `noundef`.)
  size_t mainAt = ir.find(" @main(");
  ASSERT_NE(mainAt, std::string::npos) << ir;
  mainAt = ir.rfind('\n', mainAt) + 1;
  ASSERT_EQ(ir.compare(mainAt, 7, "define "), 0) << ir;
  std::string mainIR = ir.substr(mainAt, ir.find("\n}\n", mainAt) - mainAt);
  EXPECT_EQ(mainIR.find("alloca"), std::string::npos) << mainIR;
  // Cached bitcode is the unoptimised translation (the opt level is
  // applied to the linked program), so one entry serves every level: a
  // run at another level reuses it unchanged.
  auto bc = dir / ".paykan_cache" / "lib.bc";
  ASSERT_TRUE(fs::exists(bc)) << bc;
  auto stamp = fs::last_write_time(bc);
  runWith("-O1");
  runWith("-O3");
  EXPECT_EQ(fs::last_write_time(bc), stamp);
  fs::remove_all(dir);
}

// -- The standard library's private members

// The `__` names of `::collections` and `::memory` are private
// (docs/language/15-modules.md): a program cannot reach a container's `__`
// fields and methods, or the modules' `__` functions and classes.
// What a container hands out through a `view` is a `view`: an object
// element is read, not stored or changed, whatever the container.  Its
// copies (get(), getFront()) share an object, so they are not taken through
// a `view`.
TEST(Driver, StandardLibraryViewsDoNotChangeElements) {
  auto src = writeTmp(
      "import ::collections as c;\n"
      "import ::memory as mem;\n"
      "class Cell { n: int; }\n"
      "fn poke(xs: view c::List<Cell>, m: view c::HashMap<int, Cell>,\n"
      "        q: view c::MinPriorityQueue<Cell>,\n"
      "        s: view c::Stack<Cell>, l: view c::LinkedList<Cell>,\n"
      "        b: view mem::Buffer<Cell>) {\n"
      "  xs.at(0).n = 1;\n"
      "  kept = xs.last();\n"
      "  m.at(1).n = 2;\n"
      "  q.peek().n = 3;\n"
      "  s.peek().n = 4;\n"
      "  l.front().n = 5;\n"
      "  e = b.get(0);\n"
      "  f = xs.get(0);\n"
      "  g = m.getOr(1, Cell());\n"
      "  h = q.get();\n"
      "  i = l.getBack();\n"
      "}\n"
      "fn main() -> int { return 0; }\n");
  auto [rc, out] = run(std::string(kPaykan) + " --check-only " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  for (const char *line :
       {"the result of 'at' is a 'view'; cannot assign to its field 'n'",
        "the result of 'last' is a 'view'; it cannot be stored",
        "the result of 'peek' is a 'view'; cannot assign to its field 'n'",
        "the result of 'front' is a 'view'; cannot assign to its field 'n'",
        "'b' is a 'view' parameter; 'get' is not a 'view fn'",
        "'xs' is a 'view' parameter; 'get' is not a 'view fn'",
        "'m' is a 'view' parameter; 'getOr' is not a 'view fn'",
        "'q' is a 'view' parameter; 'get' is not a 'view fn'",
        "'l' is a 'view' parameter; 'getBack' is not a 'view fn'"})
    EXPECT_NE(out.find(line), std::string::npos) << line << "\n" << out;
}

TEST(Driver, StandardLibraryPrivateMembersAreEnforced) {
  auto src = writeTmp("import ::collections;\n"
                      "import ::memory;\n"
                      "import ::io;\n"
                      "fn main() -> int {\n"
                      "  m = collections::HashMap<int, int>();\n"
                      "  n = m.__table;\n"
                      "  l = collections::List<int>();\n"
                      "  l.__check(1, 1);\n"
                      "  b = collections::_bucket(1, 2);\n"
                      "  x = collections::_Entry<int, int>(1, 2, 3, None);\n"
                      "  buf = memory::Buffer<int>(1);\n"
                      "  d = buf.__data;\n"
                      "  f = io::File(m);\n"
                      "  return 0;\n}\n");
  auto [rc, out] = run(std::string(kPaykan) + " --check-only " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  for (const char *line :
       {"'__table' is private to 'HashMap<int, int>': it is reachable only "
        "from its own methods",
        "'__check' is private to 'List<int>': it is reachable only from its "
        "own methods",
        "'collections::_bucket' is private to module '::collections'",
        "'collections::_Entry' is private to module '::collections'",
        "'__data' is private to 'Buffer<int>': it is reachable only from its "
        "own methods",
        "'io::File' cannot be constructed here: its initializer '___init__' "
        "is private to module '::io'"})
    EXPECT_NE(out.find(line), std::string::npos) << line << "\n" << out;
}
