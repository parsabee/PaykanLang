# src/AST/CMakeLists.txt
# ----------------------------------------------------------------------------
# paykan_ast -- static library for AST node definitions and ASTContext.
# ----------------------------------------------------------------------------

add_library(paykan_ast STATIC
    AST.cpp
    ASTContext.cpp
)

target_include_directories(paykan_ast
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(paykan_ast
    PRIVATE paykan_compile_options
)
