# LetDeclsWithoutAnAST.cmake -- the LetDeclsWithoutAnAST ctest (LET-1).
# Runs scripts/let_check.py on one sample with a "compiler" that prints no
# AST (Python, given paykan's options) and checks that the check fails: it
# exits with status 1 and says that no program parses.  A test with only
# ctest's PASS_REGULAR_EXPRESSION would pass on the message whatever the
# status.
#
#   cmake -DPYTHON=<python3> -DSOURCE_DIR=<src> -P LetDeclsWithoutAnAST.cmake

foreach(var PYTHON SOURCE_DIR)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "LetDeclsWithoutAnAST: ${var} is not set")
    endif()
endforeach()

execute_process(
    COMMAND "${PYTHON}" "${SOURCE_DIR}/scripts/let_check.py"
            --paykan "${PYTHON}"
            "${SOURCE_DIR}/samples/codegen/01_literals.pkn"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
message("${output}")

if(NOT status STREQUAL "1")
    message(FATAL_ERROR
        "LetDeclsWithoutAnAST: let_check.py exited with '${status}', not 1")
endif()
string(FIND "${output}" "error: no program parses with" at)
if(at EQUAL -1)
    message(FATAL_ERROR
        "LetDeclsWithoutAnAST: let_check.py did not say that no program parses")
endif()
