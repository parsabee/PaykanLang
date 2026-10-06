# PaykanTests.cmake
# ----------------------------------------------------------------------------
# Which test suites the build includes.  None by default: a plain configure
# builds only the compiler and downloads nothing.
#
#   -DPAYKAN_BUILD_TESTS="parser;sema"   the named suites (a CMake list)
#   -DPAYKAN_BUILD_ALL_TESTS=ON          every suite
#
# The suites (tests/CMakeLists.txt):
#   parser           tests/Parser          ParserTests.<frontend>
#   sema             tests/Sema            SemaTests.<frontend>
#   codegen          tests/CodeGen         CodeGenTests.<backend>
#   pir-llvm         tests/Backends        PIRLLVMTests (llvm backend only)
#   pir              tests/PIR             PIRTests
#   lowering         tests/Lowering        LoweringTests
#   ast-interchange  tests/AST             ASTInterchangeTests
#   plugin           tests/Plugin          PluginTests, PluginLoaderTests
#   runtime          tests/Runtime         RuntimeTests
#   driver           tests/Driver          DriverTests.<backend>
#   frontend         tests/Frontend        FrontendTests
#   samples          scripts/samples_parity.py, cache_race.py, runtime_lookup.py
#                                          SamplesParity*, CacheRace, RuntimeLookup
#   c-strict         scripts/c_strict.py   CStrictC11 (c backend only)
#   docs             scripts/check_links.py, doc_examples.py
#                                          MarkdownLinks, DocExamples
#   configure        tests/DefaultConfigure.cmake
#                                          DefaultConfigure
#   out-of-tree      tests/OutOfTree       PrintPIROutOfTree,
#                                          PluginCompatOutOfTree,
#                                          FrontendTestsOutOfTree
#
# scripts/affected_tests.py picks the suites a change can affect; CI builds
# only those.
#
# Sets:
#   PAYKAN_TEST_SUITES            every suite name
#   PAYKAN_ENABLED_TEST_SUITES    the suites this build includes
#   PAYKAN_TESTS_NEED_GTEST       whether any of them uses GoogleTest
# Provides:
#   paykan_test_suite_enabled(<suite> <out-var>)
# ----------------------------------------------------------------------------

set(PAYKAN_TEST_SUITES
    parser sema codegen pir-llvm pir lowering ast-interchange plugin runtime
    driver frontend samples c-strict docs configure out-of-tree)
# The suites built on GoogleTest (FrontendTestsOutOfTree builds the exported
# GoogleTest suites, so out-of-tree is one).
set(PAYKAN_GTEST_SUITES
    parser sema codegen pir-llvm pir lowering ast-interchange plugin runtime
    driver frontend out-of-tree)

set(PAYKAN_BUILD_TESTS "" CACHE STRING
    "Test suites to build, a list (${PAYKAN_TEST_SUITES}); empty builds none")
option(PAYKAN_BUILD_ALL_TESTS "Build every test suite" OFF)

if(PAYKAN_BUILD_ALL_TESTS)
    set(PAYKAN_ENABLED_TEST_SUITES ${PAYKAN_TEST_SUITES})
elseif(NOT PAYKAN_BUILD_TESTS OR PAYKAN_BUILD_TESTS STREQUAL "OFF")
    # "", OFF, NO, FALSE, 0: no tests.
    set(PAYKAN_ENABLED_TEST_SUITES "")
else()
    set(PAYKAN_ENABLED_TEST_SUITES "")
    foreach(suite IN LISTS PAYKAN_BUILD_TESTS)
        if(suite STREQUAL "")
            continue()
        endif()
        if(NOT suite IN_LIST PAYKAN_TEST_SUITES)
            list(JOIN PAYKAN_TEST_SUITES ", " known)
            if(suite MATCHES "^(ON|on|On|TRUE|true|YES|yes|1)$")
                message(FATAL_ERROR
                    "PAYKAN_BUILD_TESTS is a list of test suites, not ON: use "
                    "-DPAYKAN_BUILD_ALL_TESTS=ON for every suite, or name the "
                    "ones to build (${known}).")
            endif()
            message(FATAL_ERROR
                "PAYKAN_BUILD_TESTS: unknown test suite '${suite}' "
                "(the suites: ${known}).")
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
    list(JOIN PAYKAN_ENABLED_TEST_SUITES ", " enabled)
    message(STATUS "Test suites: ${enabled}")
else()
    message(STATUS "Test suites: none (-DPAYKAN_BUILD_TESTS=<suites> or "
                   "-DPAYKAN_BUILD_ALL_TESTS=ON to build them)")
endif()

# paykan_test_suite_enabled(<suite> <out-var>): <out-var> is TRUE when the
# build includes <suite>.
function(paykan_test_suite_enabled suite out)
    if(NOT suite IN_LIST PAYKAN_TEST_SUITES)
        message(FATAL_ERROR "paykan_test_suite_enabled: unknown suite '${suite}'")
    endif()
    if(suite IN_LIST PAYKAN_ENABLED_TEST_SUITES)
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()
