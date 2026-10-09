// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The system toolchain shared by the native backends (C and llvm): runtime
// lookup, process spawning and linking.

#include "paykan/backends/Toolchain.h"

#include "Platform.h"
#include "ToolchainNames.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#ifndef PAYKAN_BUILD_TREE_DIR
#define PAYKAN_BUILD_TREE_DIR ""
#endif
#ifndef PAYKAN_RUNTIME_LIB_PATH
#define PAYKAN_RUNTIME_LIB_PATH ""
#endif
#ifndef PAYKAN_RUNTIME_INCLUDE_DIR
#define PAYKAN_RUNTIME_INCLUDE_DIR ""
#endif
#ifndef PAYKAN_INSTALLED_RUNTIME_LIB
#define PAYKAN_INSTALLED_RUNTIME_LIB ""
#endif
#ifndef PAYKAN_INSTALLED_RUNTIME_INCLUDE_DIR
#define PAYKAN_INSTALLED_RUNTIME_INCLUDE_DIR ""
#endif
#ifndef PAYKAN_SANITIZER_FLAGS
#define PAYKAN_SANITIZER_FLAGS ""
#endif
#ifndef PAYKAN_COVERAGE_FLAGS
#define PAYKAN_COVERAGE_FLAGS ""
#endif
#ifndef PAYKAN_RUNTIME_TARGET_FLAGS
#define PAYKAN_RUNTIME_TARGET_FLAGS ""
#endif
#ifndef PAYKAN_DEFAULT_CC
#define PAYKAN_DEFAULT_CC ""
#endif
#ifndef PAYKAN_DEFAULT_CC_FLAGS
#define PAYKAN_DEFAULT_CC_FLAGS ""
#endif

