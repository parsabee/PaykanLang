# src/Backends/LLVM/CMakeLists.txt
# ----------------------------------------------------------------------------
# paykan_backend_llvm -- the `llvm` backend plugin: PIR -> LLVM IR translation
# (PIRToLLVM.cpp, with the per-module bitcode cache), the ORC JIT (JIT/)
# and native object emission for `build` (NativeBuild.cpp, linked with the
# shared system toolchain, src/Backends/Toolchain) behind the backend
# interface.  Built only when `llvm` is listed
# in PAYKAN_BACKENDS; the only consumer of LLVM in the compiler.
# ----------------------------------------------------------------------------

add_subdirectory(JIT)

llvm_map_components_to_libnames(BACKEND_LLVM_LIBS
    core support passes orcjit native bitreader bitwriter linker
)

add_library(paykan_backend_llvm STATIC
    LLVMBackend.cpp
    NativeBuild.cpp
    PIRToLLVM.cpp
)
target_include_directories(paykan_backend_llvm SYSTEM
    PRIVATE ${LLVM_INCLUDE_DIRS}
)
target_include_directories(paykan_backend_llvm
    PUBLIC ${PROJECT_SOURCE_DIR}/src/Backends/LLVM
)
target_link_libraries(paykan_backend_llvm
    PUBLIC  paykan_backend
    PRIVATE paykan_jit
            paykan_backend_toolchain
            paykan_runtime
            paykan_compile_options
            ${BACKEND_LLVM_LIBS}
)

paykan_add_plugin(paykan_backend_llvm)
