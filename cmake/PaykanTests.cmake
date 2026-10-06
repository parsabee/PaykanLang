# PaykanTests.cmake
# ----------------------------------------------------------------------------
# Which test suites the build includes.  None by default: a plain configure
# builds only the compiler and downloads nothing.
#
#   -DPAYKAN_BUILD_TESTS="parser;sema"   the named suites (a CMake list)
#   -DPAYKAN_BUILD_ALL_TESTS=ON          every suite
#
# The suites are declared in tests/CMakeLists.txt, one paykan_test_suite()
# section each.  scripts/gen_test_suites.py generates tests/suites.json from
# that (each suite, whether it is built on GoogleTest, its tests) and from the
# src/ link graph (the rules scripts/affected_tests.py uses to pick the suites
# a change can affect; CI builds only those).  This file reads the suites
# from tests/suites.json, because it needs them before tests/CMakeLists.txt
# runs; configure fails if the two disagree (regenerate the file).
#
# Sets:
#   PAYKAN_TEST_SUITES            every suite name (the file's order)
#   PAYKAN_ENABLED_TEST_SUITES    the suites this build includes
#   PAYKAN_TESTS_NEED_GTEST       whether any of them uses GoogleTest
# Provides:
#   paykan_test_suite(<suite> <out-var>)  declare a suite's section of
#                                         tests/CMakeLists.txt; <out-var> is
#                                         TRUE when the build includes it
#   paykan_check_test_suites()            every suite was declared
# ----------------------------------------------------------------------------

set(PAYKAN_TEST_SUITES_FILE "${PROJECT_SOURCE_DIR}/tests/suites.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PAYKAN_TEST_SUITES_FILE}")
file(READ "${PAYKAN_TEST_SUITES_FILE}" _paykan_suites_json)
string(JSON _paykan_suite_count LENGTH "${_paykan_suites_json}" suites)
set(PAYKAN_TEST_SUITES "")
set(PAYKAN_GTEST_SUITES "")
math(EXPR _paykan_last "${_paykan_suite_count} - 1")
foreach(i RANGE ${_paykan_last})
    string(JSON suite MEMBER "${_paykan_suites_json}" suites ${i})
    list(APPEND PAYKAN_TEST_SUITES ${suite})
    string(JSON gtest GET "${_paykan_suites_json}" suites ${suite} gtest)
    if(gtest)
        list(APPEND PAYKAN_GTEST_SUITES ${suite})
    endif()
endforeach()
list(JOIN PAYKAN_TEST_SUITES ", " _paykan_known_suites)

set(PAYKAN_BUILD_TESTS "" CACHE STRING
    "Test suites to build, a list (${_paykan_known_suites}); empty builds none")
option(PAYKAN_BUILD_ALL_TESTS "Build every test suite" OFF)

if(PAYKAN_BUILD_ALL_TESTS)
    set(PAYKAN_ENABLED_TEST_SUITES ${PAYKAN_TEST_SUITES})
elseif(NOT PAYKAN_BUILD_TESTS)
    # "", OFF, NO, FALSE, 0: no tests.
    set(PAYKAN_ENABLED_TEST_SUITES "")
else()
    set(PAYKAN_ENABLED_TEST_SUITES "")
    foreach(suite IN LISTS PAYKAN_BUILD_TESTS)
        if(suite STREQUAL "")
            continue()
        endif()
        if(NOT suite IN_LIST PAYKAN_TEST_SUITES)
            if(suite MATCHES "^(ON|on|On|TRUE|true|YES|yes|1)$")
                message(FATAL_ERROR
                    "PAYKAN_BUILD_TESTS is a list of test suites, not ON: use "
                    "-DPAYKAN_BUILD_ALL_TESTS=ON for every suite, or name the "
                    "ones to build (${_paykan_known_suites}).")
            endif()
            message(FATAL_ERROR
                "PAYKAN_BUILD_TESTS: unknown test suite '${suite}' "
                "(the suites: ${_paykan_known_suites}).")
        endif()
        list(APPEND PAYKAN_ENABLED_TEST_SUITES ${suite})
    endforeach()
    list(REMOVE_DUPLICATES PAYKAN_ENABLED_TEST_SUITES)
endif()

set(PAYKAN_TESTS_NEED_GTEST OFF)
foreach(suite IN LISTS PAYKAN_ENABLED_TEST_SUITES)
    if(suite IN_LIST PAYKAN_GTEST_SUITES)
        set(PAYKAN_TESTS_NEED_GTEST ON)
    endif()
endforeach()

if(PAYKAN_ENABLED_TEST_SUITES)
    list(JOIN PAYKAN_ENABLED_TEST_SUITES ", " _paykan_enabled)
    message(STATUS "Test suites: ${_paykan_enabled}")
else()
    message(STATUS "Test suites: none (-DPAYKAN_BUILD_TESTS=<suites> or "
                   "-DPAYKAN_BUILD_ALL_TESTS=ON to build them)")
endif()

function(paykan_test_suite suite out)
    if(NOT suite IN_LIST PAYKAN_TEST_SUITES)
        message(FATAL_ERROR "paykan_test_suite: '${suite}' is not a suite of "
                            "${PAYKAN_TEST_SUITES_FILE}: run scripts/gen_test_suites.py")
    endif()
    set_property(GLOBAL APPEND PROPERTY PAYKAN_DECLARED_TEST_SUITES ${suite})
    if(suite IN_LIST PAYKAN_ENABLED_TEST_SUITES)
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(paykan_check_test_suites)
    get_property(declared GLOBAL PROPERTY PAYKAN_DECLARED_TEST_SUITES)
    foreach(suite IN LISTS PAYKAN_TEST_SUITES)
        if(NOT suite IN_LIST declared)
            message(FATAL_ERROR "test suite '${suite}' (${PAYKAN_TEST_SUITES_FILE}) "
                                "has no paykan_test_suite() section in "
                                "tests/CMakeLists.txt: run scripts/gen_test_suites.py")
        endif()
    endforeach()
endfunction()
