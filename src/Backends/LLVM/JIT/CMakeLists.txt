# paykan_jit – ORC-based JIT execution engine for Paykan

llvm_map_components_to_libnames(JIT_LLVM_LIBS
    orcjit
    jitlink            # LLJIT's default object linking layer
    orctargetprocess   # the in-process EH-frame registration it uses
    native
)

add_library(paykan_jit STATIC JIT.cpp)
target_include_directories(paykan_jit
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    PRIVATE ${PROJECT_SOURCE_DIR}/include
)
target_include_directories(paykan_jit SYSTEM
    PRIVATE ${LLVM_INCLUDE_DIRS}
)
paykan_llvm_no_werror(paykan_jit)
# The concurrency prototype's runtimes (docs/design/concurrency.md): MLIR's
# async runtime and LLVM's OpenMP runtime, shared libraries of the LLVM the
# compiler is built with, loaded when a program uses them.
target_compile_definitions(paykan_jit PRIVATE
    PAYKAN_MLIR_ASYNC_RUNTIME="${LLVM_LIBRARY_DIR}/libmlir_async_runtime${CMAKE_SHARED_LIBRARY_SUFFIX}"
    PAYKAN_OPENMP_RUNTIME="${LLVM_LIBRARY_DIR}/libomp${CMAKE_SHARED_LIBRARY_SUFFIX}"
)
target_link_libraries(paykan_jit
    PRIVATE paykan_runtime
            paykan_compile_options
            ${JIT_LLVM_LIBS}
)
