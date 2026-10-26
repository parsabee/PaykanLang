// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// The compiler's time on large programs: each program here is one that a
// cost quadratic in its size, or worse, made take minutes, compiled under a
// bound that leaves the linear compiler a wide margin.  The time measured is
// the processor time `paykan` spends (getrusage of the children), which the
// tests running beside these leave alone; ctest runs them as DriverScaling,
// labelled `scaling`, apart from the other driver tests.

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include <sys/resource.h>
#include <sys/wait.h>

#ifndef PAYKAN_BIN
#error "PAYKAN_BIN must be defined via CMake compile definition"
#endif

namespace {

const char *const kPaykan = PAYKAN_BIN;

/// How much slower than a plain build the compiler under test may be: a
/// sanitized or coverage build's is several times slower.
double slowdown() {
#if defined(PAYKAN_TEST_SANITIZED) || defined(PAYKAN_TEST_COVERAGE)
  return 4;
#else
  return 1;
#endif
}

struct Timed {
  int ExitCode = -1;
  std::string Out;
  double Seconds = 0; // processor time, user and system
};

double cpuSeconds(const rusage &u) {
  return static_cast<double>(u.ru_utime.tv_sec + u.ru_stime.tv_sec) +
         static_cast<double>(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}

/// Run @p cmd through the shell: its exit status, its standard output, and
/// the processor time it took.
Timed timed(const std::string &cmd) {
  Timed t;
  rusage before{};
  getrusage(RUSAGE_CHILDREN, &before);
  FILE *fp = popen(cmd.c_str(), "r");
  if (!fp)
    return t;
  std::array<char, 4096> buf{};
  while (fgets(buf.data(), buf.size(), fp))
    t.Out += buf.data();
  const int rc = pclose(fp);
  rusage after{};
  getrusage(RUSAGE_CHILDREN, &after);
  t.ExitCode = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
  t.Seconds = cpuSeconds(after) - cpuSeconds(before);
  return t;
}

/// A fresh directory for one test's files, removed with it.
class ScratchDir {
public:
  explicit ScratchDir(const std::string &name)
      : Path(std::filesystem::temp_directory_path() /
             ("paykan_scaling_" + name + "_" + std::to_string(getpid()))) {
    std::filesystem::remove_all(Path);
    std::filesystem::create_directories(Path);
  }
  ~ScratchDir() {
    std::error_code ec;
    std::filesystem::remove_all(Path, ec);
  }
  ScratchDir(const ScratchDir &) = delete;
  ScratchDir &operator=(const ScratchDir &) = delete;

  /// Write @p text to @p file in the directory: its path.
  std::string write(const std::string &file, const std::string &text) const {
    const std::filesystem::path p = Path / file;
    std::ofstream(p) << text;
    return p.string();
  }

  const std::filesystem::path Path;
};

/// A function matching an int against @p arms literal arms, each returning
/// its value modulo 100, called on 7.
std::string manyArms(int arms) {
  std::string src = "fn pick(x: int) -> int {\n  match x {\n";
  for (int i = 0; i < arms; ++i) {
    const std::string k = std::to_string(i);
    src += "    ";
    src += k;
    src += " { return ";
    src += std::to_string(i % 100);
    src += "; }\n";
  }
  src += "    _ { return 0; }\n  }\n}\n"
         "fn main() -> int { return pick(7); }\n";
  return src;
}

} // namespace

// Every integer division is behind two checks that panic, and a panic does
// not return: what follows a check runs beside it.  When each check nested
// the rest of its function, 3000 divisions in one function took 97 s of
// processor time to reach `--emit-mlir=scf` (SW-02); they take well under a
// second now.
TEST(Scaling, PanicGuardsLeaveTheFunctionFlat) {
  ScratchDir dir("guards");
  std::string src = "fn id(x: int) -> int => x;\n"
                    "fn main() -> int {\n"
                    "  a = id(1000000);\n"
                    "  b = id(3);\n"
                    "  s = 0;\n";
  for (int i = 0; i < 3000; ++i)
    src += "  s += a / b;\n";
  src += "  return s % 256;\n}\n";
  const std::string path = dir.write("guards.pkn", src);
  Timed t = timed(std::string(kPaykan) + " --emit-mlir=scf " + path +
                  " 2>&1 >/dev/null");
  ASSERT_EQ(t.ExitCode, 0) << t.Out;
  EXPECT_LT(t.Seconds, 20 * slowdown());
}

// A match's literal arms are a switch whose cases the lowering and PIR's
// verifier each check for one listed twice, with a set of the cases seen
// (SW-16): 20000 arms took 35 s to reach --emit-pir when each case was
// compared with every case before it, and take half a second now.
TEST(Scaling, ALargeMatchFindsItsRepeatedCasesInLinearTime) {
  ScratchDir dir("arms");
  const std::string path = dir.write("arms.pkn", manyArms(20000));
  Timed t =
      timed(std::string(kPaykan) + " --emit-pir " + path + " 2>&1 >/dev/null");
  ASSERT_EQ(t.ExitCode, 0) << t.Out;
  EXPECT_LT(t.Seconds, 10 * slowdown());
}

// `run -O0` generates code as `build -O0` does, with the fast instruction
// selector (SW-16): `run -O0` of a 5000-armed match took 47 s, against 2.6 s
// for `build -O0`, when the JIT generated code at its default level from
// the unoptimised module.  Now it takes about as long as the build, which
// also links.
TEST(Scaling, RunAtO0GeneratesCodeAsBuildDoes) {
  ScratchDir dir("run_o0");
  const std::string path = dir.write("arms.pkn", manyArms(5000));
  const std::string exe = (dir.Path / "arms").string();
  Timed build = timed(std::string(kPaykan) + " build -O0 " + path + " -o " +
                      exe + " 2>&1");
  ASSERT_EQ(build.ExitCode, 0) << build.Out;
  Timed run = timed(std::string(kPaykan) + " run -O0 " + path + " 2>&1");
  EXPECT_EQ(run.ExitCode, 7) << run.Out;
  EXPECT_LT(run.Seconds, 2 * build.Seconds + 5 * slowdown())
      << "build -O0: " << build.Seconds << " s";
}
