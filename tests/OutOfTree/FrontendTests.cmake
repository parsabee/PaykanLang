# The exported frontend test support (cmake/PaykanTestSupport.cmake): install
# this build tree to a scratch prefix, build tests/OutOfTree/frontend-tests
# (a loadable frontend plugin, rd-plugin) against it with find_package(Paykan)
# and paykan_add_frontend_tests() (docs/writing-a-frontend-plugin.md), and run
# the suites it adds: in process with the plugin loaded, and through the
# installed paykan.
#
# Run as a ctest (tests/CMakeLists.txt, FrontendTestsOutOfTree):
#   cmake -DPAYKAN_BUILD_DIR=... -DPAYKAN_SOURCE_DIR=... -DWORK_DIR=...
#         -DGENERATOR=... [-DLLVM_DIR=...] [-DGTEST_SOURCE_DIR=...]
#         [-DGTest_DIR=...] [-DCMAKE_C_COMPILER=... -DCMAKE_CXX_COMPILER=...]
#         -P tests/OutOfTree/FrontendTests.cmake
cmake_minimum_required(VERSION 3.24)

foreach(var PAYKAN_BUILD_DIR PAYKAN_SOURCE_DIR WORK_DIR GENERATOR)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "FrontendTests.cmake: ${var} is not set")
    endif()
endforeach()

function(run_step what)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${what} failed (${rc}):\n${out}\n${err}")
    endif()
    set(STEP_OUTPUT "${out}" PARENT_SCOPE)
endfunction()

set(prefix ${WORK_DIR}/install)
set(build ${WORK_DIR}/build)
file(REMOVE_RECURSE ${WORK_DIR})

run_step("install" ${CMAKE_COMMAND} --install ${PAYKAN_BUILD_DIR} --prefix ${prefix})
foreach(f share/paykan/frontend-tests/TestUtils.h
          share/paykan/samples/codegen/01_literals.pkn)
    if(NOT EXISTS ${prefix}/${f})
        message(FATAL_ERROR "the installation has no ${f}")
    endif()
endforeach()

set(configure_args -S ${PAYKAN_SOURCE_DIR}/tests/OutOfTree/frontend-tests
    -B ${build} -G ${GENERATOR} -DCMAKE_PREFIX_PATH=${prefix}
    -DCMAKE_BUILD_TYPE=Debug)
if(GTEST_SOURCE_DIR)
    list(APPEND configure_args -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=${GTEST_SOURCE_DIR})
endif()
foreach(var LLVM_DIR GTest_DIR CMAKE_C_COMPILER CMAKE_CXX_COMPILER)
    if(${var})
        list(APPEND configure_args -D${var}=${${var}})
    endif()
endforeach()
run_step("configure tests/OutOfTree/frontend-tests" ${CMAKE_COMMAND} ${configure_args})
run_step("build tests/OutOfTree/frontend-tests"
    ${CMAKE_COMMAND} --build ${build} --parallel)
run_step("ctest" ${CMAKE_CTEST_COMMAND} --test-dir ${build} --output-on-failure)
foreach(t ParserTests SemaTests FrontendTests InstalledPaykan)
    if(NOT STEP_OUTPUT MATCHES "${t}\\.rd-plugin[ .]+Passed")
        message(FATAL_ERROR "${t}.rd-plugin did not run:\n${STEP_OUTPUT}")
    endif()
endforeach()

file(REMOVE_RECURSE ${WORK_DIR})
message(STATUS "the exported frontend suites build and pass out of tree")
