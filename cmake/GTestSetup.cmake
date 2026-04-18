# GTestSetup.cmake
# ----------------------------------------------------------------------------
# Fetches Google Test via FetchContent (CMake-native, integrates directly
# into the build graph).  Downloaded once into ${GTEST_INSTALL_DIR}.
#
# Provides targets:  gtest, gtest_main, gmock, gmock_main
# ----------------------------------------------------------------------------

include(FetchContent)

if(NOT EXISTS ${GTEST_INSTALL_DIR})
    message(STATUS "Fetching GTest ${GTEST_VERSION}...")
endif()

FetchContent_Declare(googletest
    URL                        "https://github.com/google/googletest/archive/refs/tags/v${GTEST_VERSION}.tar.gz"
    URL_HASH                   SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    SOURCE_DIR                 ${GTEST_INSTALL_DIR}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(INSTALL_GTEST OFF)          # Don't install GTest into the project prefix
set(BUILD_GMOCK   ON)           # Build GMock alongside GTest
FetchContent_MakeAvailable(googletest)

# GTest triggers Clang's -Wcovered-switch-default; silence it for third-party code.
foreach(target gtest gtest_main gmock gmock_main)
    if(TARGET ${target})
        target_compile_options(${target} PRIVATE -Wno-covered-switch-default)
    endif()
endforeach()
