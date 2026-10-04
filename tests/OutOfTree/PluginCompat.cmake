# The configure-time plugin compatibility check (#103): install this build
# tree to a scratch prefix and configure utils/print-pir against it, once
# pinned to a PaykanLang version the installation accepts (configure
# succeeds) and once pinned to one it does not (configure fails with the
# expected message).  Configure only: PrintPIROutOfTree builds and runs it.
#
# Run as a ctest (tests/CMakeLists.txt, PluginCompatOutOfTree):
#   cmake -DPAYKAN_BUILD_DIR=... -DPAYKAN_SOURCE_DIR=... -DWORK_DIR=...
#         -DGENERATOR=... -DACCEPTED_VERSION=... -DACCEPTED_LIST=...
#         [-DLLVM_DIR=...] [-DCMAKE_C_COMPILER=... -DCMAKE_CXX_COMPILER=...]
#         -P tests/OutOfTree/PluginCompat.cmake
cmake_minimum_required(VERSION 3.24)

foreach(var PAYKAN_BUILD_DIR PAYKAN_SOURCE_DIR WORK_DIR GENERATOR
            ACCEPTED_VERSION ACCEPTED_LIST)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "PluginCompat.cmake: ${var} is not set")
    endif()
endforeach()

set(prefix ${WORK_DIR}/install)
file(REMOVE_RECURSE ${WORK_DIR})

execute_process(
    COMMAND ${CMAKE_COMMAND} --install ${PAYKAN_BUILD_DIR} --prefix ${prefix}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "install failed (${rc}):\n${out}\n${err}")
endif()

# Configure utils/print-pir pinned to <version>; sets CONFIGURE_RC and
# CONFIGURE_OUTPUT (stdout and stderr, whitespace collapsed: CMake wraps
# long messages).
function(configure_print_pir version build)
    set(args -S ${PAYKAN_SOURCE_DIR}/utils/print-pir -B ${build}
        -G ${GENERATOR} -DCMAKE_PREFIX_PATH=${prefix}
        -DCMAKE_BUILD_TYPE=Debug -DPRINT_PIR_BUILT_WITH=${version})
    foreach(var LLVM_DIR CMAKE_C_COMPILER CMAKE_CXX_COMPILER)
        if(${var})
            list(APPEND args -D${var}=${${var}})
        endif()
    endforeach()
    execute_process(COMMAND ${CMAKE_COMMAND} ${args}
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    string(REGEX REPLACE "[ \t\r\n]+" " " collapsed "${out} ${err}")
    set(CONFIGURE_RC ${rc} PARENT_SCOPE)
    set(CONFIGURE_OUTPUT "${collapsed}" PARENT_SCOPE)
endfunction()

# Accepted: configure succeeds.
configure_print_pir("${ACCEPTED_VERSION}" ${WORK_DIR}/accepted)
if(NOT CONFIGURE_RC EQUAL 0)
    message(FATAL_ERROR "configuring print-pir built with the accepted "
        "PaykanLang ${ACCEPTED_VERSION} failed:\n${CONFIGURE_OUTPUT}")
endif()

# Not accepted (pinned to a version that is on no list): configure fails,
# naming the plugin, both versions and the list.
set(rejected "0.0.9")
configure_print_pir("${rejected}" ${WORK_DIR}/rejected)
if(CONFIGURE_RC EQUAL 0)
    message(FATAL_ERROR "configuring print-pir built with PaykanLang "
        "${rejected} succeeded; it must fail:\n${CONFIGURE_OUTPUT}")
endif()
set(expected_head "Paykan backend plugin 'paykan_backend_print_pir' is incompatible: built with PaykanLang ${rejected}; the installed PaykanLang ${ACCEPTED_VERSION} (")
set(expected_tail ") accepts ${ACCEPTED_LIST}")
string(FIND "${CONFIGURE_OUTPUT}" "${expected_head}" head)
string(FIND "${CONFIGURE_OUTPUT}" "${expected_tail}" tail)
if(head EQUAL -1 OR tail EQUAL -1 OR tail LESS head)
    message(FATAL_ERROR "configuring print-pir built with PaykanLang "
        "${rejected} failed without the expected message\n"
        "  expected: ${expected_head}<Paykan_DIR>${expected_tail}\n"
        "  output: ${CONFIGURE_OUTPUT}")
endif()

file(REMOVE_RECURSE ${WORK_DIR})
message(STATUS "print-pir: accepted ${ACCEPTED_VERSION}, rejected ${rejected} at configure time")
