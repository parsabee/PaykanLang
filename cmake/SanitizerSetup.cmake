# SanitizerSetup.cmake
# ----------------------------------------------------------------------------
# Adds optional sanitizer build configurations.
# Include this module in the root CMakeLists.txt after the compiler gate.
#
# Usage:
#   cmake -B build-asan  -DPAYKAN_ASAN=ON
#   cmake -B build-ubsan -DPAYKAN_UBSAN=ON
#   cmake -B build-tsan  -DPAYKAN_TSAN=ON
#
# Note: ASan and TSan are mutually exclusive — do not combine them.
# UBSan can be combined with either: -DPAYKAN_ASAN=ON -DPAYKAN_UBSAN=ON
# ----------------------------------------------------------------------------

option(PAYKAN_ASAN  "Build with AddressSanitizer + LeakSanitizer" OFF)
option(PAYKAN_UBSAN "Build with UndefinedBehaviorSanitizer"       OFF)
option(PAYKAN_TSAN  "Build with ThreadSanitizer"                  OFF)

if(PAYKAN_ASAN AND PAYKAN_TSAN)
    message(FATAL_ERROR "PAYKAN_ASAN and PAYKAN_TSAN are mutually exclusive.")
endif()

if(PAYKAN_ASAN)
    # Apple's clang has no LeakSanitizer on arm64 ("unsupported option
    # '-fsanitize=leak' for target 'arm64-apple-darwin'"), so on that
    # toolchain the build is ASan only; everywhere else ASan + LSan.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        set(_paykan_asan_flags -fsanitize=address)
        message(STATUS "Sanitizer: AddressSanitizer enabled (LeakSanitizer is "
                       "not available with Apple clang on this platform)")
    else()
        set(_paykan_asan_flags -fsanitize=address,leak)
        message(STATUS "Sanitizer: AddressSanitizer + LeakSanitizer enabled")
    endif()
    # Run tests with ASAN_OPTIONS=detect_container_overflow=0: the prebuilt
    # LLVM is not instrumented, and mixing it with instrumented libc++
    # containers yields false container-overflow reports (see ci.yml).
    add_compile_options(${_paykan_asan_flags} -fno-omit-frame-pointer)
    add_link_options(${_paykan_asan_flags})
    # Death tests under ASan need threadsafe mode (fork-based death tests are
    # unreliable under ASan because the child inherits shadow memory state).
    add_compile_definitions(GTEST_HAS_DEATH_TEST=1)
endif()

if(PAYKAN_UBSAN)
    message(STATUS "Sanitizer: UndefinedBehaviorSanitizer enabled")
    # The project (and the LLVM it links) is built with -fno-rtti, so UBSan's
    # vptr check has no type information to work with and reports every
    # virtual call on a googletest fixture as "does not point to an object of
    # type ..." -- a false positive that failed the whole suite.
    add_compile_options(-fsanitize=undefined -fno-sanitize=vptr
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=undefined -fno-sanitize=vptr)
endif()

if(PAYKAN_TSAN)
    message(STATUS "Sanitizer: ThreadSanitizer enabled")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()
