# PaykanTestSupport.cmake
# ----------------------------------------------------------------------------
# Installs the test support for out-of-tree frontends, so that a frontend
# plugin built against an installed Paykan runs the same suites as the
# in-tree frontend (docs/writing-a-frontend.md):
#
#   share/paykan/frontend-tests/      TestUtils.h, Parser/, Sema/ and
#                                     Frontend/{FuzzSmoke,Differential}Tests.cpp
#   share/paykan/samples/             the samples corpus
#   lib/cmake/Paykan/PaykanFrontendTests.cmake
#                                     paykan_add_frontend_tests(), included
#                                     by PaykanConfig.cmake
#
# Sources only: nothing is compiled here, and GoogleTest is the consumer's.
# -DPAYKAN_INSTALL_TEST_SUPPORT=OFF (the option is in the top-level
# CMakeLists.txt) leaves all of it out.  Included after PaykanExport
# (PAYKAN_INSTALL_CMAKEDIR).
# ----------------------------------------------------------------------------

if(NOT PAYKAN_INSTALL_TEST_SUPPORT)
    return()
endif()

set(PAYKAN_INSTALL_TEST_SUPPORT_DIR ${CMAKE_INSTALL_DATADIR}/paykan/frontend-tests)
set(PAYKAN_INSTALL_SAMPLES_DIR ${CMAKE_INSTALL_DATADIR}/paykan/samples)

install(FILES ${PROJECT_SOURCE_DIR}/tests/TestUtils.h
    DESTINATION ${PAYKAN_INSTALL_TEST_SUPPORT_DIR})
install(DIRECTORY ${PROJECT_SOURCE_DIR}/tests/Parser ${PROJECT_SOURCE_DIR}/tests/Sema
    DESTINATION ${PAYKAN_INSTALL_TEST_SUPPORT_DIR}
    FILES_MATCHING PATTERN "*.cpp")
install(FILES ${PROJECT_SOURCE_DIR}/tests/Frontend/FuzzSmokeTests.cpp
              ${PROJECT_SOURCE_DIR}/tests/Frontend/DifferentialTests.cpp
    DESTINATION ${PAYKAN_INSTALL_TEST_SUPPORT_DIR}/Frontend)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/samples/
    DESTINATION ${PAYKAN_INSTALL_SAMPLES_DIR}
    PATTERN ".paykan_cache" EXCLUDE)

# The installed module finds both directories relative to itself, wherever
# the installation is moved.
file(RELATIVE_PATH PAYKAN_TEST_SUPPORT_RELDIR
    "/${PAYKAN_INSTALL_CMAKEDIR}" "/${PAYKAN_INSTALL_TEST_SUPPORT_DIR}")
file(RELATIVE_PATH PAYKAN_SAMPLES_RELDIR
    "/${PAYKAN_INSTALL_CMAKEDIR}" "/${PAYKAN_INSTALL_SAMPLES_DIR}")
configure_file(${PROJECT_SOURCE_DIR}/cmake/PaykanFrontendTests.cmake.in
    ${CMAKE_BINARY_DIR}/cmake/PaykanFrontendTests.cmake @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/cmake/PaykanFrontendTests.cmake
    DESTINATION ${PAYKAN_INSTALL_CMAKEDIR})
