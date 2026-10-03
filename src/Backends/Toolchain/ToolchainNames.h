// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Centralized string constants of the native toolchain: the C compiler /
// linker command line (Toolchain.cpp, and the C backend's CBuild.cpp) and
// the runtime files it looks for.

#pragma once

namespace paykan::toolchain::tcnames {

// -- The C compiler / linker --------------------------------------------------

inline constexpr const char *kEnvCC = "CC";     ///< overrides the compiler
inline constexpr const char *kDefaultCC = "cc"; ///< when $CC is unset
inline constexpr const char *kFlagStd = "-std=c11";
inline constexpr const char *kFlagInclude = "-I"; ///< -I<dir>
inline constexpr const char *kFlagCompileOnly = "-c";
inline constexpr const char *kFlagOutput = "-o";
inline constexpr const char *kFlagLibm = "-lm";
inline constexpr const char *kCExt = ".c";
inline constexpr const char *kObjExt = ".o";

// -- The Paykan runtime -------------------------------------------------------

/// The runtime header the generated C includes (CNames.h's kRuntimeH).
inline constexpr const char *kRuntimeHeader = "Runtime.h";
/// The runtime archive, under `<prefix>/lib`.
inline constexpr const char *kRuntimeLib = "libpaykan_runtime.a";
/// Explicit runtime location: `$PAYKAN_RUNTIME_DIR/{lib,include/paykan}`.
inline constexpr const char *kEnvRuntimeDir = "PAYKAN_RUNTIME_DIR";
/// The file at the top of a Paykan build tree (written by
/// src/Backends/Toolchain/CMakeLists.txt): a binary below it uses that build
/// tree's runtime.
inline constexpr const char *kBuildTreeMarker = ".paykan-build-tree";

} // namespace paykan::toolchain::tcnames
