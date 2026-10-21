# paykan_backend_mlir -- the `mlir` backend plugin (docs/design/mlir-backend.md):
# PIR imported into the `pir` MLIR dialect (Dialect/, Import/) and lowered
# through MLIR.  Built only when `mlir` is listed in PAYKAN_BACKENDS.

add_subdirectory(Dialect)

add_library(paykan_backend_mlir STATIC
    MLIRBackend.cpp
    Import/PIRImport.cpp
)
target_include_directories(paykan_backend_mlir
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(paykan_backend_mlir SYSTEM
    PRIVATE ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS}
)
paykan_llvm_no_werror(paykan_backend_mlir)
target_link_libraries(paykan_backend_mlir
    PUBLIC  paykan_backend
            paykan_mlir_pir_dialect
    PRIVATE paykan_compile_options
            MLIRArithDialect
            MLIRIR
)

paykan_add_plugin(paykan_backend_mlir)
