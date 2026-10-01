# src/Backends/LLVM/CMakeLists.txt
# ----------------------------------------------------------------------------
# paykan_backend_llvm -- the `llvm` backend plugin: PIR -> LLVM IR translation
# (PIRToLLVM.cpp, with the per-module bitcode cache) and the ORC JIT
# (src/JIT) behind the backend interface.  Built only when `llvm` is listed
# in PAYKAN_BACKENDS; the only consumer of LLVM in the compiler.
# ----------------------------------------------------------------------------

add_subdirectory(${PROJECT_SOURCE_DIR}/src/JIT ${CMAKE_BINARY_DIR}/src/JIT)

llvm_map_components_to_libnames(BACKEND_LLVM_LIBS
    core support passes orcjit native bitreader bitwriter linker
)

add_library(paykan_backend_llvm STATIC LLVMBackend.cpp PIRToLLVM.cpp)
target_include_directories(paykan_backend_llvm SYSTEM
    PRIVATE ${LLVM_INCLUDE_DIRS}
)
target_include_directories(paykan_backend_llvm
    PUBLIC ${PROJECT_SOURCE_DIR}/src/Backends/LLVM
)
target_link_libraries(paykan_backend_llvm
    PUBLIC  paykan_backend
    PRIVATE paykan_jit
            paykan_runtime
            paykan_compile_options
            ${BACKEND_LLVM_LIBS}
)

paykan_add_plugin(paykan_backend_llvm)
