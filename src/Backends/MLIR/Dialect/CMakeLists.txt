# paykan_mlir_pir_dialect -- the `pir` MLIR dialect (PIR.td): PIR's items,
# statements and instructions as MLIR ops.

set(LLVM_TARGET_DEFINITIONS PIR.td)
mlir_tablegen(PIRDialect.h.inc -gen-dialect-decls -dialect=pir)
mlir_tablegen(PIRDialect.cpp.inc -gen-dialect-defs -dialect=pir)
mlir_tablegen(PIRTypes.h.inc -gen-typedef-decls -typedefs-dialect=pir)
mlir_tablegen(PIRTypes.cpp.inc -gen-typedef-defs -typedefs-dialect=pir)
mlir_tablegen(PIREnums.h.inc -gen-enum-decls)
mlir_tablegen(PIREnums.cpp.inc -gen-enum-defs)
mlir_tablegen(PIROps.h.inc -gen-op-decls)
mlir_tablegen(PIROps.cpp.inc -gen-op-defs)
add_public_tablegen_target(paykan_mlir_pir_dialect_incgen)

add_library(paykan_mlir_pir_dialect STATIC PIRDialect.cpp)
add_dependencies(paykan_mlir_pir_dialect paykan_mlir_pir_dialect_incgen)
target_include_directories(paykan_mlir_pir_dialect
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
# The TableGen output (the .inc files, in the binary directory) is included
# as system headers, like MLIR's own: the generated code draws warnings
# (unused parameters, mostly) that the dialect's sources can't fix, in every
# translation unit that includes the dialect.
target_include_directories(paykan_mlir_pir_dialect SYSTEM
    PUBLIC ${CMAKE_CURRENT_BINARY_DIR} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS}
)
paykan_llvm_no_werror(paykan_mlir_pir_dialect)
target_link_libraries(paykan_mlir_pir_dialect
    PRIVATE paykan_compile_options
    PUBLIC  MLIRFunctionInterfaces
            MLIRInferTypeOpInterface
            MLIRSideEffectInterfaces
            MLIRIR
)
