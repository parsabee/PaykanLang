// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Smoke tests for the paykan driver CLI (--check-only, --dump-ast, --version).
// The PAYKAN_BIN CMake variable is injected so tests find the binary.

#include <gtest/gtest.h>

#include "Version.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// ---------------------------------------------------------------------------
// conversion panics (#64): int<float> outside int64, char<int> outside 0..255
// ---------------------------------------------------------------------------

static void expectConversionPanic(const std::string &decl,
                                  const std::string &conv,
                                  const std::string &message) {
  // (Nothing printed after the conversion may appear; the output printed
  // before a panic is covered by PanicFlushesStdoutOnEveryBackend.)
  auto src = writeTmp("fn main() -> int { " + decl + " " + conv +
                      " println(\"after\"); return 0; }");
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
      "fn main() -> int { lo: float = -9223372036854775808.0; "
      "hi: float = 9223372036854774784.0; "
      "println(Str<int>(int<float>(lo))); println(Str<int>(int<float>(hi))); "
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
  auto src = writeTmp("fn main() -> int { m: int = -9223372036854775807 - 1; "
                      "d: int = -1; r: int = m % d; println(Str<int>(r)); "
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

// A runtime panic aborts the program.  `paykan run` reports it the same way
// on every backend, as the exit status 128 + SIGABRT (the C backend's child
// process dies by the signal, the JIT turns it into that status), and an
// executable from `build` dies by SIGABRT on every backend (#79).
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
  auto backends = run(std::string(kPaykan) + " --list-backends 2>&1").out;
  int ran = 0;
  for (const std::string be : {"llvm", "c"}) {
    if (backends.find(be + "\n") == std::string::npos &&
        backends.find(be + " ") == std::string::npos)
      continue;
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
  EXPECT_GT(ran, 0) << backends;
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
      {"index", "xs = [1, 2, 3]; println(Str<int>(xs[10]));",
       "paykan: array index 10 out of bounds (len=3)"},
      {"store", "xs = [1, 2, 3]; i: int = 5; xs[i] = 4;",
       "paykan: array index 5 out of bounds (len=3)"},
      {"pop", "xs: int[] = []; v = xs.pop(); println(Str<int>(v));",
       "paykan: pop on empty array"},
      {"strindex", "s: Str = \"ab\"; c = s[7]; println(Str<char>(c));",
       "paykan: string index 7 out of bounds (len=2)"},
      {"divzero", "z: int = 0; println(Str<int>(7 / z));",
       "paykan: integer division or modulo by zero"},
      {"intfloat",
       "z: float = 0.0; n = int<float>(1.0 / z); "
       "println(Str<int>(n));",
       "paykan: int<float>(inf): the value is NaN, infinite or outside the "
       "int range"},
      {"charint", "n: int = 300; c = char<int>(n); println(Str<char>(c));",
       "paykan: char<int>(300): the value is outside the char range 0..255"},
  };
  const std::string before = "first line\nabout to panic...\n";
  auto backends = run(std::string(kPaykan) + " --list-backends 2>&1").out;
  int ran = 0;
  for (const std::string be : {"llvm", "c"}) {
    if (backends.find(be + "\n") == std::string::npos &&
        backends.find(be + " ") == std::string::npos)
      continue;
    ++ran;
    for (const auto &c : cases) {
      auto src = (dir / (std::string(c.name) + ".pkn")).string();
      {
        std::ofstream ofs(src);
        ofs << "fn main() -> int { println(\"first line\"); "
               "print(\"about to panic...\\n\"); "
            << c.body << " println(\"after\"); return 0; }";
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
  EXPECT_GT(ran, 0) << backends;
}

// A panic also flushes what the program has written to a File (#92).
TEST(Driver, PanicFlushesOpenFiles) {
  REQUIRE_BACKEND();
  auto out = std::filesystem::temp_directory_path() /
             ("drv_panic_file_" + std::to_string(getpid()) + ".txt");
  std::filesystem::remove(out);
  auto src = writeTmp("fn main() -> int { match open(\"" + out.string() +
                      "\", \"w\") { err: Error { return 1; } "
                      "f: File { f.write(\"kept\\n\"); xs = [1]; i: int = 3; "
                      "println(Str<int>(xs[i])); } } return 0; }");
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
      " println(Str<bool>(n != n) + Str<bool>(n == n) + Str<bool>(n < 1.0)"
      " + Str<bool>(1.0 != n) + Str<bool>(1.0 != 1.0)); return 0; }");
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

// A cached object that was truncated or corrupted after it was written (disk
// full, a crash, an outside edit) is detected through the size and hash in
// its `.key` and rebuilt, instead of failing every later link (#74).
TEST(Driver, CBackendRebuildsACorruptCachedObject) {
  REQUIRE_BACKEND();
  auto dir = std::filesystem::temp_directory_path() /
             ("drv_corrupt_" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "dep.pkn")
      << "class P { a: int; fn __init__() { self.a = 1; }"
         " fn get() -> int { return self.a; } }\n"
         "fn v() -> int { return 1; }\n";
  std::ofstream(dir / "mid.pkn")
      << "import dep;\nfn mk() -> dep::P { return dep::P(); }\n";
  std::ofstream(dir / "main.pkn") << "import mid;\nimport dep;\n"
                                     "fn main() -> int { p = mid::mk(); "
                                     "println(Str<int>(p.get() + dep::v()));"
                                     " return 0; }\n";
  std::string src = (dir / "main.pkn").string();
  std::string runCmd = std::string(kPaykan) + " --backend=c " + src + " 2>&1";
  std::string exe = (dir / "main").string();
  std::string buildCmd = std::string(kPaykan) + " --backend=c -o " + exe +
                         " build " + src + " 2>&1";
  auto [rc, out] = run(runCmd);
  ASSERT_EQ(rc, 0) << out;
  ASSERT_EQ(out, "2\n");
  auto obj = dir / ".paykan_cache" / "dep.pkn.o";
  ASSERT_TRUE(std::filesystem::exists(obj)) << obj;
  auto size = std::filesystem::file_size(obj);
  ASSERT_GT(size, 16u);

  // An intact entry is reused: the object is not rewritten.
  auto stamp = std::filesystem::last_write_time(obj);
  auto [rcSame, outSame] = run(runCmd);
  EXPECT_EQ(rcSame, 0) << outSame;
  EXPECT_EQ(std::filesystem::last_write_time(obj), stamp);

  // Truncated (the audit's repro), then same-size corruption (only the hash
  // can tell), for both `run` and `build`.
  for (const std::string &cmd : {runCmd, buildCmd}) {
    std::filesystem::resize_file(obj, 10);
    auto [rc1, out1] = run(cmd);
    EXPECT_EQ(rc1, 0) << cmd << "\n" << out1;
    EXPECT_EQ(std::filesystem::file_size(obj), size);

    {
      std::fstream f(obj, std::ios::in | std::ios::out | std::ios::binary);
      f.seekp(0);
      f.write("\0\0\0\0\0\0\0\0", 8); // clobber the ELF / Mach-O magic
    }
    EXPECT_EQ(std::filesystem::file_size(obj), size);
    auto [rc2, out2] = run(cmd);
    EXPECT_EQ(rc2, 0) << cmd << "\n" << out2;
  }
  auto [rcExe, outExe] = run(exe + " 2>&1");
  EXPECT_EQ(rcExe, 0) << outExe;
  EXPECT_EQ(outExe, "2\n");
  auto [rcRun, outRun] = run(runCmd);
  EXPECT_EQ(rcRun, 0) << outRun;
  EXPECT_EQ(outRun, "2\n");
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
