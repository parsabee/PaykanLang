# src/Frontends/Handwritten/CMakeLists.txt
# ----------------------------------------------------------------------------
# paykan_frontend_handwritten -- the default frontend: a handwritten,
# reentrant lexer and recursive-descent parser.  Standard C++ only; builds
# with the project's -fno-exceptions -fno-rtti policy.
# ----------------------------------------------------------------------------

add_library(paykan_frontend_handwritten STATIC
    Lexer.cpp
    Parser.cpp
)

target_include_directories(paykan_frontend_handwritten
    PUBLIC  ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(paykan_frontend_handwritten
    PUBLIC  paykan_ast
            paykan_diag
    PRIVATE paykan_compile_options
)
