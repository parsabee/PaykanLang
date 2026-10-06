# paykan_diag -- central diagnostic engine (shared by parser and sema).
# Lives below paykan_parser and paykan_sema so both can link it without
# introducing a circular dependency.

add_library(paykan_diag STATIC
    DiagEngine.cpp
)

target_include_directories(paykan_diag
    PUBLIC  ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(paykan_diag
    PUBLIC  paykan_ast
    PRIVATE paykan_compile_options
)
