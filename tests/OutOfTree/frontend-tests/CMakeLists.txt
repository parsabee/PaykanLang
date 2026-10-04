# The exported frontend test support, as an out-of-tree frontend uses it
# (docs/writing-a-frontend-plugin.md), run against the installation's own
# recursive-descent frontend: tests/OutOfTree/FrontendTests.cmake configures,
# builds and runs this project against a scratch install.
cmake_minimum_required(VERSION 3.24)
project(PaykanFrontendTestsOutOfTree LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Paykan REQUIRED)

# GoogleTest: FETCHCONTENT_SOURCE_DIR_GOOGLETEST, an installed GTest, or the
# pinned release.
include(FetchContent)
FetchContent_Declare(googletest
    URL      "https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz"
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    FIND_PACKAGE_ARGS 1.11 CONFIG NAMES GTest
)
set(INSTALL_GTEST OFF)
FetchContent_MakeAvailable(googletest)

enable_testing()
paykan_add_frontend_tests(recursive-descent)
