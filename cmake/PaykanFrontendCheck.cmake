# PaykanFrontendCheck.cmake -- installed with Paykan's test support
# The differential check of a frontend plugin through the INSTALLED `paykan`
# (paykan_add_frontend_tests adds it as InstalledPaykan.<frontend>): with
# only the plugin loaded (--no-plugins --plugin=<file>), the plugin's
# frontend is listed with its file, and for every .pkn file of the samples
# corpus, `paykan --frontend=<frontend> --dump-ast` exits with the same status
# and prints the same AST as `paykan --dump-ast` with the installation's
# recursive-descent frontend.  Diagnostics are not compared (their wording may
# differ between frontends, docs/grammar.md section 9).
#
#   cmake -DPAYKAN=<paykan> -DPLUGIN=<file> -DFRONTEND=<name>
#         -DSAMPLES=<dir> -P PaykanFrontendCheck.cmake
cmake_minimum_required(VERSION 3.24)

foreach(var PAYKAN PLUGIN FRONTEND SAMPLES)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "PaykanFrontendCheck.cmake: ${var} is not set")
    endif()
endforeach()

set(args --no-plugins --plugin=${PLUGIN})
execute_process(COMMAND ${PAYKAN} ${args} --list-frontends
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
string(FIND "${out}" "${FRONTEND}" at)
string(FIND "${out}" "[${PLUGIN}]" file_at)
if(NOT rc EQUAL 0 OR at EQUAL -1 OR file_at EQUAL -1 OR out MATCHES "rejected plugin|incompatible|ambiguous")
    message(FATAL_ERROR "${PAYKAN} does not list the frontend '${FRONTEND}' "
                        "of ${PLUGIN}:\n${out}${err}")
endif()

file(GLOB_RECURSE files "${SAMPLES}/*.pkn")
list(SORT files)
set(differ 0)
foreach(f IN LISTS files)
    execute_process(COMMAND ${PAYKAN} ${args} --frontend=${FRONTEND} --dump-ast ${f}
        RESULT_VARIABLE rc_plugin OUTPUT_VARIABLE ast_plugin ERROR_QUIET)
    execute_process(COMMAND ${PAYKAN} ${args} --frontend=recursive-descent --dump-ast ${f}
        RESULT_VARIABLE rc_rd OUTPUT_VARIABLE ast_rd ERROR_QUIET)
    if(NOT rc_plugin EQUAL rc_rd OR NOT ast_plugin STREQUAL ast_rd)
        math(EXPR differ "${differ} + 1")
        message(SEND_ERROR "${f}: '${FRONTEND}' (exit ${rc_plugin}) and "
                           "recursive-descent (exit ${rc_rd}) differ")
    endif()
endforeach()
list(LENGTH files n)
if(differ)
    message(FATAL_ERROR "${differ} of ${n} files differ")
endif()
message(STATUS "${FRONTEND} through ${PAYKAN}: ${n} files, 0 differ")
