// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The C backend's portability layer: every operating-system API the backend
// uses (locating the running executable, spawning the C compiler and the
// built program, creating a temporary directory, the process id) lives
// behind these few functions, implemented side by side for each supported
// platform in Platform.cpp.
//
// Supported: Linux and macOS.  Other POSIX systems build too, with the
// documented fallbacks noted on each function.

#pragma once

#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace paykan::backend_c::platform {

/// Absolute path of the running executable, or "" when it cannot be found.
///   macOS: _NSGetExecutablePath, resolved with realpath.
///   Linux: the /proc/self/exe symlink.
///   Other POSIX systems (or when the OS query fails): the first executable
///   `paykan` on $PATH.
std::string executablePath();

/// Run @p path (looked up on $PATH when it has no slash) with @p args after
/// @p argv0 as the child's argv (no argv at all when @p argv0 is null, i.e.
/// argc == 0) and @p extraEnv added to its environment, and wait for it.
/// Returns the exit status, 128 + the signal number, or -1 (with a message
/// on @p errs) when it could not be run.  POSIX fork / execvp / waitpid on
/// every platform.
int spawn(const std::string &path, const std::vector<std::string> &args,
          const std::string *argv0,
          const std::vector<std::pair<std::string, std::string>> &extraEnv,
          std::ostream &errs);

/// Create a fresh, private directory named @p prefix plus a unique suffix in
/// the system temporary directory; returns its path, or "" on failure.
/// POSIX mkdtemp on every platform.
std::string makeTempDir(const std::string &prefix);

/// The id of the running process (POSIX getpid on every platform).
unsigned long processId();

} // namespace paykan::backend_c::platform
