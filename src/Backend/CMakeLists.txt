# src/Backend/CMakeLists.txt
# ----------------------------------------------------------------------------
# paykan_backend -- the backend interface (include/paykan/Backend.h) and its
# registry.  Standard C++ only; it knows no backend by name except the
# build's default.
# ----------------------------------------------------------------------------

add_library(paykan_backend STATIC
    Backend.cpp
)

target_include_directories(paykan_backend
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_compile_definitions(paykan_backend
    PRIVATE PAYKAN_DEFAULT_BACKEND="${PAYKAN_DEFAULT_BACKEND}"
)

target_link_libraries(paykan_backend
    PUBLIC  paykan_plugin
            paykan_pir
            paykan_sema
    PRIVATE paykan_compile_options
)
