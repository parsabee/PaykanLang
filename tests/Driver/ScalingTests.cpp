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
// the rest of its function, 3000 divisions in one function took 75 to 86 s
// of processor time to reach `--emit-mlir=scf` (SW-02); they take well under
// a second now.
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
// (SW-16): 20000 arms took 46 s to reach --emit-pir when each case was
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

// Each arm of a match on a Str compares with a string constant, and the C
// path drops a constant nothing reads (C warns about an unused one).  It
// looked for a constant's readers with a walk of the whole module, once
// for each constant, so 2000 arms took 32 s of processor time to reach
// --emit-c, against 1.1 s to reach --emit-mlir=scf, and 3000 arms 72 s.
// One walk of the module finds every constant's readers now, and the
// 3000 arms take 2 s.
TEST(Scaling, AMatchOfManyStrArmsReachesCInLinearTime) {
  ScratchDir dir("str_arms");
  const std::string src = "fn g(s: Str) -> int {\n  match s {\n" +
                          repeated("    \"k#\" { return #; }\n", 3000) +
                          "    _ { return -1; }\n  }\n}\n"
                          "fn main() -> int { return g(\"k5\"); }\n";
  const std::string path = dir.write("arms.pkn", src);
  Timed t =
      timed(std::string(kPaykan) + " --emit-c " + path + " 2>&1 >/dev/null");
  ASSERT_EQ(t.ExitCode, 0) << t.Out;
  EXPECT_LT(t.Seconds, 15 * slowdown());
}

// Leaving a scope releases each value held in memory there, and a tagged
// union's references are every variant's fields: a match returning from
// each arm of a 200-variant union released them all at each return, code
// quadratic in the variants that took 28 to 30 s to run (SW-03).  A
// value of more than four references is released by one call of its
// type's drop function now once the function's ways out have written 512
// releases in place, and the call releases its variant's fields only.
// It runs at -O0, so the time is that of the code the lowering writes (19 s
// of processor time on base, 0.5 s now), not that of LLVM's optimiser on
// one large function, which at -O2 varies several-fold between machines.
TEST(Scaling, AScopesWayOutReleasesALargeValueByOneCall) {
  ScratchDir dir("drop");
  std::string variants;
  for (int i = 0; i < 200; ++i)
    variants += numbered(i % 3 == 0 ? ", V#(a: int, s: Str)" : ", V#", i);
  const std::string src = "import ::io;\nenum Many { " + variants.substr(2) +
                          " }\nfn name(m: Many) -> Str {\n  match m {\n" +
                          repeated("    V# { return \"V#\"; }\n", 200) +
                          "  }\n}\nfn main() -> int {\n"
                          "  io::println(name(Many::V3(1, \"q\")));\n"
                          "  return 0;\n}\n";
  const std::string path = dir.write("many.pkn", src);
  Timed t = timed(std::string(kPaykan) + " run -O0 " + path + " 2>&1");
  ASSERT_EQ(t.ExitCode, 0) << t.Out;
  EXPECT_EQ(t.Out, "V3\n");
  EXPECT_LT(t.Seconds, 10 * slowdown());
}

// The lending pass lends a call each copy of a reference the caller keeps
// alive through it, and removes the copy's retain.  It removed the retains
// from their block one at a time, each removal moving the rest of the
// block, so a function of 3000 statements each lending `reset(c)` its `c`
// (beside the copies of `p` and `c.p` the later operands make, SW-01) took
// 37 s of processor time to reach --emit-pir, 31 s before those copies.  A
// block's retains go in one pass over it now, and the function takes 2.5 s.
TEST(Scaling, TheLendingPassRemovesABlocksRetainsInOnePass) {
  ScratchDir dir("lend");
  const std::string src =
      "struct P { x: int; y: int; }\nclass C { p: P; }\n"
      "fn first(a: P, b: int) -> int => a.x + b;\n"
      "fn bump(p: inout P) -> int { p.x = p.x + 1; return 0; }\n"
      "fn reset(c: C) -> int { c.p = P(9, 9); return 0; }\n"
      "fn main() -> int {\n  p = P(1, 2);\n  c = C(P(1, 2));\n  r = 0;\n" +
      repeated("  r += first(p, bump(p)) + first(c.p, reset(c));\n", 3000) +
      "  return r % 256;\n}\n";
  const std::string path = dir.write("lend.pkn", src);
  Timed t =
      timed(std::string(kPaykan) + " --emit-pir " + path + " 2>&1 >/dev/null");
  ASSERT_EQ(t.ExitCode, 0) << t.Out;
  EXPECT_LT(t.Seconds, 15 * slowdown());
}

