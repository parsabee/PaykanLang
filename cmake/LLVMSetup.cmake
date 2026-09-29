# LLVMSetup.cmake
# ----------------------------------------------------------------------------
# Downloads pre-built LLVM binaries on first configure and caches them under
# ${LLVM_INSTALL_DIR}.  Subsequent configures reuse the cached copy.
#
# Provides:
#   paykan_llvm  -- INTERFACE library carrying LLVM include dirs & definitions.
#                  Link with: target_link_libraries(<t> PRIVATE paykan_llvm)
#   LLVM CMake   -- AddLLVM, HandleLLVMOptions, and the full LLVM package.
#
# Supported platforms:
#   • macOS  ARM64 (Apple Silicon)
#   • Linux  x86_64
#   • Linux  AArch64
# ----------------------------------------------------------------------------

# Fast path: reuse a previously-downloaded copy.
if(EXISTS ${LLVM_INSTALL_DIR}/lib/cmake/llvm)
    set(LLVM_DIR ${LLVM_INSTALL_DIR}/lib/cmake/llvm)
    message(STATUS "Found prebuilt LLVM in ${LLVM_INSTALL_DIR}")
endif()

if(DEFINED LLVM_DIR)
    find_package(LLVM CONFIG QUIET PATHS ${LLVM_DIR} NO_DEFAULT_PATH)
endif()

# Slow path: download, verify hash, extract, then find_package.
if(NOT LLVM_FOUND)
    message(STATUS "LLVM not found, attempting to download prebuilt binaries...")

    if(NOT EXISTS ${LLVM_INSTALL_DIR})
        message(STATUS "Downloading prebuilt LLVM...")

        # Select platform-specific URL suffix and SHA-256 hash.
        if(APPLE)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
                set(LLVM_PLATFORM "arm64-apple-darwin22.0")
                set(LLVM_HASH "SHA256=1264eb3c2a4a6d5e9354c3e5dc5cb6c6481e678f6456f36d2e0e566e9400fcad")
            else()
                message(FATAL_ERROR "Only Apple Silicon (ARM64) is supported on macOS. Intel x64 is not supported.")
            endif()
        elseif(UNIX)
            # Upstream only publishes one glibc build per architecture; the
            # Ubuntu-22.04 x86_64 build runs on any distro with glibc >= 2.34.
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
                set(LLVM_PLATFORM "x86_64-linux-gnu-ubuntu-22.04")
                set(LLVM_HASH "SHA256=884ee67d647d77e58740c1e645649e29ae9e8a6fe87c1376be0f3a30f3cc9ab3")
            elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
                set(LLVM_PLATFORM "aarch64-linux-gnu")
                set(LLVM_HASH "SHA256=6dd62762285326f223f40b8e4f2864b5c372de3f7de0731cb7cd55ca5287b75a")
            else()
                message(FATAL_ERROR
                    "Unsupported Linux architecture: ${CMAKE_SYSTEM_PROCESSOR}.\n"
                    "Supported platforms:\n"
                    "  - macOS ARM64 (Apple Silicon)\n"
                    "  - Linux x86_64\n"
                    "  - Linux AArch64"
                )
            endif()
        else()
            message(FATAL_ERROR
                "Unsupported platform: ${CMAKE_SYSTEM_NAME}.\n"
                "Supported platforms:\n"
                "  - macOS ARM64 (Apple Silicon)\n"
                "  - Linux x86_64\n"
                "  - Linux AArch64"
            )
        endif()

        set(LLVM_URL "https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/clang+llvm-${LLVM_VERSION}-${LLVM_PLATFORM}.tar.xz")
        set(LLVM_ARCHIVE "${THIRD_PARTY_DIR}/llvm-${LLVM_VERSION}.tar.xz")
        file(MAKE_DIRECTORY ${THIRD_PARTY_DIR})

        # Download with SHA-256 verification.
        message(STATUS "Downloading LLVM from: ${LLVM_URL}")
        file(DOWNLOAD ${LLVM_URL} ${LLVM_ARCHIVE}
             SHOW_PROGRESS
             EXPECTED_HASH ${LLVM_HASH}
             STATUS DOWNLOAD_STATUS)

        list(GET DOWNLOAD_STATUS 0 DOWNLOAD_CODE)
        if(NOT DOWNLOAD_CODE EQUAL 0)
            message(FATAL_ERROR "Failed to download LLVM: ${DOWNLOAD_STATUS}")
        endif()

        # Extract and rename to a stable directory name.
        message(STATUS "Extracting LLVM to ${LLVM_INSTALL_DIR}")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar xf ${LLVM_ARCHIVE}
            WORKING_DIRECTORY ${THIRD_PARTY_DIR}
            RESULT_VARIABLE EXTRACT_RESULT
        )
        if(NOT EXTRACT_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to extract LLVM archive")
        endif()

        # The archive extracts as clang+llvm-<version>-<triple>/; normalise.
        file(GLOB EXTRACTED_DIR "${THIRD_PARTY_DIR}/clang+llvm-*")
        if(EXTRACTED_DIR)
            file(RENAME ${EXTRACTED_DIR} ${LLVM_INSTALL_DIR})
        else()
            message(FATAL_ERROR "Could not find extracted LLVM directory")
        endif()

        file(REMOVE ${LLVM_ARCHIVE})
        message(STATUS "LLVM installed to: ${LLVM_INSTALL_DIR}")
    endif()

    set(LLVM_DIR ${LLVM_INSTALL_DIR}/lib/cmake/llvm)
    find_package(LLVM REQUIRED CONFIG PATHS ${LLVM_DIR} NO_DEFAULT_PATH)
endif()

# -- Import LLVM's own CMake helpers ------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)              # llvm_add_library, llvm_add_executable, etc.
include(HandleLLVMOptions)    # Applies LLVM-recommended compiler flags

# -- paykan_llvm INTERFACE target ---------------------------------------------
# Scoped alternative to global include_directories() / add_definitions().
# Link only the targets that actually need LLVM headers & definitions.
add_library(paykan_llvm INTERFACE)
target_include_directories(paykan_llvm INTERFACE ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(paykan_llvm INTERFACE ${LLVM_DEFINITIONS_LIST})
