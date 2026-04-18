# src/Sema/CMakeLists.txt
# ----------------------------------------------------------------------------
# paykan_sema -- static library for semantic analysis.
# ----------------------------------------------------------------------------

add_library(paykan_sema STATIC Sema.cpp)
target_include_directories(paykan_sema SYSTEM
    PRIVATE ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(paykan_sema
    PUBLIC  paykan_parser
    PRIVATE paykan_compile_options
            LLVMSupport
)