// The standard library's string functions find what they look for with the
// runtime's search (C's memchr and memcmp) and copy each part out once, so a
// split, the lines and the words of a string take time linear in its
// length.  A 10 MB string of a million lines, doubled up from one, is split
// at each line feed, cut into lines and into words, and searched for a needle
// it does not hold: `run -O0` takes 1.5 s of processor time, compiling
// included.  A split that searched from the start of the string for each
// field, or copied the rest of it after each, would take hours.
TEST(Scaling, StringFunctionsTakeLinearTime) {
  ScratchDir dir("strings");
  const std::string src =
      "import ::io;\nimport ::collections as c;\n"
      "fn main() -> int {\n"
      "  s = \"word word\\n\";\n"
      "  for _ in 0..20 {\n    s = s + s;\n  }\n"
      "  io::println(Str(s.len()) + \" \" + Str(c::split(s, \"\\n\").len()));\n"
      "  io::println(c::lines(s).len());\n"
      "  io::println(c::words(s).len());\n"
      "  io::println(c::contains(s, \"word\\nwore\"));\n"
      "  return 0;\n}\n";
  const std::string path = dir.write("strings.pkn", src);
  Timed t = timed(std::string(kPaykan) + " run -O0 " + path + " 2>&1");
  ASSERT_EQ(t.ExitCode, 0) << t.Out;
  EXPECT_EQ(t.Out, "10485760 1048577\n1048576\n2097152\nFalse\n");
  EXPECT_LT(t.Seconds, 20 * slowdown());
}

// join and replace put their pieces together in pairs, as a binary counter
// carries, so each byte of the result is copied about log2 of the number of
// pieces times, and repeat doubles its string.  They appended each piece
// with `concat`, which copies the whole string so far: replacing the line
// feeds of a 720 KB string took 9 s, so the million of a 10 MB string would
// take about 40 minutes, and the join of its lines over ten.  The three,
// with the split, take 2.2 s of processor time under `run -O0`, compiling
// included.
TEST(Scaling, StringFunctionsThatBuildAStringMergeItsPieces) {
  ScratchDir dir("strings_built");
  const std::string src =
      "import ::io;\nimport ::collections as c;\n"
      "fn main() -> int {\n"
      "  s = \"word word\\n\";\n"
      "  for _ in 0..20 {\n    s = s + s;\n  }\n"
      "  io::println(c::replace(s, \"\\n\", \"\\r\\n\").len());\n"
      "  io::println(c::join(c::split(s, \"\\n\"), \"\\r\\n\").len());\n"
      "  io::println(c::repeat(\"word word\\n\", 1048576) == s);\n"
      "  return 0;\n}\n";
  const std::string path = dir.write("strings.pkn", src);
  Timed t = timed(std::string(kPaykan) + " run -O0 " + path + " 2>&1");
  ASSERT_EQ(t.ExitCode, 0) << t.Out;
  EXPECT_EQ(t.Out, "11534336\n11534336\nTrue\n");
  EXPECT_LT(t.Seconds, 20 * slowdown());
}

// Each imported module's code is cached under a key made from its MLIR,
// printed (SW-18).  Printing a module nested in the program verified and
// numbered the whole program each time, so a program of 400 tiny modules
// took 19 s of processor time to print its LLVM IR from the cache, and
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
