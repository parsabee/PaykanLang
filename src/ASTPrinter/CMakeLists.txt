# src/ASTPrinter/CMakeLists.txt
# ----------------------------------------------------------------------------
# paykan_ast_printer – static library for AST pretty-printing.
# ----------------------------------------------------------------------------

add_library(paykan_ast_printer STATIC ASTPrinter.cpp)
target_link_libraries(paykan_ast_printer
    PUBLIC  paykan_ast
    PRIVATE paykan_compile_options
)
