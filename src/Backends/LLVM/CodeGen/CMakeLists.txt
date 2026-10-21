# paykan_llvm_codegen -- what the backends that produce LLVM IR (llvm, mlir)
# share: the per-project bitcode cache, linking and -O<n> (ModuleCache.cpp),
# and running the program with the JIT or building it as a native object
# linked with the system toolchain (NativeBuild.cpp).  Built when a listed
# backend needs LLVM.

llvm_map_components_to_libnames(CODEGEN_LLVM_LIBS
    core support passes native bitreader bitwriter linker target
)

add_library(paykan_llvm_codegen STATIC
    ModuleCache.cpp
    NativeBuild.cpp
)
target_include_directories(paykan_llvm_codegen
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(paykan_llvm_codegen SYSTEM
    PUBLIC ${LLVM_INCLUDE_DIRS}
)
paykan_llvm_no_werror(paykan_llvm_codegen)
target_link_libraries(paykan_llvm_codegen
    PUBLIC  paykan_pir
            paykan_jit
    PRIVATE paykan_backend_toolchain
            paykan_runtime
            paykan_compile_options
            ${CODEGEN_LLVM_LIBS}
)
