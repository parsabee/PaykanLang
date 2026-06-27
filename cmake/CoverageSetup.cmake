# CoverageSetup.cmake
# ----------------------------------------------------------------------------
# Adds an optional source-based code-coverage build configuration using
# Clang/LLVM's instrumentation (`-fprofile-instr-generate -fcoverage-mapping`),
# reported via the vendored `llvm-cov` / `llvm-profdata`.
#
# Usage:
#   cmake -B build-cov -DPAYKAN_COVERAGE=ON
#   cmake --build build-cov --parallel
#   ctest --test-dir build-cov --output-on-failure   # produces .profraw files
#   ./scripts/coverage.sh build-cov                   # merges + reports
#
# Coverage requires Clang (the instrumentation flags and llvm-cov are
# Clang/LLVM specific). The project targets ~100% line coverage of first-party
# sources; scripts/coverage.sh enforces a configurable floor.
# ----------------------------------------------------------------------------

option(PAYKAN_COVERAGE "Build with LLVM source-based code coverage" OFF)

if(PAYKAN_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "PAYKAN_COVERAGE requires Clang (got ${CMAKE_CXX_COMPILER_ID}). "
            "Configure with the vendored clang, e.g. "
            "-DCMAKE_CXX_COMPILER=<build>/third-party/llvm/bin/clang++")
    endif()
    message(STATUS "Coverage: LLVM source-based coverage enabled")
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping -O0 -g)
    add_link_options(-fprofile-instr-generate -fcoverage-mapping)
endif()
