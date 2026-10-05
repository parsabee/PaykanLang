# src/Backends/LLVM/JIT/CMakeLists.txt
# ----------------------------------------------------------------------------
# paykan_jit – ORC-based JIT execution engine for Paykan
# ----------------------------------------------------------------------------

llvm_map_components_to_libnames(JIT_LLVM_LIBS
    orcjit
    jitlink            # ObjectLinkingLayer / EHFrameRegistrar (JIT.cpp)
    orctargetprocess   # llvm_orc_registerEHFrameSectionWrapper (JIT.cpp)
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
target_link_libraries(paykan_jit
    PRIVATE paykan_runtime
            paykan_compile_options
            ${JIT_LLVM_LIBS}
)
