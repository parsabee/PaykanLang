# src/CodeGen/CMakeLists.txt
# ----------------------------------------------------------------------------
# paykan_codegen -- static library for LLVM IR code generation.
# ----------------------------------------------------------------------------

llvm_map_components_to_libnames(CODEGEN_LLVM_LIBS
    core support passes bitreader bitwriter linker
)

add_library(paykan_codegen STATIC CodeGen.cpp ClassCodeGen.cpp CodeGenImport.cpp)
target_include_directories(paykan_codegen SYSTEM
    PRIVATE ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(paykan_codegen
    PUBLIC  paykan_frontend
    PRIVATE paykan_compile_options
            ${CODEGEN_LLVM_LIBS}
)
