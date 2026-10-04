# Loadable plugins against an installed PaykanLang, as CI's install job
# checks them: install this build tree to a scratch prefix, build the
# plain-C utils/print-pir backend against it with find_package(Paykan)
# (docs/writing-a-backend.md), and load it into the INSTALLED `paykan`, with
# no rebuild of PaykanLang:
#   - with --plugin=<file>, and from $PAYKAN_PLUGIN_PATH: listed with its
#     file, compatible, and its output is exactly `paykan --emit-pir`;
#   - after `cmake --install` of the plugin into the installation's plugin
#     directory, with the plugin's build tree deleted: found by itself.
# Then the same for the Rust example (utils/pir-stats-rust), when rustc is
# available (RUSTC).
#
# Run as a ctest (tests/CMakeLists.txt, PrintPIROutOfTree):
#   cmake -DPAYKAN_BUILD_DIR=... -DPAYKAN_SOURCE_DIR=... -DWORK_DIR=...
#         -DGENERATOR=... [-DRUSTC=...] [-DLLVM_DIR=...]
#         [-DCMAKE_C_COMPILER=... -DCMAKE_CXX_COMPILER=...]
#         -P tests/OutOfTree/PrintPIR.cmake
cmake_minimum_required(VERSION 3.24)

foreach(var PAYKAN_BUILD_DIR PAYKAN_SOURCE_DIR WORK_DIR GENERATOR)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "PrintPIR.cmake: ${var} is not set")
    endif()
endforeach()

set(prefix ${WORK_DIR}/install)
set(home ${WORK_DIR}/home)
file(REMOVE_RECURSE ${WORK_DIR})
file(MAKE_DIRECTORY ${home})

# Run a command; fails the test unless it exits 0.  STEP_OUTPUT is its
# stdout.  `paykan` runs see only the plugins the step names: a scratch
# $HOME, no $PAYKAN_PLUGIN_PATH unless given in ENV.
function(run_step what)
    cmake_parse_arguments(ARG "" "" "ENV;COMMAND" ${ARGN})
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env --unset=PAYKAN_NO_PLUGINS
                --unset=PAYKAN_PLUGIN_PATH HOME=${home} ${ARG_ENV}
                ${ARG_COMMAND}
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${what} failed (${rc}):\n${out}\n${err}")
    endif()
    set(STEP_OUTPUT "${out}" PARENT_SCOPE)
endfunction()

run_step("install" COMMAND ${CMAKE_COMMAND} --install ${PAYKAN_BUILD_DIR} --prefix ${prefix})
set(paykan ${prefix}/bin/paykan)
file(GLOB plugin_dir LIST_DIRECTORIES true "${prefix}/lib*/paykan/plugins/*")
list(LENGTH plugin_dir n)
if(NOT n EQUAL 1 OR NOT IS_DIRECTORY "${plugin_dir}")
    message(FATAL_ERROR "the installation has no plugin directory: '${plugin_dir}'")
endif()
if(NOT EXISTS ${prefix}/include/paykan/plugin_api.h)
    message(FATAL_ERROR "the installation has no include/paykan/plugin_api.h")
endif()

set(configure_args -G ${GENERATOR} -DCMAKE_PREFIX_PATH=${prefix} -DCMAKE_BUILD_TYPE=Debug)
foreach(var LLVM_DIR CMAKE_C_COMPILER CMAKE_CXX_COMPILER)
    if(${var})
        list(APPEND configure_args -D${var}=${${var}})
    endif()
endforeach()

set(sample ${PAYKAN_SOURCE_DIR}/samples/codegen/01_literals.pkn)
run_step("paykan --emit-pir" COMMAND ${paykan} --emit-pir ${sample})
set(expected_pir "${STEP_OUTPUT}")

# -- print-pir (C) -------------------------------------------------------------
set(build ${WORK_DIR}/print-pir)
run_step("configure utils/print-pir"
    COMMAND ${CMAKE_COMMAND} -S ${PAYKAN_SOURCE_DIR}/utils/print-pir -B ${build}
            ${configure_args})
run_step("build utils/print-pir" COMMAND ${CMAKE_COMMAND} --build ${build})
file(GLOB module "${build}/libpaykan_backend_print_pir.*")
if(NOT module)
    message(FATAL_ERROR "utils/print-pir built no libpaykan_backend_print_pir")
endif()

# 1. --plugin=<file>.
run_step("paykan --plugin --list-backends"
    COMMAND ${paykan} --no-plugins --plugin=${module} --list-backends)
string(FIND "${STEP_OUTPUT}" "\nprint-pir: prints the Paykan IR [${module}]\n" at)
if(at EQUAL -1)
    message(FATAL_ERROR "print-pir is not listed with its file:\n${STEP_OUTPUT}")
endif()
run_step("paykan --plugin --version"
    COMMAND ${paykan} --no-plugins --plugin=${module} --version)
if(NOT STEP_OUTPUT MATCHES "\nbackend print-pir \\(built with PaykanLang [^,\n]+, compatible\\): prints the Paykan IR \\[")
    message(FATAL_ERROR "print-pir is not listed as compatible:\n${STEP_OUTPUT}")
