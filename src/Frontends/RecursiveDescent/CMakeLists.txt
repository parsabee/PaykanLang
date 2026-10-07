# paykan_frontend_recursive_descent -- the default frontend plugin: a
# reentrant lexer and recursive-descent parser.  Standard C++ only; builds
# with the project's -fno-exceptions -fno-rtti policy and is always built.
#
# paykan_lexer_recursive_descent -- its lexer, a library of its own so that
#                                   the formatter reads the same tokens.
# paykan_format                  -- the source formatter (`paykan fmt`,
#                                   paykan/format/Format.h).

add_library(paykan_lexer_recursive_descent STATIC Lexer.cpp)
target_include_directories(paykan_lexer_recursive_descent
    PUBLIC  ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(paykan_lexer_recursive_descent
    PUBLIC  paykan_frontend
    PRIVATE paykan_compile_options
)

add_library(paykan_frontend_recursive_descent STATIC
    Parser.cpp
    RecursiveDescentFrontend.cpp
)

target_include_directories(paykan_frontend_recursive_descent
    PUBLIC  ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(paykan_frontend_recursive_descent
    PUBLIC  paykan_frontend
            paykan_lexer_recursive_descent
    PRIVATE paykan_compile_options
)

paykan_add_plugin(paykan_frontend_recursive_descent)

add_library(paykan_format STATIC Format.cpp)
target_include_directories(paykan_format
    PUBLIC  ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(paykan_format
    PRIVATE paykan_lexer_recursive_descent
            paykan_compile_options
)
