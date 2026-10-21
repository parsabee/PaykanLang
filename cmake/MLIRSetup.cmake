# MLIR, for the mlir backend: the package of the LLVM LLVMSetup.cmake found
# (the prebuilt release ships it), and MLIR's TableGen helpers for the
# backend's dialect.
#
# Provides:
#   MLIR CMake   -- the MLIR package (MLIR_INCLUDE_DIRS, the MLIR* library
#                   targets), AddMLIR and TableGen: mlir_tablegen().

if(NOT MLIR_DIR)
    set(MLIR_DIR "${LLVM_INSTALL_DIR}/lib/cmake/mlir")
    if(LLVM_DIR AND NOT EXISTS "${MLIR_DIR}")
        # An LLVM given with LLVM_DIR: its MLIR sits next to it.
        get_filename_component(MLIR_DIR "${LLVM_DIR}/../mlir" ABSOLUTE)
    endif()
endif()
find_package(MLIR REQUIRED CONFIG PATHS ${MLIR_DIR} NO_DEFAULT_PATH)
message(STATUS "Found MLIR ${LLVM_PACKAGE_VERSION} in ${MLIR_DIR}")

list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}")
include(TableGen)             # tablegen(), add_public_tablegen_target()
include(AddMLIR)              # mlir_tablegen()

# The TableGen files of the backend's dialect include MLIR's .td files.
include_directories(SYSTEM ${MLIR_INCLUDE_DIRS})

