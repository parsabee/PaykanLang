# The Paykan compiler's tests, in suites (cmake/PaykanTests.cmake): each
# section below registers one suite, and only when the build includes it
# (-DPAYKAN_BUILD_TESTS=<suites> or -DPAYKAN_BUILD_ALL_TESTS=ON).


# The plugin build versions this build accepts, for the plugin, driver and
# out-of-tree suites.
list(JOIN PAYKAN_PLUGIN_COMPATIBLE_VERSIONS ", " PAYKAN_TEST_ACCEPTED_VERSIONS)

if("llvm" IN_LIST PAYKAN_BACKENDS)
    llvm_map_components_to_libnames(TEST_LLVM_LIBS
        core support passes orcjit native
    )
endif()

# --- Parser tests (parser) ---------------------------------------------------
paykan_test_suite(parser PAYKAN_TEST_SUITE_parser)
if(PAYKAN_TEST_SUITE_parser)
add_executable(parser_tests
    Parser/ArithTests.cpp
    Parser/FuncTests.cpp
    Parser/ControlFlowTests.cpp
    Parser/ClassTests.cpp
    Parser/ModuleTests.cpp
    Parser/ArrayTests.cpp
    Parser/MovTests.cpp
    Parser/OwnershipTests.cpp
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
endif()

# --- Sema tests (sema) -------------------------------------------------------
paykan_test_suite(sema PAYKAN_TEST_SUITE_sema)
if(PAYKAN_TEST_SUITE_sema)
add_executable(sema_tests
    Sema/ArithTests.cpp
    Sema/FuncTests.cpp
    Sema/ControlFlowTests.cpp
    Sema/ClassTests.cpp
    Sema/ModuleTests.cpp
    Sema/ImportTypeTests.cpp
    Sema/ArrayTests.cpp
    Sema/EnumTests.cpp
    Sema/GenericsTests.cpp
    Sema/ConversionTests.cpp
    Sema/OptionalTests.cpp
    Sema/OptionalPrimitiveTests.cpp
    Sema/TupleTests.cpp
    Sema/PoisonTests.cpp
    Sema/InterfaceConversionTests.cpp
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
endif()

# --- CodeGen / E2E tests, every backend (codegen) ----------------------------
paykan_test_suite(codegen PAYKAN_TEST_SUITE_codegen)
if(PAYKAN_TEST_SUITE_codegen)
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
    CodeGen/OwnershipTests.cpp
    CodeGen/GenericsTests.cpp
    CodeGen/OptionalTests.cpp
    CodeGen/OptionalPrimitiveTests.cpp
    CodeGen/TupleTests.cpp
    CodeGen/RegressionTests.cpp
    CodeGen/LiteralObjectTests.cpp
    CodeGen/RuntimeNameTests.cpp
    CodeGen/HarnessTests.cpp
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
    paykan_llvm_no_werror(codegen_tests)
    target_compile_definitions(codegen_tests
        PRIVATE PAYKAN_TEST_HAVE_LLVM=1
    )
    target_link_libraries(codegen_tests
        PRIVATE paykan_jit
                ${TEST_LLVM_LIBS}
    )
endif()
paykan_link_plugins(codegen_tests)
foreach(be IN LISTS PAYKAN_BACKENDS)
    if(be STREQUAL "llvm" OR be STREQUAL "c")
        add_test(NAME CodeGenTests.${be} COMMAND codegen_tests)
        set_tests_properties(CodeGenTests.${be} PROPERTIES
            ENVIRONMENT "PAYKAN_TEST_BACKEND=${be}")
    endif()
endforeach()
endif()

# --- PIR -> LLVM translation tests: hand-written PIR, run through the JIT ----
# --- (pir-llvm) ---------------------------------------------------------------
paykan_test_suite(pir-llvm PAYKAN_TEST_SUITE_pir_llvm)
if(PAYKAN_TEST_SUITE_pir_llvm AND "llvm" IN_LIST PAYKAN_BACKENDS)
    add_executable(pir_llvm_tests
        Backends/PIRLLVMTests.cpp
    )
    target_include_directories(pir_llvm_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_include_directories(pir_llvm_tests SYSTEM
        PRIVATE ${LLVM_INCLUDE_DIRS}
    )
    paykan_llvm_no_werror(pir_llvm_tests)
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

# --- PIR tests: printer, parser, verifier, binary codec (pir) ------------------------------
paykan_test_suite(pir PAYKAN_TEST_SUITE_pir)
if(PAYKAN_TEST_SUITE_pir)
add_executable(pir_tests
    PIR/BinaryTests.cpp
    PIR/PIRTests.cpp
)
target_link_libraries(pir_tests
    PRIVATE gtest_main
            paykan_pir
            paykan_compile_options
)
add_test(NAME PIRTests COMMAND pir_tests)
endif()

# --- .pkm module file tests: container, manifest, interface, dump (pkm) ------
paykan_test_suite(pkm PAYKAN_TEST_SUITE_pkm)
if(PAYKAN_TEST_SUITE_pkm)
add_executable(pkm_tests
    PKM/ContainerTests.cpp
    PKM/ManifestTests.cpp
    PKM/InterfaceTests.cpp
    PKM/DumpTests.cpp
)
target_link_libraries(pkm_tests
    PRIVATE gtest_main
            paykan_pkm
            paykan_compile_options
)
add_test(NAME PKMTests COMMAND pkm_tests)
endif()

# --- Lowering tests (AST -> PIR, checked as printed PIR; PIR text round trip
# --- over the samples corpus) (lowering) -------------------------------------
paykan_test_suite(lowering PAYKAN_TEST_SUITE_lowering)
if(PAYKAN_TEST_SUITE_lowering)
add_executable(lowering_tests
    Lowering/LoweringTests.cpp
    Lowering/RoundTripTests.cpp
)
target_include_directories(lowering_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(lowering_tests
    PRIVATE PAYKAN_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/samples"
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
endif()

# --- The AST interchange format: the round trip over the samples corpus and --
# --- the reader's errors, tests/AST/InterchangeTests.cpp (ast-interchange) ----
paykan_test_suite(ast-interchange PAYKAN_TEST_SUITE_ast_interchange)
if(PAYKAN_TEST_SUITE_ast_interchange)
add_executable(ast_interchange_tests
    AST/InterchangeTests.cpp
)
target_compile_definitions(ast_interchange_tests
    PRIVATE PAYKAN_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/samples"
)
target_link_libraries(ast_interchange_tests
    PRIVATE gtest_main
            paykan_ast_interchange
            paykan_frontend
            paykan_ast_printer
            paykan_compile_options
)
paykan_link_plugins(ast_interchange_tests)
add_test(NAME ASTInterchangeTests COMMAND ast_interchange_tests)
endif()

# --- Plugin registry: the compatibility check, #103 (plugin) ------------------
paykan_test_suite(plugin PAYKAN_TEST_SUITE_plugin)
if(PAYKAN_TEST_SUITE_plugin)
add_executable(plugin_tests
    Plugin/RegistryTests.cpp
)
target_compile_definitions(plugin_tests
    PRIVATE PAYKAN_TEST_ACCEPTED_VERSIONS="${PAYKAN_TEST_ACCEPTED_VERSIONS}"
)
target_link_libraries(plugin_tests
    PRIVATE gtest_main
            paykan_plugin
            paykan_compile_options
)
add_test(NAME PluginTests COMMAND plugin_tests)

# --- The plugin loader (tests/Plugin/LoaderTests.cpp): C plugins built from ---
# --- tests/Plugin/modules/test_plugin.c, one variant per module, loaded into ---
# --- the real paykan through every discovery path -------------------------------
set(PAYKAN_TEST_PLUGIN_DIR ${CMAKE_CURRENT_BINARY_DIR}/plugins)
set(PAYKAN_TEST_PLUGIN_TARGETS "")
# paykan_test_plugin(<name> <subdir> [<definition>...]): the module
# <subdir>/lib<name><suffix> below PAYKAN_TEST_PLUGIN_DIR.
function(paykan_test_plugin name subdir)
    paykan_test_plugin_from(test_plugin.c ${name} ${subdir} ${ARGN})
    set(PAYKAN_TEST_PLUGIN_TARGETS ${PAYKAN_TEST_PLUGIN_TARGETS} PARENT_SCOPE)
endfunction()
function(paykan_test_frontend name subdir)
    paykan_test_plugin_from(test_frontend.c ${name} ${subdir} ${ARGN})
    set(PAYKAN_TEST_PLUGIN_TARGETS ${PAYKAN_TEST_PLUGIN_TARGETS} PARENT_SCOPE)
endfunction()
function(paykan_test_plugin_from source name subdir)
    set(target paykan_test_plugin_${name})
    add_library(${target} MODULE Plugin/modules/${source})
    target_link_libraries(${target} PRIVATE paykan_plugin_api)
    target_compile_definitions(${target} PRIVATE ${ARGN})
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
        PREFIX "lib" OUTPUT_NAME ${name}
        SUFFIX "${PAYKAN_TEST_PLUGIN_SUFFIX}"
        LIBRARY_OUTPUT_DIRECTORY ${PAYKAN_TEST_PLUGIN_DIR}/${subdir})
    if(PAYKAN_COVERAGE)
        # Test code: not measured, and no profile runtime of its own.
        target_compile_options(${target} PRIVATE -fno-profile-instr-generate
                                                 -fno-coverage-mapping)
        target_link_options(${target} PRIVATE -fno-profile-instr-generate)
    endif()
    set(PAYKAN_TEST_PLUGIN_TARGETS ${PAYKAN_TEST_PLUGIN_TARGETS} ${target} PARENT_SCOPE)
endfunction()
if(APPLE)
    set(PAYKAN_TEST_PLUGIN_SUFFIX ".dylib")
else()
    set(PAYKAN_TEST_PLUGIN_SUFFIX ".so")
endif()
paykan_test_plugin(good good)
paykan_test_plugin(old old [=[TP_NAME="test-old"]=] [=[TP_BUILD_VERSION="0.0.9"]=])
paykan_test_plugin(api api [=[TP_NAME="test-api"]=] TP_API_VERSION=999)
paykan_test_plugin(null null [=[TP_NAME="test-null"]=] TP_NULL_DESCRIPTOR)
paykan_test_plugin(noentry noentry [=[TP_NAME="test-noentry"]=] TP_NO_ENTRY_POINT)
paykan_test_plugin(noemit noemit [=[TP_NAME="test-noemit"]=] TP_NO_EMIT)
paykan_test_plugin(norun norun [=[TP_NAME="test-norun"]=] TP_NO_RUN)
paykan_test_plugin(noname noname [=[TP_NAME="test-noname"]=] TP_NO_NAME)
paykan_test_plugin(twice twice [=[TP_NAME="test-twice"]=] TP_TWICE)
paykan_test_plugin(none none [=[TP_NAME="test-none"]=] TP_NUM_BACKENDS=0)
paykan_test_plugin(tiny tiny [=[TP_NAME="test-tiny"]=] TP_PLUGIN_SIZE=4)
paykan_test_plugin(short short [=[TP_NAME="test-short"]=] TP_PLUGIN_SIZE=16)
paykan_test_plugin(bshort bshort [=[TP_NAME="test-bshort"]=] TP_BACKEND_SIZE=8)
paykan_test_plugin(early early [=[TP_NAME="test-early"]=]
    "TP_PLUGIN_SIZE=offsetof(PaykanPlugin, num_frontends)")
paykan_test_plugin(future future [=[TP_NAME="test-future"]=] TP_FUTURE)
paykan_test_frontend(fe fe)
paykan_test_frontend(fe_old fe_old [=[TP_NAME="test-fe-old"]=] [=[TP_BUILD_VERSION="0.0.9"]=])
paykan_test_frontend(fe_nofree fe_nofree [=[TP_NAME="test-fe-nofree"]=] TP_NO_FREE)
paykan_test_frontend(fe_noparse fe_noparse [=[TP_NAME="test-fe-noparse"]=] TP_NO_PARSE)
paykan_test_frontend(fe_clash fe_clash [=[TP_NAME="recursive-descent"]=])
paykan_test_plugin(dup_a dup [=[TP_NAME="dup"]=])
paykan_test_plugin(dup_b dup [=[TP_NAME="dup"]=])
paykan_test_plugin(dup_old dupold [=[TP_NAME="dup"]=] [=[TP_BUILD_VERSION="0.0.9"]=])
paykan_test_plugin(clash clash [=[TP_NAME="c"]=])
# Not a library at all.
file(WRITE ${PAYKAN_TEST_PLUGIN_DIR}/corrupt/libcorrupt${PAYKAN_TEST_PLUGIN_SUFFIX}
    "this is not a shared library\n")

include(GNUInstallDirs)
file(RELATIVE_PATH PAYKAN_TEST_PLUGIN_DIR_FROM_BINDIR
    "${CMAKE_INSTALL_FULL_BINDIR}"
    "${CMAKE_INSTALL_FULL_LIBDIR}/paykan/plugins/${PAYKAN_VERSION}")
add_executable(plugin_loader_tests Plugin/LoaderTests.cpp)
target_compile_definitions(plugin_loader_tests PRIVATE
    PAYKAN_BIN="$<TARGET_FILE:paykan>"
    PAYKAN_TEST_PLUGIN_DIR="${PAYKAN_TEST_PLUGIN_DIR}"
    PAYKAN_TEST_PLUGIN_SUFFIX="${PAYKAN_TEST_PLUGIN_SUFFIX}"
    PAYKAN_TEST_PLUGIN_DIR_FROM_BINDIR="${PAYKAN_TEST_PLUGIN_DIR_FROM_BINDIR}"
    PAYKAN_TEST_ACCEPTED_VERSIONS="${PAYKAN_TEST_ACCEPTED_VERSIONS}"
    PAYKAN_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/samples"
)
target_link_libraries(plugin_loader_tests
    PRIVATE gtest_main
            paykan_plugin_api
            paykan_compile_options
)
add_dependencies(plugin_loader_tests paykan ${PAYKAN_TEST_PLUGIN_TARGETS})
add_test(NAME PluginLoaderTests COMMAND plugin_loader_tests)
endif()

# A driver with every plugin of the build plus a frontend and a backend that
# claim to be built with a PaykanLang version this one does not accept
# (Driver/IncompatiblePlugins.cpp), for the driver tests (driver).
paykan_test_suite(driver PAYKAN_TEST_SUITE_driver)
if(PAYKAN_TEST_SUITE_driver)
add_executable(paykan_incompatible_plugins
    ${CMAKE_SOURCE_DIR}/src/Driver/driver_main.cpp
    Driver/IncompatiblePlugins.cpp
)
target_link_libraries(paykan_incompatible_plugins
    PRIVATE paykan_driver
            paykan_compile_options
)
paykan_link_plugins(paykan_incompatible_plugins)
endif()

# --- Runtime tests (runtime) -------------------------------------------------
paykan_test_suite(runtime PAYKAN_TEST_SUITE_runtime)
if(PAYKAN_TEST_SUITE_runtime)
add_executable(runtime_tests
    Runtime/ArrayTests.cpp
    Runtime/DeferredDestroyTests.cpp
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
endif()

# --- Driver smoke tests: --check-only, --dump-ast (driver) -------------------
paykan_test_suite(driver PAYKAN_TEST_SUITE_driver)
if(PAYKAN_TEST_SUITE_driver)
add_executable(driver_tests
    Driver/DriverTests.cpp
    Driver/PkmTests.cpp
)
target_compile_definitions(driver_tests
    PRIVATE PAYKAN_BIN="$<TARGET_FILE:paykan>"
            PAYKAN_INCOMPATIBLE_PLUGINS_BIN="$<TARGET_FILE:paykan_incompatible_plugins>"
            PAYKAN_TEST_ACCEPTED_VERSIONS="${PAYKAN_TEST_ACCEPTED_VERSIONS}"
            PAYKAN_EXPECTED_DEFAULT_BACKEND="${PAYKAN_DEFAULT_BACKEND}"
)
target_link_libraries(driver_tests
    PRIVATE gtest_main
            paykan_pkm
            paykan_compile_options
)
add_dependencies(driver_tests paykan paykan_incompatible_plugins)
if(PAYKAN_COVERAGE)
    target_compile_definitions(driver_tests PRIVATE PAYKAN_TEST_COVERAGE=1)
endif()
# Once per backend that runs programs (see backendFlag() in DriverTests.cpp).
foreach(be IN LISTS PAYKAN_BACKENDS)
    if(be STREQUAL "llvm" OR be STREQUAL "c")
        add_test(NAME DriverTests.${be} COMMAND driver_tests)
        # No plugins from the plugin directories: the listings these tests
        # check exactly are the build's own (tests/Plugin/LoaderTests.cpp
        # covers discovery).
        set_tests_properties(DriverTests.${be} PROPERTIES
            ENVIRONMENT "PAYKAN_TEST_BACKEND=${be};PAYKAN_NO_PLUGINS=1")
    endif()
endforeach()
endif()

# The script-driven tests (scripts/*.py, standard library only) run with the
# system's Python 3, or the one given with -DPython3_EXECUTABLE=<path>.
# Without one they are not registered.
find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "Python 3 not found: the script-driven tests are skipped "
                   "(set Python3_EXECUTABLE to enable them)")
endif()
# paykan_add_python_test(<name> <script> [args...]): a ctest running
# scripts/<script> with that interpreter.
function(paykan_add_python_test name script)
    if(Python3_Interpreter_FOUND)
        add_test(NAME ${name}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/${script} ${ARGN})
    endif()
endfunction()

# The samples corpus on every backend that runs programs: identical stdout,
# exit codes and zero live heap blocks (scripts/samples_parity.py) (samples).
set(PAYKAN_SAMPLES_BACKENDS "")
foreach(be IN LISTS PAYKAN_BACKENDS)
    if(be STREQUAL "llvm" OR be STREQUAL "c")
        list(APPEND PAYKAN_SAMPLES_BACKENDS --backend ${be})
    endif()
endforeach()
paykan_test_suite(samples PAYKAN_TEST_SUITE_samples)
if(PAYKAN_TEST_SUITE_samples AND PAYKAN_SAMPLES_BACKENDS)
    paykan_add_python_test(SamplesParity samples_parity.py
                --paykan $<TARGET_FILE:paykan> ${PAYKAN_SAMPLES_BACKENDS})
    # The same corpus built ahead of time (`paykan build`) and run as
    # executables.
    paykan_add_python_test(SamplesParityBuild samples_parity.py --build
                --paykan $<TARGET_FILE:paykan> ${PAYKAN_SAMPLES_BACKENDS})
    # The corpus at -O0 too (the default is -O2), which keeps the
    # unoptimised code paths of both backends covered.
    paykan_add_python_test(SamplesParityO0 samples_parity.py -O 0
                --paykan $<TARGET_FILE:paykan> ${PAYKAN_SAMPLES_BACKENDS})
    # They compile the same import samples through the same per-project
    # object / bitcode caches (samples/imports/*/.paykan_cache): one at a
    # time.
    if(TEST SamplesParity)
        set_tests_properties(SamplesParity SamplesParityBuild SamplesParityO0
            PROPERTIES RESOURCE_LOCK paykan_samples_cache)
    endif()

    # Concurrent builds with different flags (-O0 / -O2, plain / ASan) of one
    # project never link each other's cached objects (#134).  Not in a
    # coverage build: its programs must be compiled by the compiler that
    # built the runtime, which the script's ASan `CC` wrapper is not.
    if(NOT PAYKAN_COVERAGE)
        paykan_add_python_test(CacheRace cache_race.py
                    --paykan $<TARGET_FILE:paykan> ${PAYKAN_SAMPLES_BACKENDS})
    endif()

    # Where `paykan` finds the runtime (Toolchain.cpp): an installed binary
    # uses its own prefix even while the build tree exists, $PAYKAN_RUNTIME_DIR
    # overrides it, and only a binary inside the build tree uses the build
    # tree's runtime (scripts/runtime_lookup.py).
    include(GNUInstallDirs)
    paykan_add_python_test(RuntimeLookup runtime_lookup.py
                --paykan $<TARGET_FILE:paykan>
                --runtime-lib $<TARGET_FILE:paykan_runtime>
                --runtime-header ${CMAKE_SOURCE_DIR}/src/Runtime/Runtime.h
                --build-dir ${CMAKE_BINARY_DIR}
                --installed-lib ${CMAKE_INSTALL_FULL_LIBDIR}/$<TARGET_FILE_NAME:paykan_runtime>
                ${PAYKAN_SAMPLES_BACKENDS})
endif()

# The C backend's output is strict ISO C11 (docs/c-backend.md): every
# sample's C compiles with -std=c11 -pedantic-errors -Wall -Wextra -Werror on
# every C compiler found (GCC and Clang), at -O0 and -O2, and runs exactly
# like `paykan --backend=c run` (scripts/c_strict.py).  `paykan build` is
# checked too, unless the runtime archive is sanitizer- or
# coverage-instrumented (it then links only with the compiler that built it).
# (c-strict)
paykan_test_suite(c-strict PAYKAN_TEST_SUITE_c_strict)
if(PAYKAN_TEST_SUITE_c_strict AND "c" IN_LIST PAYKAN_BACKENDS)
    set(PAYKAN_STRICT_CCS "")
    find_program(PAYKAN_STRICT_GCC NAMES gcc)
    find_program(PAYKAN_STRICT_CLANG NAMES clang)
    foreach(cc IN ITEMS ${PAYKAN_STRICT_GCC} ${PAYKAN_STRICT_CLANG})
        if(cc)
            list(APPEND PAYKAN_STRICT_CCS --cc ${cc})
            if(NOT PAYKAN_SANITIZER_FLAGS AND NOT PAYKAN_COVERAGE_FLAGS)
                list(APPEND PAYKAN_STRICT_CCS --build-cc ${cc})
            endif()
        endif()
    endforeach()
    if(PAYKAN_STRICT_CCS)
        paykan_add_python_test(CStrictC11 c_strict.py
                    --paykan $<TARGET_FILE:paykan> ${PAYKAN_STRICT_CCS})
        # `paykan build` writes the import samples' object caches.
        if(TEST CStrictC11)
            set_tests_properties(CStrictC11 PROPERTIES
                RESOURCE_LOCK paykan_samples_cache)
        endif()
    endif()
endif()

# --- Frontend tests: lexer, recovery and fuzz smoke of the recursive-descent --
# --- frontend; and the differential check of every enabled frontend over -----
# --- the samples corpus (an out-of-tree frontend runs the fuzz smoke and -----
# --- differential tests, and the parser and Sema suites, through ------------
# --- paykan_add_frontend_tests: cmake/PaykanTestSupport.cmake) (frontend) ----
paykan_test_suite(frontend PAYKAN_TEST_SUITE_frontend)
if(PAYKAN_TEST_SUITE_frontend)
add_executable(frontend_tests
    Frontend/LexerTests.cpp
    Frontend/RecoveryTests.cpp
    Frontend/FuzzSmokeTests.cpp
    Frontend/DifferentialTests.cpp
)
target_compile_definitions(frontend_tests
    PRIVATE PAYKAN_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/samples"
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
endif()

# A plain configure (no options) is the core build:
# recursive-descent + c, nothing downloaded, no third-party/ or _deps/
# (#123).  Configure only, in a scratch directory of its own.  Not in a
# coverage build: it runs no instrumented code, and scripts/coverage.py
# requires every test to write a profile.  (configure)
paykan_test_suite(configure PAYKAN_TEST_SUITE_configure)
if(PAYKAN_TEST_SUITE_configure AND NOT PAYKAN_COVERAGE)
    add_test(NAME DefaultConfigure
        COMMAND ${CMAKE_COMMAND}
                -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
                -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/default-configure
                -DGENERATOR=${CMAKE_GENERATOR}
                -DC_COMPILER=${CMAKE_C_COMPILER}
                -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/DefaultConfigure.cmake)
endif()

# --- Documentation: every relative link in the Markdown files resolves -------
# --- (scripts/check_links.py), and every doc example that states its output --
# --- prints it (scripts/doc_examples.py). MarkdownLinks is not in a coverage
# --- build: it runs no instrumented code, and scripts/coverage.py requires
# --- every test to write a profile. (docs)
paykan_test_suite(docs PAYKAN_TEST_SUITE_docs)
if(PAYKAN_TEST_SUITE_docs AND NOT PAYKAN_COVERAGE)
    paykan_add_python_test(MarkdownLinks check_links.py)
endif()
paykan_test_suite(docs PAYKAN_TEST_SUITE_docs)
if(PAYKAN_TEST_SUITE_docs AND PAYKAN_SAMPLES_BACKENDS)
    paykan_add_python_test(DocExamples doc_examples.py
                --paykan $<TARGET_FILE:paykan> ${PAYKAN_SAMPLES_BACKENDS})
endif()

# --- Loadable plugins against an installed PaykanLang: install this tree, ----
# --- build src/Backends/PrintPIR (C) against it with find_package(Paykan) -
# --- load it into the INSTALLED paykan via --plugin, $PAYKAN_PLUGIN_PATH ----
# --- and its plugin directory; its output must be --emit-pir's --------------
# --- (tests/OutOfTree/PrintPIR.cmake; CI's install job does the same). ------
# --- Skipped for instrumented builds, whose installed libraries only link ---
# --- with the instrumentation flags. (out-of-tree)
paykan_test_suite(out-of-tree PAYKAN_TEST_SUITE_out_of_tree)
if(PAYKAN_TEST_SUITE_out_of_tree AND NOT PAYKAN_SANITIZER_FLAGS
   AND NOT PAYKAN_COVERAGE_FLAGS)
    set(PAYKAN_OOT_ARGS "")
    if(TARGET paykan_backend_llvm)
        list(APPEND PAYKAN_OOT_ARGS -DLLVM_DIR=${LLVM_DIR})
    endif()
    add_test(NAME PrintPIROutOfTree
        COMMAND ${CMAKE_COMMAND}
                -DPAYKAN_BUILD_DIR=${CMAKE_BINARY_DIR}
                -DPAYKAN_SOURCE_DIR=${CMAKE_SOURCE_DIR}
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/print-pir-out-of-tree
                -DGENERATOR=${CMAKE_GENERATOR}
                -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                ${PAYKAN_OOT_ARGS}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/OutOfTree/PrintPIR.cmake)
    # The configure-time plugin compatibility check (#103): src/Backends/PrintPIR
    # configured against a scratch install, pinned to an accepted and to an
    # unaccepted PaykanLang version (tests/OutOfTree/PluginCompat.cmake).
    add_test(NAME PluginCompatOutOfTree
        COMMAND ${CMAKE_COMMAND}
                -DPAYKAN_BUILD_DIR=${CMAKE_BINARY_DIR}
                -DPAYKAN_SOURCE_DIR=${CMAKE_SOURCE_DIR}
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/plugin-compat-out-of-tree
                -DGENERATOR=${CMAKE_GENERATOR}
                -DACCEPTED_VERSION=${PAYKAN_VERSION}
                "-DACCEPTED_LIST=${PAYKAN_TEST_ACCEPTED_VERSIONS}"
                -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                ${PAYKAN_OOT_ARGS}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/OutOfTree/PluginCompat.cmake)
endif()

# --- The exported frontend test support: install this tree, build the -------
# --- parser, Sema and frontend suites against it with ------------------------
# --- paykan_add_frontend_tests() for the installation's recursive-descent ---
# --- frontend and run them (tests/OutOfTree/FrontendTests.cmake), as an -----
# --- out-of-tree frontend does. Skipped for instrumented builds, like -------
# --- PrintPIROutOfTree.
paykan_test_suite(out-of-tree PAYKAN_TEST_SUITE_out_of_tree)
if(PAYKAN_TEST_SUITE_out_of_tree AND PAYKAN_INSTALL_TEST_SUPPORT
   AND NOT PAYKAN_SANITIZER_FLAGS AND NOT PAYKAN_COVERAGE_FLAGS)
    set(PAYKAN_OOT_FE_ARGS "")
    if(TARGET paykan_backend_llvm)
        list(APPEND PAYKAN_OOT_FE_ARGS -DLLVM_DIR=${LLVM_DIR})
    endif()
    if(googletest_SOURCE_DIR)
        list(APPEND PAYKAN_OOT_FE_ARGS -DGTEST_SOURCE_DIR=${googletest_SOURCE_DIR})
    elseif(GTest_DIR)
        list(APPEND PAYKAN_OOT_FE_ARGS -DGTest_DIR=${GTest_DIR})
    endif()
    add_test(NAME FrontendTestsOutOfTree
        COMMAND ${CMAKE_COMMAND}
                -DPAYKAN_BUILD_DIR=${CMAKE_BINARY_DIR}
                -DPAYKAN_SOURCE_DIR=${CMAKE_SOURCE_DIR}
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/frontend-tests-out-of-tree
                -DGENERATOR=${CMAKE_GENERATOR}
                -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                ${PAYKAN_OOT_FE_ARGS}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/OutOfTree/FrontendTests.cmake)
    # They all install this build tree (and write its install_manifest.txt).
    set_tests_properties(PrintPIROutOfTree PluginCompatOutOfTree
                         FrontendTestsOutOfTree PROPERTIES
        RESOURCE_LOCK paykan_install)
endif()

# Every suite of tests/suites.json has a section above.
paykan_check_test_suites()
