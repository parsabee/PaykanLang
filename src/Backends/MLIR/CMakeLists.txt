# paykan_backend_mlir -- the mlir backend plugin (docs/design/mlir-backend.md):
# PIR imported into the `pir` MLIR dialect (Dialect/, Import/), the lowering
# both targets share (Lowering/), the LLVM path (ToLLVM/: `run` and `build`,
# through paykan_llvm_codegen), the C path (ToEmitC/: --emit-c) and the
# pipeline that runs them (Pipeline.cpp).  The in-tree backend, always built.

add_subdirectory(Dialect)

add_library(paykan_backend_mlir STATIC
    MLIRBackend.cpp
    Pipeline.cpp
    Import/PIRImport.cpp
    Lowering/PIRToStandard.cpp
    ToEmitC/ToEmitC.cpp
    ToLLVM/ToLLVM.cpp
)
target_include_directories(paykan_backend_mlir
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    # The toolchain's private headers (ToolchainNames.h).
    PRIVATE ${CMAKE_SOURCE_DIR}/src/Backends/Toolchain
)
target_include_directories(paykan_backend_mlir SYSTEM
    PRIVATE ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS}
)
paykan_llvm_no_werror(paykan_backend_mlir)
target_link_libraries(paykan_backend_mlir
    PUBLIC  paykan_backend
            paykan_mlir_pir_dialect
    PRIVATE paykan_llvm_codegen
            paykan_compile_options
            MLIRArithDialect
            MLIRArithToEmitC
            MLIRArithToLLVM
            MLIRBuiltinToLLVMIRTranslation
            MLIRControlFlowToLLVM
            MLIREmitCDialect
            MLIREmitCTransforms
            MLIRFuncDialect
            MLIRFuncToEmitC
            MLIRFuncToLLVM
            MLIRIR
            MLIRLLVMCommonConversion
            MLIRLLVMDialect
            MLIRLLVMToLLVMIRTranslation
            MLIRPass
            MLIRReconcileUnrealizedCasts
            MLIRSCFDialect
            MLIRSCFToControlFlow
            MLIRSCFToEmitC
            MLIRTargetCpp
            MLIRTargetLLVMIRExport
            MLIRTransforms
)

paykan_add_plugin(paykan_backend_mlir)
