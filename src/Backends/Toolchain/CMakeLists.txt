# src/Backends/Toolchain/CMakeLists.txt
# ----------------------------------------------------------------------------
# paykan_backend_toolchain -- the system toolchain shared by the native
# backends (include/paykan/backends/Toolchain.h): runtime lookup, process
# spawning and linking against libpaykan_runtime.a.  The C backend compiles
# and links with it; the llvm backend links the objects it emits with it.
# Standard C++ plus POSIX.
# ----------------------------------------------------------------------------

add_library(paykan_backend_toolchain STATIC Toolchain.cpp)
target_include_directories(paykan_backend_toolchain
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)
# Where this build tree's runtime lives, so `paykan build` / `run` work
# without an install step; the install layout is the fallback at run time.
# A sanitizer build instruments libpaykan_runtime.a, so programs linked
# against it need the same -fsanitize flags (SanitizerSetup.cmake).
list(JOIN PAYKAN_SANITIZER_FLAGS " " _paykan_toolchain_sanitizer_flags)
target_compile_definitions(paykan_backend_toolchain PRIVATE
    PAYKAN_RUNTIME_LIB_PATH="$<TARGET_FILE:paykan_runtime>"
    PAYKAN_RUNTIME_INCLUDE_DIR="${CMAKE_SOURCE_DIR}/src/Runtime"
    PAYKAN_SANITIZER_FLAGS="${_paykan_toolchain_sanitizer_flags}"
)
target_link_libraries(paykan_backend_toolchain
    PRIVATE paykan_compile_options
)
add_dependencies(paykan_backend_toolchain paykan_runtime)
