# tests/CMakeLists.txt
# ----------------------------------------------------------------------------
# Unit tests for the Paykan compiler (Google Test)
# ----------------------------------------------------------------------------

llvm_map_components_to_libnames(TEST_LLVM_LIBS
    core support passes orcjit native
)

# --- Parser tests -----------------------------------------------------------
add_executable(parser_tests
    Parser/ArithTests.cpp
    Parser/FuncTests.cpp
    Parser/ControlFlowTests.cpp
    Parser/ClassTests.cpp
    Parser/ModuleTests.cpp
    Parser/ArrayTests.cpp
)
target_include_directories(parser_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(parser_tests SYSTEM
    PRIVATE ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(parser_tests
    PRIVATE gtest_main
            paykan_parser
            paykan_compile_options
            ${TEST_LLVM_LIBS}
)
add_test(NAME ParserTests COMMAND parser_tests)

# --- Sema tests -------------------------------------------------------------
add_executable(sema_tests
    Sema/ArithTests.cpp
    Sema/FuncTests.cpp
    Sema/ControlFlowTests.cpp
    Sema/ClassTests.cpp
    Sema/ModuleTests.cpp
    Sema/ArrayTests.cpp
    Sema/EnumTests.cpp
)
target_include_directories(sema_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(sema_tests SYSTEM
    PRIVATE ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(sema_tests
    PRIVATE gtest_main
            paykan_sema
            paykan_parser
            paykan_compile_options
            ${TEST_LLVM_LIBS}
)
add_test(NAME SemaTests COMMAND sema_tests)

# --- CodeGen / E2E tests ----------------------------------------------------
add_executable(codegen_tests
    CodeGen/ArithTests.cpp
    CodeGen/FuncTests.cpp
    CodeGen/ControlFlowTests.cpp
    CodeGen/ClassTests.cpp
    CodeGen/ModuleTests.cpp
    CodeGen/CharTests.cpp
    CodeGen/ArrayTests.cpp
    CodeGen/LeakTests.cpp
    CodeGen/BoxedPrimitiveTests.cpp
    CodeGen/ArgsTests.cpp
    CodeGen/EnumTests.cpp
)
target_include_directories(codegen_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/src/Runtime
)
target_include_directories(codegen_tests SYSTEM
    PRIVATE ${LLVM_INCLUDE_DIRS}
)
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

# --- Runtime tests ----------------------------------------------------------
add_executable(runtime_tests
    Runtime/ArrayTests.cpp
    Runtime/ObjectTests.cpp
    Runtime/StringTests.cpp
    Runtime/SharedTests.cpp
    Runtime/FileTests.cpp
    Runtime/BoxedPrimitiveTests.cpp
)
target_include_directories(runtime_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/src/Runtime
)
target_link_libraries(runtime_tests
    PRIVATE gtest_main
            paykan_runtime
            paykan_compile_options
)
add_test(NAME RuntimeTests COMMAND runtime_tests)

# --- Driver smoke tests (--check-only, --dump-ast) --------------------------
add_executable(driver_tests
    Driver/DriverTests.cpp
)
target_compile_definitions(driver_tests
    PRIVATE PAYKAN_BIN="$<TARGET_FILE:paykan>"
)
target_include_directories(driver_tests SYSTEM
    PRIVATE ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(driver_tests
    PRIVATE gtest_main
            paykan_compile_options
)
add_dependencies(driver_tests paykan)
add_test(NAME DriverTests COMMAND driver_tests)
