# src/Driver/CMakeLists.txt
# ────────────────────────────────────────────────────────────────────────────
# Executable entry points for the Paykan project.
# Each binary is defined here so they share a single location for mains.
# ────────────────────────────────────────────────────────────────────────────

# ── paykan – compiler driver ─────────────────────────────────────────────────
add_executable(paykan main.cpp)
target_include_directories(paykan SYSTEM
    PRIVATE ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(paykan
    PRIVATE paykan_ast_printer
            paykan_parser
            paykan_compile_options
            LLVMSupport
)
