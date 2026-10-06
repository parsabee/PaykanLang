# Provides GoogleTest for the test suite.  Included only when
# a selected test suite uses it (cmake/PaykanTests.cmake).
#
# Resolution order:
#   1. FETCHCONTENT_SOURCE_DIR_GOOGLETEST=<dir>: build GoogleTest from that
#      local source tree (no download, no find_package).
#   2. find_package(GTest ${GTEST_MIN_VERSION} CONFIG): an installed
#      GoogleTest (a distro package, Homebrew, a CMAKE_PREFIX_PATH entry), so
#      an offline machine with GoogleTest installed needs no network.
#      Set FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER to skip this step.
#   3. Download the pinned release with FetchContent into ${GTEST_INSTALL_DIR}
#      (once; later configures reuse it).
#
# Provides targets:  gtest, gtest_main, gmock, gmock_main

include(FetchContent)

# Oldest release whose CMake package exports the GTest:: targets used below.
set(GTEST_MIN_VERSION "1.11")

FetchContent_Declare(googletest
    URL                        "https://github.com/google/googletest/archive/refs/tags/v${GTEST_VERSION}.tar.gz"
    URL_HASH                   SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    SOURCE_DIR                 ${GTEST_INSTALL_DIR}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    FIND_PACKAGE_ARGS          ${GTEST_MIN_VERSION} CONFIG QUIET NAMES GTest
)
set(INSTALL_GTEST OFF)          # Don't install GTest into the project prefix
set(BUILD_GMOCK   ON)           # Build GMock alongside GTest
if(NOT FETCHCONTENT_SOURCE_DIR_GOOGLETEST AND NOT EXISTS ${GTEST_INSTALL_DIR})
    message(STATUS "Looking for GTest >= ${GTEST_MIN_VERSION}, else fetching GTest ${GTEST_VERSION}...")
endif()
FetchContent_MakeAvailable(googletest)

# find_package() ran inside FetchContent_MakeAvailable's function scope, so
# GTest_FOUND is not visible here; the targets (directory-scoped) are.  A
# fetched GoogleTest defines plain gtest_main; an installed one only the
# namespaced, imported GTest::gtest_main.
if(NOT TARGET gtest_main AND TARGET GTest::gtest_main)
    # Give the installed targets the plain names the tests link against.
    message(STATUS "Using installed GTest (${googletest_DIR})")
    foreach(target gtest gtest_main gmock gmock_main)
        if(NOT TARGET ${target} AND TARGET GTest::${target})
            add_library(${target} ALIAS GTest::${target})
        endif()
    endforeach()
else()
    # GTest triggers Clang's -Wcovered-switch-default; silence it for third-party code.
    foreach(target gtest gtest_main gmock gmock_main)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE -Wno-covered-switch-default)
        endif()
    endforeach()
endif()
