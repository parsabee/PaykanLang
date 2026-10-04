# src/ASTInterchange/CMakeLists.txt
# ----------------------------------------------------------------------------
# paykan_ast_interchange -- the AST interchange format
# (include/paykan/ast/Interchange.h, docs/plugins/ast-format.md): the text a
# frontend plugin returns its AST in, its writer (paykan --emit-ast) and its
# reader.  Standard C++ only.
# ----------------------------------------------------------------------------

add_library(paykan_ast_interchange STATIC
    Writer.cpp
    Reader.cpp
)
target_include_directories(paykan_ast_interchange
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(paykan_ast_interchange
    PUBLIC  paykan_ast
    PRIVATE paykan_compile_options
)
