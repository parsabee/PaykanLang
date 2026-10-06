# paykan_runtime: the C runtime linked into every compiled Paykan program.

add_library(paykan_runtime STATIC
    Object.c
    Basic.c
    String.c
    Array.c
    Tuple.c
    File.c
    Error.c
    Shared.c
    IO.c
    Heap.c
)

target_include_directories(paykan_runtime
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

# Pure C: no RTTI / exception flags.
target_compile_options(paykan_runtime PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -std=c11
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3>
)
