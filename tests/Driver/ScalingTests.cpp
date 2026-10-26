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
