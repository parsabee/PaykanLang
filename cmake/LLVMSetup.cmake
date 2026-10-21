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
#   • Linux  ARM64 (aarch64)

# The prebuilt LLVM links with zstd (see "The libraries the prebuilt LLVM
# links with" below); Homebrew's prefix is not on CMake's search path on
# every Mac, so look for its zstd there.
if(APPLE AND NOT zstd_ROOT)
    find_program(PAYKAN_BREW brew)
    if(PAYKAN_BREW)
        execute_process(COMMAND ${PAYKAN_BREW} --prefix zstd
                        OUTPUT_VARIABLE PAYKAN_BREW_ZSTD
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        RESULT_VARIABLE PAYKAN_BREW_RC ERROR_QUIET)
        if(PAYKAN_BREW_RC EQUAL 0 AND EXISTS "${PAYKAN_BREW_ZSTD}/include/zstd.h")
            set(zstd_ROOT "${PAYKAN_BREW_ZSTD}")
        endif()
    endif()
endif()

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

        # Select platform-specific URL suffix and SHA-256 hash.  Upstream
        # publishes one build per platform (LLVM-<version>-<platform>.tar.xz),
        # MLIR included.
        if(APPLE)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
                set(LLVM_PLATFORM "macOS-ARM64")
                set(LLVM_HASH "SHA256=f260f4f7c0d430828a81ae8a3826a1d63fc0963ec2459489308cc23b1f7eab4f")
            else()
                message(FATAL_ERROR "Only Apple Silicon (ARM64) is supported on macOS. Intel x64 is not supported.")
            endif()
        elseif(UNIX)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
                set(LLVM_PLATFORM "Linux-X64")
                set(LLVM_HASH "SHA256=df0e1ecf16caf3489a272a5eea4eec9b0d82878f6477fa309504f918a0006384")
            elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
                set(LLVM_PLATFORM "Linux-ARM64")
                set(LLVM_HASH "SHA256=805efad2bb91cb4967fa569e0881d10c0f69c04461cf671cccbae19f547acc34")
            else()
                message(FATAL_ERROR
                    "Unsupported Linux architecture: ${CMAKE_SYSTEM_PROCESSOR}.\n"
                    "Supported platforms:\n"
                    "  - macOS ARM64 (Apple Silicon)\n"
                    "  - Linux x86_64\n"
                    "  - Linux ARM64 (aarch64)"
                )
            endif()
        else()
            message(FATAL_ERROR
                "Unsupported platform: ${CMAKE_SYSTEM_NAME}.\n"
                "Supported platforms:\n"
                "  - macOS ARM64 (Apple Silicon)\n"
                "  - Linux x86_64\n"
                "  - Linux ARM64 (aarch64)"
            )
        endif()

        set(LLVM_URL "https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/LLVM-${LLVM_VERSION}-${LLVM_PLATFORM}.tar.xz")
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

        # Extract what Paykan uses and rename it to a stable directory name:
        # the headers, the libraries with their CMake packages, and the tools
        # the build (mlir-tblgen) and the scripts (clang, clang-format,
        # clang-tidy, the coverage and sanitizer tools) run.  The rest --
        # flang, lldb, lld, the other tools and the libraries only they use
        # -- is most of the archive's 12 GB.
        message(STATUS "Extracting LLVM to ${LLVM_INSTALL_DIR}")
        set(LLVM_TOP "LLVM-${LLVM_VERSION}-${LLVM_PLATFORM}")
        string(REGEX MATCH "^[0-9]+" LLVM_MAJOR "${LLVM_VERSION}")
        set(LLVM_MEMBERS "${LLVM_TOP}/include" "${LLVM_TOP}/lib")
        foreach(tool mlir-tblgen llvm-tblgen llvm-config clang clang++
                     clang-${LLVM_MAJOR} clang-format clang-tidy llvm-cov
                     llvm-profdata llvm-symbolizer)
            list(APPEND LLVM_MEMBERS "${LLVM_TOP}/bin/${tool}")
        endforeach()
        set(LLVM_EXCLUDES)
        set(LLVM_EXCLUDED_LIBS libFortran* libflang* liblldb* libclang.*
                               libclang-cpp.* LLVMPolly.*)
        if(APPLE)
            # The archive's libc++, which Paykan never uses: it builds with
            # the system's, the one the archive's libraries were built
            # against.  Its dylibs would shadow the system libc++ in the
            # linker's process (see libLTO below), and without its headers
            # the vendored clang (the coverage build's) compiles with the
            # SDK's, which match the libc++ a program links with.
            list(APPEND LLVM_EXCLUDED_LIBS libc++.* libc++abi.* libunwind.*)
            list(APPEND LLVM_EXCLUDES "--exclude=${LLVM_TOP}/include/c++")
        else()
            # macOS links the libraries through it (see below).
            list(APPEND LLVM_EXCLUDED_LIBS libLTO.*)
        endif()
        foreach(lib IN LISTS LLVM_EXCLUDED_LIBS)
            list(APPEND LLVM_EXCLUDES "--exclude=${LLVM_TOP}/lib/${lib}")
        endforeach()
        # (The system tar: CMake's cannot leave members out.)
        execute_process(
            COMMAND tar -xJf ${LLVM_ARCHIVE} ${LLVM_EXCLUDES} ${LLVM_MEMBERS}
            WORKING_DIRECTORY ${THIRD_PARTY_DIR}
            RESULT_VARIABLE EXTRACT_RESULT
        )
        if(NOT EXTRACT_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to extract LLVM archive")
        endif()
        if(EXISTS ${THIRD_PARTY_DIR}/${LLVM_TOP})
            file(RENAME ${THIRD_PARTY_DIR}/${LLVM_TOP} ${LLVM_INSTALL_DIR})
        else()
            message(FATAL_ERROR "Could not find extracted LLVM directory")
        endif()

        # LLVM's and MLIR's CMake packages check that every file they export
        # exists; the ones left out above are empty placeholders, which
        # nothing builds with or runs.
        foreach(exports llvm/LLVMExports-release.cmake mlir/MLIRTargets-release.cmake)
            file(STRINGS "${LLVM_INSTALL_DIR}/lib/cmake/${exports}" LLVM_IMPORTS
                 REGEX "IMPORTED_LOCATION_RELEASE")
            foreach(line IN LISTS LLVM_IMPORTS)
                string(REGEX REPLACE ".*_IMPORT_PREFIX}/([^\"]+)\".*" "\\1"
                       imported "${line}")
                if(NOT EXISTS "${LLVM_INSTALL_DIR}/${imported}")
                    file(TOUCH "${LLVM_INSTALL_DIR}/${imported}")
                endif()
            endforeach()
        endforeach()

        file(REMOVE ${LLVM_ARCHIVE})
        message(STATUS "LLVM installed to: ${LLVM_INSTALL_DIR}")
    endif()

    set(LLVM_DIR ${LLVM_INSTALL_DIR}/lib/cmake/llvm)
    find_package(LLVM REQUIRED CONFIG PATHS ${LLVM_DIR} NO_DEFAULT_PATH)
endif()

# -- The libraries the prebuilt LLVM links with ---------------------------------
# LLVMSupport links zlib and zstd (compressed sections and bitcode), which
# the system provides: libzstd-dev / zlib1g-dev on Debian and Ubuntu,
# `brew install zstd` on macOS.  LLVM's package looks for them; say what is
# missing here rather than leave a "target was not found" error for later.
if(LLVM_ENABLE_ZSTD AND NOT TARGET zstd::libzstd_static)
    message(FATAL_ERROR
        "LLVM ${LLVM_PACKAGE_VERSION} links with zstd, and no static libzstd "
        "was found: install it (Debian/Ubuntu: libzstd-dev, macOS: brew install "
        "zstd), or point zstd_ROOT at one.")
endif()
if(LLVM_ENABLE_ZLIB AND NOT TARGET ZLIB::ZLIB)
    message(FATAL_ERROR
        "LLVM ${LLVM_PACKAGE_VERSION} links with zlib, which was not found: "
        "install it (Debian/Ubuntu: zlib1g-dev), or point ZLIB_ROOT at one.")
endif()
# Only LLVMWindowsManifest uses libxml2, and Paykan does not link it: without
# libxml2 the target LLVM's package names just has to exist.
if(LLVM_ENABLE_LIBXML2 AND NOT TARGET LibXml2::LibXml2)
    add_library(LibXml2::LibXml2 INTERFACE IMPORTED)
endif()

# -- macOS: the libraries are LLVM bitcode --------------------------------------
# The macOS release is built with ThinLTO, so its static libraries hold LLVM
# 22 bitcode, which Xcode's linker reads only through a libLTO of the same
# LLVM: the archive's own, which compiles the members an executable uses
# when it is linked (cached under <build>/lto.cache, so a relink compiles
# only what changed).  (The Linux releases hold native objects.)  The linker
# loads it with the directory it is in first on its library search path, so
# it is given from a directory of its own: from lib/, a dylib there named
# like one of the linker's own (the archive's libc++.1.dylib) would replace
# it, and the linker would abort.
if(APPLE AND EXISTS "${LLVM_INSTALL_DIR}/lib/libLTO.dylib")
    set(PAYKAN_LIBLTO "${LLVM_INSTALL_DIR}/lto/libLTO.dylib")
    if(NOT EXISTS "${PAYKAN_LIBLTO}")
        file(MAKE_DIRECTORY "${LLVM_INSTALL_DIR}/lto")
        file(CREATE_LINK "${LLVM_INSTALL_DIR}/lib/libLTO.dylib"
             "${PAYKAN_LIBLTO}" COPY_ON_ERROR)
    endif()
    add_link_options(
        "LINKER:-lto_library,${PAYKAN_LIBLTO}"
        "LINKER:-cache_path_lto,${CMAKE_BINARY_DIR}/lto.cache")
endif()

# -- Import LLVM's own CMake helpers ------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)              # llvm_add_library, llvm_add_executable, etc.
include(HandleLLVMOptions)    # Applies LLVM-recommended compiler flags

# -- paykan_llvm INTERFACE target ---------------------------------------------
# Scoped alternative to global include_directories() / add_definitions().
# Link only the targets that actually need LLVM headers & definitions.
add_library(paykan_llvm INTERFACE)

# paykan_llvm_no_werror(<target>...): warnings are not errors in <target>.
# For the targets that include LLVM headers (the llvm backend and its JIT,
# and the tests that use them): the project is C++20, and LLVM's headers,
# C++17 code, draw warnings there that our code can't fix, e.g. libstdc++
# 15's #warning on the <ciso646> include in llvm/Support/Threading.h.  The
# warnings are still printed.
function(paykan_llvm_no_werror)
    set_target_properties(${ARGN} PROPERTIES PAYKAN_NO_WERROR ON)
endfunction()
target_include_directories(paykan_llvm INTERFACE ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(paykan_llvm INTERFACE ${LLVM_DEFINITIONS_LIST})
