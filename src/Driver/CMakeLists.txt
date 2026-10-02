# src/Driver/CMakeLists.txt
# ----------------------------------------------------------------------------
# Executable entry points for the Paykan project.
# Each binary is defined here so they share a single location for mains.
# ----------------------------------------------------------------------------

# -- paykan – compiler driver -------------------------------------------------
add_executable(paykan main.cpp Options.cpp)

target_link_libraries(paykan
    PRIVATE paykan_ast_printer
            paykan_sema
            paykan_frontend
            paykan_backend
            paykan_lowering
            paykan_pir
            paykan_compile_options
)
paykan_link_plugins(paykan)