endif()
run_step("paykan --plugin --backend=print-pir --emit-source"
    COMMAND ${paykan} --no-plugins --plugin=${module} --backend=print-pir
            --emit-source ${sample})
if(NOT STEP_OUTPUT STREQUAL expected_pir)
    message(FATAL_ERROR "print-pir's output differs from paykan --emit-pir")
endif()

# 2. $PAYKAN_PLUGIN_PATH.
run_step("PAYKAN_PLUGIN_PATH paykan --backend=print-pir"
    ENV PAYKAN_PLUGIN_PATH=${build}
    COMMAND ${paykan} --backend=print-pir --emit-source ${sample})
if(NOT STEP_OUTPUT STREQUAL expected_pir)
    message(FATAL_ERROR "print-pir from PAYKAN_PLUGIN_PATH differs from --emit-pir")
endif()

# 3. Installed into the installation's plugin directory, its build tree
#    gone: the installed paykan finds it by itself.
run_step("install utils/print-pir" COMMAND ${CMAKE_COMMAND} --install ${build})
file(REMOVE_RECURSE ${build})
run_step("installed paykan --backend=print-pir"
    COMMAND ${paykan} --backend=print-pir --emit-source ${sample})
if(NOT STEP_OUTPUT STREQUAL expected_pir)
    message(FATAL_ERROR "the installed print-pir differs from --emit-pir")
endif()
run_step("installed paykan --list-backends" COMMAND ${paykan} --list-backends)
string(FIND "${STEP_OUTPUT}" "\nprint-pir: prints the Paykan IR [${plugin_dir}/libpaykan_backend_print_pir." at)
if(at EQUAL -1)
    message(FATAL_ERROR "print-pir is not listed from the plugin directory:\n${STEP_OUTPUT}")
endif()
# --no-plugins leaves it out.
run_step("installed paykan --no-plugins --list-backends"
    COMMAND ${paykan} --no-plugins --list-backends)
if(STEP_OUTPUT MATCHES "print-pir")
    message(FATAL_ERROR "--no-plugins still lists print-pir:\n${STEP_OUTPUT}")
endif()

# -- pir-stats (Rust) ----------------------------------------------------------
if(RUSTC)
    set(build ${WORK_DIR}/pir-stats-rust)
    run_step("configure utils/pir-stats-rust"
        COMMAND ${CMAKE_COMMAND} -S ${PAYKAN_SOURCE_DIR}/utils/pir-stats-rust
                -B ${build} ${configure_args} -DRUSTC=${RUSTC})
    run_step("build utils/pir-stats-rust" COMMAND ${CMAKE_COMMAND} --build ${build})
    run_step("install utils/pir-stats-rust" COMMAND ${CMAKE_COMMAND} --install ${build})
    file(REMOVE_RECURSE ${build})
    run_step("installed paykan --backend=pir-stats"
        COMMAND ${paykan} --backend=pir-stats --emit-source ${sample})
    set(expected "pir-stats for ${sample}\nmodules: 1\nfunctions: 1\n")
    string(FIND "${STEP_OUTPUT}" "${expected}" at)
    if(NOT at EQUAL 0 OR NOT STEP_OUTPUT MATCHES "\ninstructions: [1-9][0-9]*\n$")
        message(FATAL_ERROR "pir-stats printed:\n${STEP_OUTPUT}")
    endif()
    message(STATUS "pir-stats (Rust) loads into the installed paykan")
else()
    message(STATUS "rustc not found: the Rust example plugin is not checked")
endif()

# -- The advanced static path: a C++ backend in a driver of its own ----------
set(build ${WORK_DIR}/static-driver)
run_step("configure tests/OutOfTree/static-driver"
    COMMAND ${CMAKE_COMMAND} -S ${CMAKE_CURRENT_LIST_DIR}/static-driver
            -B ${build} ${configure_args})
run_step("build tests/OutOfTree/static-driver" COMMAND ${CMAKE_COMMAND} --build ${build})
run_step("paykan-static-echo --list-backends"
    COMMAND ${build}/paykan-static-echo --no-plugins --list-backends)
string(FIND "${STEP_OUTPUT}" "\nstatic-echo: linked in statically\n" at)
if(at EQUAL -1)
    message(FATAL_ERROR "static-echo is not linked in:\n${STEP_OUTPUT}")
endif()
run_step("paykan-static-echo --emit-source"
    COMMAND ${build}/paykan-static-echo --backend=static-echo --emit-source ${sample})
if(NOT STEP_OUTPUT STREQUAL expected_pir)
    message(FATAL_ERROR "static-echo's output differs from paykan --emit-pir")
endif()
# The custom driver loads plugins too: the installed print-pir is not in its
# plugin directory (it lives elsewhere), but --plugin works.
file(GLOB installed "${plugin_dir}/libpaykan_backend_print_pir.*")
run_step("paykan-static-echo --plugin --backend=print-pir"
    COMMAND ${build}/paykan-static-echo --plugin=${installed}
            --backend=print-pir --emit-source ${sample})
if(NOT STEP_OUTPUT STREQUAL expected_pir)
    message(FATAL_ERROR "print-pir in the custom driver differs from --emit-pir")
endif()

file(REMOVE_RECURSE ${WORK_DIR})
message(STATUS "print-pir loads into the installed paykan and matches --emit-pir")
