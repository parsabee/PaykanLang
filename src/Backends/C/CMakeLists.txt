# src/Backends/C/CMakeLists.txt
# ----------------------------------------------------------------------------
# paykan_backend_c -- the C backend (PIR -> C11, built with the system cc).
# Part of the core: depends on nothing but the standard library and POSIX
# (the system toolchain helpers live in src/Backends/Toolchain).
# ----------------------------------------------------------------------------

add_library(paykan_backend_c STATIC
    CBackend.cpp
    CEmitter.cpp
    CBuild.cpp
)
target_include_directories(paykan_backend_c
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(paykan_backend_c
    PUBLIC  paykan_backend
            paykan_backend_toolchain
    PRIVATE paykan_compile_options
)
add_dependencies(paykan_backend_c paykan_runtime)
paykan_add_plugin(paykan_backend_c)
