# paykan_sema -- static library for semantic analysis.

add_library(paykan_sema STATIC
    Sema.cpp
    SemaClass.cpp
    SemaImport.cpp
    SemaParamModes.cpp
)
target_link_libraries(paykan_sema
    PUBLIC  paykan_diag
            paykan_frontend
            paykan_modules
    PRIVATE paykan_compile_options
)
