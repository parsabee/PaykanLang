# src/Backends/MLIR/CMakeLists.txt
# ----------------------------------------------------------------------------
# The `mlir` backend slot.  The option, the registration point and the LLVM
# + MLIR dependency wiring are in place; the backend itself (PIR -> paykan
# dialect -> scf/memref) is future work, so listing it is an error for now.
#
# When it lands: add_library(paykan_backend_mlir ...) implementing
# paykan::backend::Backend, registered with PAYKAN_REGISTER_BACKEND(mlir,
# "mlir", ...), and paykan_add_plugin(paykan_backend_mlir).
# ----------------------------------------------------------------------------

message(FATAL_ERROR
    "PAYKAN_BACKENDS lists 'mlir', but the MLIR backend is not implemented yet "
    "(see src/Backends/MLIR/CMakeLists.txt).")
