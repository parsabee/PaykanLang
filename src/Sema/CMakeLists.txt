# src/Sema/CMakeLists.txt
# ----------------------------------------------------------------------------
# paykan_sema -- static library for semantic analysis.
# ----------------------------------------------------------------------------

add_library(paykan_sema STATIC
    Sema.cpp
    SemaClass.cpp
    SemaImport.cpp
)
target_link_libraries(paykan_sema
    PUBLIC  paykan_diag
            paykan_frontend
    PRIVATE paykan_compile_options
)
