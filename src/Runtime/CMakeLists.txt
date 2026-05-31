# src/Runtime/CMakeLists.txt
# --------------------------------------------------------------------------
# paykan_runtime — C runtime library for Paykan's builtin class types.
#
# This static library is linked into every compiled Paykan program.
# It provides Object/String constructors, destructors, vtables, and
# method implementations with C linkage.
# --------------------------------------------------------------------------

add_library(paykan_runtime STATIC
    Object.c
    String.c
    Shared.c
    IO.c
)

target_include_directories(paykan_runtime
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

# The runtime is pure C — no RTTI/exception flags needed.
target_compile_options(paykan_runtime PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -std=c11
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3>
)
