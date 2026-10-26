// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// The compiler's time and memory on large programs: each program here is
// one that a cost quadratic in its size made take minutes or gigabytes,
// compiled under a bound that leaves the linear compiler a wide margin.
// The time measured is the processor time `paykan` spends, which the tests
// running beside these leave alone, and the memory its peak resident size;
// ctest runs them as DriverScaling, labelled `scaling`, apart from the
// other driver tests.

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PAYKAN_BIN
#error "PAYKAN_BIN must be defined via CMake compile definition"
#endif

namespace {

const char *const kPaykan = PAYKAN_BIN;

/// How much slower and larger than a plain build's the compiler under test
/// may be: a sanitized or coverage build's is several times either.
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
  double PeakMiB = 0; // the largest resident size of a process it ran
};

/// Run @p cmd through the shell: its exit status, its standard output, and
/// the processor time and memory it and the processes it waited for took.
Timed timed(const std::string &cmd) {
  Timed t;
  int out[2];
  if (pipe(out) != 0)
    return t;
  const pid_t pid = fork();
  if (pid < 0)
    return t;
  if (pid == 0) {
    dup2(out[1], STDOUT_FILENO);
    close(out[0]);
    close(out[1]);
    execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char *>(nullptr));
    _exit(127);
  }
  close(out[1]);
  std::array<char, 4096> buf{};
  for (ssize_t n; (n = read(out[0], buf.data(), buf.size())) != 0;) {
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0)
      break;
    t.Out.append(buf.data(), static_cast<size_t>(n));
  }
  close(out[0]);
  int status = 0;
  rusage usage{};
  while (wait4(pid, &status, 0, &usage) < 0 && errno == EINTR) {
  }
  t.ExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  t.Seconds =
      static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
      static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) /
          1e6;
#ifdef __APPLE__
  t.PeakMiB = static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0);
#else
  t.PeakMiB = static_cast<double>(usage.ru_maxrss) / 1024.0;
#endif
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

/// @p text, `#` standing for @p i.
std::string numbered(const char *text, int i) {
  const std::string k = std::to_string(i);
  std::string s;
  for (const char *c = text; *c; ++c) {
    if (*c == '#')
      s += k;
    else
      s += *c;
  }
  return s;
}

/// numbered(@p text, i) for each i of 0 .. @p n - 1.
std::string repeated(const char *text, int n) {
  std::string s;
  for (int i = 0; i < n; ++i)
    s += numbered(text, i);
  return s;
}

/// A function matching an int against @p arms literal arms, each returning
/// its value, called on 7.
std::string manyArms(int arms) {
  return "fn pick(x: int) -> int {\n  match x {\n" +
         repeated("    # { return #; }\n", arms) +
         "    _ { return 0; }\n  }\n}\n"
         "fn main() -> int { return pick(7); }\n";
}

} // namespace

// Every integer division is behind two checks that panic, and a panic does
// not return: what follows a check runs beside it.  When each check nested
// the rest of its function, 3000 divisions in one function took 97 s of
// processor time to reach `--emit-mlir=scf` (SW-02); they take well under a
// second now.
TEST(Scaling, PanicGuardsLeaveTheFunctionFlat) {
  ScratchDir dir("guards");
  const std::string src = "fn id(x: int) -> int => x;\n"
                          "fn main() -> int {\n"
                          "  a = id(1000000);\n"
                          "  b = id(3);\n"
                          "  s = 0;\n" +
                          repeated("  s += a / b;\n", 3000) +
                          "  return s % 256;\n}\n";
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

// A long run of `if`s in one function, and a match on a Str with many arms
// (a test of each in turn), are thousands of scf.if ops in one block, which
// the lowering to cf splits one after another (SW-17).  The conversion
// driver it ran under recorded every op each split moved, to undo it, and
// each split moved everything after it: 3000 ifs and 2000 arms took 1.3 GB
// and 19.5 s to reach --emit-mlir=llvm.  Lowering the ops of a block from
// its last, with nothing to undo, takes 0.1 GB and 2 s.
TEST(Scaling, ALongRunOfIfsLowersToCFInLinearMemory) {
  ScratchDir dir("ifs");
  const std::string src =
      "fn f(k: int) -> int {\n  r = -1;\n" +
      repeated("  if k == # {\n    r = #;\n  }\n", 3000) +
      "  return r;\n}\nfn g(s: Str) -> int {\n  match s {\n" +
      repeated("    \"k#\" { return #; }\n", 2000) +
      "    _ { return -1; }\n  }\n}\n"
      "fn main() -> int { return f(2999) % 256 + g(\"k5\"); }\n";
  const std::string path = dir.write("ifs.pkn", src);
  Timed t = timed(std::string(kPaykan) + " --emit-mlir=llvm " + path +
                  " 2>&1 >/dev/null");
  ASSERT_EQ(t.ExitCode, 0) << t.Out;
  EXPECT_LT(t.PeakMiB, 400 * slowdown());
  EXPECT_LT(t.Seconds, 10 * slowdown());
  Timed run = timed(std::string(kPaykan) + " run -O0 " + path + " 2>&1");
  EXPECT_EQ(run.ExitCode, 2999 % 256 + 5) << run.Out;
}

// Each imported module's code is cached under a key made from its MLIR,
// printed (SW-18).  Printing a module nested in the program verified and
// numbered the whole program each time, so a program of 400 tiny modules
// took 16.5 s of processor time to print its LLVM IR from the cache, and
// takes 1 s now.
TEST(Scaling, AModulesCacheKeyIsMadeFromItsOwnCode) {
  ScratchDir dir("modules");
  std::filesystem::create_directories(dir.Path / "w");
  for (int i = 0; i < 400; ++i)
    dir.write(
        numbered("w/m#.pkn", i),
        numbered("class C# { v: int; }\nfn f() -> C# { return C#(#); }\n", i));
  const std::string main = "import ::io;\n" + repeated("import w::m#;\n", 400) +
                           "fn main() -> int {\n  io::println(Str(0" +
                           repeated(" + m#::f().v", 400) +
                           "));\n  return 0;\n}\n";
  const std::string path = dir.write("main.pkn", main);
  Timed first = timed(std::string(kPaykan) + " run " + path + " 2>&1");
  ASSERT_EQ(first.ExitCode, 0) << first.Out;
  EXPECT_EQ(first.Out, "79800\n");
  Timed t =
      timed(std::string(kPaykan) + " --emit-llvm " + path + " 2>&1 >/dev/null");
  ASSERT_EQ(t.ExitCode, 0) << t.Out;
  EXPECT_LT(t.Seconds, 5 * slowdown());
}
