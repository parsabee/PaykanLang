cmake_minimum_required(VERSION 3.24)

project(PaykanLang
    VERSION 0.1.0
    DESCRIPTION "A clean and simple, statically typed, object-oriented programming language"
    LANGUAGES C CXX
)

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

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# -- Third-party dependency versions ------------------------------------------
# Pinned versions; hashes are verified in each Setup module.
set(LLVM_VERSION  "17.0.6")
set(BISON_VERSION "3.8")
set(FLEX_VERSION  "2.6.4")
set(GTEST_VERSION "1.15.2")

# All third-party artifacts live under <build>/third-party/.
set(THIRD_PARTY_DIR  ${CMAKE_BINARY_DIR}/third-party)
set(LLVM_INSTALL_DIR ${THIRD_PARTY_DIR}/llvm)
set(BISON_INSTALL_DIR ${THIRD_PARTY_DIR}/bison)
set(FLEX_INSTALL_DIR  ${THIRD_PARTY_DIR}/flex)
set(GTEST_INSTALL_DIR ${THIRD_PARTY_DIR}/gtest)

# -- Compiler gate ------------------------------------------------------------
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. Only Clang and GCC are supported.")
endif()

# -- Third-party setup --------------------------------------------------------
# Each module downloads/builds its dependency on first configure and then
# reuses the cached result on subsequent runs.  See cmake/ for details.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(LLVMSetup)          # → paykan_llvm       INTERFACE target
include(BisonFlexSetup)     # → bison_ext, flex_ext ExternalProject targets
include(GTestSetup)         # → gtest, gmock       via FetchContent

enable_testing()

# -- Project compile options --------------------------------------------------
# INTERFACE library that carries warnings, RTTI/exception policy, and
# per-config optimisation flags.  Link to every first-party target:
#   target_link_libraries(<target> PRIVATE paykan_compile_options)
add_library(paykan_compile_options INTERFACE)
target_compile_options(paykan_compile_options INTERFACE
    -Wall -Wextra -Wpedantic
    -fno-rtti -fno-exceptions
)
target_compile_options(paykan_compile_options INTERFACE
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3>
)
target_compile_definitions(paykan_compile_options INTERFACE
    $<$<CONFIG:Release>:NDEBUG>
)

# -- Subdirectories -----------------------------------------------------------
add_subdirectory(src)
add_subdirectory(tests)
