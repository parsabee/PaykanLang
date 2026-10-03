# tests/CMakeLists.txt
# ----------------------------------------------------------------------------
# Unit tests for the Paykan compiler (Google Test)
# ----------------------------------------------------------------------------

if("llvm" IN_LIST PAYKAN_BACKENDS)
    llvm_map_components_to_libnames(TEST_LLVM_LIBS
        core support passes orcjit native
    )
endif()

# --- Parser tests -----------------------------------------------------------
add_executable(parser_tests
    Parser/ArithTests.cpp
    Parser/FuncTests.cpp
    Parser/ControlFlowTests.cpp
    Parser/ClassTests.cpp
    Parser/ModuleTests.cpp
    Parser/ArrayTests.cpp
    Parser/MovTests.cpp
    Parser/GenericsTests.cpp
    Parser/ConversionTests.cpp
    Parser/OptionalTests.cpp
    Parser/TupleTests.cpp
    Parser/GrammarEdgeTests.cpp
)
target_include_directories(parser_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(parser_tests
    PRIVATE gtest_main
            paykan_frontend
            paykan_ast_printer
            paykan_compile_options
)
paykan_link_plugins(parser_tests)
# The parser tests run once per enabled frontend; TestUtils.h reads
# PAYKAN_TEST_FRONTEND to pick the one under test (and compares every
# parse against the other enabled frontends).
foreach(fe IN LISTS PAYKAN_FRONTENDS)
    add_test(NAME ParserTests.${fe} COMMAND parser_tests)
    set_tests_properties(ParserTests.${fe} PROPERTIES
        ENVIRONMENT "PAYKAN_TEST_FRONTEND=${fe}")
endforeach()

# --- Sema tests -------------------------------------------------------------
add_executable(sema_tests
    Sema/ArithTests.cpp
    Sema/FuncTests.cpp
    Sema/ControlFlowTests.cpp
    Sema/ClassTests.cpp
    Sema/ModuleTests.cpp
    Sema/ImportTypeTests.cpp
    Sema/ArrayTests.cpp
    Sema/EnumTests.cpp
    Sema/MovTests.cpp
    Sema/GenericsTests.cpp
    Sema/ConversionTests.cpp
    Sema/OptionalTests.cpp
    Sema/OptionalPrimitiveTests.cpp
    Sema/TupleTests.cpp
    Sema/PoisonTests.cpp
)
target_include_directories(sema_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(sema_tests
    PRIVATE gtest_main
            paykan_sema
            paykan_frontend
            paykan_ast_printer
            paykan_compile_options
)
paykan_link_plugins(sema_tests)
foreach(fe IN LISTS PAYKAN_FRONTENDS)
    add_test(NAME SemaTests.${fe} COMMAND sema_tests)
    set_tests_properties(SemaTests.${fe} PROPERTIES
        ENVIRONMENT "PAYKAN_TEST_FRONTEND=${fe}")
endforeach()

# --- CodeGen / E2E tests (every backend) ------------------------------------
# One executable; ctest runs it once per backend that can run programs
# (PAYKAN_TEST_BACKEND selects the one the compile-and-run helpers use, see
# CodeGenTestUtils.h), so the backends check each other's output.
add_executable(codegen_tests
    CodeGen/ArithTests.cpp
    CodeGen/FuncTests.cpp
    CodeGen/ControlFlowTests.cpp
    CodeGen/ClassTests.cpp
    CodeGen/ModuleTests.cpp
    CodeGen/ImportTypeTests.cpp
    CodeGen/CharTests.cpp
    CodeGen/ArrayTests.cpp
    CodeGen/LeakTests.cpp
    CodeGen/BoxedPrimitiveTests.cpp
    CodeGen/ConversionTests.cpp
    CodeGen/ArgsTests.cpp
    CodeGen/EnumTests.cpp
    CodeGen/MovTests.cpp
    CodeGen/OwnershipTests.cpp
    CodeGen/GenericsTests.cpp
    CodeGen/OptionalTests.cpp
    CodeGen/OptionalPrimitiveTests.cpp
    CodeGen/TupleTests.cpp
    CodeGen/RegressionTests.cpp
)
target_include_directories(codegen_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/src/Runtime
)
target_link_libraries(codegen_tests
    PRIVATE gtest_main
            paykan_lowering
            paykan_sema
            paykan_frontend
            paykan_ast_printer
            paykan_runtime
            paykan_compile_options
)
if("llvm" IN_LIST PAYKAN_BACKENDS)
    target_include_directories(codegen_tests SYSTEM
        PRIVATE ${LLVM_INCLUDE_DIRS}
    )
    target_compile_definitions(codegen_tests
        PRIVATE PAYKAN_TEST_HAVE_LLVM=1
    )
    target_link_libraries(codegen_tests
        PRIVATE paykan_jit
                ${TEST_LLVM_LIBS}
    )

# --- PIR -> LLVM translation tests (hand-written PIR, run through the JIT) --
    add_executable(pir_llvm_tests
        Backends/PIRLLVMTests.cpp
    )
    target_include_directories(pir_llvm_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_include_directories(pir_llvm_tests SYSTEM
        PRIVATE ${LLVM_INCLUDE_DIRS}
    )
    target_link_libraries(pir_llvm_tests
        PRIVATE gtest_main
                paykan_jit
                paykan_pir
                paykan_frontend
                paykan_runtime
                paykan_compile_options
                ${TEST_LLVM_LIBS}
    )
    paykan_link_plugins(pir_llvm_tests)
    add_test(NAME PIRLLVMTests COMMAND pir_llvm_tests)
endif()
paykan_link_plugins(codegen_tests)
foreach(be IN LISTS PAYKAN_BACKENDS)
    if(be STREQUAL "llvm" OR be STREQUAL "c")
        add_test(NAME CodeGenTests.${be} COMMAND codegen_tests)
        set_tests_properties(CodeGenTests.${be} PROPERTIES
            ENVIRONMENT "PAYKAN_TEST_BACKEND=${be}")
    endif()
endforeach()

# --- PIR tests (printer, parser, verifier) ----------------------------------
add_executable(pir_tests
    PIR/PIRTests.cpp
)
target_link_libraries(pir_tests
    PRIVATE gtest_main
            paykan_pir
            paykan_compile_options
)
add_test(NAME PIRTests COMMAND pir_tests)

# --- Lowering tests (AST -> PIR, checked as printed PIR; PIR text round trip
# --- over the samples corpus) ------------------------------------------------
add_executable(lowering_tests
    Lowering/LoweringTests.cpp
    Lowering/RoundTripTests.cpp
)
target_include_directories(lowering_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(lowering_tests
    PRIVATE PAYKAN_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/samples"
            PAYKAN_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/example_program"
)
target_link_libraries(lowering_tests
    PRIVATE gtest_main
            paykan_lowering
            paykan_pir
            paykan_sema
            paykan_frontend
            paykan_ast_printer
            paykan_compile_options
)
paykan_link_plugins(lowering_tests)
add_test(NAME LoweringTests COMMAND lowering_tests)

# --- Runtime tests ----------------------------------------------------------
add_executable(runtime_tests
    Runtime/ArrayTests.cpp
    Runtime/EqualsNullTests.cpp
    Runtime/ObjectTests.cpp
    Runtime/StringTests.cpp
    Runtime/SharedTests.cpp
    Runtime/FileTests.cpp
    Runtime/BoxedPrimitiveTests.cpp
    Runtime/OptionalNullTests.cpp
    Runtime/TupleTests.cpp
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
target_link_libraries(driver_tests
    PRIVATE gtest_main
            paykan_compile_options
)
add_dependencies(driver_tests paykan)
if(PAYKAN_COVERAGE)
    target_compile_definitions(driver_tests PRIVATE PAYKAN_TEST_COVERAGE=1)
endif()
# The samples corpus on every backend that runs programs: identical stdout,
# exit codes and zero live heap blocks (scripts/samples_parity.py).
set(PAYKAN_SAMPLES_BACKENDS "")
foreach(be IN LISTS PAYKAN_BACKENDS)
    if(be STREQUAL "llvm" OR be STREQUAL "c")
        list(APPEND PAYKAN_SAMPLES_BACKENDS --backend ${be})
    endif()
endforeach()
if(PAYKAN_SAMPLES_BACKENDS)
    add_test(NAME SamplesParity
        COMMAND ${CMAKE_COMMAND} -E env python3
                ${CMAKE_SOURCE_DIR}/scripts/samples_parity.py
                --paykan $<TARGET_FILE:paykan> ${PAYKAN_SAMPLES_BACKENDS})
    # The same corpus built ahead of time (`paykan build`) and run as
    # executables.
    add_test(NAME SamplesParityBuild
        COMMAND ${CMAKE_COMMAND} -E env python3
                ${CMAKE_SOURCE_DIR}/scripts/samples_parity.py --build
                --paykan $<TARGET_FILE:paykan> ${PAYKAN_SAMPLES_BACKENDS})
    # The corpus at -O0 too (the default is -O2), which keeps the
    # unoptimised code paths of both backends covered.
    add_test(NAME SamplesParityO0
        COMMAND ${CMAKE_COMMAND} -E env python3
                ${CMAKE_SOURCE_DIR}/scripts/samples_parity.py -O 0
                --paykan $<TARGET_FILE:paykan> ${PAYKAN_SAMPLES_BACKENDS})
    # They compile the same import samples through the same per-project
    # object / bitcode caches (samples/imports/*/.paykan_cache): one at a
    # time.
    set_tests_properties(SamplesParity SamplesParityBuild SamplesParityO0
        PROPERTIES RESOURCE_LOCK paykan_samples_cache)
endif()

# Once per backend that runs programs (see backendFlag() in DriverTests.cpp).
foreach(be IN LISTS PAYKAN_BACKENDS)
    if(be STREQUAL "llvm" OR be STREQUAL "c")
        add_test(NAME DriverTests.${be} COMMAND driver_tests)
        set_tests_properties(DriverTests.${be} PROPERTIES
            ENVIRONMENT "PAYKAN_TEST_BACKEND=${be}")
    endif()
endforeach()

# --- Frontend tests: lexer, recovery, fuzz smoke of the recursive-descent -----
# --- frontend; and the differential check of every enabled frontend over -----
# --- the samples corpus
add_executable(frontend_tests
    Frontend/LexerTests.cpp
    Frontend/RecoveryTests.cpp
    Frontend/FuzzSmokeTests.cpp
    Frontend/DifferentialTests.cpp
)
target_compile_definitions(frontend_tests
    PRIVATE PAYKAN_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/samples"
            PAYKAN_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/example_program"
)
target_link_libraries(frontend_tests
    PRIVATE gtest_main
            paykan_frontend
            paykan_ast_printer
            paykan_compile_options
)
# paykan_frontend_recursive_descent (whose public header the lexer/recovery/
# fuzz
# tests use directly) comes in through the plugin link below.
paykan_link_plugins(frontend_tests)
add_test(NAME FrontendTests COMMAND frontend_tests)

# The same check through the driver (`paykan --frontend=<name> --dump-ast`),
# for the first two enabled frontends.
if(PAYKAN_NUM_FRONTENDS GREATER 1)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        list(GET PAYKAN_FRONTENDS 0 PAYKAN_DIFF_FE_A)
        list(GET PAYKAN_FRONTENDS 1 PAYKAN_DIFF_FE_B)
        add_test(NAME FrontendDifferential
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/diff_frontends.py
                    --paykan $<TARGET_FILE:paykan>
                    --frontends ${PAYKAN_DIFF_FE_A},${PAYKAN_DIFF_FE_B})
    endif()
endif()
