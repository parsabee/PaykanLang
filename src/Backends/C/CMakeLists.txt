# paykan_c_build -- building a C translation of PIR with the system cc, one
# cached object per module (paykan/backends/c/CBuild.h): what the c backend
# and the mlir backend's C path share.
# paykan_backend_c -- the C backend (PIR -> C11, built with the system cc).
# Both are part of the core: they depend on nothing but the standard library
# and POSIX (every OS call is behind the toolchain's portability layer,
# src/Backends/Toolchain/Platform.h).

add_library(paykan_c_build STATIC
    CBuild.cpp
)
target_include_directories(paykan_c_build
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    # The toolchain's private headers (Platform.h, ToolchainNames.h).
    PRIVATE ${CMAKE_SOURCE_DIR}/src/Backends/Toolchain
)
target_link_libraries(paykan_c_build
    PUBLIC  paykan_backend
            paykan_backend_toolchain
    PRIVATE paykan_compile_options
)
add_dependencies(paykan_c_build paykan_runtime)

add_library(paykan_backend_c STATIC
    CBackend.cpp
    CEmitter.cpp
)
target_include_directories(paykan_backend_c
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(paykan_backend_c
    PUBLIC  paykan_backend
            paykan_c_build
    PRIVATE paykan_compile_options
)
paykan_add_plugin(paykan_backend_c)
