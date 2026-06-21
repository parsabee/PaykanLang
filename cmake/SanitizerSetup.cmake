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
    message(STATUS "Sanitizer: AddressSanitizer + LeakSanitizer enabled")
    add_compile_options(-fsanitize=address,leak -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,leak)
    # Death tests under ASan need threadsafe mode (fork-based death tests are
    # unreliable under ASan because the child inherits shadow memory state).
    add_compile_definitions(GTEST_HAS_DEATH_TEST=1)
endif()

if(PAYKAN_UBSAN)
    message(STATUS "Sanitizer: UndefinedBehaviorSanitizer enabled")
    add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all)
    add_link_options(-fsanitize=undefined)
endif()

if(PAYKAN_TSAN)
    message(STATUS "Sanitizer: ThreadSanitizer enabled")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()
