# src/Backends/Toolchain/CMakeLists.txt
# ----------------------------------------------------------------------------
# paykan_backend_toolchain -- the system toolchain shared by the native
# backends (include/paykan/backends/Toolchain.h): runtime lookup, process
# spawning and linking against libpaykan_runtime.a.  The C backend compiles
# and links with it; the llvm backend links the objects it emits with it.
# Standard C++ plus POSIX, every OS call behind the portability layer in
# Platform.h (Linux and macOS side by side).
# ----------------------------------------------------------------------------

add_library(paykan_backend_toolchain STATIC Toolchain.cpp Platform.cpp)
target_include_directories(paykan_backend_toolchain
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)
# Where this build tree's runtime lives, so `paykan build` / `run` work
# without an install step, and where `cmake --install` puts it; the install
# layout next to the executable is searched in between at run time.
# A sanitizer or coverage build instruments libpaykan_runtime.a, so programs
# linked against it need the same flags (SanitizerSetup.cmake,
# CoverageSetup.cmake) whichever backend builds them.  Coverage flags are
# Clang's, and the profile runtime must match the instrumentation, so a
# coverage build also compiles / links programs with the C compiler that
# built the runtime (unless $CC says otherwise).  That compiler may be a clang
# without a default SDK on macOS (the vendored one): it is handed the SDK this
# build used, as CMake does for the build.
include(GNUInstallDirs)
list(JOIN PAYKAN_SANITIZER_FLAGS " " _paykan_toolchain_sanitizer_flags)
list(JOIN PAYKAN_COVERAGE_FLAGS " " _paykan_toolchain_coverage_flags)
set(_paykan_toolchain_default_cc "")
set(_paykan_toolchain_default_cc_flags "")
if(PAYKAN_COVERAGE_FLAGS)
    set(_paykan_toolchain_default_cc "${CMAKE_C_COMPILER}")
    if(APPLE)
        set(_paykan_toolchain_sdk "${CMAKE_OSX_SYSROOT}")
        if(NOT IS_ABSOLUTE "${_paykan_toolchain_sdk}")
            # Unset, or an SDK name rather than a path.
            execute_process(COMMAND xcrun --show-sdk-path
                OUTPUT_VARIABLE _paykan_toolchain_sdk
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
        endif()
        if(IS_ABSOLUTE "${_paykan_toolchain_sdk}")
            set(_paykan_toolchain_default_cc_flags
                "-isysroot${_paykan_toolchain_sdk}")
        endif()
    endif()
endif()
target_compile_definitions(paykan_backend_toolchain PRIVATE
    PAYKAN_RUNTIME_LIB_PATH="$<TARGET_FILE:paykan_runtime>"
    PAYKAN_RUNTIME_INCLUDE_DIR="${CMAKE_SOURCE_DIR}/src/Runtime"
    PAYKAN_INSTALLED_RUNTIME_LIB="${CMAKE_INSTALL_FULL_LIBDIR}/$<TARGET_FILE_NAME:paykan_runtime>"
    PAYKAN_INSTALLED_RUNTIME_INCLUDE_DIR="${CMAKE_INSTALL_FULL_INCLUDEDIR}/paykan"
    PAYKAN_SANITIZER_FLAGS="${_paykan_toolchain_sanitizer_flags}"
    PAYKAN_COVERAGE_FLAGS="${_paykan_toolchain_coverage_flags}"
    PAYKAN_DEFAULT_CC="${_paykan_toolchain_default_cc}"
    PAYKAN_DEFAULT_CC_FLAGS="${_paykan_toolchain_default_cc_flags}"
)
target_link_libraries(paykan_backend_toolchain
    PRIVATE paykan_compile_options
)
add_dependencies(paykan_backend_toolchain paykan_runtime)
