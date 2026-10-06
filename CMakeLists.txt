cmake_minimum_required(VERSION 3.24)

project(PaykanLang
    VERSION 0.1.0
    DESCRIPTION "A performant, productive, statically-typed, object-oriented programming language."
    LANGUAGES C CXX
)

# The version's single source of truth: project()'s VERSION above plus this
# pre-release label make PAYKAN_VERSION (below), and everything else (Version.h,
# the plugin headers, the accepted plugin list, the plugin directory, the CMake
# package version, `paykan --version`, the release workflow's tag check) derives
# from it. Bumping the version is a change to these two lines only.
#
# Pre-release label appended to the dotted version as "-<label>" (a label
# "rc1" makes 1.2.3 into "1.2.3-rc1"). Empty for a final release, whose
# version is the plain dotted one.
set(PAYKAN_VERSION_PRERELEASE "")

# -- Language standard --------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)           # Strict ISO C++, no GNU extensions

# -- Build defaults -----------------------------------------------------------
# Prevents an empty CMAKE_BUILD_TYPE (which applies no optimisation flags).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "No build type specified, defaulting to Debug")
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # compile_commands.json for clangd/LSP
# Position-independent code everywhere, so a plugin module may link the
# installed static libraries into itself (a C++ frontend that reuses the
# core's AST and interchange writer internally, say: tests/OutOfTree/
# frontend-tests/RDPlugin.cpp).  Plugins never link them into paykan.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# -- Third-party dependency versions ------------------------------------------
# Pinned versions; hashes are verified in each Setup module.
set(LLVM_VERSION  "17.0.6")
set(GTEST_VERSION "1.15.2")

# All third-party artifacts live under <build>/third-party/.
set(THIRD_PARTY_DIR  ${CMAKE_BINARY_DIR}/third-party)
set(LLVM_INSTALL_DIR ${THIRD_PARTY_DIR}/llvm)
set(GTEST_INSTALL_DIR ${THIRD_PARTY_DIR}/gtest)

# -- Compiler gate ------------------------------------------------------------
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. Only Clang and GCC are supported.")
endif()

# -- Third-party setup --------------------------------------------------------
# Each module downloads/builds its dependency on first configure and then
# reuses the cached result on subsequent runs.  See cmake/ for details.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(SanitizerSetup)     # -> PAYKAN_ASAN / PAYKAN_UBSAN / PAYKAN_TSAN options
include(CoverageSetup)      # -> PAYKAN_COVERAGE option (LLVM source coverage)
include(PaykanPlugins)      # -> PAYKAN_FRONTENDS / PAYKAN_BACKENDS, plugin helpers
if(PAYKAN_NEEDS_LLVM)
    include(LLVMSetup)      # -> paykan_llvm       INTERFACE target
endif()

# The test suite (GoogleTest) is off by default, so a plain configure builds
# only the compiler and downloads nothing.  -DPAYKAN_BUILD_TESTS=ON builds it
# (and fetches GoogleTest when no installed one is found).
option(PAYKAN_BUILD_TESTS "Build the test suite (needs GoogleTest)" OFF)
# The frontend test suites and the samples corpus, installed as sources for
# out-of-tree frontends (cmake/PaykanTestSupport.cmake).
option(PAYKAN_INSTALL_TEST_SUPPORT
    "Install the frontend test suites and the samples corpus for out-of-tree frontends" ON)
if(PAYKAN_BUILD_TESTS)
    include(GTestSetup)     # -> gtest, gmock       installed or via FetchContent
    enable_testing()
endif()

# -- Project compile options --------------------------------------------------
# INTERFACE library that carries warnings, RTTI/exception policy, and
# per-config optimisation flags.  Link to every first-party target:
#   target_link_libraries(<target> PRIVATE paykan_compile_options)
add_library(paykan_compile_options INTERFACE)
target_compile_options(paykan_compile_options INTERFACE
    -Wall -Wextra -Wpedantic -Werror
    -fno-rtti -fno-exceptions
)
target_compile_options(paykan_compile_options INTERFACE
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3>
)
target_compile_definitions(paykan_compile_options INTERFACE
    $<$<CONFIG:Release>:NDEBUG>
)

# -- Generated version header -------------------------------------------------
# Single source of truth: project(... VERSION ...) above is expanded into
# Version.h via the template in include/Version.h.in.  The generated header is
# placed on the include path of every first-party target through the shared
# compile-options interface library.
set(PAYKAN_VERSION       ${PROJECT_VERSION})
set(PAYKAN_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(PAYKAN_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(PAYKAN_VERSION_PATCH ${PROJECT_VERSION_PATCH})
if(PAYKAN_VERSION_PRERELEASE)
    set(PAYKAN_VERSION "${PAYKAN_VERSION}-${PAYKAN_VERSION_PRERELEASE}")
endif()
set(PAYKAN_GENERATED_INCLUDE_DIR ${CMAKE_BINARY_DIR}/generated/include)
configure_file(
    ${CMAKE_SOURCE_DIR}/include/Version.h.in
    ${PAYKAN_GENERATED_INCLUDE_DIR}/Version.h
    @ONLY
)
# The plugin compatibility list (cmake/PluginCompat.cmake) and the version a
# plugin records as the one it was built with, for the plugin registry
# (include/paykan/Registry.h) and every plugin compiled against these headers.
include(PluginCompat)
configure_file(
    ${CMAKE_SOURCE_DIR}/include/paykan/PluginCompat.h.in
    ${PAYKAN_GENERATED_INCLUDE_DIR}/paykan/PluginCompat.h
    @ONLY
)
# The C plugin interface's version (paykan/plugin_api.h includes it).
configure_file(
    ${CMAKE_SOURCE_DIR}/include/paykan/plugin_api_version.h.in
    ${PAYKAN_GENERATED_INCLUDE_DIR}/paykan/plugin_api_version.h
    @ONLY
)
target_include_directories(paykan_compile_options INTERFACE
    $<BUILD_INTERFACE:${PAYKAN_GENERATED_INCLUDE_DIR}>)

# -- Subdirectories -----------------------------------------------------------
add_subdirectory(src)
if(PAYKAN_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# -- Install rules and the find_package(Paykan) package -----------------------
include(PaykanExport)
include(PaykanTestSupport)  # -> the frontend test suites for out-of-tree frontends
include(Packaging)         # -> CPack: the Debian package (cpack -G DEB)
