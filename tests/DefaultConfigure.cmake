# DefaultConfigure.cmake -- the DefaultConfigure ctest (#123).
# ----------------------------------------------------------------------------
# Configures the source tree with nothing but -DPAYKAN_BUILD_TESTS=OFF (what
# release.yml and packagers run) in a scratch build directory and checks that
# the defaults are the core: the recursive-descent frontend and the c backend,
# with nothing downloaded or built from third-party sources (no third-party/
# or _deps/ directory).  Configure only: nothing is compiled.
#
#   cmake -DSOURCE_DIR=<src> -DBINARY_DIR=<scratch> -DGENERATOR=<gen>
#         [-DC_COMPILER=<cc>] [-DCXX_COMPILER=<c++>] -P DefaultConfigure.cmake
# ----------------------------------------------------------------------------

foreach(var SOURCE_DIR BINARY_DIR GENERATOR)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "DefaultConfigure: ${var} is not set")
    endif()
endforeach()

set(args -S "${SOURCE_DIR}" -B "${BINARY_DIR}" -G "${GENERATOR}"
         -DPAYKAN_BUILD_TESTS=OFF)
# The same compilers as the enclosing build, so the compiler gate sees a
# supported one; no other setting of the enclosing build is passed on.
if(C_COMPILER)
    list(APPEND args "-DCMAKE_C_COMPILER=${C_COMPILER}")
endif()
if(CXX_COMPILER)
    list(APPEND args "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
endif()

file(REMOVE_RECURSE "${BINARY_DIR}")
execute_process(COMMAND "${CMAKE_COMMAND}" ${args}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
set(log "${out}${err}")

set(failures "")
if(NOT rc EQUAL 0)
    string(APPEND failures "  configure failed (${rc})\n")
endif()
foreach(expected
        "Frontends: recursive-descent (default: recursive-descent)"
        "Backends: c (default: c)")
    string(FIND "${log}" "${expected}" at)
    if(at EQUAL -1)
        string(APPEND failures "  missing '${expected}'\n")
    endif()
endforeach()
foreach(dir third-party _deps)
    if(EXISTS "${BINARY_DIR}/${dir}")
        string(APPEND failures "  ${dir}/ was created\n")
    endif()
endforeach()
if(log MATCHES "Downloading|LLVM not found|prebuilt LLVM|LLVM installed|Bison|Flex|GTest")
    string(APPEND failures "  the log mentions a third-party dependency: '${CMAKE_MATCH_0}'\n")
endif()

file(REMOVE_RECURSE "${BINARY_DIR}")
if(failures)
    message(FATAL_ERROR "DefaultConfigure: a plain configure is not the core build:\n"
                        "${failures}--- configure output ---\n${log}")
endif()
message(STATUS "DefaultConfigure: recursive-descent + c, nothing downloaded")