namespace paykan::toolchain {

namespace fs = std::filesystem;

namespace {

/// Append the space-separated words of @p flags to @p out.
void appendFlags(const std::string &flags, std::vector<std::string> &out) {
  std::istringstream words(flags);
  std::string f;
  while (words >> f)
    out.push_back(f);
}

/// The runtime recorded by setPackageRuntime().
std::pair<std::string, std::string> &packageRuntime() {
  static std::pair<std::string, std::string> runtime;
  return runtime;
}

/// Whether @p exeDir lies inside the build tree this code was compiled in:
/// the nearest enclosing directory that holds the build-tree marker is that
/// build tree.  An installed binary is not below a marker, so a build tree
/// that merely still exists (perhaps stale) is not used for it.
bool insideBuildTree(const std::string &exeDir) {
  const std::string buildDir = PAYKAN_BUILD_TREE_DIR;
  if (exeDir.empty() || buildDir.empty())
    return false;
  std::error_code ec;
  for (fs::path dir = exeDir; !dir.empty(); dir = dir.parent_path()) {
    if (fs::exists(dir / tcnames::kBuildTreeMarker, ec))
      return fs::equivalent(dir, buildDir, ec);
    if (dir == dir.root_path())
      break;
  }
  return false;
}

} // namespace

void setPackageRuntime(std::string lib, std::string includeDir) {
  packageRuntime() = {std::move(lib), std::move(includeDir)};
}

/// Directory of the running executable, or "".
std::string executableDir() {
  std::string exe = platform::executablePath();
  return exe.empty() ? "" : fs::path(exe).parent_path().string();
}

/// Run @p path with @p args after @p argv0 as the child's argv (no argv at
/// all when @p argv0 is null, i.e. argc == 0) and @p extraEnv added to the
/// environment.  Returns the exit status, 128 + signal, or -1.
int spawn(const std::string &path, const std::vector<std::string> &args,
          const std::string *argv0,
          const std::vector<std::pair<std::string, std::string>> &extraEnv,
          std::ostream &errs) {
  return platform::spawn(path, args, argv0, extraEnv, errs);
}

TempDir::TempDir() : Path(platform::makeTempDir("paykan-c-")) {}

TempDir::~TempDir() {
  if (!Path.empty()) {
    std::error_code ec;
    fs::remove_all(Path, ec);
  }
}

bool resolveToolchain(Toolchain &tc, std::ostream &errs) {
  if (tc.CC.empty()) {
    const char *cc = std::getenv(tcnames::kEnvCC);
    // A coverage build names the compiler that built the runtime: its
    // profile runtime matches the archive's instrumentation.  It comes with
    // the flags it needs to find the system headers and libraries (the
    // macOS SDK for a clang that has no default one).
    const char *buildCC = PAYKAN_DEFAULT_CC;
    if (cc && cc[0]) {
      tc.CC = cc;
    } else if (buildCC[0]) {
      tc.CC = buildCC;
      appendFlags(PAYKAN_DEFAULT_CC_FLAGS, tc.ExtraFlags);
    } else {
      tc.CC = tcnames::kDefaultCC;
    }
  }
  // The build tree's runtime archive is sanitizer- or coverage-instrumented
  // when the compiler was built that way, and on macOS built for the build's
  // deployment target: programs linked against it need the same flags (the C
  // backend's compile and every backend's link).
  if (tc.RuntimeLib.empty()) {
    appendFlags(PAYKAN_SANITIZER_FLAGS, tc.ExtraFlags);
    appendFlags(PAYKAN_COVERAGE_FLAGS, tc.ExtraFlags);
    appendFlags(PAYKAN_RUNTIME_TARGET_FLAGS, tc.ExtraFlags);
  }
  auto exists = [](const std::string &p) {
    std::error_code ec;
    return !p.empty() && fs::exists(p, ec);
  };
  if (tc.RuntimeLib.empty() || tc.RuntimeIncludeDir.empty()) {
    std::vector<std::pair<std::string, std::string>> candidates;
    // 1. $PAYKAN_RUNTIME_DIR/{lib,include/paykan}: the explicit override
    if (const char *env = std::getenv(tcnames::kEnvRuntimeDir); env && env[0])
      candidates.emplace_back(
          (fs::path(env) / "lib" / tcnames::kRuntimeLib).string(),
          (fs::path(env) / "include" / "paykan").string());
    // 2. the runtime of the installed package an out-of-tree driver was
    //    built against (paykan_add_driver)
    if (const auto &[lib, inc] = packageRuntime(); !lib.empty())
      candidates.emplace_back(lib, inc);
    // 3. the install layout around the executable (an installed, possibly
    //    relocated, binary: <prefix>/bin/paykan)
    std::string exeDir = executableDir();
    if (!exeDir.empty()) {
      fs::path prefix = fs::path(exeDir).parent_path();
      candidates.emplace_back((prefix / "lib" / tcnames::kRuntimeLib).string(),
                              (prefix / "include" / "paykan").string());
    }
    // 4. the build tree this compiler was built in, for a binary inside it
    if (insideBuildTree(exeDir))
      candidates.emplace_back(PAYKAN_RUNTIME_LIB_PATH,
                              PAYKAN_RUNTIME_INCLUDE_DIR);
    // 5. the install location configured at build time
    candidates.emplace_back(PAYKAN_INSTALLED_RUNTIME_LIB,
                            PAYKAN_INSTALLED_RUNTIME_INCLUDE_DIR);
    for (const auto &[lib, inc] : candidates) {
      if (exists(lib) && exists(inc + "/" + tcnames::kRuntimeHeader)) {
        if (tc.RuntimeLib.empty())
          tc.RuntimeLib = lib;
        if (tc.RuntimeIncludeDir.empty())
          tc.RuntimeIncludeDir = inc;
        break;
      }
    }
  }
  if (!exists(tc.RuntimeLib) ||
      !exists(tc.RuntimeIncludeDir + "/" + tcnames::kRuntimeHeader)) {
    errs << "cannot find the Paykan runtime (libpaykan_runtime.a and "
            "Runtime.h); set "
         << tcnames::kEnvRuntimeDir << "\n";
    return false;
  }
  return true;
}

bool linkExecutable(const std::vector<std::string> &objects,
                    const std::string &outputPath, const Toolchain &tc,
                    std::ostream &errs) {
  std::vector<std::string> link = objects;
  for (const auto &f : tc.ExtraFlags)
    link.push_back(f);
  link.push_back(tc.RuntimeLib);
  link.push_back(tcnames::kFlagLibm);
  link.push_back(tcnames::kFlagOutput);
  link.push_back(outputPath);
  int rc = spawn(tc.CC, link, &tc.CC, {}, errs);
  if (rc != 0) {
    errs << "linking failed (" << tc.CC << " exited with " << rc << ")\n";
    return false;
  }
  return true;
}

} // namespace paykan::toolchain
