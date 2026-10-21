# paykan_backend_llvm -- the `llvm` backend plugin: PIR -> LLVM IR translation
# (PIRToLLVM.cpp) behind the backend interface; the bitcode cache, the ORC
# JIT and native object emission for `build` are paykan_llvm_codegen's
# (CodeGen/) and paykan_jit's (JIT/), which the mlir backend shares.  Built
# only when `llvm` is listed in PAYKAN_BACKENDS.

llvm_map_components_to_libnames(BACKEND_LLVM_LIBS
    core support
)

add_library(paykan_backend_llvm STATIC
    LLVMBackend.cpp
    PIRToLLVM.cpp
)
target_include_directories(paykan_backend_llvm SYSTEM
    PRIVATE ${LLVM_INCLUDE_DIRS}
)
paykan_llvm_no_werror(paykan_backend_llvm)
target_include_directories(paykan_backend_llvm
    PUBLIC ${PROJECT_SOURCE_DIR}/src/Backends/LLVM
)
target_link_libraries(paykan_backend_llvm
    PUBLIC  paykan_backend
    PRIVATE paykan_llvm_codegen
            paykan_runtime
            paykan_compile_options
            ${BACKEND_LLVM_LIBS}
)

paykan_add_plugin(paykan_backend_llvm)
