# GTestSetup.cmake - Fetches and configures Google Test

include(FetchContent)

if(NOT EXISTS ${GTEST_INSTALL_DIR})
    message(STATUS "Fetching GTest ${GTEST_VERSION}...")
endif()

FetchContent_Declare(googletest
    URL                        "https://github.com/google/googletest/archive/refs/tags/v${GTEST_VERSION}.tar.gz"
    SOURCE_DIR                 ${GTEST_INSTALL_DIR}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(INSTALL_GTEST OFF)
set(BUILD_GMOCK   ON)
FetchContent_MakeAvailable(googletest)

# Suppress warnings in GTest's own source files
foreach(target gtest gtest_main gmock gmock_main)
    if(TARGET ${target})
        target_compile_options(${target} PRIVATE -Wno-covered-switch-default)
    endif()
endforeach()
