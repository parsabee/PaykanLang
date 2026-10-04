# src/Frontend/CMakeLists.txt
# ----------------------------------------------------------------------------
# paykan_frontend -- the frontend interface (include/paykan/Frontend.h), the
# plugin registry (include/paykan/Registry.h) and the ParserDriver facade that
# parses a file with the selected frontend.  Standard C++ only; it knows no
# frontend by name except the build's default.
# ----------------------------------------------------------------------------

add_library(paykan_frontend STATIC
    ParserDriver.cpp
)

target_include_directories(paykan_frontend
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_compile_definitions(paykan_frontend
    PRIVATE PAYKAN_DEFAULT_FRONTEND="${PAYKAN_DEFAULT_FRONTEND}"
)

target_link_libraries(paykan_frontend
    PUBLIC  paykan_plugin
            paykan_ast
            paykan_diag
    PRIVATE paykan_compile_options
)
