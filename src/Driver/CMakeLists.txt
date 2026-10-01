# src/Driver/CMakeLists.txt
# ----------------------------------------------------------------------------
# Executable entry points for the Paykan project.
# Each binary is defined here so they share a single location for mains.
# ----------------------------------------------------------------------------

# -- paykan – compiler driver -------------------------------------------------
add_executable(paykan main.cpp Options.cpp)
target_include_directories(paykan SYSTEM
    PRIVATE ${LLVM_INCLUDE_DIRS}
)
llvm_map_components_to_libnames(DRIVER_LLVM_LIBS
    core support passes orcjit native
)

target_link_libraries(paykan
    PRIVATE paykan_ast_printer
            paykan_codegen
            paykan_sema
            paykan_parser
            paykan_jit
            paykan_runtime
            paykan_compile_options
            ${DRIVER_LLVM_LIBS}
)
