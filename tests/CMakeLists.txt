# tests/CMakeLists.txt
# ----------------------------------------------------------------------------
# Unit tests for the Paykan compiler (Google Test)
# ----------------------------------------------------------------------------

llvm_map_components_to_libnames(TEST_LLVM_LIBS
    core support passes orcjit native
)

# --- Parser tests -----------------------------------------------------------
add_executable(parser_tests ParserTests.cpp)
target_include_directories(parser_tests SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(parser_tests
    PRIVATE gtest_main
            paykan_parser
            paykan_compile_options
            ${TEST_LLVM_LIBS}
)
add_test(NAME ParserTests COMMAND parser_tests)

# --- Sema tests -------------------------------------------------------------
add_executable(sema_tests SemaTests.cpp)
target_include_directories(sema_tests SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(sema_tests
    PRIVATE gtest_main
            paykan_sema
            paykan_parser
            paykan_compile_options
            ${TEST_LLVM_LIBS}
)
add_test(NAME SemaTests COMMAND sema_tests)

# --- CodeGen / E2E tests ----------------------------------------------------
add_executable(codegen_tests CodeGenTests.cpp)
target_include_directories(codegen_tests SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(codegen_tests
    PRIVATE gtest_main
            paykan_jit
            paykan_codegen
            paykan_sema
            paykan_parser
            paykan_runtime
            paykan_compile_options
            ${TEST_LLVM_LIBS}
)
add_test(NAME CodeGenTests COMMAND codegen_tests)
