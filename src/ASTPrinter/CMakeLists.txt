# src/ASTPrinter/CMakeLists.txt
# ────────────────────────────────────────────────────────────────────────────
# paykan_ast_printer – static library for AST pretty-printing.
# ────────────────────────────────────────────────────────────────────────────

add_library(paykan_ast_printer STATIC ASTPrinter.cpp)
target_include_directories(paykan_ast_printer SYSTEM
    PRIVATE ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(paykan_ast_printer
    PUBLIC  paykan_parser
    PRIVATE paykan_compile_options
            LLVMSupport
)
