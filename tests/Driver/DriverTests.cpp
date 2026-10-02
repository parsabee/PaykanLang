// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Smoke tests for the paykan driver CLI (--check-only, --dump-ast, --version).
// The PAYKAN_BIN CMake variable is injected so tests find the binary.

#include <gtest/gtest.h>

#include "Version.h"

#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

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

// ---------------------------------------------------------------------------
// --check-only
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// --version / -v
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// --frontend / --list-frontends
// ---------------------------------------------------------------------------

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

TEST(Driver, UnknownFrontendIsRejected) {
  auto src = writeTmp("fn main() -> int { return 0; }");
  auto [rc, out] = run(std::string(kPaykan) + " --frontend=no-such-frontend " +
                       src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_NE(out.find("unknown frontend 'no-such-frontend'"), std::string::npos)
      << out;
}

// ---------------------------------------------------------------------------
// --dump-tokens
// ---------------------------------------------------------------------------

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

TEST(Driver, ProgramArgumentsFollowTheSourceFile) {
  REQUIRE_BACKEND();
  // Everything after the source file is the program's, even if it looks
  // like an option.
  auto src = writeTmp("fn main(args: Str[]) -> int { return args.len(); }");
  auto [rc, out] = run(paykanRun() + " " + src + " --dump-ast -O2 x 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 4) << out;
}

// ---------------------------------------------------------------------------
// integer divide / modulo by zero trap
// ---------------------------------------------------------------------------

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

// INT64_MIN % -1 is mathematically 0 and must not trap.
TEST(Driver, IntModMinByNegOneIsZero) {
  REQUIRE_BACKEND();
  auto src = writeTmp("fn main() -> int { m: int = -9223372036854775807 - 1; "
                      "d: int = -1; r: int = m % d; println(StrInt(r)); "
                      "return 7 % d + 3; }");
  auto [rc, out] = run(paykanRun() + " " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 3) << out;
  EXPECT_EQ(out, "0\n");
}

TEST(Driver, IntDivNonZeroSucceeds) {
  REQUIRE_BACKEND();
  auto src = writeTmp("fn main() -> int { return 17 / 5; }");
  auto [rc, _] = run(paykanRun() + " " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 3); // 17 / 5 == 3, returned as the process exit code
}

// ---------------------------------------------------------------------------
// --dump-ast
// ---------------------------------------------------------------------------

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

TEST(Driver, OptionalPrimitiveIsRejectedAtParse) {
  auto src = writeTmp("fn main() -> int { x: int? = None; return 0; }");
  auto [rc, out] = run(std::string(kPaykan) + " --check-only " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_NE(out.find("optional primitive types are not supported yet"),
            std::string::npos)
      << out;
}

// ---------------------------------------------------------------------------
// PIR, the C backend, build, and the object cache
// ---------------------------------------------------------------------------

TEST(Driver, EmitPirPrintsTheProgram) {
  REQUIRE_BACKEND();
  auto src = writeTmp("fn main() -> int { s: Str = \"hi\"; println(s); "
                      "return 0; }");
  auto [rc, out] = run(std::string(kPaykan) + " --emit-pir " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 0) << out;
  EXPECT_NE(out.find("fn @main() -> i64 {"), std::string::npos) << out;
  EXPECT_NE(out.find("release"), std::string::npos) << out;
  EXPECT_NE(out.find("call @Paykan_println("), std::string::npos) << out;
}

TEST(Driver, EmitCPrintsCSource) {
  REQUIRE_BACKEND();
  auto src = writeTmp("class P { x: int; fn __init__(x: int) { self.x = x; } }"
                      "fn main() -> int { p = P(3); return p.x; }");
  auto [rc, out] = run(std::string(kPaykan) + " --emit-c " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 0) << out;
  EXPECT_NE(out.find("#include \"Runtime.h\""), std::string::npos) << out;
  EXPECT_NE(out.find("int main(int argc, char **argv)"), std::string::npos)
      << out;
  EXPECT_NE(out.find("PaykanShared *shared;"), std::string::npos) << out;
}

TEST(Driver, EmitCRejectsAnotherBackend) {
  REQUIRE_BACKEND();
  auto src = writeTmp("fn main() -> int { return 0; }");
  auto [rc, out] =
      run(std::string(kPaykan) + " --backend=llvm --emit-c " + src + " 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_NE(out.find("--emit-c needs the c backend"), std::string::npos) << out;
}

// Float `!=` is IEEE's unordered not-equal (true for a NaN operand) and the
// other comparisons are ordered, identically on every backend built: the LLVM
// backend emits `fcmp une`, the C backend C's own `!=`.
TEST(Driver, FloatNotEqualIsUnorderedOnEveryBackend) {
  REQUIRE_BACKEND();
  auto src = writeTmp(
      "fn main() -> int { inf: float = 1.0e308 * 10.0; n: float = inf - inf;"
      " println(StrBool(n != n) + StrBool(n == n) + StrBool(n < 1.0)"
      " + StrBool(1.0 != n) + StrBool(1.0 != 1.0)); return 0; }");
  auto backends = run(std::string(kPaykan) + " --list-backends 2>&1").out;
  int ran = 0;
  for (const char *be : {"llvm", "c"}) {
    if (backends.find(std::string(be) + "\n") == std::string::npos &&
        backends.find(std::string(be) + " ") == std::string::npos)
      continue;
    ++ran;
    auto [rc, out] =
        run(std::string(kPaykan) + " --backend=" + be + " " + src + " 2>&1");
    EXPECT_EQ(rc, 0) << be << ": " << out;
    EXPECT_EQ(out, "TrueFalseFalseTrueFalse\n") << be;
    auto [srcRc, code] = run(std::string(kPaykan) + " --backend=" + be +
                             " --emit-source " + src + " 2>&1");
    EXPECT_EQ(srcRc, 0) << be << ": " << code;
    if (std::string(be) == "llvm") {
      EXPECT_NE(code.find("fcmp une double"), std::string::npos) << code;
      EXPECT_EQ(code.find("fcmp one"), std::string::npos) << code;
    } else {
      EXPECT_NE(code.find("_n != v"), std::string::npos) << code;
      EXPECT_EQ(code.find(" || "), std::string::npos) << code;
    }
  }
  std::filesystem::remove(src);
  EXPECT_GT(ran, 0) << backends;
}

// `build` writes an executable that runs on its own, and the C backend reuses
// the cached object for a module whose generated C is unchanged.
TEST(Driver, CBackendBuildsAnExecutableAndCachesObjects) {
  REQUIRE_BACKEND();
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_build_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  auto src = (dir / "prog.pkn").string();
  {
    std::ofstream ofs(src);
    ofs << "fn main(args: Str[]) -> int { println(\"built\"); return "
           "args.len(); }";
  }
  auto exe = (dir / "prog").string();
  // Options (including -o) come before the source file.
  auto [rc, out] = run(std::string(kPaykan) + " --backend=c -o " + exe +
                       " build " + src + " 2>&1");
  ASSERT_EQ(rc, 0) << out;
  ASSERT_TRUE(std::filesystem::exists(exe));
  auto cache = dir / ".paykan_cache" / "prog.pkn.o";
  ASSERT_TRUE(std::filesystem::exists(cache)) << cache;
  auto stamp = std::filesystem::last_write_time(cache);

  auto [rc2, out2] = run(exe + " a b 2>&1");
  EXPECT_EQ(rc2, 3) << out2; // the executable itself plus two arguments
  EXPECT_EQ(out2, "built\n");

  // A second build leaves the cached object alone.
  auto [rc3, out3] = run(std::string(kPaykan) + " --backend=c -o " + exe +
                         " build " + src + " 2>&1");
  EXPECT_EQ(rc3, 0) << out3;
  EXPECT_EQ(std::filesystem::last_write_time(cache), stamp);

  // Changing the source regenerates it.
  {
    std::ofstream ofs(src);
    ofs << "fn main() -> int { println(\"rebuilt\"); return 5; }";
  }
  auto [rc4, out4] = run(std::string(kPaykan) + " --backend=c -o " + exe +
                         " build " + src + " 2>&1");
  EXPECT_EQ(rc4, 0) << out4;
  auto [rc5, out5] = run(exe + " 2>&1");
  EXPECT_EQ(rc5, 5) << out5;
  EXPECT_EQ(out5, "rebuilt\n");

  // The cache key covers the runtime header and the compiler's version: an
  // object compiled by another paykan, or against another Runtime.h, is
  // rebuilt even though the module's C is unchanged.  Simulate one by
  // rewriting the stored key and corrupting the object (reusing it would
  // fail the link).
  auto keyPath = dir / ".paykan_cache" / "prog.pkn.key";
  std::string key;
  {
    std::ifstream in(keyPath);
    key.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
  }
  EXPECT_NE(key.find(std::string("\npaykan:") + paykan::kVersion + "\n"),
            std::string::npos)
      << key;
  EXPECT_NE(key.find("\nruntime.h:"), std::string::npos) << key;
  EXPECT_NE(key.find("\nc:"), std::string::npos) << key;
  // Entries are written through temporaries renamed into place; none are
  // left behind.
  for (const auto &e :
       std::filesystem::directory_iterator(dir / ".paykan_cache"))
    EXPECT_EQ(e.path().string().find(".tmp"), std::string::npos) << e.path();
  for (const char *field : {"\npaykan:", "\nruntime.h:"}) {
    std::string stale = key;
    stale.insert(stale.find(field) + std::strlen(field), "old-");
    std::ofstream(keyPath) << stale;
    std::ofstream(cache) << "not an object file";
    std::string rebuild = kPaykan;
    rebuild += " --backend=c -o " + exe;
    rebuild += " build " + src + " 2>&1";
    auto [rc6, out6] = run(rebuild);
    EXPECT_EQ(rc6, 0) << field << out6;
    auto [rc7, out7] = run(exe + " 2>&1");
    EXPECT_EQ(rc7, 5) << field << out7;
  }
  std::filesystem::remove_all(dir);
}

// `build` on the test backend (each backend that runs programs: the C
// backend through the system C compiler, the llvm backend through a native
// object linked against the runtime) gives an executable that behaves like
// `run`: arguments, exit code, PAYKAN_TRACK_HEAP, at -O0 and -O2.
TEST(Driver, BuildProducesAStandaloneExecutable) {
  REQUIRE_BACKEND();
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_build_any_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  auto src = (dir / "prog.pkn").string();
  {
    std::ofstream ofs(src);
    ofs << "class Box { v: int; fn __init__(v: int) { self.v = v; } }"
           "fn main(args: Str[]) -> int { b = Box(40); println(args[1]); "
           "return b.v + args.len(); }";
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
    ofs << "fn main() -> int { println(\"no args\"); return 7; }";
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
  std::ofstream(src) << "fn main() -> int { println(\"ok\"); return 7; }";
  // Before and after the source file.
  for (const std::string &args :
       {std::string(" --backend=c -o ") + (dir / "before").string() +
            " build " + src,
        std::string(" build ") + src + " -o " + (dir / "after").string() +
            " --backend=c"}) {
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

// A coverage build's runtime archive is instrumented, so every backend that
// builds programs must link them with the same coverage flags (and a
// compiler whose profile runtime matches; the C backend also compiles the
// program with them): the program builds, runs and writes its own profile.
TEST(Driver, ProgramsAreInstrumentedInACoverageBuild) {
#ifndef PAYKAN_TEST_COVERAGE
  GTEST_SKIP() << "not a PAYKAN_COVERAGE build";
#else
  REQUIRE_BACKEND();
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_cov_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  auto src = (dir / "cov.pkn").string();
  std::ofstream(src) << "fn main() -> int { println(\"cov\"); return 3; }";
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
  auto [rc, out] =
      run(std::string(kPaykan) + " --backend=c build " + src + " extra 2>&1");
  std::filesystem::remove(src);
  EXPECT_NE(rc, 0);
  EXPECT_NE(out.find("unexpected argument 'extra'"), std::string::npos) << out;
}

TEST(Driver, CBackendRunForwardsArgumentsAndTracksTheHeap) {
  REQUIRE_BACKEND();
  auto src = writeTmp("fn main(args: Str[]) -> int { println(args[1]); "
                      "return args.len(); }");
  auto [rc, out] = run(std::string(kPaykan) + " --backend=c --track-heap " +
                       src + " hello 2>&1");
  std::filesystem::remove(src);
  EXPECT_EQ(rc, 2) << out;
  EXPECT_NE(out.find("hello\n"), std::string::npos) << out;
  EXPECT_NE(out.find("live blocks       : 0"), std::string::npos) << out;
}
