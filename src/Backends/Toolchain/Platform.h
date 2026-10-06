// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The compiler's portability layer: every operating-system API the
// compiler uses beyond standard C++ lives behind these few functions,
// implemented side by side for each supported platform in Platform.cpp:
//   - for the backends that produce native programs: locating the running
//     executable, spawning the C compiler / linker and the built program,
//     creating a temporary directory, the process id (Toolchain.cpp and the
//     C backend's CBuild.cpp are the users);
//   - for the plugin loader (src/PluginHost): loading a shared library and
//     looking up a symbol in it (the platform's dynamic loader).
//
// Supported: Linux and macOS.  Other POSIX systems build too, with the
// documented fallbacks noted on each function.  Windows is not supported;
// the dynamic-loading functions have a stub there that fails with a message.

#pragma once

#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace paykan::toolchain::platform {

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

/// Load the shared library @p path, resolving all its symbols now and
/// keeping them local to it (they never satisfy another library's
/// references).  Returns an opaque handle, or null with the loader's message
/// in @p error.  The library stays loaded until the process exits; there is
/// no unload.  Loading runs the library's global constructors (and those of
/// the libraries it depends on): nothing else in it runs until the caller
/// calls one of its functions.
///   POSIX: dlopen(path, RTLD_NOW | RTLD_LOCAL); the message is dlerror().
///   On Linux an ELF file whose loadable segments extend past its end (a
///   truncated file, which dlopen would map and crash on with SIGBUS) is
///   refused first.
///   Windows: not supported yet; always fails.
void *loadLibrary(const std::string &path, std::string &error);

/// The address of the exported symbol @p name in a library returned by
/// loadLibrary, or null.
///   POSIX: dlsym.  Windows: not supported yet; always null.
void *librarySymbol(void *library, const char *name);

/// The file name suffix of a loadable plugin on this platform: ".so" on
/// Linux and other POSIX systems, ".dylib" on macOS, ".dll" on Windows.
const char *sharedLibrarySuffix();

} // namespace paykan::toolchain::platform
