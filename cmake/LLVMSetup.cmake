# LLVMSetup.cmake - Downloads and configures LLVM prebuilt binaries

# Check if we have prebuilt LLVM in third-party first
if(EXISTS ${LLVM_INSTALL_DIR}/lib/cmake/llvm)
    set(LLVM_DIR ${LLVM_INSTALL_DIR}/lib/cmake/llvm)
    message(STATUS "Found prebuilt LLVM in ${LLVM_INSTALL_DIR}")
endif()

# Try to find LLVM only in the project's third-party directory
if(DEFINED LLVM_DIR)
    find_package(LLVM CONFIG QUIET PATHS ${LLVM_DIR} NO_DEFAULT_PATH)
endif()

# If not found, download prebuilt binaries
if(NOT LLVM_FOUND)
    message(STATUS "LLVM not found, attempting to download prebuilt binaries...")

    if(NOT EXISTS ${LLVM_INSTALL_DIR})
        message(STATUS "Downloading prebuilt LLVM...")

        # Determine platform
        if(APPLE)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
                set(LLVM_PLATFORM "arm64-apple-darwin22.0")
            else()
                message(FATAL_ERROR "Only Apple Silicon (ARM64) is supported on macOS. Intel x64 is not supported.")
            endif()
        elseif(UNIX)
            set(LLVM_PLATFORM "x86_64-linux-gnu-ubuntu-18.04")
        else()
            message(FATAL_ERROR "Unsupported platform for prebuilt LLVM")
        endif()

        set(LLVM_URL "https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/clang+llvm-${LLVM_VERSION}-${LLVM_PLATFORM}.tar.xz")
        set(LLVM_ARCHIVE "${THIRD_PARTY_DIR}/llvm-${LLVM_VERSION}.tar.xz")

        file(MAKE_DIRECTORY ${THIRD_PARTY_DIR})

        message(STATUS "Downloading LLVM from: ${LLVM_URL}")
        file(DOWNLOAD ${LLVM_URL} ${LLVM_ARCHIVE}
             SHOW_PROGRESS
             STATUS DOWNLOAD_STATUS)

        list(GET DOWNLOAD_STATUS 0 DOWNLOAD_CODE)
        if(NOT DOWNLOAD_CODE EQUAL 0)
            message(FATAL_ERROR "Failed to download LLVM: ${DOWNLOAD_STATUS}")
        endif()

        message(STATUS "Extracting LLVM to ${LLVM_INSTALL_DIR}")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar xf ${LLVM_ARCHIVE}
            WORKING_DIRECTORY ${THIRD_PARTY_DIR}
            RESULT_VARIABLE EXTRACT_RESULT
        )

        if(NOT EXTRACT_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to extract LLVM archive")
        endif()

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

# Configure LLVM
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)
include(HandleLLVMOptions)

include_directories(${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})
