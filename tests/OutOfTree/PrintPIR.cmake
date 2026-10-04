# The out-of-tree plugin path, as CI's install job checks it: install this
# build tree to a scratch prefix, configure and build utils/print-pir against
# it with find_package(Paykan) (docs/writing-a-backend.md), then check that the
# resulting driver lists the print-pir backend and that its output is exactly
# `paykan --emit-pir`.
#
# Run as a ctest (tests/CMakeLists.txt, PrintPIROutOfTree):
#   cmake -DPAYKAN_BUILD_DIR=... -DPAYKAN_SOURCE_DIR=... -DPAYKAN_BIN=...
#         -DWORK_DIR=... -DGENERATOR=... [-DLLVM_DIR=...]
#         [-DCMAKE_C_COMPILER=... -DCMAKE_CXX_COMPILER=...]
#         -P tests/OutOfTree/PrintPIR.cmake
cmake_minimum_required(VERSION 3.24)

foreach(var PAYKAN_BUILD_DIR PAYKAN_SOURCE_DIR PAYKAN_BIN WORK_DIR GENERATOR)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "PrintPIR.cmake: ${var} is not set")
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

set(configure_args -S ${PAYKAN_SOURCE_DIR}/utils/print-pir -B ${build}
    -G ${GENERATOR} -DCMAKE_PREFIX_PATH=${prefix} -DCMAKE_BUILD_TYPE=Debug)
foreach(var LLVM_DIR CMAKE_C_COMPILER CMAKE_CXX_COMPILER)
    if(${var})
        list(APPEND configure_args -D${var}=${${var}})
    endif()
endforeach()
run_step("configure utils/print-pir" ${CMAKE_COMMAND} ${configure_args})
run_step("build utils/print-pir" ${CMAKE_COMMAND} --build ${build})

set(driver ${build}/paykan-print-pir)
run_step("paykan-print-pir --list-backends" ${driver} --list-backends)
if(NOT STEP_OUTPUT MATCHES "print-pir")
    message(FATAL_ERROR "print-pir is not registered:\n${STEP_OUTPUT}")
endif()

set(sample ${PAYKAN_SOURCE_DIR}/samples/codegen/01_literals.pkn)
run_step("print-pir --emit-source" ${driver} --backend=print-pir --emit-source ${sample})
set(plugin_pir "${STEP_OUTPUT}")
run_step("paykan --emit-pir" ${PAYKAN_BIN} --emit-pir ${sample})
if(NOT plugin_pir STREQUAL STEP_OUTPUT)
    message(FATAL_ERROR "print-pir's output differs from paykan --emit-pir")
endif()

file(REMOVE_RECURSE ${WORK_DIR})
message(STATUS "print-pir built out of tree and matches --emit-pir")
