# paykan_frontend_recursive_descent -- the default frontend plugin: a
# reentrant lexer and recursive-descent parser.  Standard C++ only; builds
# with the project's -fno-exceptions -fno-rtti policy and is always built.

add_library(paykan_frontend_recursive_descent STATIC
    Lexer.cpp
    Parser.cpp
    RecursiveDescentFrontend.cpp
)

target_include_directories(paykan_frontend_recursive_descent
    PUBLIC  ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(paykan_frontend_recursive_descent
    PUBLIC  paykan_frontend
    PRIVATE paykan_compile_options
)

paykan_add_plugin(paykan_frontend_recursive_descent)
